// BeatShoreDesktop -- the process described in PROTOCOL.md and the plan's
// architecture layer diagram as "BeatShore desktop process". Owns the named
// pipe endpoint and the shared-memory audio segments; delegates actual
// analysis to spawned `node analyze.js` child process(es) that reuse
// beatshore-dsp.js unmodified (see native/BeatShoreDesktop/engine/).
//
// Multiple simultaneous plugin connections, genuine cancellation of a
// running request, no GUI, no installer, no auto-update -- see STATUS.md
// in the plugin dir for what's still not built.
//
// THREADING (load-bearing, found the hard way, still true): no two threads
// ever call ReadFile/WriteFile on the SAME pipe handle. An earlier version
// read on one thread while a second thread wrote to the same synchronous,
// byte-mode duplex pipe handle concurrently; that reliably hung forever on
// the writer's WriteFile (verified with timestamped logs -- the write only
// ever "resolved" when the peer process was killed). Root cause not fully
// nailed down against the Win32 docs, so treated as an empirical
// constraint, not a fully explained one.
//
// What changed: this used to mean "therefore all pipe I/O lives on one
// thread, and an ANALYSIS_REQUEST blocks HEARTBEAT/CANCEL for its
// duration." That's no longer true, but the constraint above is still
// honored exactly -- each pipe handle (one per connected plugin, one per
// Node child) is still touched by exactly one thread for its entire
// lifetime. What's different is that a single thread no longer has to
// *block* inside one call to service its handle: OverlappedPipeIO's
// beginRead()/pollRead() (native/protocol/OverlappedPipeIO.h) let a thread
// wait on "my pipe's next line" *alongside* other event sources (a
// per-session outgoing-message queue, a per-job cancellation signal) via
// WaitForMultipleObjects, verified in isolation before use here (see
// STATUS.md). A PipeSessionOwner thread owns one plugin connection's pipe
// end to end; a NodeWorker thread owns one Node child's pipes end to end;
// they hand work to each other only through thread-safe queues
// (AnalysisScheduler.h), never by touching each other's handles.
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include "resource.h"
#include <string>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <vector>
#include <deque>
#include <random>
#include <thread>
#include <mutex>
#include <atomic>
#include "../../protocol/OverlappedPipeIO.h"
#include "../../protocol/MiniJson.h"
#include "../../protocol/SharedAudioBuffer.h"
#include "NodeEngine.h"
#include "AnalysisScheduler.h"

using namespace bsjson;

static const char* PIPE_NAME = "\\\\.\\pipe\\BeatShoreBridge.v1";

// Real-world cap on simultaneous plugin instances in one REAPER session --
// generous, but not PIPE_UNLIMITED_INSTANCES, so a runaway bug (a plugin
// stuck in a reconnect loop, say) can't exhaust system pipe resources
// unboundedly.
static constexpr DWORD kMaxPipeInstances = 32;

// Moved ahead of PipeSecurity (below) so its init() can log a fallback
// warning -- logLine itself doesn't depend on anything declared later in
// this file, so there's no ordering downside to defining it this early.
static ULONGLONG startTick = GetTickCount64();
static std::mutex logMutex; // logLine is now called from multiple threads

// Diagnostic (verbose) logging is opt-in, not the default -- see
// redactPath()/redactContent() below for what this gates and why. Read
// once at process start (main()) via GetEnvironmentVariableA rather than
// re-checked on every log call.
static bool g_diagnosticLogging = false;

// A simple token-bucket rate limit on logLine() itself, independent of
// what any individual call site chooses to log -- without this, a flood
// of malformed/rejected messages from a hostile or buggy same-user peer
// (each rejection already logs a line) could still turn into a
// disk-I/O amplification vector even with per-connection/per-message
// limits in place elsewhere. Deliberately coarse (a fixed budget per
// rolling second, refilled lazily) rather than a precise leaky-bucket --
// good enough to cap worst-case log volume without adding meaningful
// overhead to the common case of well-behaved, low-frequency logging.
static constexpr int kMaxLogLinesPerSecond = 200;
static std::atomic<int> g_logLinesThisSecond{0};
static std::atomic<ULONGLONG> g_logWindowStartMs{0};
static std::atomic<int> g_logLinesSuppressedThisWindow{0};

static void logLine(const std::string& s)
{
    std::lock_guard<std::mutex> lock(logMutex);
    ULONGLONG now = GetTickCount64();
    ULONGLONG windowStart = g_logWindowStartMs.load();
    if (now - windowStart >= 1000)
    {
        int suppressed = g_logLinesSuppressedThisWindow.exchange(0);
        if (suppressed > 0)
            std::cout << "[+" << (now - startTick) << "ms] [desktop] (" << suppressed << " log lines suppressed -- rate limit)" << std::endl;
        g_logWindowStartMs.store(now);
        g_logLinesThisSecond.store(0);
    }
    if (g_logLinesThisSecond.fetch_add(1) >= kMaxLogLinesPerSecond)
    {
        g_logLinesSuppressedThisWindow.fetch_add(1);
        return;
    }
    std::cout << "[+" << (now - startTick) << "ms] " << s << std::endl;
}

// Full filesystem paths and shared-memory names are not logged by default
// -- a support/debug log is a real place for a local path to leak (a
// username embedded in "C:\Users\<name>\...", a project/track name chosen
// by the user, etc.) that has no business being written to disk for every
// routine request. Set BEATSHORE_DIAGNOSTIC_LOG=1 in the environment
// before launching BeatShoreDesktop.exe to see real values instead --
// an explicit, user-activated diagnostic mode, not the default.
static std::string redactPath(const std::string& path)
{
    return g_diagnosticLogging ? path : std::string("<redacted>");
}

// Same idea for full protocol message content (a raw incoming/outgoing
// JSON line can carry a hostTrackName, a shm name, or other
// caller-supplied content) -- logged as a byte count by default, the
// actual content only under the same explicit diagnostic flag.
static std::string redactContent(const std::string& content)
{
    return g_diagnosticLogging ? content : ("<" + std::to_string(content.size()) + " bytes>");
}

// Without an explicit security descriptor, CreateNamedPipeA's default
// DACL can be broader than intended (which principals get access by
// default isn't obvious or guaranteed the same across Windows versions).
//
// Built from the actual current-user SID (via the process token), not the
// generic "OW" (Owner) relative identifier this used previously -- "OW"
// happened to be correct for this process's normal usage (a per-user tray
// app creating its own pipe, where the creator IS the owner), but it names
// a *relationship*, not a principal, which is harder to audit ("who,
// exactly, can open this?" requires knowing who owns it) and would answer
// differently under a different hosting context (e.g. impersonation) even
// though this project doesn't do that today. An explicit SID is
// unambiguous either way. The leading "P" in "D:P(...)" additionally
// marks the DACL protected, so it can't be silently widened by ACE
// inheritance from a parent object. Falls back to the previous "OW"-based
// descriptor (logged, not silent) if building the SID-based one fails for
// any reason -- still same-user-only either way, just less explicit --
// rather than refusing to start at all over what should be a rare failure
// mode (e.g. GetTokenInformation denied by an unusual token type).
//
// Built once at startup (these Windows security APIs are not cheap to
// call per-connection) and reused for both CreateNamedPipeA calls in
// runAcceptLoop().
//
// Not verified from this environment: the specific behavior when
// BeatShoreDesktop.exe and the connecting DAW process run at different
// Windows integrity levels (e.g. the desktop launched elevated while the
// DAW is not) -- Mandatory Integrity Control operates independently of
// this DACL and could, in that specific mixed-elevation scenario, still
// block a legitimate same-user connection. This project's normal
// deployment (Start Menu shortcut, tray "Start at login") never elevates
// BeatShoreDesktop.exe, so this hasn't been a reachable case in any
// testing performed here; the standing guidance is simply: don't run
// BeatShoreDesktop.exe elevated. Deliberately not "fixed" by adding a
// mandatory-label SACL override here, since relaxing MIC's default
// write-up policy is its own security trade-off that shouldn't be made
// silently just to paper over an unverified, non-default scenario.
struct PipeSecurity
{
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    bool init()
    {
        std::string sddl;
        std::string currentUserSid = queryCurrentUserSid();
        if (!currentUserSid.empty())
        {
            sddl = "D:P(A;;GA;;;" + currentUserSid + ")";
        }
        else
        {
            logLine("[desktop] WARNING: could not resolve the current user's SID for the pipe ACL -- falling back to the owner-relative descriptor (still same-user-only, just less explicit)");
            sddl = "D:(A;;GA;;;OW)";
        }

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        {
            // Last-resort fallback: if even the "OW" form somehow fails
            // to convert (sddl.c_str() might itself already BE "OW" if we
            // got here from the branch above, in which case this retry is
            // a harmless no-op that will fail the same way and correctly
            // propagate false to the caller).
            if (sddl != "D:(A;;GA;;;OW)")
            {
                logLine("[desktop] WARNING: SID-based pipe ACL failed to convert -- falling back to the owner-relative descriptor");
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
                    return false;
            }
            else
            {
                return false;
            }
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }

    // Deliberately never freed -- this lives for the process's entire
    // lifetime (same reasoning as the single-instance mutex handle
    // above), and LocalFree() on the descriptor is the OS's job to worry
    // about at process exit, not something to add destructor complexity
    // for here.

private:
    // Returns the current process token's user SID as an SDDL-format
    // string (e.g. "S-1-5-21-..."), or an empty string on any failure --
    // every failure path here is handled by init()'s own fallback above,
    // not treated as fatal in this function.
    static std::string queryCurrentUserSid()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "";

        DWORD tokenInfoLen = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &tokenInfoLen);
        if (tokenInfoLen == 0) { CloseHandle(token); return ""; }

