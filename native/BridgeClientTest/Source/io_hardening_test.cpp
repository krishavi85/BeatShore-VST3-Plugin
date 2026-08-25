// Deterministic tests for the exact failure modes overlapped I/O was added
// to handle -- driving the REAL BridgeClient class (not a reimplementation)
// against a MOCK server this test controls precisely, so "the desktop
// never responds" isn't a hope, it's guaranteed. Doesn't run alongside a
// real BeatShoreDesktop.exe -- this owns \\.\pipe\BeatShoreBridge.v1 itself.
#include "../../BeatShoreBridge/Source/BridgeClient.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>

static const char* PIPE_NAME = "\\\\.\\pipe\\BeatShoreBridge.v1";

// Minimal mock desktop: accepts one connection, then behaves however the
// caller's lambda says. Runs on its own thread so the test's main thread
// can drive a real BridgeClient concurrently, same as a real desktop
// process and plugin would.
struct MockServer
{
    std::thread thread;
    std::atomic<bool> connected { false };

    template <typename Behavior>
    void start(Behavior behavior)
    {
        thread = std::thread([this, behavior]{
            HANDLE h = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { std::cerr << "[mock] CreateNamedPipeA failed, GetLastError=" << GetLastError() << "\n"; return; }
            std::cout << "[mock] pipe created" << std::endl;

            OVERLAPPED ov{};
            ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = ConnectNamedPipe(h, &ov);
            DWORD connectErr = ok ? 0 : GetLastError();
            if (!ok && connectErr == ERROR_IO_PENDING)
            {
                DWORD waitResult = WaitForSingleObject(ov.hEvent, 5000);
                std::cout << "[mock] ConnectNamedPipe pending, wait result=" << waitResult << std::endl;
            }
            else
            {
                std::cout << "[mock] ConnectNamedPipe returned immediately, ok=" << ok << " err=" << connectErr << std::endl;
            }
            CloseHandle(ov.hEvent);
            connected.store(true);
            std::cout << "[mock] connected, entering behavior()" << std::endl;

            OverlappedPipeIO io(h);
            behavior(io);
            std::cout << "[mock] behavior() returned" << std::endl;
            CloseHandle(h);
        });
    }

    ~MockServer() { if (thread.joinable()) thread.join(); }
};

static int failures = 0;
static void check(bool cond, const std::string& label)
{
    std::cout << "[test] " << label << ": " << (cond ? "PASS" : "FAIL") << std::endl;
    if (!cond) failures++;
}

