#pragma once
// Spawns `node analyze.js` and exposes its stdin/stdout as a deadline-bound,
// cancellable line interface via OverlappedPipeIO -- the same primitive
// already proven (native/protocol/OverlappedPipeIO.h and its isolated test)
// for the plugin<->desktop pipe.
//
// This does NOT use CreatePipe() (Win32 anonymous pipes), because anonymous
// pipes cannot be opened with FILE_FLAG_OVERLAPPED -- there is no way to get
// a cancellable, deadline-bound read on one. Instead this creates two
// PRIVATE NAMED pipes (unique per process id, so multiple desktop instances
// never collide), with FILE_FLAG_OVERLAPPED on the parent's own ends only;
// the child's ends are ordinary synchronous handles (Node has no idea its
// stdin/stdout are named pipes rather than anonymous ones -- from its side
// they behave identically).
//
// Before this, node.readLine() was a plain blocking ReadFile with no
// timeout of its own (see main.cpp's prior comments at the READY read and
// inside handleAnalysisRequest): a Node process that started but never
// printed READY, or one that went silent mid-request, hung the desktop
// process forever. That gap is why this conversion isn't optional cleanup --
// it's the same class of bug already fixed on the plugin-facing pipe.
#include <windows.h>
#include <string>
#include <memory>
#include <vector>
#include "../../protocol/OverlappedPipeIO.h"

class NodeEngine
{
public:
    // nodeExe: path to node.exe. scriptPath: path to analyze.js.
    bool start(const std::string& nodeExe, const std::string& scriptPath)
    {
        const DWORD pid = GetCurrentProcessId();
        const std::string stdinPipeName = "\\\\.\\pipe\\BeatShoreNodeStdin." + std::to_string(pid);
        const std::string stdoutPipeName = "\\\\.\\pipe\\BeatShoreNodeStdout." + std::to_string(pid);

        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        // Parent's write end of Node's stdin -- overlapped, so writeLine()
        // below gets a real deadline instead of blocking forever against a
        // Node process that's stopped reading.
        HANDLE stdinServer = CreateNamedPipeA(stdinPipeName.c_str(),
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (stdinServer == INVALID_HANDLE_VALUE) return false;

        // Parent's read end of Node's stdout -- overlapped, same reason.
        HANDLE stdoutServer = CreateNamedPipeA(stdoutPipeName.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (stdoutServer == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); return false; }

        // Child's ends: opened here by the parent (inheritable), before the
        // child process exists -- CreateProcessA below hands them down via
        // STARTUPINFO. Plain CreateFileA, no FILE_FLAG_OVERLAPPED: Node does
        // ordinary synchronous stdio reads/writes on these, unaware they're
        // named pipes rather than anonymous ones.
        HANDLE childStdinRead = CreateFileA(stdinPipeName.c_str(), GENERIC_READ, 0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childStdinRead == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); CloseHandle(stdoutServer); return false; }
        HANDLE childStdoutWrite = CreateFileA(stdoutPipeName.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childStdoutWrite == INVALID_HANDLE_VALUE) { CloseHandle(stdinServer); CloseHandle(stdoutServer); CloseHandle(childStdinRead); return false; }

        // Complete the connection on both server ends now that the client
        // (this same process, on the child's behalf) has already opened
        // them -- mirrors the plugin-facing pipe's ConnectNamedPipe pattern
        // (main.cpp): ERROR_PIPE_CONNECTED here just means "already
        // connected," not an error.
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
        std::string cmdLine = "\"" + nodeExe + "\" \"" + scriptPath + "\"";
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
    // itself: a Node process that's stopped producing output entirely now
    // surfaces as Timeout at this deadline instead of hanging the caller.
    OverlappedPipeIO::ReadResult readLine(DWORD deadlineMs, std::string& outLine)
    {
        if (!reader) return OverlappedPipeIO::ReadResult::Closed;
        return reader->readLine(deadlineMs, outLine);
    }

    // Pass-throughs to the reader's multiplexable read interface (see
    // OverlappedPipeIO.h) -- lets a worker thread wait on Node's next line
    // of output ALONGSIDE a "this job was just cancelled" signal, instead
    // of being blocked unresponsively inside a single readLine() call for
    // however long the kind-aware deadline allows. Don't mix these with
    // readLine() calls on the same NodeEngine instance (same caveat as
    // OverlappedPipeIO itself).
    HANDLE beginRead() { return reader ? reader->beginRead() : nullptr; }
    OverlappedPipeIO::PollResult pollRead(std::string& outLine) { return reader ? reader->pollRead(outLine) : OverlappedPipeIO::PollResult::Closed; }
    void cancelPendingRead() { if (reader) reader->cancelPendingRead(); }

    bool isRunning() const
    {
        if (!processHandle) return false;
        DWORD code = 0;
        return GetExitCodeProcess(processHandle, &code) && code == STILL_ACTIVE;
    }

    ~NodeEngine()
    {
        // Drop the I/O wrappers (and their handles, via ~OverlappedPipeIO /
        // the raw HANDLEs it owns are actually owned by `writer`/`reader`'s
        // constructor argument -- CloseHandle isn't called by
        // OverlappedPipeIO itself, so do it explicitly here) before
        // terminating the process, same ordering rationale as BridgeClient's
        // destructor: don't leave a wrapper pointing at a handle that's
        // about to be invalidated out from under a read that hasn't
        // returned yet.
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

    HANDLE processHandle = nullptr;
    std::unique_ptr<OverlappedPipeIO> writer;
    std::unique_ptr<OverlappedPipeIO> reader;
};