        std::vector<BYTE> tokenInfoBuf(tokenInfoLen);
        BOOL ok = GetTokenInformation(token, TokenUser, tokenInfoBuf.data(), tokenInfoLen, &tokenInfoLen);
        CloseHandle(token);
        if (!ok) return "";

        auto* tokenUser = reinterpret_cast<TOKEN_USER*>(tokenInfoBuf.data());
        LPSTR sidString = nullptr;
        if (!ConvertSidToStringSidA(tokenUser->User.Sid, &sidString) || sidString == nullptr) return "";

        std::string result(sidString);
        LocalFree(sidString);
        return result;
    }
};
static PipeSecurity g_pipeSecurity;

// File-scope (not local to main()) so both the accept loop and the tray
// window's Quit handler can reach them -- these outlive the process
// (main() never returns in normal operation short of a graceful
// shutdown), so worker/session threads capturing references to them,
// detached, is safe: nothing here is destroyed while they might still be
// running.
static JobQueue g_jobQueue;
static JobRegistry g_jobRegistry;
static SessionRegistry g_sessionRegistry;
static std::atomic<bool> g_shuttingDown{false};
static HANDLE g_singleInstanceMutex = nullptr;

// Running total of audio bytes currently reserved by accepted-but-not-yet-
// cleaned-up requests -- see kMaxTotalReservedAudioBytes above for the
// budget this is checked against, and releaseJobResources() below for the
// single, idempotent place this is ever decremented.
static std::atomic<uint64_t> g_reservedAudioBytes{0};

// Idempotent per-job cleanup: deletes the job's temp .bsmraw dump (if any)
// and releases its share of g_reservedAudioBytes -- guarded by the job's
// own resourcesReleased flag (AnalysisScheduler.h) so it's safe to call
// from more than one place that might independently observe "this job is
// done" (a worker's own terminal-state transitions, primarily) without
// double-releasing the budget or attempting a redundant delete. Best-
// effort on the file delete: if the underlying Node process still has it
// open (a genuinely timed-out request that's still running past its
// deadline, the one case this project's job-cancellation model doesn't
// forcibly kill Node for -- see the TIMEOUT branch in runNodeWorker), the
// delete simply fails and is logged, not treated as fatal; nothing about
// this project's temp-file handling depended on synchronous cleanup
// before this change, and the alternative (delete NOTHING, ever, this
// function's own reason for existing) was strictly worse.
static void releaseJobResources(const std::shared_ptr<AnalysisJob>& job)
{
    bool expected = false;
    if (!job->resourcesReleased.compare_exchange_strong(expected, true)) return; // already released

    if (!job->tempAudioPath.empty() && !DeleteFileA(job->tempAudioPath.c_str()))
    {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND)
            logLine("[desktop] WARNING: could not delete temp audio file for requestId=" + job->requestId + " (error=" + std::to_string(err) + ")");
    }
    if (job->reservedAudioBytes > 0) g_reservedAudioBytes.fetch_sub(job->reservedAudioBytes);
}

// Distinct from the existing per-message "v":1 field (which every message
// already carries): this specifically guards the HEARTBEAT/HEARTBEAT_ACK
// exchange's own shape, matching BridgeClient.h's kHeartbeatProtocolVersion.
static constexpr int kProtocolVersion = 1;

// How many Node child processes concurrently pull from the shared job
// queue. 1 today, deliberately: true parallel throughput for
// transcribePolyphonic needs multiple resident tfjs-node processes
// (~200MB+ native TensorFlow library each), a real resource cost not yet
// weighed against benefit. The architecture below does not assume 1,
// though -- see native/BridgeClientTest/Source/multi_session_test.cpp for
// a 2-worker run using lightweight (non-TF) kinds, proving the pool
// mechanics themselves (not TF's own parallelism) work correctly.
static constexpr int kMaxConcurrentNodeJobs = 1;

// Payload/resource limits enforced on every ANALYSIS_REQUEST before any
// work is done for it -- a local IPC peer being well-behaved isn't a
// substitute for actually bounding what it's allowed to ask for; a buggy
// or hostile one could otherwise submit an oversized request and exhaust
// memory or disk before this desktop ever gets to reject it downstream.
//
// The frame limit is duration-relative to the REQUEST's own claimed
// sample rate (maxFrames = sampleRate * kMaxCaptureSeconds), not a single
// flat frame count -- a flat 50,000,000-frame cap (this constant's
// previous value) permits ~400MB of interleaved stereo float32 per
// request at a high sample rate, wildly beyond anything this project's
// actual 10-second capture window produces. 60s is deliberately more
// than the plugin's current 10s capture (real headroom for a
// longer-capture feature later, not a hypothetical), documented here so
// a future capture-length change doesn't silently blow this budget:
// worst case at the sample-rate ceiling below, 192000 * 60 * 2 channels *
// 4 bytes ~= 92MB for a single request -- see kMaxTotalReservedAudioBytes
// below for the SYSTEM-WIDE ceiling across every accepted-but-not-yet-
// cleaned-up request, which is the number that actually bounds total
// memory/disk exposure.
static constexpr uint32_t kMaxCaptureSeconds = 60;
static constexpr uint32_t kMaxAudioChannels = 2;                   // this project only ever produces mono/stereo captures
static constexpr uint32_t kMinSampleRateHz = 8000;
static constexpr uint32_t kMaxSampleRateHz = 192000;

// Deliberately small: one worker (kMaxConcurrentNodeJobs above) means at
// most one TensorFlow inference genuinely runs at a time regardless of
// how many sessions or requests are queued behind it -- a deep global
// queue doesn't buy real throughput, it just lets more expensive,
// already-admitted work pile up waiting. "One active + one queued per
// session" is enough for normal interactive use (fire an analysis, and
// at most queue a second one while the first finishes) without letting
// one session monopolize the shared queue.
static constexpr int kMaxActiveJobsPerSession = 2;
static constexpr size_t kMaxGlobalQueueDepth = 24;                 // soft cap on total queued (not yet running) jobs across all sessions

// System-wide ceiling on how much captured-audio memory this desktop has
// currently accepted (reserved at ANALYSIS_REQUEST admission time,
// released once that request's temp file is cleaned up -- see
// g_reservedAudioBytes/releaseJobResources() below) regardless of how it's
// distributed across sessions and the job queue. ~512MB is several times
// the worst-case single-request size above (~92MB), enough headroom for a
// handful of genuinely concurrent large requests without allowing
// unbounded growth as more sessions pile on. A request that would push
// the running total over this budget is rejected with SERVER_BUSY rather
// than admitted and hoped to fit.
static constexpr uint64_t kMaxTotalReservedAudioBytes = 512ull * 1024 * 1024;

// Real-world cap on simultaneously connected sessions -- distinct from
// kMaxPipeInstances above (which bounds OS-level pipe *instances*, a
// lower-level resource): this bounds how many sessions this application
// actually tries to service at once, rejected before a PipeSessionOwner
// thread is even spawned for the excess.
static constexpr size_t kMaxConcurrentSessions = 16;

// Single source of truth for both buildCapabilities() (what the desktop
// advertises) and the ANALYSIS_REQUEST validation below (what it actually
// accepts) -- previously these were two separately-written lists that
// could silently drift apart.
static const char* kSupportedKinds[] = {
    "loudness", "tempo", "key", "structure", "chords", "timbre",
    "transcribeDrums", "transcribeMono", "transcribePolyphonic"
};
static bool isSupportedKind(const std::string& kind)
{
    for (auto k : kSupportedKinds) if (kind == k) return true;
    return false;
}

// requestId is attacker/bug-controllable JSON input that gets used to
// build a real filesystem path (the temp .bsmraw dump below) -- without
// this check, a requestId like "..\\..\\Windows\\System32\\evil" would let
// a malicious or buggy plugin make this desktop write outside its own
// temp directory. juce::Uuid's own toString() only ever produces pure
// lowercase hex (no hyphens -- confirmed against this project's own
// logs), so this is intentionally strict rather than merely
// "no path separators": anything outside [A-Za-z0-9-], or an empty or
// implausibly long string, is rejected outright.
static bool isValidRequestId(const std::string& id)
{
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    return true;
}

// A separate, desktop-generated identifier used for anything that
// actually touches the filesystem or other internal resource naming (the
// temp .bsmraw dump below) -- the client-supplied requestId is validated
// above and safe to use as-is today, but filesystem safety shouldn't
// depend on a client-controlled string's validation staying correct
// forever. From this point on, requestId is protocol metadata only:
// stored in the job registry, echoed back to correlate responses with
// the request that produced them, never used to build a path. Formatted
// as 32 lowercase hex characters (128 bits from a non-cryptographic PRNG
// seeded from std::random_device) -- collision-resistant enough for a
// same-process temp-file discriminator; this is not a security token.
static std::string generateInternalId()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    static std::mutex rngMutex;
    uint64_t hi, lo;
    { std::lock_guard<std::mutex> lock(rngMutex); hi = rng(); lo = rng(); }
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)hi, (unsigned long long)lo);
    return std::string(buf, 32);
}

// frames and channels are both attacker/bug-controllable JSON-derived
// values; this computes frames*channels*sizeof(float) via an explicit
// overflow check rather than relying on operand promotion happening to
// be safe for today's specific limit constants -- a defense that doesn't
// depend on those limits never changing. Returns false (leaving outBytes
// unspecified) if the calculation would overflow a uint64_t.
static bool safeAudioByteCount(uint64_t frames, uint64_t channels, uint64_t& outBytes)
{
    if (frames != 0 && channels > (UINT64_MAX / frames)) return false;
    uint64_t product = frames * channels;
    if (product > (UINT64_MAX / sizeof(float))) return false;
    outBytes = product * sizeof(float);
    return true;
}

