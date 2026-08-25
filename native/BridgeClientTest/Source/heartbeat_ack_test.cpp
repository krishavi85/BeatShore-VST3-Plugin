// Deterministic tests for BridgeClient.h's sendHeartbeat() -- specifically
// that it requires a real, matched HEARTBEAT_ACK (type + heartbeatId +
// protocolVersion) rather than accepting any line as an acknowledgement.
// The old behavior let an unrelated message (a late analysis result, in
// particular) get silently consumed as a heartbeat ack instead of being
// routed or discarded on its own terms; see STATUS.md's "Overlapped I/O
// hardening" section and PROTOCOL.md's HEARTBEAT_ACK section for the full
// story. Drives the REAL BridgeClient class against a MockServer that
// completes HELLO/CAPABILITIES normally, then scripts exactly what happens
// when the client's next idle-loop HEARTBEAT arrives.
#include "../../BeatShoreBridge/Source/BridgeClient.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>

static const char* PIPE_NAME = "\\\\.\\pipe\\BeatShoreBridge.v1";

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
            if (h == INVALID_HANDLE_VALUE) { std::cerr << "[mock] CreateNamedPipeA failed\n"; return; }

            OVERLAPPED ov{};
            ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = ConnectNamedPipe(h, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(ov.hEvent, 5000);
            CloseHandle(ov.hEvent);
            connected.store(true);

            OverlappedPipeIO io(h);
            std::string line;
            if (io.readLine(5000, line) == OverlappedPipeIO::ReadResult::Ok) // HELLO
            {
                bsjson::Value caps = bsjson::Value::object();
                caps.set("type", "CAPABILITIES");
                caps.set("desktopVersion", "mock");
                bsjson::Value analysis = bsjson::Value::array();
                analysis.push(bsjson::Value("tempo"));
                caps.set("analysis", analysis);
                io.writeLine(bsjson::stringify(caps));
                behavior(io);
            }
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

// Waits for the client's next HEARTBEAT and returns its heartbeatId, or ""
// if the pipe closed/errored first.
static std::string awaitHeartbeat(OverlappedPipeIO& io, DWORD deadlineMs = 10000)
{
    std::string line;
    if (io.readLine(deadlineMs, line) != OverlappedPipeIO::ReadResult::Ok) return "";
    try
    {
        bsjson::Value msg = bsjson::parse(line);
        if (msg["type"].asString() != "HEARTBEAT") return "";
        return msg.has("heartbeatId") ? msg["heartbeatId"].asString() : "";
    }
    catch (const std::exception&) { return ""; }
}

static void writeValidAck(OverlappedPipeIO& io, const std::string& heartbeatId)
{
    bsjson::Value ack = bsjson::Value::object();
    ack.set("type", "HEARTBEAT_ACK");
    ack.set("heartbeatId", heartbeatId);
    ack.set("protocolVersion", 1);
    io.writeLine(bsjson::stringify(ack));
}

// Connects a real BridgeClient and waits for it to reach Connected, up to
// 10s. Returns the client, still owned by the caller.
static bool waitConnected(BridgeClient& client)
{
    for (int i = 0; i < 100; ++i)
    {
        if (client.getStatus() == BridgeStatus::Connected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Polls status for up to maxMs, returning true if it observes Connected
// continuously (never drops to Disconnected) for the whole window --
// i.e. the heartbeat(s) during that window all succeeded.
static bool staysConnectedFor(BridgeClient& client, int maxMs)
{
    for (int elapsed = 0; elapsed < maxMs; elapsed += 100)
    {
        if (client.getStatus() != BridgeStatus::Connected) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

// Polls status for up to maxMs, returning true if it observes Disconnected
// at some point -- i.e. the heartbeat failed and the client gave up.
static bool becomesDisconnectedWithin(BridgeClient& client, int maxMs)
{
    for (int elapsed = 0; elapsed < maxMs; elapsed += 100)
    {
        if (client.getStatus() == BridgeStatus::Disconnected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int main()
{
    // --- 1: Late MIDI_RESULT arrives while heartbeat is waiting ---
    // A stray MIDI_RESULT (for a request nothing asked for) must not be
    // mistaken for the ack; the real ack that follows must still be
    // accepted.
    {
        std::cout << "\n=== 1: late MIDI_RESULT while heartbeat is waiting ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            bsjson::Value stray = bsjson::Value::object();
            stray.set("type", "MIDI_RESULT");
            stray.set("requestId", "some-old-abandoned-request");
            stray.set("noteCount", 42);
            io.writeLine(bsjson::stringify(stray));
            writeValidAck(io, hbId);
            // Keep the connection open past the test's own observation
            // window so a clean pipe-close doesn't get mistaken for a
            // heartbeat failure.
            std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "1: client reached Connected");
        check(staysConnectedFor(client, 7000), "1: client stays Connected -- stray MIDI_RESULT did not break the heartbeat ack");
    }

    // --- 2: Heartbeat acknowledgement arrives after its deadline ---
    // A technically-valid ack that arrives too late (client's own 5s
    // deadline already passed) must not rescue the heartbeat -- the client
    // must disconnect on schedule, not hang waiting past its own bound.
    {
        std::cout << "\n=== 2: valid ack arrives after the 5s deadline ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5500)); // past the client's 5s deadline
            writeValidAck(io, hbId); // technically correct, but too late to matter
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "2: client reached Connected");
        // Timeline from here: the client's own idle loop doesn't send a
        // heartbeat until ~5s after connecting, then that heartbeat itself
        // has a further 5s ack deadline -- so disconnection isn't expected
        // until roughly t=10s, not immediately. Window generous above that,
        // not tight, since the point is "bounded," not "fast."
        const auto t0 = std::chrono::steady_clock::now();
        bool disconnected = becomesDisconnectedWithin(client, 14000);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] disconnected after " << elapsedMs << "ms" << std::endl;
        check(disconnected, "2: client disconnects on schedule rather than waiting indefinitely for the late ack");
        check(elapsedMs < 14000, "2: disconnect happens within a bounded window (not stuck)");
    }

    // --- 3: Wrong heartbeat ID arrives ---
    // An ack with a mismatched heartbeatId must not satisfy the wait; the
    // correct ack that follows must still be accepted.
    {
        std::cout << "\n=== 3: ack with the wrong heartbeatId ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            writeValidAck(io, "not-the-real-heartbeat-id-" + hbId); // guaranteed mismatch
            writeValidAck(io, hbId); // the real one
            std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "3: client reached Connected");
        check(staysConnectedFor(client, 7000), "3: client stays Connected -- mismatched heartbeatId was rejected, correct one accepted");
    }

    // --- 4: Malformed JSON arrives ---
    // A garbage, non-JSON line must not crash the parser or be mistaken for
    // an ack; the correct ack that follows must still be accepted.
    {
        std::cout << "\n=== 4: malformed JSON before the real ack ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            io.writeLine("{this is not valid json at all!!");
            writeValidAck(io, hbId);
            std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "4: client reached Connected");
        check(staysConnectedFor(client, 7000), "4: client stays Connected -- malformed JSON was discarded, not crashed on or mistaken for an ack");
    }

    // --- 5: ERROR arrives instead of HEARTBEAT_ACK ---
    // A well-formed ERROR message must not be mistaken for an ack either;
    // the correct ack that follows must still be accepted.
    {
        std::cout << "\n=== 5: ERROR message instead of HEARTBEAT_ACK ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            bsjson::Value err = bsjson::Value::object();
            err.set("type", "ERROR");
            err.set("requestId", "unrelated");
            err.set("errorCode", "SOMETHING_ELSE");
            err.set("message", "not a heartbeat ack");
            io.writeLine(bsjson::stringify(err));
            writeValidAck(io, hbId);
            std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "5: client reached Connected");
        check(staysConnectedFor(client, 7000), "5: client stays Connected -- ERROR message was discarded, not mistaken for an ack");
    }

    // --- 6: A valid acknowledgement follows an unrelated stale result ---
    // Same shape as #1, but with a plain ANALYSIS_RESULT (a different
    // terminal message type) rather than MIDI_RESULT, covering the other
    // result-shaped message the discard logic must also reject.
    {
        std::cout << "\n=== 6: valid ack follows an unrelated stale ANALYSIS_RESULT ===" << std::endl;
        MockServer mock;
        mock.start([](OverlappedPipeIO& io){
            std::string hbId = awaitHeartbeat(io);
            if (hbId.empty()) return;
            bsjson::Value stale = bsjson::Value::object();
            stale.set("type", "ANALYSIS_RESULT");
            stale.set("requestId", "some-other-old-request");
            stale.set("result", bsjson::Value(999.0));
            io.writeLine(bsjson::stringify(stale));
            writeValidAck(io, hbId);
            std::this_thread::sleep_for(std::chrono::milliseconds(7000));
        });

        BridgeClient client;
        client.start();
        check(waitConnected(client), "6: client reached Connected");
        check(staysConnectedFor(client, 7000), "6: client stays Connected -- stale ANALYSIS_RESULT discarded, real ack that followed it accepted");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
