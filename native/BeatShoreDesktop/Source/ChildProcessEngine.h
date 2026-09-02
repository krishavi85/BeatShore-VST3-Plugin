#pragma once
// Generalized from NodeEngine.h's own mechanism (which turned out to
// already be process-agnostic in everything but name -- start() just took
// an exe path and a script path and built "<exe>" "<script>") into a real,
// reusable base: spawns ANY child process and exposes its stdin/stdout as
// a deadline-bound, cancellable line interface via OverlappedPipeIO. This
// exists specifically so a future ML backend (DAC/EnCodec/MT3/etc, see
// the "Real Trained ML Model Stack" scoping in STATUS.md) has a proven
// child-process/IPC mechanism to build on rather than inventing IPC from
// scratch a second time -- Node already validated this approach in
// production (real cancellation, real crash/respawn handling in main.cpp).
//
// NodeEngine.h now just wraps this with its original two-argument
// start(exe, script) signature and a fixed "Node" tag -- unchanged
// behavior, unchanged public API, zero edits needed to main.cpp or the
// existing NodeEngineTest. See that file's own (now much shorter) comment.
//
// A REAL bug found and fixed while doing this generalization, not
// invented for it: the original pipe names were
// `\\.\pipe\BeatShoreNodeStdin.<pid>` / `...Stdout.<pid>` -- unique per
// PROCESS, not per ENGINE INSTANCE. main.cpp's own runNodeWorker() already
// creates one NodeEngine per worker thread, all inside the same process,
// all sharing the same PID -- meaning a second concurrent worker's
// CreateNamedPipeA (which uses FILE_FLAG_FIRST_PIPE_INSTANCE, so a name
// collision fails outright rather than silently sharing the pipe) would
// have failed to start at all. Latent today if only one worker is
// actually configured; would become live and immediately visible the
// moment a second Node worker OR a Python engine runs alongside it. Fixed
// here by keying pipe names on PID + tag + a process-wide atomic instance
// counter, guaranteeing uniqueness regardless of how many engines (of
// whatever kind) end up running concurrently.
//
// This does NOT use CreatePipe() (Win32 anonymous pipes), because anonymous
// pipes cannot be opened with FILE_FLAG_OVERLAPPED -- there is no way to get
// a cancellable, deadline-bound read on one. Instead this creates two
// PRIVATE NAMED pipes, with FILE_FLAG_OVERLAPPED on the parent's own ends
// only; the child's ends are ordinary synchronous handles (the child has
// no idea its stdin/stdout are named pipes rather than anonymous ones --
// from its side they behave identically).
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "../../protocol/OverlappedPipeIO.h"

class ChildProcessEngine
{
public:
    // exePath: the interpreter/executable to run (node.exe, python.exe, a
    // frozen model-server .exe, ...). args: passed through verbatim, in
    // order (e.g. {"-u", "engine.py"} for Python -- see the header comment
    // on PythonEngine's own callers for why "-u"/unbuffered mode matters
    // for a child whose stdout is a pipe, not a terminal). tag: a short,
    // human-readable identifier (e.g. "Node", "Python") folded into the
    // pipe name purely for uniqueness and log-readability -- has no
    // protocol meaning.
    bool start(const std::string& exePath, const std::vector<std::string>& args, const std::string& tag)
    {
        const DWORD pid = GetCurrentProcessId();
        const unsigned instance = nextInstanceId.fetch_add(1, std::memory_order_relaxed);
        const std::string pipeBase = "\\\\.\\pipe\\BeatShoreChild." + tag + "." + std::to_string(pid) + "." + std::to_string(instance);
        const std::string stdinPipeName = pipeBase + ".stdin";
        const std::string stdoutPipeName = pipeBase + ".stdout";

        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        // Parent's write end of the child's stdin -- overlapped, so
        // writeLine() below gets a real deadline instead of blocking
        // forever against a child that's stopped reading.
        HANDLE stdinServer = CreateNamedPipeA(stdinPipeName.c_str(),
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (stdinServer == INVALID_HANDLE_VALUE) return false;

        // Parent's read end of the child's stdout -- overlapped, same reason.
        HANDLE stdoutServer = CreateNamedPipeA(stdoutPipeName.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (stdoutServer == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); return false; }

        // Child's ends: opened here by the parent (inheritable), before the
        // child process exists -- CreateProcessA below hands them down via
        // STARTUPINFO. Plain CreateFileA, no FILE_FLAG_OVERLAPPED: the
        // child does ordinary synchronous stdio reads/writes on these,
        // unaware they're named pipes rather than anonymous ones.
        HANDLE childStdinRead = CreateFileA(stdinPipeName.c_str(), GENERIC_READ, 0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childStdinRead == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); CloseHandle(stdoutServer); return false; }
        HANDLE childStdoutWrite = CreateFileA(stdoutPipeName.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childStdoutWrite == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); CloseHandle(stdoutServer); CloseHandle(childStdinRead); return false; }