static std::string toLine(Value msg)
{
    if (!msg.has("v")) msg.set("v", 1);
    return stringify(msg);
}

static Value buildCapabilities()
{
    Value v = Value::object();
    v.set("type", "CAPABILITIES");
    v.set("desktopVersion", "0.2.0");
    Value analysis = Value::array();
    for (auto k : kSupportedKinds) analysis.push(Value(k)); // single source of truth with isSupportedKind() -- see that constant's own comment
    v.set("analysis", analysis);
    Value exportCap = Value::array();
    for (auto k : {"wav", "midi", "rpp"}) exportCap.push(Value(k));
    v.set("export", exportCap);
    return v;
}

// A relative default path resolved against the process's *current working
// directory* is fragile: CWD depends on how the exe was launched (a
// double-click, a shortcut with a "Start in" folder, a different terminal)
// and isn't reliably "this exe's own folder". Anchoring to the exe's actual
// location via GetModuleFileNameA makes the default work the same way no
// matter how BeatShoreDesktop.exe was started.
static std::string exeDirectory()
{
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exePath(buf, len);
    size_t pos = exePath.find_last_of("\\/");
    return pos == std::string::npos ? "." : exePath.substr(0, pos);
}

static bool fileExists(const std::string& path)
{
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Checks real candidate locations and picks whichever actually has
// analyze.js, rather than assuming one fixed relative depth -- a real
// bug, not a hypothetical one: BeatShoreSetup.iss's [Icons] entry passes
// the installed engine path explicitly as an argument, which masked this
// for every launch this project's own testing exercised (a double-click
// shortcut, a manual command line), but the tray app's "Start at login"
// registry entry launches bare `BeatShoreDesktop.exe` with no argument at
// all -- confirmed directly: run with CWD=C:\Windows\System32 (mirroring
// how the Run key actually launches it) and no arguments, the old
// single-candidate default resolved to a path one directory *above* the
// install root entirely, and the Node engine never recovered. Checks the
// installed layout first (native/installer/BeatShoreSetup.iss's
// intentionally dev-tree-depth-mirroring nesting -- see that file's own
// "Staging layout" note for why it's shaped this way) before falling
// back to this project's own dev-tree layout, so both a real install and
// local development get a working default without either one needing to
// pass an explicit argument.
static std::string defaultScriptPath()
{
    std::string installedLayout = exeDirectory() + "\\engine\\native\\BeatShoreDesktop\\engine\\analyze.js";
    if (fileExists(installedLayout)) return installedLayout;

    std::string devTreeLayout = exeDirectory() + "\\..\\engine\\analyze.js";
    if (fileExists(devTreeLayout)) return devTreeLayout;

    // Neither candidate exists -- return the installed-layout guess so
    // the resulting FATAL error message (main.cpp's own READY validation)
    // points at the path someone would actually need to fix, matching
    // what a real install is expected to look like rather than this
    // project's own dev tree.
    return installedLayout;
}

// A packaged install bundles its own pinned node.exe next to
// BeatShoreDesktop.exe (see the installer script) so the app works on a
// clean machine with no system-wide Node install and isn't at the mercy of
// whatever version happens to be on PATH. Falls back to PATH's "node" for
// this project's own dev/test workflow, where no bundled copy exists in
// the build tree -- both paths go through the exact same
// startAndValidateNode() either way, so this is purely about which
// executable gets spawned, not a second code path to keep in sync.
static std::string defaultNodeExe()
{
    std::string bundled = exeDirectory() + "\\node\\node.exe";
    if (fileExists(bundled)) return bundled;
    return "node";
}

// Spawns (or respawns, after a hard-cancel kills the previous process) a
// Node child and validates its READY line. Shared by main()'s initial
// startup and NodeWorker's post-cancel respawn so both paths get the exact
// same bounded-wait + validation behavior, not two subtly different copies.
// Bounded, not just validated: a Node process that starts (the process
// object exists, isRunning() would say true) but never actually prints
// anything -- stuck before its first line of output, e.g. a slow require()
// on a broken node_modules install -- used to hang this read forever. 30s
// is generous without being unbounded. Validated because Node's stderr is
// merged into this same stream (see NodeEngine::start), so a module-load
// failure's stack trace looks like a line of output too.
static bool startAndValidateNode(NodeEngine& node, const std::string& nodeExe, const std::string& scriptPath, const std::string& logPrefix)
{
    if (!node.start(nodeExe, scriptPath))
    {
        logLine(logPrefix + "FATAL: failed to start node engine");
        return false;
    }
    logLine(logPrefix + "node engine process started");

    std::string readyLine;
    const auto readyResult = node.readLine(30000, readyLine);
    if (readyResult == OverlappedPipeIO::ReadResult::Timeout)
    {
        logLine(logPrefix + "FATAL: node engine never printed anything within 30s of starting -- node.exe likely hung before reaching its first line of output.");
        return false;
    }
    if (readyResult != OverlappedPipeIO::ReadResult::Ok)
    {
        logLine(logPrefix + "FATAL: node engine's stdout closed before sending READY -- node.exe likely failed to start. Is 'node' on PATH?");
        return false;
    }

    bool validReady = false;
    try { validReady = parse(readyLine)["type"].asString() == "READY"; }
    catch (const std::exception&) {}

    if (!validReady)
    {
        logLine(logPrefix + "FATAL: node engine's first line wasn't a valid READY message -- it was:");
        logLine(logPrefix + "  " + readyLine);
        logLine(logPrefix + "This usually means node.exe failed to load the script (wrong path, missing beatshore-dsp.js, or a syntax error). Script path: " + scriptPath);
        return false;
    }
    logLine(logPrefix + "node engine ready: " + readyLine);
    return true;
}

// ---------------------------------------------------------------------
// NodeWorker: owns one Node child end to end. Pulls jobs from the shared
// queue, runs each one against its own Node process, and routes progress/
// terminal messages to the owning session's outbox (never touching that
// session's pipe handle directly -- see SessionRegistry).
// ---------------------------------------------------------------------
static void runNodeWorker(int workerIndex, JobQueue& jobQueue, SessionRegistry& sessionRegistry,
                           std::atomic<bool>& shouldExit, std::string nodeExe, std::string scriptPath)
{
    const std::string logPrefix = "[desktop worker " + std::to_string(workerIndex) + "] ";
    auto node = std::make_unique<NodeEngine>();
    bool nodeHealthy = startAndValidateNode(*node, nodeExe, scriptPath, logPrefix);
    if (!nodeHealthy)
        logLine(logPrefix + "starting in a degraded state -- every job this worker pops will fail immediately with a clear error until node is available.");

    HANDLE workerWakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    while (!shouldExit.load())
    {
        auto job = jobQueue.waitPop(shouldExit);
        if (!job) break; // shouldExit and nothing left queued

        JobState expected = JobState::Queued;
        if (!job->state.compare_exchange_strong(expected, JobState::Running))
        {
            // Already Cancelled (e.g. its session disconnected while this
            // job was still queued) -- Node never touched it, so its temp
            // file (if any was even written) is safe to clean up right now.
            releaseJobResources(job);
            continue;
        }

        job->assignedWorkerWakeEvent.store(workerWakeEvent);
        ResetEvent(workerWakeEvent);

        auto outbox = sessionRegistry.find(job->sessionId);
        if (!outbox)
        {
            // Session disconnected between enqueue and pop, and its own
            // cleanup pass raced past this job before marking it -- no one
            // will ever receive a result, so don't spend Node time on it.
            job->state.store(JobState::Cancelled);
            releaseJobResources(job);
            job->assignedWorkerWakeEvent.store(nullptr);
            continue;
        }

        if (!nodeHealthy)
        {
            Value err = Value::object();
            err.set("type", "ERROR");
            err.set("requestId", job->requestId);
            err.set("errorCode", "NODE_UNAVAILABLE");
            err.set("message", "node analysis engine is not available (failed to start or was lost and could not be restarted)");
            err.set("audioSource", job->audioSource);
            outbox->push(toLine(err));
            job->state.store(JobState::Failed);
            releaseJobResources(job); // node never received this job -- safe to clean up now
            job->assignedWorkerWakeEvent.store(nullptr);
            continue;
        }

        logLine(logPrefix + "handling ANALYSIS_REQUEST requestId=" + job->requestId + " kind=" + job->kind + " audioSource=" + job->audioSource);

        Value nodeReq = Value::object();
        nodeReq.set("requestId", job->requestId);
        nodeReq.set("kind", job->kind);
        if (!job->role.empty()) nodeReq.set("role", job->role);
        if (job->tempo > 0.0) nodeReq.set("tempo", job->tempo);
        if (!job->hostTrackName.empty()) nodeReq.set("hostTrackName", job->hostTrackName);
        nodeReq.set("audioFile", job->tempAudioPath);
        std::string nodeReqLine = stringify(nodeReq);
        logLine(logPrefix + "-> node: " + redactContent(nodeReqLine));

        if (!node->writeLine(nodeReqLine))
        {
            logLine(logPrefix + "FAILED to write request to node engine");
            Value err = Value::object();
            err.set("type", "ERROR");
            err.set("requestId", job->requestId);
            err.set("errorCode", "NODE_WRITE_FAILED");
            err.set("message", "node analysis engine is not accepting requests");
            err.set("audioSource", job->audioSource);
            outbox->push(toLine(err));
            job->state.store(JobState::Failed);
            releaseJobResources(job); // the request line never reached node -- safe to clean up now
            job->assignedWorkerWakeEvent.store(nullptr);
            continue;
        }

        const ULONGLONG requestStartTick = GetTickCount64();
        const DWORD timeoutMs = (job->kind == "transcribePolyphonic") ? 60000 : 10000;
        const ULONGLONG deadline = requestStartTick + timeoutMs;
        int messagesRead = 0;
        bool jobDone = false;

        while (!jobDone && messagesRead++ < 10000)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                node->cancelPendingRead();
                logLine(logPrefix + "WARNING: timed out waiting for a terminal response from node for requestId=" + job->requestId);
                Value err = Value::object();
                err.set("type", "ERROR");
                err.set("requestId", job->requestId);
                err.set("errorCode", "TIMEOUT");
                err.set("message", "timed out waiting for a terminal response from the node analysis engine");
                err.set("desktopTotalMs", int(GetTickCount64() - requestStartTick));
                err.set("audioSource", job->audioSource);
                outbox->push(toLine(err));
                job->state.store(JobState::TimedOut);
                jobDone = true;
                break;
            }

            HANDLE nodeReadEvt = node->beginRead();
            HANDLE waitSet[2] = { nodeReadEvt, workerWakeEvent };
            DWORD remainingMs = DWORD(deadline - now);
            DWORD w = WaitForMultipleObjects(2, waitSet, FALSE, remainingMs);

            if (w == WAIT_TIMEOUT) continue; // loop back -- the top-of-loop deadline check above will catch this

            if (w == WAIT_OBJECT_0 + 1)
            {
                ResetEvent(workerWakeEvent);
                if (job->state.load() != JobState::CancelRequested)
                    continue; // a stale/unrelated wake (e.g. a previous job's late signal) -- not about this job, keep waiting on node

                // Genuine hard-cancel: Node itself can't be asked to abort
                // an in-flight computation, so the only way to actually
                // stop it (not just stop waiting for it) is to kill the
                // process and start a fresh one. This is what makes
                // CANCEL_REQUESTED a real state transition rather than a
                // label nothing acts on -- see STATUS.md's "Genuine
                // cancellation" section for why a softer "just stop
                // listening" approach was rejected: a killed-and-discarded
                // request can't keep occupying this worker's only Node
                // process for however long its own timeout would have
                // taken, blocking every OTHER queued job (including ones
                // from other sessions) behind it.
                logLine(logPrefix + "CANCEL_REQUESTED for requestId=" + job->requestId + " -- killing and restarting node engine");
                node->cancelPendingRead();
                node = std::make_unique<NodeEngine>();
                nodeHealthy = startAndValidateNode(*node, nodeExe, scriptPath, logPrefix);
                if (!nodeHealthy)
                    logLine(logPrefix + "WARNING: node engine failed to restart after cancellation -- subsequent jobs will fail until it recovers.");

                Value cancelled = Value::object();
                cancelled.set("type", "ERROR");
                cancelled.set("requestId", job->requestId);
                cancelled.set("errorCode", "CANCELLED");
                cancelled.set("message", "request " + job->requestId + " was cancelled");
                cancelled.set("audioSource", job->audioSource);
                outbox->push(toLine(cancelled));
                job->state.store(JobState::Cancelled);
                jobDone = true;
                break;
            }

            // w == WAIT_OBJECT_0 -- node's read event fired (or this is a
            // harmless spurious wake that pollRead() will report Pending
            // for).
            std::string line;
            auto pr = node->pollRead(line);
            if (pr == OverlappedPipeIO::PollResult::Pending) continue;
            if (pr != OverlappedPipeIO::PollResult::Ok)
            {
                logLine(logPrefix + "node engine stdout closed mid-request");
                Value err = Value::object();
                err.set("type", "ERROR");
                err.set("requestId", job->requestId);
                err.set("errorCode", "NODE_EXITED");
                err.set("message", "node analysis engine exited unexpectedly");
                err.set("audioSource", job->audioSource);
                outbox->push(toLine(err));
                job->state.store(JobState::Failed);
                nodeHealthy = false; // this worker's node is gone -- the NEXT job will hit the nodeHealthy==false path above until an operator restarts the desktop; see STATUS.md for why an automatic respawn-on-unexpected-exit isn't attempted here yet
                jobDone = true;
                break;
            }

            logLine(logPrefix + "<- node: " + redactContent(line));
            Value msg;
            try { msg = parse(line); }
            catch (const std::exception& e)
            {
                // Node's stderr is merged into this same stream -- an
                // uncaught exception's stack trace lands here as plain
                // text, not JSON. Log and keep waiting.
                logLine(std::string(logPrefix) + "non-JSON line from node (likely stderr output): " + e.what());
                continue;
            }

            // Stale-result rejection: mirrors the plugin-facing side's own
            // check (BridgeClient.h) -- a message tagged with a DIFFERENT
            // requestId than the one this worker is currently handling
            // (e.g. a leftover from a job this same Node process was
            // working on before a hard-cancel restart raced with its
            // final output) is discarded, not misattributed.
            const std::string msgRequestId = msg.has("requestId") ? msg["requestId"].asString() : "";
            if (!msgRequestId.empty() && msgRequestId != job->requestId)
            {
                logLine(logPrefix + "discarding stale message for abandoned requestId=" + msgRequestId + " (currently handling " + job->requestId + ")");
                continue;
            }

            std::string type = msg["type"].asString();
            if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT" || type == "ERROR")
            {
                msg.set("desktopTotalMs", int(GetTickCount64() - requestStartTick));
                msg.set("audioSource", job->audioSource);
            }
            outbox->push(toLine(msg));

            if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT")
            {
                job->state.store(JobState::Completed);
                jobDone = true;
            }
            else if (type == "ERROR")
            {
                job->state.store(JobState::Failed);
                jobDone = true;
            }
            // else: ANALYSIS_PROGRESS or unrecognized -- keep waiting for a terminal message
        }

        if (!jobDone)
        {
            // messagesRead cap reached -- a pathological engine spraying
            // endless output within the time budget (rare; the timeout
            // above is the real, common bound).
            logLine(logPrefix + "WARNING: node sent an excessive number of messages without a terminal response for requestId=" + job->requestId);
            Value err = Value::object();
            err.set("type", "ERROR");
            err.set("requestId", job->requestId);
            err.set("errorCode", "TOO_MANY_MESSAGES_CLIENT_SIDE");
            err.set("message", "node analysis engine sent an excessive number of messages without a terminal response");
            err.set("audioSource", job->audioSource);
            outbox->push(toLine(err));
            job->state.store(JobState::Failed);
        }

        // Consolidated release point for every path that reaches here with
        // a genuinely terminal job state (hard-cancel, node-exited, normal
        // completion/failure, the too-many-messages cap above, and the
        // TIMEOUT branch earlier in this loop, which also falls through to
        // here) -- releaseJobResources() is idempotent (guarded by the
        // job's own resourcesReleased flag), so it's safe to call
        // unconditionally on every path rather than duplicating this call
        // at each individual branch above.
        releaseJobResources(job);
        job->assignedWorkerWakeEvent.store(nullptr);
    }

    CloseHandle(workerWakeEvent);
}

