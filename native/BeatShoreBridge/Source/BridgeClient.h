#pragma once

// The plugin-side half of PROTOCOL.md: connects to the BeatShore desktop
// process's named pipe, does HELLO/CAPABILITIES, and turns "analyze this
// captured audio" requests from the UI into a real ANALYSIS_REQUEST /
// ANALYSIS_RESULT round trip. This is the client role proven working by
// BeatShoreTestClient (native/BeatShoreTestClient) -- the logic here is the
// same sequence, just living inside the actual VST3 instead of a standalone
// harness, and running on a JUCE background thread instead of a console
// process's main().
//
// THREADING: everything that touches the plugin<->desktop pipe (connect,
// HELLO, HEARTBEAT, ANALYSIS_REQUEST, all reads) happens on this class's own
// juce::Thread and nowhere else. That's not a style choice -- splitting pipe
// reads and writes across two threads was empirically found to hang Win32
// named pipes solid during BeatShoreDesktop's own development (see the
// threading note at the top of BeatShoreDesktop/Source/main.cpp). Don't
// add a second thread that touches `pipe` without re-verifying that finding.
//
// The audio thread (processBlock) never touches the pipe, never allocates,
// and never blocks on anything here -- it only does lock-free atomic status
// reads via getStatus(), same as the rest of the plugin's host-context
// reporting.
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros break std::min/std::max in any TU that includes this header
#endif
#include <windows.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>
#include <sstream>
#include "../../protocol/OverlappedPipeIO.h"
#include "../../protocol/MiniJson.h"
#include "../../protocol/SharedAudioBuffer.h"
#include "BridgeTypes.h"

