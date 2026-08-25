// Real load-boundary test against a REAL, already-running
// BeatShoreDesktop.exe -- exercises kMaxConcurrentSessions (16) and
// kMaxGlobalQueueDepth (24) with genuine HELLO-completed connections and
// genuine ANALYSIS_REQUESTs referencing real shared-memory audio, not
// synthetic/malformed requests. The earlier session-load test
// (native/installer's own PowerShell-orchestrated BridgeClientTest.exe
// burst) never reached either boundary because it used the real,
// production BridgeClient class -- which is exactly the point of THAT
// test (proving the real plugin-side code holds up under real
// concurrent load) but structurally cannot hold two requests open on one
// connection (see BridgeClient.h's requestInFlight guard) and completes
// each round trip fast enough that live session count never builds past
// the ceiling either. This test is deliberately NOT built on BridgeClient
// for that reason -- it's a thin, direct raw client whose only job is
// proving these two specific limits reject correctly under real load,
// not a second implementation of the production IPC client.
//
// Two modes:
//   --sessions <N> <holdMs>   Open N real HELLO-completed connections
//                             (multi-threaded, near-simultaneous), hold
//                             them open for holdMs, report how many
//                             connected vs were rejected
//                             (kMaxConcurrentSessions=16).
//   --queue <sessions> <perSession>  Open <sessions> real connections,
//                             each firing <perSession> real
//                             ANALYSIS_REQUESTs back-to-back (real SHM
//                             audio each) without waiting between them,
//                             then collect every terminal response.
//                             Reports ACCEPTED vs QUEUE_FULL vs other.
#include "../../protocol/NamedPipeIO.h"
#include "../../protocol/MiniJson.h"
#include "../../protocol/SharedAudioBuffer.h"
#include <windows.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>

using namespace bsjson;

static const char* kPipeName = "\\\\.\\pipe\\BeatShoreBridge.v1";

static std::mutex logMutex;
static void log(const std::string& s)
{
    std::lock_guard<std::mutex> lock(logMutex);
    std::cout << s << std::endl;
}

static HANDLE connectPipe()
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        HANDLE h = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        WaitNamedPipeA(kPipeName, 2000);
    }
    return INVALID_HANDLE_VALUE;
}

static bool doHello(PipeLineIO& io)
{
    Value hello = Value::object();
    hello.set("type", "HELLO");
    hello.set("v", 1);
    hello.set("clientName", "load-boundary-test");
    if (!io.writeLine(stringify(hello))) return false;
    auto resp = io.readLine();
    if (!resp) return false;
    Value msg = parse(*resp);
    return msg["type"].asString() == "CAPABILITIES";
}