// ---------------------------------------------------------------------
// PipeSessionOwner: owns one connected plugin's pipe end to end. Services
// its own incoming messages and its own outbox (results/progress routed
// here by NodeWorker threads) via a single multiplexed wait -- never blocks
// indefinitely inside one call the way the old single-threaded main loop
// did, so HEARTBEAT/CANCEL for THIS session stay responsive even while a
// DIFFERENT session's request is running, and a long-running request on
// THIS session doesn't block this session's own HEARTBEAT/CANCEL either
// (those are handled directly, inline, the moment they're read -- only
// ANALYSIS_REQUEST handoffs to the job queue).
// ---------------------------------------------------------------------
static void runPipeSession(HANDLE pipeHandle, std::string sessionId, JobQueue& jobQueue, JobRegistry& jobRegistry,
                            SessionRegistry& sessionRegistry, std::string tempDir)
{
    const std::string logPrefix = "[desktop " + sessionId + "] ";
    OverlappedPipeIO io(pipeHandle);

    // HELLO is the connection handshake, not idle waiting -- bounded
    // (10s), unlike the idle wait for subsequent messages below, so a
    // plugin that connects but never completes its handshake doesn't hold
    // a pipe slot forever.
    std::string helloLine;
    if (io.readLine(10000, helloLine) != OverlappedPipeIO::ReadResult::Ok)
    {
        logLine(logPrefix + "no valid HELLO within 10s -- closing connection");
        DisconnectNamedPipe(pipeHandle);
        CloseHandle(pipeHandle);
        return;
    }
    logLine(logPrefix + "<- " + redactContent(helloLine));
    try
    {
        Value hello = parse(helloLine);
        if (hello["type"].asString() != "HELLO") throw std::runtime_error("first message wasn't HELLO");
    }
    catch (const std::exception& e)
    {
        logLine(logPrefix + "invalid HELLO (" + e.what() + ") -- closing connection");
        DisconnectNamedPipe(pipeHandle);
        CloseHandle(pipeHandle);
        return;
    }
    io.writeLine(toLine(buildCapabilities()));
    logLine(logPrefix + "plugin connected and handshaked");

    HANDLE outboxWakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    auto outbox = std::make_shared<SessionOutbox>(outboxWakeEvent);
    sessionRegistry.add(sessionId, outbox);

    // Per-session connection hygiene: a session that repeatedly sends
    // malformed JSON or unrecognized message types costs this desktop a
    // log line and a bit of parsing for each one -- bounded here rather
    // than tolerated indefinitely, since a same-user peer that's buggy
    // (or deliberately hostile) shouldn't get unlimited free attempts on
    // an otherwise-open connection. Separately, recentRequestTimes bounds
    // how often THIS session can submit ANALYSIS_REQUESTs regardless of
    // whether each one is individually valid -- kMaxActiveJobsPerSession
    // already bounds concurrent work, but doesn't stop a tight loop of
    // submit-then-immediately-cancel from generating request volume far
    // beyond normal interactive use.
    static constexpr int kMaxInvalidMessages = 10;
    static constexpr int kMaxRequestsPerWindow = 20;
    static constexpr ULONGLONG kRequestRateWindowMs = 10000;
    int invalidMessageCount = 0;
    std::deque<ULONGLONG> recentRequestTimes;

    for (;;)
    {
        HANDLE readEvt = io.beginRead();
        HANDLE waitSet[2] = { readEvt, outboxWakeEvent };
        DWORD w = WaitForMultipleObjects(2, waitSet, FALSE, INFINITE); // idle-waiting for the next event is correct server behavior, same as before

        if (w == WAIT_OBJECT_0 + 1)
        {
            for (auto& outLine : outbox->drain())
            {
                if (io.writeLine(outLine)) logLine(logPrefix + "-> " + redactContent(outLine));
                else logLine(logPrefix + "WARNING: write to plugin pipe failed, lastError=" + std::to_string(GetLastError()));
            }
            // Graceful shutdown (gracefulShutdownAndExit()) broadcasts a
            // BROKER_SHUTTING_DOWN message to every session's outbox
            // before setting g_shuttingDown -- by the time this session
            // wakes and drains it (just above), the flag is already set,
            // so checking it here (rather than a dedicated "please close"
            // signal) is sufficient: deliver the notice, then close this
            // session instead of continuing to wait for more incoming
            // messages that will never usefully arrive.
            if (g_shuttingDown.load())
            {
                logLine(logPrefix + "closing (graceful shutdown)");
                break;
            }
            continue;
        }

        std::string line;
        auto pr = io.pollRead(line);
        if (pr == OverlappedPipeIO::PollResult::Pending) continue;
        if (pr != OverlappedPipeIO::PollResult::Ok)
        {
            logLine(logPrefix + "plugin disconnected");
            break;
        }
        if (line.empty()) continue;
        logLine(logPrefix + "<- " + redactContent(line));

        Value msg;
        try { msg = parse(line); }
        catch (const std::exception& e)
        {
            logLine(logPrefix + "bad json from plugin: " + std::string(e.what()));
            if (++invalidMessageCount >= kMaxInvalidMessages)
            {
                logLine(logPrefix + "closing -- too many malformed messages (" + std::to_string(invalidMessageCount) + ")");
                break;
            }
            continue;
        }

        std::string type = msg["type"].asString();
        if (type == "HEARTBEAT")
        {
            Value ack = Value::object();
            ack.set("type", "HEARTBEAT_ACK");
            ack.set("heartbeatId", msg.has("heartbeatId") ? msg["heartbeatId"].asString() : "");
            ack.set("protocolVersion", kProtocolVersion);
            io.writeLine(toLine(ack));
        }
        else if (type == "HOST_STATE")
        {
            // v1: nothing consumes this yet beyond logging -- reserved for
            // e.g. tempo-aware analysis defaults later.
        }
        else if (type == "ANALYSIS_REQUEST")
        {
            std::string requestId = msg["requestId"].asString();
            std::string kind = msg["kind"].asString();
            std::string audioSource = msg.has("audioSource") ? msg["audioSource"].asString() : "unspecified";
            const Value& audio = msg["audio"];
            std::string shmName = audio["shm"].asString();

            // Checked first, before requestId is used to build a real
            // filesystem path below (or anywhere else) -- see
            // isValidRequestId()'s own comment for why this specific
            // check exists.
            if (!isValidRequestId(requestId))
            {
                logLine(logPrefix + "REJECTED ANALYSIS_REQUEST with an invalid requestId (rejected before use, not logged verbatim)");
                Value err = Value::object();
                err.set("type", "ERROR");
                err.set("errorCode", "BAD_REQUEST_JSON");
                err.set("message", "requestId is missing, empty, too long, or contains characters outside [A-Za-z0-9-]");
                io.writeLine(toLine(err));
                continue;
            }

            auto rejectRequest = [&](const std::string& errorCode, const std::string& message)
            {
                logLine(logPrefix + "REJECTED ANALYSIS_REQUEST requestId=" + requestId + " (" + errorCode + "): " + message);
                Value err = Value::object();
                err.set("type", "ERROR");
                err.set("requestId", requestId);
                err.set("errorCode", errorCode);
                err.set("message", message);
                err.set("audioSource", audioSource);
                io.writeLine(toLine(err));
            };

            // Reject unsupported kinds here, not just downstream in
            // analyze.js -- no reason to open shared memory or spend a
            // job-queue slot on a request that's already known invalid.
            if (!isSupportedKind(kind)) { rejectRequest("UNSUPPORTED_KIND", "unsupported analysis kind '" + kind + "'"); continue; }

            // Rate-limit how often THIS session can submit requests at
            // all, independent of whether kMaxActiveJobsPerSession is
            // currently satisfied -- a tight submit/cancel loop could
            // otherwise generate far more request volume than any real
            // interactive use would, without ever actually holding more
            // than the allowed number of jobs active at once.
            {
                ULONGLONG now = GetTickCount64();
                while (!recentRequestTimes.empty() && now - recentRequestTimes.front() > kRequestRateWindowMs)
                    recentRequestTimes.pop_front();
                if (int(recentRequestTimes.size()) >= kMaxRequestsPerWindow)
                {
                    rejectRequest("RATE_LIMITED", "too many requests from this session in a short window -- slow down");
                    continue;
                }
                recentRequestTimes.push_back(now);
            }

            // Bound how much of this session's own concurrent work is
            // allowed to pile up -- a session (buggy client code, or a
            // deliberate flood) shouldn't be able to starve every other
            // session by queuing unbounded work of its own.
            if (int(jobRegistry.findActiveForSession(sessionId).size()) >= kMaxActiveJobsPerSession)
            {
                rejectRequest("TOO_MANY_ACTIVE_JOBS", "this session already has " + std::to_string(kMaxActiveJobsPerSession) + " active/queued jobs -- wait for one to finish or cancel it first");
                continue;
            }
            // Bound total queued (not yet running) work across every
            // session -- protects the shared queue itself from unbounded
            // growth regardless of how many sessions are contributing to it.
            if (jobQueue.size() >= kMaxGlobalQueueDepth)
            {
                rejectRequest("QUEUE_FULL", "the desktop's job queue is at capacity (" + std::to_string(kMaxGlobalQueueDepth) + ") -- try again shortly");
                continue;
            }

            SharedAudioBuffer shm;
            if (!shm.open(shmName))
            {
                rejectRequest("SHM_OPEN_FAILED", "could not open shared memory segment '" + shmName + "'");
                continue;
            }

            // Real bounds, not just "did it open" -- SharedAudioBuffer::open()
            // already validates the header's claimed size doesn't exceed
            // what was actually mapped (see that file), but a plugin could
            // still legitimately allocate and claim an absurdly large,
            // internally-consistent mapping. Reject before this desktop
            // spends disk space writing a correspondingly large temp file.
            //
            // The frame bound is relative to THIS request's own claimed
            // sample rate (kMaxCaptureSeconds worth of it), not a single
            // flat frame count -- see kMaxCaptureSeconds's own comment for
            // why a flat cap doesn't actually bound duration consistently
            // across different sample rates.
            const uint32_t shmFrames = shm.frames();
            const uint32_t shmChannels = shm.channels();
            const uint32_t shmSampleRate = shm.sampleRate();
            const uint64_t maxFramesForThisRequest = uint64_t(shmSampleRate) * kMaxCaptureSeconds;
            if (shmFrames == 0 || uint64_t(shmFrames) > maxFramesForThisRequest ||
                shmChannels == 0 || shmChannels > kMaxAudioChannels ||
                shmSampleRate < kMinSampleRateHz || shmSampleRate > kMaxSampleRateHz)
            {
                rejectRequest("AUDIO_LIMITS_EXCEEDED",
                    "audio parameters out of the accepted range (frames=" + std::to_string(shmFrames) +
                    " channels=" + std::to_string(shmChannels) + " sampleRate=" + std::to_string(shmSampleRate) + ")");
                continue;
            }

            // Checked multiplication, not trusted to fit just because the
            // bounds above passed -- see safeAudioByteCount()'s own
            // comment for why this doesn't depend on those specific limit
            // values never changing.
            uint64_t audioByteCount = 0;
            if (!safeAudioByteCount(shmFrames, shmChannels, audioByteCount))
            {
                rejectRequest("AUDIO_LIMITS_EXCEEDED", "audio parameters would overflow the size calculation");
                continue;
            }

            // System-wide budget, on top of the per-request bound above --
            // see kMaxTotalReservedAudioBytes's own comment. Reserved here,
            // released exactly once via releaseJobResources() once this
            // job reaches a terminal state (main.cpp's runNodeWorker).
            uint64_t reservedBefore = g_reservedAudioBytes.load();
            if (reservedBefore + audioByteCount > kMaxTotalReservedAudioBytes)
            {
                rejectRequest("SERVER_BUSY", "desktop is at its total in-flight audio budget -- try again shortly");
                continue;
            }
            g_reservedAudioBytes.fetch_add(audioByteCount);

            logLine(logPrefix + "opened shared memory " + redactPath(shmName) + " frames=" + std::to_string(shmFrames));

            // Node can't touch Windows shared memory directly, so the
            // desktop process (the only side that opened the mapping)
            // dumps it to a small temp file in the same BSM1 layout for
            // the child to read. This is the one place raw audio touches
            // disk in the whole path. Fast and synchronous -- fine to do
            // inline on this session thread rather than handing off to a
            // worker; only the Node round trip itself needs the queue.
            //
            // Named with a desktop-generated internal ID, not the client's
            // own requestId -- see generateInternalId()'s own comment.
            // isValidRequestId() above already makes requestId itself safe
            // to use here, but this keeps filesystem safety from depending
            // on that validation staying correct forever: requestId is
            // protocol metadata only from this point on (stored on the job
            // for CANCEL lookups and response correlation), never used to
            // build a path.
            std::string internalId = generateInternalId();
            std::string tempPath = tempDir + "\\bsr_" + internalId + ".bsmraw";
            {
                std::ofstream out(tempPath, std::ios::binary);
                out.write("BSM1", 4);
                out.write(reinterpret_cast<const char*>(&shmSampleRate), 4);
                out.write(reinterpret_cast<const char*>(&shmChannels), 4);
                out.write(reinterpret_cast<const char*>(&shmFrames), 4);
                out.write(reinterpret_cast<const char*>(shm.samples()), std::streamsize(audioByteCount));
            }
            logLine(logPrefix + "wrote temp audio file " + redactPath(tempPath));

            auto job = std::make_shared<AnalysisJob>();
            job->sessionId = sessionId;
            job->requestId = requestId;
            job->kind = kind;
            job->audioSource = audioSource;
            job->hostTrackName = msg.has("hostTrackName") ? msg["hostTrackName"].asString() : "";
            job->tempAudioPath = tempPath;
            job->reservedAudioBytes = audioByteCount;
            if (msg.has("role")) job->role = msg["role"].asString();
            if (msg.has("tempo")) job->tempo = msg["tempo"].asNumber();

            jobRegistry.add(job);
            jobQueue.push(job);
        }
        else if (type == "CANCEL")
        {
            const std::string cancelRequestId = msg["requestId"].asString();
            auto job = jobRegistry.find(cancelRequestId);

            Value err = Value::object();
            err.set("type", "ERROR");
            err.set("requestId", cancelRequestId);

            // A CANCEL naming a requestId that belongs to a DIFFERENT
            // session is reported exactly like an unknown one -- from this
            // session's own point of view, that requestId doesn't exist,
            // and there is no legitimate reason for one plugin instance to
            // cancel another's request.
            if (!job || job->sessionId != sessionId)
            {
                err.set("errorCode", "REQUEST_NOT_FOUND");
                err.set("message", "request " + cancelRequestId + " not found");
            }
            else
            {
                JobState result = requestCancel(job);
                if (result == JobState::Cancelled)
                {
                    err.set("errorCode", "CANCELLED");
                    err.set("message", "request " + cancelRequestId + " was cancelled");
                }
                else if (result == JobState::CancelRequested)
                {
                    err.set("errorCode", "CANCEL_REQUESTED");
                    err.set("message", "cancellation requested for request " + cancelRequestId + " -- it was genuinely in flight; a CANCELLED terminal message will follow shortly");
                }
                else
                {
                    err.set("errorCode", "ALREADY_COMPLETED");
                    err.set("message", "request " + cancelRequestId + " already reached a terminal state (" + jobStateName(result) + ") -- nothing to cancel");
                }
            }
            io.writeLine(toLine(err));
        }
        else if (type != "HELLO")
        {
            logLine(logPrefix + "unknown message type: " + type);
            if (++invalidMessageCount >= kMaxInvalidMessages)
            {
                logLine(logPrefix + "closing -- too many malformed/unrecognized messages (" + std::to_string(invalidMessageCount) + ")");
                break;
            }
        }
    }

    io.cancelPendingRead();

    // Don't let a disconnected session's outstanding work run forever
    // unbounded for a client that's no longer listening.
    for (auto& job : jobRegistry.findActiveForSession(sessionId))
        requestCancel(job);

    sessionRegistry.remove(sessionId);
    CloseHandle(outboxWakeEvent);
    DisconnectNamedPipe(pipeHandle);
    CloseHandle(pipeHandle);
    logLine(logPrefix + "session cleaned up");
}

