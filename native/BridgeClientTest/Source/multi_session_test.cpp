// Integration tests for BeatShoreDesktop's multi-session architecture --
// drives TWO real, unmodified BridgeClient instances against a REAL,
// already-running BeatShoreDesktop.exe (not a mock; see main.cpp for the
// single-client equivalent). Requires a real audio file (bsmraw), same as
// main.cpp, so it can actually exercise transcribePolyphonic's real Node/
// tfjs-node work -- this is what makes "cancel a request that's genuinely
// running" a real reproduction rather than a guess.
//
// usage: MultiSessionTest <bsmraw-audio-path>
#include <juce_core/juce_core.h>
#include "../../BeatShoreBridge/Source/BridgeClient.h"
#include <fstream>
#include <iostream>
#include <cstring>

static bool loadBsmRaw(const std::string& path, uint32_t& sr, uint32_t& ch, uint32_t& frames, std::vector<float>& interleaved)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4];
    in.read(magic, 4);
    if (std::memcmp(magic, "BSM1", 4) != 0) return false;
    in.read(reinterpret_cast<char*>(&sr), 4);
    in.read(reinterpret_cast<char*>(&ch), 4);
    in.read(reinterpret_cast<char*>(&frames), 4);
    interleaved.resize(size_t(frames) * ch);
    in.read(reinterpret_cast<char*>(interleaved.data()), std::streamsize(interleaved.size() * sizeof(float)));
    return bool(in) || in.eof();
}

static int failures = 0;
static void check(bool cond, const std::string& label)
{
    std::cout << "[test] " << label << ": " << (cond ? "PASS" : "FAIL") << std::endl;
    if (!cond) failures++;
}

static bool waitConnected(BridgeClient& c, int maxMs = 10000)
{
    for (int i = 0; i < maxMs / 100; ++i)
    {
        if (c.getStatus() == BridgeStatus::Connected) return true;
        juce::Thread::sleep(100);
    }
    return false;
}