class BridgeClient final : private juce::Thread
{
public:
    BridgeClient() : juce::Thread("BeatShoreBridgeIPC") { cancelEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr); }

    // Order matters and is the actual fix, not just cleanup ordering: if
    // the IPC thread is blocked inside a read when the plugin is destroyed,
    // stopThread() alone could time out with the thread still blocked --
    // and this destructor would otherwise go on to destroy `pipe` right out
    // from under it (a real use-after-free, not a hypothetical one).
    // signalThreadShouldExit() first so the loop stops at its next
    // cooperative check; closePipeHandle() second so a *currently blocked*
    // read is interrupted promptly rather than waiting out its deadline.
    // stopThread() last to actually join.
    //
    // Two layers of interruption now, not one: `pipe` is an
    // OverlappedPipeIO, so every read already has a real, bounded deadline
    // (CancelIoEx-based, verified in isolation -- see
    // native/protocol/OverlappedPipeIO.h and the standalone test that
    // proves a cancelled read leaves the handle usable afterward) instead
    // of blocking indefinitely. closePipeHandle() here is a *second*,
    // faster interrupt on top of that bound -- Windows resolves a pending
    // overlapped operation on a handle that gets closed by completing it
    // with an error, which is documented behavior for overlapped handles
    // (unlike the old synchronous-mode PipeLineIO, where this same pattern
    // was an empirically-necessary-but-not-fully-proven-safe workaround).
    // Still verified only for the normal (not-currently-blocked) destroy
    // path via the existing test suite, not independently re-verified for
    // "destroy while genuinely mid-read," which is hard to trigger
    // deterministically through the real BridgeClient/BeatShoreDesktop
    // stack (as opposed to the isolated OverlappedPipeIO primitive, which
    // *is* deterministically tested for exactly this).
    ~BridgeClient() override
    {
        signalThreadShouldExit();
        closePipeHandle();
        stopThread(4000);
        if (cancelEvent) CloseHandle(cancelEvent);
    }

    void start() { startThread(juce::Thread::Priority::normal); }

    // Must be called before start() (or at least before a connection can be
    // established) if HOST_STATE is to be sent. Not owned -- the caller
    // (BeatShoreBridgeAudioProcessor) outlives this object, since it holds
    // BridgeClient by unique_ptr.
    void setHostSnapshotSource(const HostSnapshot* snapshot) { hostSnapshotSource = snapshot; }

    BridgeStatus getStatus() const { return status.load(); }

    // Called from the message thread (a button click). Copies the given
    // interleaved audio into a pending-request slot; the IPC thread picks
    // it up on its next loop iteration. Returns false (and queues nothing)
    // if a request is already in flight or the bridge isn't connected --
    // the UI should disable the trigger control in that case rather than
    // relying on this to no-op silently.
    // audioSource is caller-declared, not inferred: BeatShoreBridgeAudioProcessor
    // always passes "live-captured" (this class's only real caller and the
    // only one whose audio genuinely came from a DAW track via
    // processBlock). Test code that drives this same class directly
    // (native/BridgeClientTest) must pass "file" or similar -- the whole
    // point of this field is to make that distinction auditable in the
    // desktop's logs, so it must reflect where the caller actually got the
    // audio, not be hardcoded true for every caller of this method.
    // trackName: the host DAW's name for the track this plugin instance is
    // on (JUCE's AudioProcessor::updateTrackProperties(), when the host
    // provides it -- not every host does). Purely for human-readable MIDI
    // export filenames on the desktop side (see midi-export.js) -- true
    // collision-proofing still comes entirely from requestId's UUID below,
    // never from this string, so an empty/missing trackName (test callers,
    // or a host that doesn't report track names) is fine and expected.
    bool requestAnalysis(const std::vector<float>& interleaved, uint32_t sampleRate, uint32_t channels, uint32_t frames, const juce::String& kind, const juce::String& audioSource, const juce::String& trackName = juce::String())
    {
        if (status.load() != BridgeStatus::Connected) return false;
        if (requestInFlight.exchange(true)) return false;

        // Reset here, not inside handleAnalysisRequest(): a caller is only
        // allowed to call requestCancel() once requestInFlight is true,
        // which this line just made true -- so any requestCancel() for
        // THIS request can only happen after this point. Resetting inside
        // handleAnalysisRequest() instead (as an earlier version of this
        // code did) raced against exactly that: the IPC thread might not
        // reach handleAnalysisRequest() until after the message thread
        // already called requestCancel(), and a reset there would silently
        // wipe out the cancel signal before the wait loop ever saw it --
        // found via MultiSessionTest's queued-cancel scenario, not by
        // reasoning about the code.
        cancelRequestedFlag.store(false);
        ResetEvent(cancelEvent);

        // A real UUID, not a small per-process counter: request IDs from
        // different simultaneously-connected plugin instances must never
        // collide (see PROTOCOL.md, STATUS.md's "Multiple plugin
        // instances").
        std::lock_guard<std::mutex> lock(requestMutex);
        pendingRequest = PendingRequest { interleaved, sampleRate, channels, frames, kind.toStdString(), audioSource.toStdString(), juce::Uuid().toString().toStdString(), trackName.toStdString() };
        return true;
    }

    bool isRequestInFlight() const { return requestInFlight.load(); }

    // Called from the message thread (a future Cancel button). Returns
    // false immediately if there's nothing in flight to cancel. Otherwise
    // wakes the IPC thread's result-wait loop (see handleAnalysisRequest)
    // even if it's genuinely blocked waiting on the desktop right now --
    // this is what makes cancellation from the plugin side real rather
    // than "only works if we happen to not be waiting for anything,"
    // mirroring the same multiplexed-wait technique BeatShoreDesktop's own
    // PipeSessionOwner uses (see OverlappedPipeIO.h's beginRead()/
    // pollRead()). Does not itself guarantee the desktop actually stops
    // the work -- see PROTOCOL.md's CANCEL section for the real
    // CANCELLED/CANCEL_REQUESTED/ALREADY_COMPLETED semantics.
    bool requestCancel()
    {
        if (!requestInFlight.load()) return false;
        cancelRequestedFlag.store(true);
        SetEvent(cancelEvent);
        return true;
    }

    // 0..1, updated as ANALYSIS_PROGRESS messages arrive for the in-flight
    // request; meaningless (stale) once isRequestInFlight() is false again
    // -- callers should only read this while a request is actually running.
    double getProgress() const { return progress.load(); }

    // Called from the message thread (editor Timer). Returns true and fills
    // `out` exactly once per completed request.
    bool takeResult(BridgeAnalysisResult& out)
    {
        if (!hasNewResult.exchange(false)) return false;
        std::lock_guard<std::mutex> lock(resultMutex);
        out = latestResult;
        return true;
    }