// Creates a fresh pipe instance for each new connection and hands it off
// to its own PipeSessionOwner thread, then immediately loops back to
// accept the NEXT connection -- this runs concurrently with however many
// sessions are already being served, which is what makes multiple
// simultaneous plugin instances real rather than "one at a time, reused
// after disconnect." Runs forever on its own background thread (spawned
// from main()) so the main thread is free to host the tray icon's window
// and message loop.
static void runAcceptLoop(JobQueue& jobQueue, JobRegistry& jobRegistry, SessionRegistry& sessionRegistry, std::string tempDir)
{
    HANDLE pipe = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        kMaxPipeInstances, 65536, 65536, 0, &g_pipeSecurity.attributes);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        logLine("[desktop] FATAL: could not create named pipe " + std::string(PIPE_NAME));
        return;
    }

    static std::atomic<int> sessionCounter{0};

    // Connection-attempt throttling: this loop is the one place that sees
    // every raw connection before a session even exists, so it's the
    // right place to make repeated connect-fail-reconnect cost something.
    // A sliding window of recent connect timestamps (not a persistent
    // per-identity ban list -- a local named pipe doesn't expose a stable
    // identity to ban beyond a session's own short lifetime): once more
    // than kMaxConnectsPerWindow connections have landed within
    // kConnectRateWindowMs, each further one is met with a short sleep
    // before this loop goes back to accepting, rather than serviced
    // immediately -- real backpressure, not just a counter nothing acts
    // on.
    static constexpr int kMaxConnectsPerWindow = 20;
    static constexpr ULONGLONG kConnectRateWindowMs = 5000;
    static constexpr DWORD kConnectBackoffMs = 250;
    std::deque<ULONGLONG> recentConnectTimes;

    while (!g_shuttingDown.load())
    {
        logLine("[desktop] waiting for a plugin to connect on " + std::string(PIPE_NAME) + " ...");
        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &connectOverlapped);
        bool shuttingDownMidWait = false;
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                // Bounded, not INFINITE, specifically so graceful shutdown
                // (gracefulShutdownAndExit()) can stop this loop from
                // accepting further connections within a bounded time
                // rather than being stuck waiting for a client that may
                // never come. Loops on the SAME pending operation across
                // wake-ups (not a fresh ConnectNamedPipe call per
                // iteration -- restarting it while the previous call is
                // still pending on the same handle is not valid) until it
                // either actually completes or shutdown is requested.
                // 500ms per wait is frequent enough that shutdown still
                // feels prompt without meaningfully busy-waiting.
                for (;;)
                {
                    DWORD waitResult = WaitForSingleObject(connectOverlapped.hEvent, 500);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        DWORD dummy = 0;
                        connected = GetOverlappedResult(pipe, &connectOverlapped, &dummy, FALSE);
                        break;
                    }
                    if (g_shuttingDown.load())
                    {
                        CancelIoEx(pipe, &connectOverlapped);
                        DWORD dummy = 0;
                        GetOverlappedResult(pipe, &connectOverlapped, &dummy, TRUE); // wait for the cancel to actually land before touching connectOverlapped/pipe again
                        shuttingDownMidWait = true;
                        break;
                    }
                    // WAIT_TIMEOUT, not shutting down -- keep waiting on this same still-pending operation
                }
            }
            else if (err == ERROR_PIPE_CONNECTED)
            {
                connected = TRUE;
            }
        }
        CloseHandle(connectOverlapped.hEvent);
        if (shuttingDownMidWait)
        {
            CloseHandle(pipe);
            logLine("[desktop] accept loop stopping (graceful shutdown)");
            return;
        }
        if (!connected)
        {
            logLine("[desktop] WARNING: ConnectNamedPipe failed, error=" + std::to_string(GetLastError()) + " -- dropping this pipe instance and creating a new one");
            CloseHandle(pipe);
        }
        else
        {
            ULONGLONG now = GetTickCount64();
            while (!recentConnectTimes.empty() && now - recentConnectTimes.front() > kConnectRateWindowMs)
                recentConnectTimes.pop_front();
            recentConnectTimes.push_back(now);
            if (int(recentConnectTimes.size()) > kMaxConnectsPerWindow)
            {
                logLine("[desktop] WARNING: connection rate exceeded (" + std::to_string(recentConnectTimes.size()) + " in " + std::to_string(kConnectRateWindowMs) + "ms) -- applying backoff before accepting the next one");
                Sleep(kConnectBackoffMs);
            }

            // Approximate, not exact: sessionRegistry only counts sessions
            // that have already completed HELLO (see runPipeSession), so a
            // burst of simultaneous connections mid-handshake could
            // transiently exceed this cap before the registry catches up.
            // kMaxPipeInstances above is the hard OS-level backstop either
            // way; this is a soft cap on how many sessions this
            // application actually tries to service concurrently.
            if (sessionRegistry.count() >= kMaxConcurrentSessions)
            {
                logLine("[desktop] WARNING: rejecting connection -- already at kMaxConcurrentSessions (" + std::to_string(kMaxConcurrentSessions) + ")");
                // No thread takes ownership of this pipe instance in this
                // branch (unlike the spawned-session case below, where
                // runPipeSession's own cleanup eventually closes it) --
                // both calls are needed here or this handle leaks on every
                // rejected connection.
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
            else
            {
                std::string sessionId = "session-" + std::to_string(sessionCounter.fetch_add(1));
                logLine("[desktop] plugin connected -- spawning " + sessionId);
                std::thread(runPipeSession, pipe, sessionId, std::ref(jobQueue), std::ref(jobRegistry), std::ref(sessionRegistry), tempDir).detach();
            }
        }

        pipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            kMaxPipeInstances, 65536, 65536, 0, &g_pipeSecurity.attributes);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            logLine("[desktop] FATAL: could not create a new named pipe instance");
            return;
        }
    }
}