static bool waitResult(BridgeClient& c, BridgeAnalysisResult& out, int maxMs)
{
    for (int i = 0; i < maxMs / 50; ++i)
    {
        if (c.takeResult(out)) return true;
        juce::Thread::sleep(50);
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: MultiSessionTest <bsmraw-audio-path>" << std::endl;
        return 1;
    }

    uint32_t sr, ch, frames;
    std::vector<float> interleaved;
    if (!loadBsmRaw(argv[1], sr, ch, frames, interleaved))
    {
        std::cerr << "failed to load audio from " << argv[1] << std::endl;
        return 1;
    }
    std::cout << "[test] loaded audio: " << frames << " frames, " << ch << " ch, " << sr << " Hz" << std::endl;

    // --- Test 1: two simultaneous plugin instances, both connect and both
    // get their own, correctly-routed, non-cross-contaminated results. ---
    {
        std::cout << "\n=== Test 1: two simultaneous instances, independent results ===" << std::endl;
        BridgeClient a, b;
        a.start();
        b.start();
        check(waitConnected(a), "client A connects");
        check(waitConnected(b), "client B connects");

        bool startedA = a.requestAnalysis(interleaved, sr, ch, frames, "tempo", "file");
        bool startedB = b.requestAnalysis(interleaved, sr, ch, frames, "tempo", "file");
        check(startedA, "client A's request accepted");
        check(startedB, "client B's request accepted");

        BridgeAnalysisResult ra, rb;
        bool gotA = waitResult(a, ra, 15000);
        bool gotB = waitResult(b, rb, 15000);
        check(gotA && ra.success && ra.hasNumericValue, "client A gets its own successful tempo result");
        check(gotB && rb.success && rb.hasNumericValue, "client B gets its own successful tempo result");
        check(gotA && gotB && std::abs(ra.numericValue - rb.numericValue) < 0.01, "both results agree (same audio, same algorithm) -- no cross-session corruption");
    }

    // --- Test 2: cancel a request while it's still QUEUED (behind another
    // session's genuinely running job) -- must resolve near-instantly as
    // CANCELLED, not wait for anything. ---
    {
        std::cout << "\n=== Test 2: cancel a QUEUED job ===" << std::endl;
        BridgeClient a, b;
        a.start();
        b.start();
        check(waitConnected(a), "client A connects");
        check(waitConnected(b), "client B connects");

        bool startedA = a.requestAnalysis(interleaved, sr, ch, frames, "transcribePolyphonic", "file");
        check(startedA, "client A's transcribePolyphonic request accepted");

        // Wait for A's job to genuinely start running (progress > 0)
        // before starting B's, so B's is guaranteed to be enqueued behind
        // an actually-Running job, not racing to run concurrently (this
        // desktop's default worker pool size is 1 -- see
        // kMaxConcurrentNodeJobs in main.cpp).
        bool aStarted = false;
        for (int i = 0; i < 100 && !aStarted; ++i) { if (a.getProgress() > 0.0) aStarted = true; juce::Thread::sleep(50); }
        check(aStarted, "client A's job reaches genuine progress > 0 (confirmed Running, not just Queued)");

        bool startedB = b.requestAnalysis(interleaved, sr, ch, frames, "tempo", "file");
        check(startedB, "client B's request accepted while A is running (B should be enqueued)");

        const auto t0 = std::chrono::steady_clock::now();
        bool cancelSent = b.requestCancel();
        check(cancelSent, "client B's cancel is accepted (a request was genuinely in flight from B's perspective)");

        BridgeAnalysisResult rb;
        bool gotB = waitResult(b, rb, 10000);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] B's cancelled result arrived after " << elapsedMs << "ms, success=" << rb.success << " errorCode=" << rb.errorCode << std::endl;
        check(gotB && !rb.success && rb.errorCode == "CANCELLED", "client B's queued job resolves as CANCELLED");
        check(elapsedMs < 3000, "cancelling a QUEUED job resolves quickly, not bounded by A's job finishing");

        // A's original job must be completely unaffected by B's cancel.
        BridgeAnalysisResult ra;
        bool gotA = waitResult(a, ra, 60000);
        check(gotA && ra.success, "client A's own (unrelated) job still completes successfully, unaffected by B's cancel");
    }

    // --- Test 3: cancel a request that's genuinely RUNNING -- must get a
    // prompt CANCEL_REQUESTED-then-CANCELLED resolution (not wait out the
    // full kind timeout), and the desktop's node engine must recover
    // (kill+restart) well enough that a SUBSEQUENT request still works. ---
    {
        std::cout << "\n=== Test 3: cancel a RUNNING job, verify node engine recovery ===" << std::endl;
        BridgeClient a;
        a.start();
        check(waitConnected(a), "client A connects");

        bool started = a.requestAnalysis(interleaved, sr, ch, frames, "transcribePolyphonic", "file");
        check(started, "transcribePolyphonic request accepted");

        bool aStarted = false;
        for (int i = 0; i < 100 && !aStarted; ++i) { if (a.getProgress() > 0.0) aStarted = true; juce::Thread::sleep(50); }
        check(aStarted, "job reaches genuine progress > 0 (confirmed Running)");

        const auto t0 = std::chrono::steady_clock::now();
        bool cancelSent = a.requestCancel();
        check(cancelSent, "cancel is accepted for the genuinely-running job");

        BridgeAnalysisResult r;
        bool got = waitResult(a, r, 15000);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] cancelled-while-running result arrived after " << elapsedMs << "ms, success=" << r.success << " errorCode=" << r.errorCode << std::endl;
        check(got && !r.success && r.errorCode == "CANCELLED", "running job resolves as CANCELLED, not TIMEOUT or a stale success");
        check(elapsedMs < 15000, "cancelling a RUNNING job resolves well within the 60s transcribePolyphonic timeout -- genuine responsiveness, not a wait-out");

        // Give the desktop's worker a moment to finish restarting node
        // (kill + spawn + READY handshake) before hitting it again.
        juce::Thread::sleep(2000);

        bool started2 = a.requestAnalysis(interleaved, sr, ch, frames, "tempo", "file");
        check(started2, "a follow-up request is accepted after the cancel");
        BridgeAnalysisResult r2;
        bool got2 = waitResult(a, r2, 15000);
        check(got2 && r2.success && r2.hasNumericValue, "the follow-up request succeeds normally -- the node engine genuinely recovered from the hard-cancel restart, not left in a broken state");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