// --- Mode: --sessions ---------------------------------------------------
static void runSessionsMode(int n, int holdMs)
{
    std::atomic<int> connected{ 0 };
    std::atomic<int> rejected{ 0 };
    std::vector<std::thread> threads;
    std::vector<HANDLE> handles(n, INVALID_HANDLE_VALUE);

    for (int i = 0; i < n; ++i)
    {
        threads.emplace_back([&, i] {
            HANDLE h = connectPipe();
            if (h == INVALID_HANDLE_VALUE) { rejected++; return; }
            PipeLineIO io(h);
            if (!doHello(io)) { rejected++; CloseHandle(h); return; }
            connected++;
            handles[i] = h;
            Sleep(holdMs);
        });
    }
    for (auto& t : threads) t.join();

    log("[boundary] sessions mode: " + std::to_string(connected.load()) + " connected, " +
        std::to_string(rejected.load()) + " rejected (out of " + std::to_string(n) + ")");

    for (auto h : handles) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// --- Mode: --queue --------------------------------------------------------
static std::string genShmName(int session, int idx)
{
    // Matches BridgeClient.h's own naming convention exactly
    // ("Local\\BeatShoreAudio.<pid>.<requestId>") rather than inventing a
    // different one -- the "Local\\" prefix scopes the kernel object to
    // this session the same way the real plugin's own SHM does.
    return "Local\\BeatShoreAudio." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(session) + "." + std::to_string(idx);
}

static std::string genRequestId(int session, int idx)
{
    return "loadtest-" + std::to_string(session) + "-" + std::to_string(idx);
}

static void runQueueMode(int sessions, int perSession)
{
    std::atomic<int> accepted{ 0 };
    std::atomic<int> queueFull{ 0 };
    std::atomic<int> otherError{ 0 };
    std::atomic<int> connectFailed{ 0 };
    std::vector<std::thread> threads;

    // kMaxCaptureSeconds (60s), the absolute maximum this desktop
    // accepts -- even 10s of audio (~500ms/request) still drained faster
    // than 32 requests could build past kMaxGlobalQueueDepth=24. At the
    // real ceiling, each request takes several seconds, giving the
    // queue an actual chance to build up before the single worker drains
    // it.
    const uint32_t sampleRate = 22050, channels = 1, frames = 22050 * 60; // 60s
    std::vector<float> samples(frames, 0.01f);

    // Two-phase, not "connect, prep, and send in one pass" -- the first
    // attempt at this test genuinely admitted every request with zero
    // QUEUE_FULL rejections even at 60s audio, and per-request SHM
    // creation + a large (~5MB) synchronous writeSamples() turned out to
    // take enough real wall-clock time, spread unevenly across 16
    // threads competing for CPU, that the single worker kept
    // interleaving pops throughout the "burst" rather than facing it
    // all at once. This version does all SHM creation/writing and HELLO
    // handshaking BEFORE any thread sends its first ANALYSIS_REQUEST,
    // released together via an atomic barrier, so the actual admission
    // burst is just N tight WriteFile calls, not N full SHM preparations.
    std::atomic<int> readyCount{ 0 };
    std::atomic<bool> go{ false };

    for (int s = 0; s < sessions; ++s)
    {
        threads.emplace_back([&, s] {
            HANDLE h = connectPipe();
            if (h == INVALID_HANDLE_VALUE) { connectFailed++; readyCount++; return; }
            PipeLineIO io(h);
            if (!doHello(io)) { connectFailed++; CloseHandle(h); readyCount++; return; }

            std::vector<SharedAudioBuffer> buffers(perSession);
            std::vector<std::string> preparedLines(perSession);

            for (int i = 0; i < perSession; ++i)
            {
                std::string shmName = genShmName(s, i);
                if (!buffers[i].create(shmName, sampleRate, channels, frames))
                {
                    log("[boundary] session " + std::to_string(s) + " failed to create SHM " + shmName);
                    continue;
                }
                buffers[i].writeSamples(samples.data(), frames * channels);

                Value req = Value::object();
                req.set("type", "ANALYSIS_REQUEST");
                req.set("requestId", genRequestId(s, i));
                req.set("kind", "transcribePolyphonic"); // slower than tempo -- needed to actually build real queue
                                                          // depth before the single worker drains it (tempo drains
                                                          // too fast to ever build a real backlog, confirmed
                                                          // empirically)
                req.set("audioSource", "file");
                Value audio = Value::object();
                audio.set("shm", shmName);
                req.set("audio", audio);
                preparedLines[i] = stringify(req);
            }

            readyCount++;
            while (!go.load()) { /* spin -- this barrier is held for milliseconds at most */ }

            // Fire every request for this session back-to-back, with NO
            // wait for a terminal response in between -- this is the
            // specific real scenario (>1 genuinely active/queued job
            // for one session) BridgeClient.h's requestInFlight guard
            // structurally prevents, and the only way to actually build
            // real global queue depth toward kMaxGlobalQueueDepth=24.
            for (int i = 0; i < perSession; ++i)
            {
                if (preparedLines[i].empty()) continue; // SHM creation failed above
                if (!io.writeLine(preparedLines[i]))
                    log("[boundary] session " + std::to_string(s) + " write failed for request " + std::to_string(i));
            }

            // Now collect exactly perSession terminal responses (ERROR or
            // ANALYSIS_RESULT), matching by requestId since responses
            // for a burst like this can arrive out of order.
            int remaining = perSession;
            int linesSeen = 0;
            while (remaining > 0)
            {
                auto line = io.readLine();
                linesSeen++;
                if (!line) { log("[boundary] session " + std::to_string(s) + " readLine() returned nullopt (pipe closed/error) after " + std::to_string(linesSeen) + " lines, " + std::to_string(remaining) + " still remaining"); break; }
                Value msg;
                try { msg = parse(*line); } catch (...) { log("[boundary] session " + std::to_string(s) + " failed to parse line as JSON: " + *line); continue; }
                std::string type = msg["type"].asString();
                // MIDI_RESULT is the terminal message for MIDI-producing
                // kinds (transcribePolyphonic/transcribeDrums/
                // transcribeMono) -- ANALYSIS_RESULT is only for
                // numeric-result kinds like tempo. An earlier version of
                // this test only checked for ANALYSIS_RESULT, silently
                // treating every real MIDI_RESULT as a non-terminal
                // message to skip (like ANALYSIS_PROGRESS) -- `remaining`
                // then never reached zero even though the desktop had
                // already correctly answered every request, hanging this
                // test's threads forever. Found by adding per-line
                // logging and seeing MIDI_RESULT arrive correctly for
                // every request the desktop log claimed to have handled.
                if (type != "ANALYSIS_RESULT" && type != "MIDI_RESULT" && type != "ERROR") continue; // ignore PROGRESS etc.
                std::string respId = msg.has("requestId") ? msg["requestId"].asString() : "(none)";
                remaining--;
                log("[boundary] session " + std::to_string(s) + " got terminal type=" + type + " requestId=" + respId + ", remaining=" + std::to_string(remaining));
                if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT") { accepted++; continue; }
                std::string code = msg.has("errorCode") ? msg["errorCode"].asString() : "";
                if (code == "QUEUE_FULL") queueFull++;
                else otherError++;
            }

            CloseHandle(h);
        });
    }
    while (readyCount.load() < sessions) { /* wait for every thread to finish HELLO + SHM prep */ }
    log("[boundary] all " + std::to_string(sessions) + " sessions ready -- releasing the burst");
    go.store(true);
    for (auto& t : threads) t.join();

    int totalRequested = sessions * perSession;
    log("[boundary] queue mode: " + std::to_string(totalRequested) + " requests attempted ("
        + std::to_string(sessions) + " sessions x " + std::to_string(perSession) + " each)");
    log("[boundary] accepted=" + std::to_string(accepted.load())
        + " QUEUE_FULL=" + std::to_string(queueFull.load())
        + " otherError=" + std::to_string(otherError.load())
        + " connectFailed=" + std::to_string(connectFailed.load()));
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: LoadBoundaryTest --sessions N holdMs | --queue sessions perSession" << std::endl; return 1; }
    std::string mode = argv[1];
    if (mode == "--sessions" && argc >= 4)
    {
        runSessionsMode(std::stoi(argv[2]), std::stoi(argv[3]));
    }
    else if (mode == "--queue" && argc >= 4)
    {
        runQueueMode(std::stoi(argv[2]), std::stoi(argv[3]));
    }
    else
    {
        std::cerr << "usage: LoadBoundaryTest --sessions N holdMs | --queue sessions perSession" << std::endl;
        return 1;
    }
    return 0;
}