int main()
{
    // --- Scenario: "Desktop accepts a connection but never responds" ---
    // Mock accepts the connection, then never writes anything -- not even
    // CAPABILITIES. BridgeClient::tryConnect()'s HELLO->CAPABILITIES read
    // has a 5s deadline; this must resolve (Disconnected, ready to retry)
    // well before this test's own generous 15s watchdog would consider it
    // hung.
    {
        std::cout << "\n=== Scenario 1: desktop accepts connection, never responds to HELLO ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string line;
            io.readLine(20000, line); // reads HELLO...
            // ...and never answers it. Critically, this must NOT let the
            // lambda return here: returning closes the pipe handle
            // (MockServer::start), which BridgeClient detects as a clean
            // disconnect (Closed), not the silent-but-still-connected peer
            // this scenario is meant to model. A second blocking read (which
            // nothing will ever satisfy) keeps the handle open past
            // BridgeClient's 5s HELLO deadline so the client genuinely
            // exercises the Timeout path.
            io.readLine(20000, line);
        });

        BridgeClient client;
        client.start();

        const auto t0 = std::chrono::steady_clock::now();
        bool sawConnecting = false, resolvedInTime = false;
        BridgeStatus lastStatus = BridgeStatus::Disconnected;
        for (int i = 0; i < 150; ++i) // up to 15s
        {
            auto status = client.getStatus();
            if (status != lastStatus || i % 20 == 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                std::cout << "[test]   status=" << int(status) << " at " << elapsed << "ms (mock connected=" << mock.connected.load() << ")" << std::endl;
                lastStatus = status;
            }
            if (status == BridgeStatus::Connecting) sawConnecting = true;
            if (sawConnecting && status == BridgeStatus::Disconnected)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                resolvedInTime = elapsed < 8000; // should resolve around the 5s HELLO deadline, generous margin
                std::cout << "[test] resolved to Disconnected after " << elapsed << "ms" << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(sawConnecting, "scenario 1: client entered Connecting");
        check(resolvedInTime, "scenario 1: client resolved (didn't hang) within the HELLO deadline");
        // client destructs here -- also exercises normal (not blocked) shutdown.
    }

    // --- Scenario: "Plugin is destroyed during a blocked read" ---
    // Mock completes HELLO/CAPABILITIES normally (so BridgeClient reaches
    // Connected), then goes silent forever on the ANALYSIS_REQUEST that
    // follows. While BridgeClient's IPC thread is genuinely blocked inside
    // that read, destroy it from this thread and measure how long
    // destruction takes -- this is the exact scenario the destructor
    // reordering (signalThreadShouldExit -> closePipeHandle -> stopThread)
    // exists for. Must be fast (well under the 60s transcribePolyphonic
    // budget the read itself is waiting on), not a hang.
    {
        std::cout << "\n=== Scenario 2: plugin destroyed while blocked mid-read ===" << std::endl;
        MockServer mock;
        std::atomic<bool> gotAnalysisRequest { false };
        mock.start([&gotAnalysisRequest](OverlappedPipeIO& io){
            std::string line;
            if (io.readLine(5000, line) != OverlappedPipeIO::ReadResult::Ok) return; // HELLO
            bsjson::Value caps = bsjson::Value::object();
            caps.set("type", "CAPABILITIES");
            caps.set("desktopVersion", "mock");
            bsjson::Value analysis = bsjson::Value::array();
            analysis.push(bsjson::Value("tempo"));
            caps.set("analysis", analysis);
            io.writeLine(bsjson::stringify(caps));

            if (io.readLine(5000, line) == OverlappedPipeIO::ReadResult::Ok) // ANALYSIS_REQUEST
                gotAnalysisRequest.store(true);
            // ...then genuinely silent. Long read so this thread stays
            // alive (and the pipe stays open) well past the test's own
            // measurement window, without actually needing the full 20s.
            io.readLine(20000, line);
        });

        auto client = std::make_unique<BridgeClient>();
        client->start();
        for (int i = 0; i < 100 && client->getStatus() != BridgeStatus::Connected; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(client->getStatus() == BridgeStatus::Connected, "scenario 2: client reached Connected");

        std::vector<float> dummyAudio(48000 * 2, 0.01f); // not silent -- avoid triggering the plugin's own silent-audio check (not exercised here, but keep it realistic)
        bool started = client->requestAnalysis(dummyAudio, 48000, 2, 48000, "tempo", "file");
        check(started, "scenario 2: requestAnalysis accepted");

        for (int i = 0; i < 50 && !gotAnalysisRequest.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(gotAnalysisRequest.load(), "scenario 2: mock server received the ANALYSIS_REQUEST");

        // Give the IPC thread a moment to actually enter the blocking read
        // for the result before we destroy it out from under that read.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const auto t0 = std::chrono::steady_clock::now();
        client.reset(); // ~BridgeClient() while genuinely blocked mid-read
        const auto destroyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] destruction took " << destroyMs << "ms" << std::endl;
        check(destroyMs < 5000, "scenario 2: destruction was fast, not a multi-second hang");
    }

    // --- Scenario: "Desktop terminates during shared-memory transfer" ---
    // Mock completes HELLO/CAPABILITIES and receives the ANALYSIS_REQUEST
    // normally, then abruptly closes the pipe -- modeling the desktop
    // process dying mid-analysis, as opposed to scenario 2's "silent but
    // still connected" peer. This must resolve via OverlappedPipeIO's
    // Closed path (fast, driven by the peer closing the handle) rather than
    // waiting out the full kind-aware timeout, and the client must publish
    // a clean PIPE_READ_FAILED error rather than hanging or crashing.
    {
        std::cout << "\n=== Scenario 3: desktop terminates mid-analysis (pipe closed, not silent) ===" << std::endl;
        MockServer mock;
        std::atomic<bool> gotAnalysisRequest3 { false };
        mock.start([&gotAnalysisRequest3](OverlappedPipeIO& io){
            std::string line;
            if (io.readLine(5000, line) != OverlappedPipeIO::ReadResult::Ok) return; // HELLO
            bsjson::Value caps = bsjson::Value::object();
            caps.set("type", "CAPABILITIES");
            caps.set("desktopVersion", "mock");
            bsjson::Value analysis = bsjson::Value::array();
            analysis.push(bsjson::Value("tempo"));
            caps.set("analysis", analysis);
            io.writeLine(bsjson::stringify(caps));

            if (io.readLine(5000, line) == OverlappedPipeIO::ReadResult::Ok)
                gotAnalysisRequest3.store(true);
            // ...then the "process" dies: the handle closes here (function
            // return -> MockServer::start's CloseHandle) instead of going
            // silent-but-open.
        });

        BridgeClient client;
        client.start();
        for (int i = 0; i < 100 && client.getStatus() != BridgeStatus::Connected; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(client.getStatus() == BridgeStatus::Connected, "scenario 3: client reached Connected");

        std::vector<float> dummyAudio(48000 * 2, 0.01f);
        bool started3 = client.requestAnalysis(dummyAudio, 48000, 2, 48000, "tempo", "file");
        check(started3, "scenario 3: requestAnalysis accepted");

        const auto t0 = std::chrono::steady_clock::now();
        bool errored = false;
        BridgeAnalysisResult result3;
        for (int i = 0; i < 100; ++i) // up to 10s
        {
            if (client.takeResult(result3)) { errored = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto elapsedMs3 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] result arrived after " << elapsedMs3 << "ms, success=" << (errored && result3.success ? "true" : "false")
                  << " errorCode=" << (errored ? result3.errorCode : "<none>") << std::endl;
        check(gotAnalysisRequest3.load(), "scenario 3: mock server received the ANALYSIS_REQUEST");
        check(errored && !result3.success, "scenario 3: client published an error result");
        check(errored && result3.errorCode == "PIPE_READ_FAILED", "scenario 3: error is PIPE_READ_FAILED (Closed), not a TIMEOUT wait-out");
        check(elapsedMs3 < 3000, "scenario 3: resolved promptly via the closed-pipe detection, not the full 10s kind timeout");
    }

    // --- Scenario: "Timeout occurs immediately before a valid result
    // arrives" --- Mock deliberately delays its response to the first
    // request until just after BridgeClient's own 10s "tempo" deadline has
    // already fired client-side, so the client experiences a real TIMEOUT
    // for request 1 while that now-stale response is still in flight. The
    // real-time gap this takes is long enough that BridgeClient's idle loop
    // legitimately sends a HEARTBEAT in the middle of it, so this also
    // exercises sendHeartbeat()'s own requestId-independent filtering
    // (BridgeClient.h): the stale ANALYSIS_RESULT must NOT be mistaken for
    // the HEARTBEAT_ACK it's waiting for (type mismatch -> discarded), and
    // conversely the real HEARTBEAT_ACK must not be mistaken for an
    // analysis result. The stale response itself then sits unread until
    // the *next* request's read loop, which must discard it by requestId
    // (handleAnalysisRequest's stale-requestId defense) -- a second, fresh
    // request must get its own correctly-matched result, not be corrupted
    // by the stale one.
    {
        std::cout << "\n=== Scenario 4: stale response arrives just after a timeout, next request must not be corrupted ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string line;
            if (io.readLine(5000, line) != OverlappedPipeIO::ReadResult::Ok) return; // HELLO
            bsjson::Value caps = bsjson::Value::object();
            caps.set("type", "CAPABILITIES");
            caps.set("desktopVersion", "mock");
            bsjson::Value analysis = bsjson::Value::array();
            analysis.push(bsjson::Value("tempo"));
            caps.set("analysis", analysis);
            io.writeLine(bsjson::stringify(caps));

            // Waits real wall-clock seconds for the client's own timeout to
            // fire, so BridgeClient's idle loop legitimately sends a
            // HEARTBEAT in the meantime (its 5s interval elapses while this
            // mock is asleep below) -- a real desktop always acks those
            // immediately (main.cpp), so this mock must too, or the
            // ANALYSIS_REQUEST/ANALYSIS_RESULT message stream gets
            // misaligned with the heartbeat traffic interleaved into it.
            auto readNextAnalysisRequest = [&io, &line]() -> std::string {
                for (;;)
                {
                    if (io.readLine(20000, line) != OverlappedPipeIO::ReadResult::Ok) return "";
                    bsjson::Value msg;
                    try { msg = bsjson::parse(line); } catch (const std::exception&) { continue; }
                    std::string type = msg["type"].asString();
                    if (type == "HEARTBEAT")
                    {
                        bsjson::Value ack = bsjson::Value::object();
                        ack.set("type", "HEARTBEAT_ACK");
                        ack.set("heartbeatId", msg.has("heartbeatId") ? msg["heartbeatId"].asString() : "");
                        ack.set("protocolVersion", 1);
                        io.writeLine(bsjson::stringify(ack));
                        continue;
                    }
                    if (type == "ANALYSIS_REQUEST") return msg["requestId"].asString();
                }
            };

            std::string requestId1 = readNextAnalysisRequest();
            if (requestId1.empty()) return;

            // Deliberately arrives ~1s after the client's own 10s "tempo"
            // deadline -- the client will already have given up and moved
            // on by the time this is written.
            std::this_thread::sleep_for(std::chrono::milliseconds(11000));
            bsjson::Value stale = bsjson::Value::object();
            stale.set("type", "ANALYSIS_RESULT");
            stale.set("requestId", requestId1);
            stale.set("result", bsjson::Value(999.0)); // implausible value -- if this leaks into request 2's result, the test catches it
            io.writeLine(bsjson::stringify(stale));

            std::string requestId2 = readNextAnalysisRequest();
            if (requestId2.empty()) return;
            bsjson::Value fresh = bsjson::Value::object();
            fresh.set("type", "ANALYSIS_RESULT");
            fresh.set("requestId", requestId2);
            fresh.set("result", bsjson::Value(123.45));
            io.writeLine(bsjson::stringify(fresh));
        });

        BridgeClient client;
        client.start();
        for (int i = 0; i < 100 && client.getStatus() != BridgeStatus::Connected; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(client.getStatus() == BridgeStatus::Connected, "scenario 4: client reached Connected");

        std::vector<float> dummyAudio(48000 * 2, 0.01f);
        bool started4a = client.requestAnalysis(dummyAudio, 48000, 2, 48000, "tempo", "file");
        check(started4a, "scenario 4: request 1 accepted");

        BridgeAnalysisResult result4a;
        bool got4a = false;
        for (int i = 0; i < 130; ++i) // up to 13s -- past the 10s client-side deadline
        {
            if (client.takeResult(result4a)) { got4a = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(got4a && !result4a.success && result4a.errorCode == "TIMEOUT", "scenario 4: request 1 resolves as a client-side TIMEOUT");

        // Give the mock's delayed stale response time to actually land in
        // the pipe before request 2 goes out, so request 2's read loop is
        // guaranteed to see (and must correctly discard) it first.
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        bool started4b = client.requestAnalysis(dummyAudio, 48000, 2, 48000, "tempo", "file");
        check(started4b, "scenario 4: request 2 accepted");

        BridgeAnalysisResult result4b;
        bool got4b = false;
        for (int i = 0; i < 100; ++i) // up to 10s
        {
            if (client.takeResult(result4b)) { got4b = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "[test] request 2 result: success=" << (got4b && result4b.success ? "true" : "false")
                  << " numericValue=" << (got4b ? result4b.numericValue : -1) << std::endl;
        check(got4b && result4b.success, "scenario 4: request 2 succeeds");
        check(got4b && result4b.success && result4b.numericValue > 100.0 && result4b.numericValue < 150.0,
              "scenario 4: request 2's result is its OWN fresh value (123.45), not the stale request 1 value (999) leaking through");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