// ---------------------------------------------------------------------
// System tray integration: lets BeatShore Desktop run as a background
// broker the user doesn't have to babysit a console window for, with a
// "Start at login" toggle they control (per-user, via the standard HKCU
// Run key -- no admin rights needed for this part, unlike the installer
// itself) rather than autostart being silently forced on or off.
// ---------------------------------------------------------------------
static const char* kRunKeyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunValueName = "BeatShoreDesktop";
static constexpr UINT WM_BEATSHORE_TRAYICON = WM_APP + 1;
static constexpr UINT_PTR ID_TRAY_STATUS = 1001;
static constexpr UINT_PTR ID_TRAY_START_AT_LOGIN = 1002;
static constexpr UINT_PTR ID_TRAY_QUIT = 1003;
static NOTIFYICONDATAA g_trayIconData{};

static std::string exePathQuoted()
{
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return "\"" + std::string(buf, len) + "\"";
}

// Real registry state, not a locally-cached flag -- reflects whatever's
// actually there even if something else (a future settings UI, a manual
// registry edit) changed it since this process started.
static bool isStartAtLoginEnabled()
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    LONG result = RegQueryValueExA(key, kRunValueName, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ;
}

static void setStartAtLoginEnabled(bool enable)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
    {
        logLine("[desktop] WARNING: could not open the Run registry key to change start-at-login");
        return;
    }
    if (enable)
    {
        // Belt and suspenders alongside defaultScriptPath()'s own
        // multi-candidate resolution (see that function's comment for the
        // real bug this addresses -- a bare exe path with no argument,
        // launched from an unrelated working directory the way the Run
        // key actually does it, used to resolve outside the install root
        // entirely): explicitly embedding the resolved script path here
        // means a future change to defaultScriptPath()'s own search order
        // can't silently break autostart specifically, since this
        // registry value pins down what already worked at the moment
        // "Start at login" was turned on.
        std::string value = exePathQuoted() + " \"" + defaultScriptPath() + "\"";
        RegSetValueExA(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD(value.size() + 1));
        logLine("[desktop] start-at-login enabled (" + value + ")");
    }
    else
    {
        RegDeleteValueA(key, kRunValueName);
        logLine("[desktop] start-at-login disabled");
    }
    RegCloseKey(key);
}

