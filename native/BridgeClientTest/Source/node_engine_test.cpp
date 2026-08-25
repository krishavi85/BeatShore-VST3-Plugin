// Deterministic tests for the two remaining Node-facing scenarios: drives
// the REAL NodeEngine class (native/BeatShoreDesktop/Source/NodeEngine.h,
// unmodified) against MockNodeProcess.exe standing in for node.exe, so
// "Node hangs before READY" and "Node goes silent mid-request" are
// guaranteed reproductions, not hopeful ones. Complements
// io_hardening_test.cpp, which covers the plugin<->desktop pipe;
// NodeEngine owns the desktop<->Node pipe, a separate class with its own
// deadline logic that deserves its own direct test rather than only being
// exercised indirectly through main.cpp (which isn't a class and can't be
// driven from a test the way BridgeClient and NodeEngine can).
#ifndef MOCK_NODE_EXE_PATH
#error "MOCK_NODE_EXE_PATH must be defined by CMake to MockNodeProcess.exe's build output path"
#endif

#include "../../BeatShoreDesktop/Source/NodeEngine.h"
#include <iostream>
#include <chrono>

static int failures = 0;
static void check(bool cond, const std::string& label)
{
    std::cout << "[test] " << label << ": " << (cond ? "PASS" : "FAIL") << std::endl;
    if (!cond) failures++;
}

int main()
{
    // --- Scenario: "Node starts but never writes READY" ---
    // The process launches successfully (isRunning() is true throughout --
    // this isn't "node.exe failed to start"), but never produces a single
    // line of output. Before the NodeEngine/main.cpp overlapped-I/O
    // conversion, the equivalent real readLine() call here had no timeout
    // parameter at all and would have blocked forever.
    {
        std::cout << "\n=== Scenario: node starts, never writes anything ===" << std::endl;
        NodeEngine node;
        bool started = node.start(MOCK_NODE_EXE_PATH, "silent");
        check(started, "mock node process started");
        check(node.isRunning(), "mock node process is running (this is not a start failure)");

        std::string line;
        const auto t0 = std::chrono::steady_clock::now();
        auto result = node.readLine(3000, line);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] readLine result=" << int(result) << " elapsedMs=" << elapsedMs << std::endl;
        check(result == OverlappedPipeIO::ReadResult::Timeout, "readLine times out rather than hanging");
        check(elapsedMs >= 2900 && elapsedMs < 4000, "timeout fires at the requested deadline, not early or late");
        // node destructs here -- also exercises tearing down a NodeEngine
        // whose child process is still alive and blocked on its own stdin
        // read (TerminateProcess in ~NodeEngine must actually kill it, not
        // hang waiting for it to exit on its own).
    }

    // --- Scenario: "Node writes progress and then becomes silent" ---
    // READY and one ANALYSIS_PROGRESS-shaped line arrive normally, then
    // nothing further -- no ANALYSIS_RESULT, no MIDI_RESULT, no ERROR. This
    // is the exact shape of main.cpp's handleAnalysisRequest read loop: a
    // caller reads READY (or here, just the first line) successfully, then
    // must not hang on the *next* read once Node stops producing output
    // mid-stream.
    {
        std::cout << "\n=== Scenario: node writes progress, then goes silent ===" << std::endl;
        NodeEngine node;
        bool started = node.start(MOCK_NODE_EXE_PATH, "progress-then-silent");
        check(started, "mock node process started");

        std::string readyLine;
        auto readyResult = node.readLine(3000, readyLine);
        check(readyResult == OverlappedPipeIO::ReadResult::Ok, "first line (READY) arrives normally");

        std::string progressLine;
        auto progressResult = node.readLine(3000, progressLine);
        check(progressResult == OverlappedPipeIO::ReadResult::Ok, "second line (ANALYSIS_PROGRESS) arrives normally");

        std::string thirdLine;
        const auto t0 = std::chrono::steady_clock::now();
        auto thirdResult = node.readLine(3000, thirdLine);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[test] third readLine result=" << int(thirdResult) << " elapsedMs=" << elapsedMs << std::endl;
        check(thirdResult == OverlappedPipeIO::ReadResult::Timeout, "read for the (never-sent) terminal message times out rather than hanging");
        check(elapsedMs >= 2900 && elapsedMs < 4000, "timeout fires at the requested deadline, not early or late");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