        // Complete the connection on both server ends now that the client
        // (this same process, on the child's behalf) has already opened
        // them -- ERROR_PIPE_CONNECTED here just means "already connected,"
        // not an error.
        if (!completeConnect(stdinServer) || !completeConnect(stdoutServer))
        {
            CloseHandle(stdinServer); CloseHandle(stdoutServer);
            CloseHandle(childStdinRead); CloseHandle(childStdoutWrite);
            return false;
        }

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childStdinRead;
        si.hStdOutput = childStdoutWrite;
        si.hStdError = childStdoutWrite; // merged with stdout, same as before

        PROCESS_INFORMATION pi{};
        std::string cmdLine = "\"" + exePath + "\"";
        for (const auto& a : args) cmdLine += " \"" + a + "\"";
        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        // The child inherited its own copies of these; the parent's copies
        // must close regardless of success so nothing is left holding the
        // pipe open on the child's behalf.
        CloseHandle(childStdinRead);
        CloseHandle(childStdoutWrite);
        if (!ok)
        {
            CloseHandle(stdinServer);
            CloseHandle(stdoutServer);
            return false;
        }
        processHandle = pi.hProcess;
        CloseHandle(pi.hThread);
        writer = std::make_unique<OverlappedPipeIO>(stdinServer);
        reader = std::make_unique<OverlappedPipeIO>(stdoutServer);
        return true;
    }

    bool writeLine(const std::string& line, DWORD timeoutMs = 5000) { return writer && writer->writeLine(line, timeoutMs); }

    // deadlineMs bounds this single call, same contract as OverlappedPipeIO
    // itself: a child that's stopped producing output entirely now surfaces
    // as Timeout at this deadline instead of hanging the caller.
    OverlappedPipeIO::ReadResult readLine(DWORD deadlineMs, std::string& outLine)
    {
        if (!reader) return OverlappedPipeIO::ReadResult::Closed;
        return reader->readLine(deadlineMs, outLine);
    }

    // Pass-throughs to the reader's multiplexable read interface (see
    // OverlappedPipeIO.h) -- lets a worker thread wait on the child's next
    // line of output ALONGSIDE a "this job was just cancelled" signal.
    // Don't mix these with readLine() calls on the same instance.
    HANDLE beginRead() { return reader ? reader->beginRead() : nullptr; }
    OverlappedPipeIO::PollResult pollRead(std::string& outLine) { return reader ? reader->pollRead(outLine) : OverlappedPipeIO::PollResult::Closed; }
    void cancelPendingRead() { if (reader) reader->cancelPendingRead(); }

    bool isRunning() const
    {
        if (!processHandle) return false;
        DWORD code = 0;
        return GetExitCodeProcess(processHandle, &code) && code == STILL_ACTIVE;
    }

    ~ChildProcessEngine()
    {
        // Drop the I/O wrappers' handles before terminating the process --
        // don't leave a wrapper pointing at a handle that's about to be
        // invalidated out from under a read that hasn't returned yet.
        if (writer) CloseHandle(writer->raw());
        if (reader) CloseHandle(reader->raw());
        if (processHandle)
        {
            TerminateProcess(processHandle, 0);
            CloseHandle(processHandle);
        }
    }

private:
    static bool completeConnect(HANDLE h)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(h, &ov);
        bool result = true;
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                DWORD waitResult = WaitForSingleObject(ov.hEvent, 5000);
                result = (waitResult == WAIT_OBJECT_0);
            }
            else if (err != ERROR_PIPE_CONNECTED)
            {
                result = false;
            }
        }
        CloseHandle(ov.hEvent);
        return result;
    }

    static std::atomic<unsigned> nextInstanceId;

    HANDLE processHandle = nullptr;
    std::unique_ptr<OverlappedPipeIO> writer;
    std::unique_ptr<OverlappedPipeIO> reader;
};

inline std::atomic<unsigned> ChildProcessEngine::nextInstanceId{ 0 };