static void showTrayContextMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING | MF_DISABLED, ID_TRAY_STATUS, "BeatShore Desktop is running");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING | (isStartAtLoginEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_START_AT_LOGIN, "Start at login");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_QUIT, "Quit BeatShore Desktop");
    // Required so the menu dismisses correctly if the user clicks
    // elsewhere instead of choosing an item -- a documented Win32 quirk
    // for TrackPopupMenu from a background-only window.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// Real graceful shutdown, not just ExitProcess() -- the broker can genuinely
// hold state that matters mid-teardown (connected sessions, queued jobs, a
// job actively running TensorFlow inference, Node child processes, shared-
// memory mappings the plugin side is still holding open). Runs the
// requested sequence in order, each step bounded so a single stuck step
// can't turn "Quit" into "hangs forever":
//   1. Stop accepting new connections (g_shuttingDown -- runAcceptLoop
//      checks this and returns within ~500ms).
//   2. Tell every currently-connected client the broker is going away.
//   3/4. Cancel every active job, queued or running (requestCancel() --
//      for a Running job this triggers the same real Node kill-and-restart
//      already verified elsewhere, not just a label).
//   5. Wake any idle worker blocked in JobQueue::waitPop so it notices
//      g_shuttingDown and exits its own loop instead of waiting for a job
//      that will never come.
//   6. Each session, once it drains the shutdown notice pushed in step 2
//      and observes g_shuttingDown, closes its own pipe handle (see
//      runPipeSession's outbox-drain branch) -- not driven from here
//      directly, since only a session's own thread may touch its pipe
//      handle (see this file's own top-of-file threading note).
//   7. Remove the tray icon.
//   8. Release the single-instance mutex.
//   9. Exit -- after a bounded grace window (not waiting indefinitely for
//      every detached thread to notice and finish, which can't be
//      guaranteed anyway since they're detached, not joined). Whatever
//      hasn't wrapped up by then is terminated by the process exit itself
//      -- the "forced termination fallback" this is allowed to have.
static void gracefulShutdownAndExit()
{
    logLine("[desktop] graceful shutdown starting");

    g_shuttingDown.store(true);

    // A dedicated message type, not a generic ERROR with a
    // BROKER_SHUTTING_DOWN errorCode -- a client shouldn't have to sniff
    // an error's errorCode to tell "the broker is going away on purpose"
    // apart from "something actually went wrong." reason is fixed today
    // (only ever "user_requested" -- there's no other trigger for a
    // graceful shutdown yet) but included now so a future trigger (an
    // installer-driven upgrade, say) doesn't need a protocol version bump
    // to add it. retryAfterMs is explicitly null: this desktop doesn't
    // auto-restart itself, so there's no concrete number to give a
    // reconnecting client that would mean anything -- see PROTOCOL.md.
    Value shutdownNotice = Value::object();
    shutdownNotice.set("type", "BROKER_SHUTTING_DOWN");
    shutdownNotice.set("reason", "user_requested");
    shutdownNotice.set("retryAfterMs", Value()); // null
    g_sessionRegistry.broadcast(toLine(shutdownNotice));

    for (auto& job : g_jobRegistry.findAllActive())
        requestCancel(job);

    g_jobQueue.wakeAll();

    // Bounded grace window: not a guarantee every session/worker has
    // actually finished by the time this returns (detached threads can't
    // be joined to confirm that), just a real chance for the steps above
    // to take effect before this function moves on regardless.
    Sleep(1500);

    logLine("[desktop] graceful shutdown: removing tray icon, releasing mutex, exiting");
    Shell_NotifyIconA(NIM_DELETE, &g_trayIconData);

    if (g_singleInstanceMutex != nullptr)
    {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }

    // Ends the message loop normally (runTrayApp() returns, main()
    // returns) rather than an abrupt ExitProcess() -- any detached
    // threads that haven't wrapped up within the grace window above are
    // still reclaimed by the OS at process exit either way, which is the
    // documented, intended forced-termination fallback, not a bug.
    PostQuitMessage(0);
}

static LRESULT CALLBACK trayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_BEATSHORE_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) showTrayContextMenu(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case ID_TRAY_START_AT_LOGIN:
                    setStartAtLoginEnabled(!isStartAtLoginEnabled());
                    return 0;
                case ID_TRAY_QUIT:
                    gracefulShutdownAndExit();
                    return 0;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Runs on the main thread for the rest of the process's life -- creates a
// message-only window (HWND_MESSAGE: no visible window, just something
// that can own the tray icon and receive its callback messages, which
// Shell_NotifyIcon requires), adds the tray icon, and pumps the message
// loop. Returns the process's exit code once the loop ends (WM_QUIT).
static int runTrayApp()
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = trayWindowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "BeatShoreDesktopTrayWindow";
    if (!RegisterClassA(&wc))
    {
        logLine("[desktop] FATAL: could not register the tray window class (error=" + std::to_string(GetLastError()) + ")");
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "BeatShore Desktop", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        logLine("[desktop] FATAL: could not create the tray message window (error=" + std::to_string(GetLastError()) + ")");
        return 1;
    }

    g_trayIconData.cbSize = sizeof(g_trayIconData);
    g_trayIconData.hWnd = hwnd;
    g_trayIconData.uID = 1;
    g_trayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIconData.uCallbackMessage = WM_BEATSHORE_TRAYICON;
    // The real BeatShore icon, embedded as a resource (resources.rc,
    // built from assets/icon/BeatShore.ico -- see
    // assets/icon/generate_icon.py) -- not the generic system
    // IDI_APPLICATION this used previously. Falls back to the system
    // icon if the resource somehow fails to load (a corrupt/missing
    // resource shouldn't take down the tray icon entirely), logged so a
    // silent fallback isn't mistaken for success.
    g_trayIconData.hIcon = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDI_APP_ICON));
    if (g_trayIconData.hIcon == nullptr)
    {
        logLine("[desktop] WARNING: failed to load embedded BeatShore icon resource -- falling back to the generic system icon");
        g_trayIconData.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    }
    strncpy_s(g_trayIconData.szTip, "BeatShore Desktop", _TRUNCATE);

    if (!Shell_NotifyIconA(NIM_ADD, &g_trayIconData))
    {
        logLine("[desktop] WARNING: Shell_NotifyIconA(NIM_ADD) failed (error=" + std::to_string(GetLastError()) + ") -- continuing without a visible tray icon; the broker itself is unaffected.");
    }
    else
    {
        logLine("[desktop] tray icon added");
        // Still a console-subsystem build (not WIN32) so this project's
        // own stdout-redirection-based test tooling keeps working
        // unchanged -- hiding the console window here, rather than
        // switching subsystems, gets the same "quiet background app"
        // result for a real launch without disturbing that. Only once
        // the tray icon is confirmed present -- a launch that failed to
        // get a tray icon keeps its console visible so the WARNING above
        // is actually seen, not hidden along with everything else.
        HWND consoleWindow = GetConsoleWindow();
        if (consoleWindow != nullptr) ShowWindow(consoleWindow, SW_HIDE);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return int(msg.wParam);
}

