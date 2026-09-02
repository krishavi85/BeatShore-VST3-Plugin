// Direct proof that PythonEngine.h/ChildProcessEngine.h genuinely work
// through a REAL python.exe, not just MockNodeProcess.exe (which proves
// the pipe mechanism works for an arbitrary child process, but says
// nothing about Python's own stdio behavior). This is the actual reason
// this test exists: Python fully buffers stdout when it isn't attached to
// a terminal, which would make a naive integration hang/timeout on every
// read despite the child process itself working perfectly -- see
// PythonEngine.h's own header comment. The scenarios below are designed
// specifically to catch that class of bug, not just prove happy-path
// request/response.
//
// Requires a real "python" on PATH (confirmed present in this dev
// environment). A production build would point PythonEngine at a bundled/
// embeddable interpreter, the same way NodeEngine is pointed at a bundled
// node.exe -- not exercised here, since packaging is a separate, later
// step from proving the IPC mechanism itself.
#ifndef ECHO_ENGINE_SCRIPT_PATH
#error "ECHO_ENGINE_SCRIPT_PATH must be defined by CMake to echo_engine.py's path"
#endif

#include "../../BeatShoreDesktop/Source/PythonEngine.h"
#include "../../BeatShoreDesktop/Source/ChildProcessEngine.h"
#include <iostream>
#include <chrono>
#include <sstream>

namespace
{
    int failures = 0;
    void check(bool condition, const std::string& label)
    {
        std::cout << "[test] " << label << ": " << (condition ? "PASS" : "FAIL") << std::endl;
        if (!condition) ++failures;
    }

    // Very small, deliberately permissive JSON string-field extractor --
    // this test only ever needs to pull a "type" (and sometimes "payload")
    // value out of a known-shape line, not parse arbitrary JSON, so a full
    // parser would be more machinery than the job needs. Tolerates an
    // optional space after the colon since Python's json.dumps() inserts
    // one by default ({"type": "READY"}) while the C++ side's own literal
    // request strings don't ({"type":"PING",...}) -- found by actually
    // running this against real python.exe output, not assumed.
    std::string extractStringField(const std::string& json, const std::string& key)
    {
        for (const std::string& needle : { "\"" + key + "\":\"", "\"" + key + "\": \"" })
        {
            auto pos = json.find(needle);
            if (pos == std::string::npos) continue;
            pos += needle.size();
            auto end = json.find('"', pos);
            if (end == std::string::npos) continue;
            return json.substr(pos, end - pos);
        }
        return {};
    }
}

int main()
{
    // --- Scenario 1: READY, then a real PING/PONG round trip ---
    // If Python's stdout buffering weren't correctly disabled (-u), this
    // would time out at the readLine() calls below -- the child process
    // would be alive and would have genuinely called print(), but nothing
    // would have actually reached this process's pipe read yet.
    {
        std::cout << "\n=== Scenario: READY handshake + PING/PONG round trip through a real python.exe ===" << std::endl;
        PythonEngine python;
        const bool started = python.start("python", ECHO_ENGINE_SCRIPT_PATH);
        check(started, "python.exe (via PATH) started");
        check(python.isRunning(), "python process is running");

        std::string readyLine;
        const auto readyResult = python.readLine(10000, readyLine);
        std::cout << "[test] READY line: " << readyLine << std::endl;
        check(readyResult == OverlappedPipeIO::ReadResult::Ok, "READY line arrives within 10s (would time out here if stdout buffering weren't disabled)");
        check(extractStringField(readyLine, "type") == "READY", "READY line has type=READY");

        const bool wrote = python.writeLine(R"({"type":"PING","payload":"hello-from-cpp"})");
        check(wrote, "PING message written to python's stdin");

        std::string pongLine;
        const auto pongResult = python.readLine(10000, pongLine);
        std::cout << "[test] PONG line: " << pongLine << std::endl;
        check(pongResult == OverlappedPipeIO::ReadResult::Ok, "PONG line arrives within 10s");
        check(extractStringField(pongLine, "type") == "PONG", "response has type=PONG");
        check(pongLine.find("hello-from-cpp") != std::string::npos, "PONG echoes back the exact payload sent");

        const bool wroteShutdown = python.writeLine(R"({"type":"SHUTDOWN"})");
        check(wroteShutdown, "SHUTDOWN message written");
    }

    // --- Scenario 2: multiple concurrent engines don't collide ---
    // The real bug found and fixed while generalizing NodeEngine.h into
    // ChildProcessEngine.h: pipe names keyed only on PID (not per-instance)
    // would make a second concurrent engine in the same process fail to
    // start at all. Runs a Python engine and confirms it starts and
    // completes a round trip successfully AT THE SAME TIME as a second one
    // -- exactly the scenario (multiple engines, one process) that was
    // broken before this generalization.
    {
        std::cout << "\n=== Scenario: two concurrent engines in the same process don't collide ===" << std::endl;
        PythonEngine pythonA;
        PythonEngine pythonB;
        const bool startedA = pythonA.start("python", ECHO_ENGINE_SCRIPT_PATH);
        const bool startedB = pythonB.start("python", ECHO_ENGINE_SCRIPT_PATH);
        check(startedA, "first concurrent engine started");
        check(startedB, "second concurrent engine started (would have failed to create its pipe under the old per-PID-only naming)");

        std::string readyA, readyB;
        check(pythonA.readLine(10000, readyA) == OverlappedPipeIO::ReadResult::Ok, "first engine's READY arrives");
        check(pythonB.readLine(10000, readyB) == OverlappedPipeIO::ReadResult::Ok, "second engine's READY arrives");

        pythonA.writeLine(R"({"type":"PING","payload":"engine-A"})");
        pythonB.writeLine(R"({"type":"PING","payload":"engine-B"})");

        std::string pongA, pongB;
        check(pythonA.readLine(10000, pongA) == OverlappedPipeIO::ReadResult::Ok, "first engine's PONG arrives");
        check(pythonB.readLine(10000, pongB) == OverlappedPipeIO::ReadResult::Ok, "second engine's PONG arrives");
        check(pongA.find("engine-A") != std::string::npos, "first engine's PONG carries its OWN payload, not the other engine's");
        check(pongB.find("engine-B") != std::string::npos, "second engine's PONG carries its OWN payload, not the other engine's");

        pythonA.writeLine(R"({"type":"SHUTDOWN"})");
        pythonB.writeLine(R"({"type":"SHUTDOWN"})");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