private:
    struct PendingRequest
    {
        std::vector<float> interleaved;
        uint32_t sampleRate, channels, frames;
        std::string kind;
        std::string audioSource;
        std::string requestId;
        std::string trackName;
    };

    static constexpr const char* PIPE_NAME = "\\\\.\\pipe\\BeatShoreBridge.v1";

    // Distinct from the existing per-message "v":1 field: this specifically
    // guards the HEARTBEAT/HEARTBEAT_ACK exchange's own shape, matching the
    // desktop's kProtocolVersion (main.cpp).
    static constexpr int kHeartbeatProtocolVersion = 1;

    void run() override
    {
        while (!threadShouldExit())
        {
            if (!pipe)
            {
                status.store(BridgeStatus::Connecting);
                if (!tryConnect())
                {
                    status.store(BridgeStatus::Disconnected); // honest: no desktop process found yet
                    wait(2000);
                    continue;
                }
            }

            std::optional<PendingRequest> req;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                req = std::move(pendingRequest);
                pendingRequest.reset();
            }

            if (req)
            {
                handleAnalysisRequest(*req);
                requestInFlight.store(false);
                continue;
            }

            // Idle: keep the connection alive and detect a dead desktop
            // process without spinning; also publish host context
            // periodically (PROTOCOL.md's HOST_STATE, one-way -- desktop
            // doesn't reply).
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastHeartbeatMs > 5000)
            {
                if (!sendHeartbeat())
                {
                    disconnect();
                    continue;
                }
                lastHeartbeatMs = juce::Time::getMillisecondCounter();
            }
            else if (hostSnapshotSource != nullptr && now - lastHostStateMs > 2000)
            {
                if (!sendHostState())
                {
                    disconnect();
                    continue;
                }
                lastHostStateMs = juce::Time::getMillisecondCounter();
            }
            wait(100);
        }
    }

    bool tryConnect()
    {
        HANDLE h = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false; // no BeatShore desktop process running -- not an error, just absent

        pipe = std::make_unique<OverlappedPipeIO>(h);
        rawHandle = h;

        juce::Uuid sessionUuid;
        bsjson::Value hello = bsjson::Value::object();
        hello.set("type", "HELLO");
        hello.set("v", 1);
        hello.set("pluginVersion", JucePlugin_VersionString);
        hello.set("pid", int(GetCurrentProcessId()));
        hello.set("sessionId", sessionUuid.toString().toStdString());

        if (!pipe->writeLine(bsjson::stringify(hello))) { disconnect(); return false; }

        // Bounded: a desktop process that accepts the connection but never
        // answers HELLO (a real scenario worth guarding, not just an
        // in-flight ANALYSIS_REQUEST) used to hang this read indefinitely.
        std::string capsLine;
        if (pipe->readLine(5000, capsLine) != OverlappedPipeIO::ReadResult::Ok) { disconnect(); return false; }

        try
        {
            bsjson::Value msg = bsjson::parse(capsLine);
            if (msg["type"].asString() != "CAPABILITIES") { disconnect(); return false; }
        }
        catch (const std::exception&) { disconnect(); return false; }

        status.store(BridgeStatus::Connected);
        lastHeartbeatMs = juce::Time::getMillisecondCounter();
        lastHostStateMs = lastHeartbeatMs;
        return true;
    }

    bool sendHostState()
    {
        bsjson::Value hs = bsjson::Value::object();
        hs.set("type", "HOST_STATE");
        hs.set("v", 1);
        hs.set("sampleRate", hostSnapshotSource->sampleRate.load());
        hs.set("blockSize", hostSnapshotSource->blockSize.load());
        hs.set("bpm", hostSnapshotSource->bpm.load());
        hs.set("timeSigNum", hostSnapshotSource->timeSigNumerator.load());
        hs.set("timeSigDen", hostSnapshotSource->timeSigDenominator.load());
        hs.set("isPlaying", hostSnapshotSource->isPlaying.load());
        hs.set("playheadSeconds", hostSnapshotSource->playheadSeconds.load());
        return pipe->writeLine(bsjson::stringify(hs)); // one-way: desktop doesn't reply to HOST_STATE
    }

    // A real, matched acknowledgement, not "any line that arrives counts."
    // The old version's `pipe->readLine(5000, resp) == Ok` accepted
    // literally anything -- including a late/stale ANALYSIS_RESULT for a
    // request this client had already given up on, which would then be
    // silently consumed here instead of reaching handleAnalysisRequest's
    // stale-requestId discard logic (found via a deterministic test; see
    // STATUS.md's "Overlapped I/O hardening" section). Now requires an
    // explicit HEARTBEAT_ACK with a matching heartbeatId and a supported
    // protocolVersion; anything else seen while waiting (a stale result, a
    // malformed line, an unrelated message) is explicitly discarded and
    // reading continues, rather than being mistaken for the ack.
    bool sendHeartbeat()
    {
        const std::string heartbeatId = juce::Uuid().toString().toStdString();
        bsjson::Value hb = bsjson::Value::object();
        hb.set("type", "HEARTBEAT");
        hb.set("v", 1);
        hb.set("heartbeatId", heartbeatId);
        hb.set("protocolVersion", kHeartbeatProtocolVersion);
        if (!pipe->writeLine(bsjson::stringify(hb))) return false;

        const ULONGLONG absoluteDeadline = juce::Time::getMillisecondCounter() + 5000;
        for (;;)
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= absoluteDeadline) return false;
            const DWORD remainingMs = DWORD(absoluteDeadline - now);

            std::string line;
            if (pipe->readLine(remainingMs, line) != OverlappedPipeIO::ReadResult::Ok) return false;

            bsjson::Value msg;
            try { msg = bsjson::parse(line); }
            catch (const std::exception&) { continue; } // malformed JSON -- not an ack, keep waiting

            const std::string type = msg.has("type") ? msg["type"].asString() : "";

            // A dedicated message, not something folded into HEARTBEAT_ACK
            // or a generic ERROR -- the desktop can broadcast this to an
            // idle session at any point, not just in response to this
            // heartbeat. Noticing it here (rather than only via the next
            // failed read once the desktop actually closes the pipe) means
            // an idle plugin reacts within this heartbeat's own remaining
            // wait window instead of needing a full extra 5s idle interval
            // first. Treated as a failed heartbeat -- run()'s own caller
            // already calls disconnect() when sendHeartbeat() returns
            // false, so nothing further is needed here.
            if (type == "BROKER_SHUTTING_DOWN") return false;

            if (type != "HEARTBEAT_ACK") continue; // e.g. a stale ANALYSIS_RESULT/MIDI_RESULT/ERROR -- not an ack, discard and keep waiting

            const std::string ackId = msg.has("heartbeatId") ? msg["heartbeatId"].asString() : "";
            if (ackId != heartbeatId) continue; // an ack for a DIFFERENT heartbeat -- not ours, keep waiting

            const int ackVersion = msg.has("protocolVersion") ? int(msg["protocolVersion"].asNumber()) : -1;
            if (ackVersion != kHeartbeatProtocolVersion) return false; // matched but unsupported version -- treat as a failed heartbeat, not a silent pass

            return true;
        }
    }

    void handleAnalysisRequest(const PendingRequest& req)
    {
        const std::string& requestId = req.requestId;
        std::string shmName = "Local\\BeatShoreAudio." + std::to_string(GetCurrentProcessId()) + "." + requestId;

        SharedAudioBuffer shm;
        if (!shm.create(shmName, req.sampleRate, req.channels, req.frames))
        {
            publishError(requestId, req.kind, "failed to create shared memory segment for capture audio", "SHM_CREATE_FAILED");
            return;
        }
        shm.writeSamples(req.interleaved.data(), req.frames * req.channels);

        bsjson::Value audio = bsjson::Value::object();
        audio.set("shm", shmName);
        audio.set("sampleRate", int(req.sampleRate));
        audio.set("channels", int(req.channels));
        audio.set("frames", int(req.frames));

        bsjson::Value msg = bsjson::Value::object();
        msg.set("type", "ANALYSIS_REQUEST");
        msg.set("v", 1);
        msg.set("requestId", requestId);
        msg.set("kind", req.kind);
        msg.set("audio", audio);
        msg.set("audioSource", req.audioSource);
        if (!req.trackName.empty()) msg.set("hostTrackName", req.trackName);

        if (!pipe->writeLine(bsjson::stringify(msg)))
        {
            disconnect();
            publishError(requestId, req.kind, "lost connection to BeatShore desktop while sending request", "PIPE_WRITE_FAILED");
            return;
        }

        progress.store(0.0);

        // Genuinely deadline-bound now, not just message-count-bound: each
        // read below carries the actual remaining time budget, enforced
        // inside the read itself via CancelIoEx (see OverlappedPipeIO),
        // not just checked *between* reads. A desktop
        // that accepts a connection and then never answers an
        // ANALYSIS_REQUEST at all -- zero messages, not just noisy ones --
        // now surfaces as a clean Timeout at the deadline instead of
        // blocking this thread forever. Kind-aware, matching the desktop's
        // own budget (main.cpp): the desktop already gives up on a
        // non-polyphonic request well before this fires, so this is a
        // backstop for a desktop that's stopped responding entirely, not
        // the primary bound in the normal case. messagesRead is still
        // capped, generously, purely against a pathological engine
        // spraying endless *valid* messages within the time budget.
        const DWORD timeoutMs = (req.kind == "transcribePolyphonic") ? 60000 : 10000;
        const ULONGLONG absoluteDeadline = juce::Time::getMillisecondCounter() + timeoutMs;
        int messagesRead = 0;
        // Deliberately NOT resetting cancelRequestedFlag/cancelEvent here
        // -- see requestAnalysis()'s comment for why that reset has to
        // happen there instead, before this method is ever reached.
        while (messagesRead++ < 10000)
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= absoluteDeadline)
            {
                pipe->cancelPendingRead();
                // Deliberately does NOT disconnect() -- the desktop may
                // still be working (e.g. a slow first-ever tfjs-node cold
                // start exceeding even the 60s budget) and could still send
                // a real, now-stale response later; the requestId check
                // below on the *next* request already handles that safely.
                // Tearing down the connection here would be needlessly
                // disruptive for what might just be a slow response.
                publishError(requestId, req.kind, "timed out waiting for a result from BeatShore desktop", "TIMEOUT");
                return;
            }
            const DWORD remainingMs = DWORD(absoluteDeadline - now);

            // Multiplexed, not a plain blocking readLine(): a cancel
            // requested via requestCancel() must be sendable even while
            // this thread is genuinely blocked waiting on the desktop --
            // otherwise "cancel" would silently do nothing for however
            // long the kind-aware timeout above allows, defeating the
            // point (mirrors BeatShoreDesktop's own PipeSessionOwner; see
            // OverlappedPipeIO.h's beginRead()/pollRead()).
            HANDLE readEvt = pipe->beginRead();
            HANDLE waitSet[2] = { readEvt, cancelEvent };
            DWORD w = WaitForMultipleObjects(2, waitSet, FALSE, remainingMs);

            if (w == WAIT_TIMEOUT) continue; // top-of-loop deadline check above handles this

            if (w == WAIT_OBJECT_0 + 1)
            {
                ResetEvent(cancelEvent);
                if (cancelRequestedFlag.exchange(false))
                {
                    bsjson::Value cancelMsg = bsjson::Value::object();
                    cancelMsg.set("type", "CANCEL");
                    cancelMsg.set("v", 1);
                    cancelMsg.set("requestId", requestId);
                    pipe->writeLine(bsjson::stringify(cancelMsg)); // pipe's read is still pending underneath -- same thread, concurrent overlapped ops, verified safe (see the isolated multiplex test)
                }
                continue; // still waiting for the desktop's actual response (the CANCEL's own reply, or the original result)
            }

            std::string line;
            const auto pr = pipe->pollRead(line);
            if (pr == OverlappedPipeIO::PollResult::Pending) continue;
            if (pr != OverlappedPipeIO::PollResult::Ok)
            {
                disconnect();
                publishError(requestId, req.kind, "lost connection to BeatShore desktop while waiting for a result", "PIPE_READ_FAILED");
                return;
            }

            bsjson::Value resp;
            try { resp = bsjson::parse(line); }
            catch (const std::exception&) { continue; }

            // Defense in depth, mirroring the desktop's own stale-result
            // check (main.cpp): if this client ever gives up on a request
            // before the desktop does, a late response for the OLD
            // requestId could otherwise be misattributed to whatever
            // request reads next. The desktop already filters this on its
            // side, so this should be a no-op in practice -- kept here so
            // the guarantee doesn't depend on trusting the other side never
            // regresses.
            const std::string respRequestId = resp.has("requestId") ? resp["requestId"].asString() : "";
            if (!respRequestId.empty() && respRequestId != requestId) continue;

            std::string type = resp["type"].asString();
            if (type == "ANALYSIS_PROGRESS")
            {
                progress.store(resp.has("progress") ? resp["progress"].asNumber() : progress.load());
                continue;
            }

            // A dedicated message, not a generic ERROR the UI would have
            // to sniff an errorCode out of to tell "the broker is going
            // away on purpose" apart from "something actually broke."
            // Disconnects immediately (status -> Disconnected, matching
            // what a lost connection would do anyway) and reports this
            // in-flight request as interrupted with a calm, specific
            // message and errorCode -- PluginEditor.cpp special-cases
            // this errorCode to show a non-alarming status instead of the
            // generic "Error: ... [CODE]" text every other errorCode gets.
            // The normal reconnect-on-timer loop in run() picks back up
            // once a new broker process becomes reachable -- nothing
            // further needed here for that part.
            if (type == "BROKER_SHUTTING_DOWN")
            {
                disconnect();
                publishError(requestId, req.kind,
                              "BeatShore Desktop is restarting -- reconnecting automatically",
                              "BROKER_SHUTTING_DOWN");
                return;
            }

            if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT")
            {
                BridgeAnalysisResult r;
                r.requestIdEcho = requestId;
                r.kind = req.kind;
                r.success = true;
                r.algorithm = resp.has("algorithm") ? resp["algorithm"].asString() : "";
                r.computeMs = resp.has("computeMs") ? int(resp["computeMs"].asNumber()) : -1;
                r.desktopTotalMs = resp.has("desktopTotalMs") ? int(resp["desktopTotalMs"].asNumber()) : -1;
                r.audioSource = resp.has("audioSource") ? resp["audioSource"].asString() : "";

                if (type == "MIDI_RESULT")
                {
                    r.noteCount = resp.has("noteCount") ? int(resp["noteCount"].asNumber()) : -1;
                    r.midiPath = resp.has("midiPath") ? resp["midiPath"].asString() : "";
                    r.midiSha256 = resp.has("sha256") ? resp["sha256"].asString() : "";
                    r.midiWriteError = resp.has("midiWriteError") ? resp["midiWriteError"].asString() : "";
                    r.midiSizeBytes = resp.has("midiSizeBytes") ? int(resp["midiSizeBytes"].asNumber()) : -1;
                    r.midiGeneratedAt = resp.has("midiGeneratedAt") ? resp["midiGeneratedAt"].asString() : "";
                    r.message = std::to_string(r.noteCount) + " notes"
                        + (r.midiPath.empty() ? "" : (" -> " + r.midiPath));
                }
                else
                {
                    const bsjson::Value& result = resp["result"];
                    if (result.type == bsjson::Type::Number)
                    {
                        r.hasNumericValue = true;
                        r.numericValue = result.asNumber();
                        std::ostringstream fmt;
                        fmt.precision(2);
                        fmt << std::fixed << r.numericValue;
                        r.message = fmt.str();
                    }
                    else
                    {
                        r.message = bsjson::stringify(result);
                    }
                }
                publish(r);
                return;
            }

            if (type == "ERROR")
            {
                const std::string errorCode = resp.has("errorCode") ? resp["errorCode"].asString() : "";
                // CANCEL_REQUESTED is an acknowledgement that a cancel was
                // received and applied to a genuinely in-flight request --
                // not the terminal outcome itself. The desktop follows up
                // with a separate terminal ERROR (errorCode CANCELLED)
                // once it actually finishes tearing the request down (see
                // PROTOCOL.md's CANCEL section) -- keep waiting for that,
                // rather than reporting "done" on the acknowledgement.
                if (errorCode == "CANCEL_REQUESTED") continue;

                publishError(requestId, req.kind,
                              resp.has("message") ? resp["message"].asString() : "unknown error",
                              errorCode);
                return;
            }
        }

        // Only reachable via the messagesRead cap now -- a real timeout
        // returns from inside the loop above with a proper TIMEOUT
        // errorCode. Getting here means the desktop sent 10000 messages
        // (or that many discarded stale/malformed lines) inside the time
        // budget without ever producing a terminal one -- a pathological
        // engine, not a slow-but-working one.
        publishError(requestId, req.kind, "desktop sent an excessive number of messages without a terminal response", "TOO_MANY_MESSAGES_CLIENT_SIDE");
    }

    void publish(const BridgeAnalysisResult& r)
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        latestResult = r;
        hasNewResult.store(true);
    }

    void publishError(const std::string& requestId, const std::string& kind, const std::string& message, const std::string& errorCode = "")
    {
        BridgeAnalysisResult r;
        r.requestIdEcho = requestId;
        r.kind = kind;
        r.success = false;
        r.message = message;
        r.errorCode = errorCode;
        publish(r);
    }

    void disconnect()
    {
        closePipeHandle();
        pipe.reset();
        status.store(BridgeStatus::Disconnected);
    }

    void closePipeHandle()
    {
        if (rawHandle != INVALID_HANDLE_VALUE) { CloseHandle(rawHandle); rawHandle = INVALID_HANDLE_VALUE; }
    }

    std::atomic<BridgeStatus> status { BridgeStatus::Disconnected };
    std::unique_ptr<OverlappedPipeIO> pipe;
    HANDLE rawHandle = INVALID_HANDLE_VALUE;
    juce::uint32 lastHeartbeatMs = 0;
    juce::uint32 lastHostStateMs = 0;
    const HostSnapshot* hostSnapshotSource = nullptr; // not owned; see setHostSnapshotSource()

    std::mutex requestMutex;
    std::optional<PendingRequest> pendingRequest;
    std::atomic<bool> requestInFlight { false };
    std::atomic<double> progress { 0.0 };

    HANDLE cancelEvent = nullptr;      // multiplexed alongside pipe->beginRead() in handleAnalysisRequest's wait loop
    std::atomic<bool> cancelRequestedFlag { false };

    std::mutex resultMutex;
    BridgeAnalysisResult latestResult;
    std::atomic<bool> hasNewResult { false };
};