// `--self-test <scriptPath>`: talks to a real NodeEngine directly (no named
// pipe, no plugin) to verify an install actually works end to end before a
// user's first real request does -- checks the bundled Node launches and
// reaches READY with the expected analysis kinds advertised, that a real
// tempo request round-trips, that a real transcribePolyphonic request
// round-trips (which only succeeds if the Basic Pitch model actually
// loads -- the single check most likely to fail from a packaging mistake,
// since it depends on vendor/basic-pitch-model/ being staged at the
// correct location), and that the MIDI export directory is writable.
// Returns 0 only if every check passes; anything else is a real,
// specific failure, not a guess.
static int runSelfTest(const std::string& nodeExe, const std::string& scriptPath)
{
    std::cout << "[self-test] starting -- node: " << nodeExe << " script: " << scriptPath << std::endl;

    NodeEngine node;
    if (!startAndValidateNode(node, nodeExe, scriptPath, "[self-test] "))
    {
        std::cout << "[self-test] FAIL: node engine failed to start or reach a valid READY" << std::endl;
        return 1;
    }

    // Re-derive READY separately from startAndValidateNode's own internal
    // check so this can additionally confirm the specific analysis kinds
    // this self-test is about to exercise are actually advertised --
    // startAndValidateNode only checks type=="READY", not the kind list.
    // (startAndValidateNode already consumed the READY line, so this
    // re-reads by re-running the same request/response pattern below
    // instead of trying to peek at an already-consumed line.)
    bool tempoOk = false;
    bool polyOk = false;
    int polyNoteCount = -1;

    char tempDirBuf[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDirBuf);
    std::string tempDir(tempDirBuf);
    if (!tempDir.empty() && tempDir.back() == '\\') tempDir.pop_back();
    std::string fixturePath = tempDir + "\\beatshore_selftest_fixture.bsmraw";

    // A synthetic two-note fixture (C4+E4, faded in/out), not real music --
    // just enough clearly-pitched signal for Basic Pitch's CNN to reliably
    // detect at least one note, proving the model actually loaded and ran
    // rather than silently returning nothing.
    {
        const uint32_t sr = 22050, ch = 1, frames = sr * 2; // 2s mono @ 22050Hz -- basic-pitch's own required rate, no resampling needed
        std::vector<float> samples(frames);
        for (uint32_t i = 0; i < frames; ++i)
        {
            double t = double(i) / sr;
            double fadeIn = (std::min)(1.0, t / 0.05);
            double fadeOut = (std::min)(1.0, (2.0 - t) / 0.05);
            double env = (std::min)(fadeIn, fadeOut);
            double s = 0.25 * std::sin(2.0 * 3.14159265358979 * 261.63 * t)   // C4
                     + 0.25 * std::sin(2.0 * 3.14159265358979 * 329.63 * t);  // E4
            samples[i] = float(s * env);
        }
        std::ofstream out(fixturePath, std::ios::binary);
        out.write("BSM1", 4);
        out.write(reinterpret_cast<const char*>(&sr), 4);
        out.write(reinterpret_cast<const char*>(&ch), 4);
        out.write(reinterpret_cast<const char*>(&frames), 4);
        out.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * sizeof(float)));
    }

    auto runFixtureRequest = [&](const std::string& requestId, const std::string& kind, DWORD timeoutMs, Value& outTerminal) -> bool
    {
        Value req = Value::object();
        req.set("requestId", requestId);
        req.set("kind", kind);
        req.set("audioFile", fixturePath);
        if (!node.writeLine(stringify(req))) return false;

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            std::string line;
            if (node.readLine(DWORD(deadline - now), line) != OverlappedPipeIO::ReadResult::Ok) return false;
            Value msg;
            try { msg = parse(line); } catch (const std::exception&) { continue; }
            const std::string msgRequestId = msg.has("requestId") ? msg["requestId"].asString() : "";
            if (!msgRequestId.empty() && msgRequestId != requestId) continue;
            const std::string type = msg.has("type") ? msg["type"].asString() : "";
            if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT") { outTerminal = msg; return true; }
            if (type == "ERROR") { outTerminal = msg; return false; }
        }
    };

    {
        Value result;
        tempoOk = runFixtureRequest("selftest-tempo", "tempo", 15000, result);
        std::cout << "[self-test] " << (tempoOk ? "PASS" : "FAIL") << ": tempo analysis round trip" << std::endl;
    }

    {
        Value result;
        bool got = runFixtureRequest("selftest-poly", "transcribePolyphonic", 60000, result);
        polyNoteCount = (got && result.has("noteCount")) ? int(result["noteCount"].asNumber()) : -1;
        polyOk = got && polyNoteCount > 0;
        std::cout << "[self-test] " << (polyOk ? "PASS" : "FAIL")
                   << ": polyphonic transcription (Basic Pitch model load + inference), noteCount=" << polyNoteCount << std::endl;
    }

    DeleteFileA(fixturePath.c_str());

    // Matches midi-export.js's own EXPORT_DIR exactly -- a real write
    // probe, not just a directory-exists check (permissions can block
    // writes to a directory that still shows as existing).
    bool exportDirOk = false;
    {
        char userProfileBuf[MAX_PATH]{};
        GetEnvironmentVariableA("USERPROFILE", userProfileBuf, MAX_PATH);
        std::string base = userProfileBuf;
        std::string beatshoreDir = base + "\\Documents\\BeatShore";
        std::string exportDir = beatshoreDir + "\\Exports";
        CreateDirectoryA(beatshoreDir.c_str(), nullptr);
        CreateDirectoryA(exportDir.c_str(), nullptr);
        std::string probePath = exportDir + "\\.selftest_write_check";
        std::ofstream probe(probePath);
        if (probe.good())
        {
            probe << "ok";
            probe.close();
            exportDirOk = true;
            DeleteFileA(probePath.c_str());
        }
    }
    std::cout << "[self-test] " << (exportDirOk ? "PASS" : "FAIL") << ": MIDI export directory is writable" << std::endl;

    const bool allOk = tempoOk && polyOk && exportDirOk;
    std::cout << "[self-test] " << (allOk ? "ALL PASSED" : "FAILED") << std::endl;
    return allOk ? 0 : 1;
}

int main(int argc, char** argv)
{
    // Explicit, user-activated diagnostic mode -- see redactPath()/
    // redactContent()'s own comments for what this gates. Checked once
    // here, not per log call.
    char diagBuf[8];
    DWORD diagLen = GetEnvironmentVariableA("BEATSHORE_DIAGNOSTIC_LOG", diagBuf, sizeof(diagBuf));
    g_diagnosticLogging = (diagLen > 0 && diagLen < sizeof(diagBuf) && diagBuf[0] != '0');
    if (g_diagnosticLogging)
        logLine("[desktop] diagnostic logging enabled (BEATSHORE_DIAGNOSTIC_LOG set) -- full paths and message content will be logged");

    std::string nodeExe = defaultNodeExe();
    std::string scriptPath = defaultScriptPath();

    if (argc > 1 && std::string(argv[1]) == "--self-test")
    {
        if (argc > 2) scriptPath = argv[2];
        return runSelfTest(nodeExe, scriptPath);
    }

    if (argc > 1) scriptPath = argv[1];

    // Only one broker instance should ever run at a time -- a second one
    // would try to create the same named pipe and fail (or, worse, race
    // for it), and would spawn its own competing Node engine(s) for no
    // benefit. "Global\\" (not "Local\\", which OverlappedPipeIO's own
    // shared-memory naming already uses per-request) so this is visible
    // across sessions on a multi-user machine too, matching how the named
    // pipe itself is already a single machine-wide resource. Stored at
    // file scope (g_singleInstanceMutex) and explicitly released during
    // graceful shutdown (see gracefulShutdownAndExit()) -- previously
    // left open deliberately since the OS releases it on any process
    // exit regardless, but an explicit release is one of the graceful-
    // shutdown steps now, so it happens on the clean path too, not only
    // implicitly at process teardown.
    g_singleInstanceMutex = CreateMutexA(nullptr, TRUE, "Global\\BeatShoreDesktopBroker");
    if (g_singleInstanceMutex == nullptr)
    {
        logLine("[desktop] FATAL: could not create the single-instance mutex (error=" + std::to_string(GetLastError()) + ")");
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        logLine("[desktop] FATAL: another BeatShoreDesktop instance is already running -- exiting rather than competing for the same named pipe.");
        return 1;
    }

    if (!g_pipeSecurity.init())
    {
        logLine("[desktop] FATAL: could not build the pipe security descriptor (error=" + std::to_string(GetLastError()) + ")");
        return 1;
    }

    char tempDirBuf[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDirBuf);
    std::string tempDir(tempDirBuf);
    if (!tempDir.empty() && tempDir.back() == '\\') tempDir.pop_back();

    logLine("[desktop] BeatShoreDesktop starting");
    logLine("[desktop] node engine script: " + scriptPath);

    std::vector<std::thread> workerThreads;
    for (int i = 0; i < kMaxConcurrentNodeJobs; ++i)
        workerThreads.emplace_back(runNodeWorker, i, std::ref(g_jobQueue), std::ref(g_sessionRegistry), std::ref(g_shuttingDown), nodeExe, scriptPath);
    for (auto& t : workerThreads) t.detach();

    // Runs on its own background thread (see runTrayApp() below) so the
    // main thread is free to host the tray icon's window and message
    // loop -- Win32 requires the thread that creates a window to be the
    // one pumping its messages.
    std::thread(runAcceptLoop, std::ref(g_jobQueue), std::ref(g_jobRegistry), std::ref(g_sessionRegistry), tempDir).detach();

    return runTrayApp();
}
