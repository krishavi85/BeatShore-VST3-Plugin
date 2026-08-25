#pragma once
// Deadline-aware, cancellable named-pipe line I/O. Replaces PipeLineIO's
// plain synchronous ReadFile/WriteFile, whose only real limitation was
// exactly what it looks like: readLine() blocks until data arrives or the
// pipe closes, with no way to interrupt it except closing the handle out
// from under the blocked thread (a hack, not a documented-correct
// mechanism -- see BridgeClient.h's destructor history). This class uses
// real Windows overlapped I/O (FILE_FLAG_OVERLAPPED) so a read can have an
// actual deadline and, on timeout, be cancelled cleanly via CancelIoEx
// instead of abandoned or worked around.
//
// Requires the underlying HANDLE to have been created/opened with
// FILE_FLAG_OVERLAPPED (CreateNamedPipeA's dwOpenMode, or CreateFileA's
// dwFlagsAndAttributes) -- using this on a handle opened without that flag
// is undefined (ReadFile/WriteFile will likely just complete synchronously
// and ignore the OVERLAPPED struct's event, defeating the whole point).
//
// Single-threaded use per instance, same as PipeLineIO: one thread owns
// all reads and writes on a given handle. Overlapped I/O does not relax
// that constraint -- it changes how a *single* thread's blocking read can
// have a deadline, not who's allowed to touch the handle.
#include <windows.h>
#include <string>
#include <optional>

class OverlappedPipeIO
{
public:
    enum class ReadResult { Ok, Timeout, Closed, Error, LineTooLarge };

    // Bounds `buffer`'s growth against a peer (well-behaved or not) that
    // sends bytes indefinitely without ever completing a line -- without
    // this, readLine()'s deadline still eventually fires, but a
    // misbehaving/hostile peer could accumulate an arbitrarily large
    // buffer in the meantime (megabytes per second is easily achievable
    // well within even a 5s deadline). Actual audio never travels over
    // this line-oriented protocol at all (it goes through shared memory --
    // see SharedAudioBuffer.h); every real NDJSON control message this
    // protocol sends is a few KB at most (the largest observed in this
    // project's own testing, a transcribePolyphonic MIDI_RESULT with 91
    // notes, is well under that). 1MB is still generous headroom above
    // that -- not the 16MB this constant previously allowed, which had no
    // principled connection to what a legitimate message actually needs.
    // Both readLine() and pollRead() below treat LineTooLarge as a reason
    // to close the connection outright (not merely clear the buffer and
    // keep reading) -- see their call sites in main.cpp's runPipeSession,
    // which folds LineTooLarge into the same "!= Ok -> disconnect" path
    // every other read failure takes.
    static constexpr size_t kMaxLineBytes = 1 * 1024 * 1024;

    explicit OverlappedPipeIO(HANDLE h) : handle(h)
    {
        readEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        writeEventHandle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }

    ~OverlappedPipeIO()
    {
        if (readEvent) CloseHandle(readEvent);
        if (writeEventHandle) CloseHandle(writeEventHandle);
    }

    OverlappedPipeIO(const OverlappedPipeIO&) = delete;
    OverlappedPipeIO& operator=(const OverlappedPipeIO&) = delete;

    // Bounded, not unbounded: a write can't complete if the peer has
    // stopped reading and the pipe's internal buffer fills up (control
    // messages here are small and well under the 64KB buffers this
    // protocol configures, so this is a defensive bound, not the primary
    // risk the deadline on readLine() below addresses).
    bool writeLine(const std::string& line, DWORD timeoutMs = 5000)
    {
        std::string withNewline = line + "\n";
        OVERLAPPED ov{};
        ov.hEvent = writeEventHandle;
        ResetEvent(writeEventHandle);
        DWORD written = 0;
        BOOL ok = WriteFile(handle, withNewline.data(), DWORD(withNewline.size()), &written, &ov);
        if (!ok)
        {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            DWORD waitResult = WaitForSingleObject(writeEventHandle, timeoutMs);
            if (waitResult != WAIT_OBJECT_0)
            {
                CancelIoEx(handle, &ov);
                DWORD dummy = 0;
                GetOverlappedResult(handle, &ov, &dummy, TRUE); // wait for the cancel itself so `ov` isn't touched after we return
                return false;
            }
            if (!GetOverlappedResult(handle, &ov, &written, FALSE)) return false;
        }
        return written == withNewline.size();
    }

    // Blocks until a full line is available, the deadline passes, the pipe
    // closes, or an error occurs. On Timeout, the pending read is properly
    // cancelled (CancelIoEx) and waited out before returning -- never left
    // dangling. deadlineMs is a duration from *now* for this call, applied
    // across however many raw chunks it takes to see a newline (a slow
    // trickle of partial data doesn't get a fresh budget per chunk).
    ReadResult readLine(DWORD deadlineMs, std::string& outLine)
    {
        const ULONGLONG absoluteDeadline = GetTickCount64() + deadlineMs;
        for (;;)
        {
            size_t nl = buffer.find('\n');
            if (nl != std::string::npos)
            {
                outLine = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
                return ReadResult::Ok;
            }

            char chunk[4096];
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD readCount = 0;
            BOOL ok = ReadFile(handle, chunk, sizeof(chunk), &readCount, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                const ULONGLONG now = GetTickCount64();
                const DWORD waitMs = now >= absoluteDeadline ? 0 : DWORD(absoluteDeadline - now);
                DWORD waitResult = WaitForSingleObject(readEvent, waitMs);
                if (waitResult == WAIT_TIMEOUT)
                {
                    CancelIoEx(handle, &ov);
                    DWORD dummy = 0;
                    GetOverlappedResult(handle, &ov, &dummy, TRUE); // wait for the cancel to actually land before `ov` goes out of scope
                    return ReadResult::Timeout;
                }
                ok = GetOverlappedResult(handle, &ov, &readCount, FALSE);
            }
            if (!ok)
            {
                DWORD err = GetLastError();
                return (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED)
                    ? ReadResult::Closed : ReadResult::Error;
            }
            if (readCount == 0) return ReadResult::Closed;
            if (buffer.size() + readCount > kMaxLineBytes)
            {
                buffer.clear(); // don't keep accumulating a line no caller will ever get
                return ReadResult::LineTooLarge;
            }
            buffer.append(chunk, readCount);
        }
    }

    HANDLE raw() const { return handle; }

    // --- Multiplexable read interface -----------------------------------
    // readLine() above owns its own wait; these let a caller wait on this
    // pipe's next line *alongside other event sources* (e.g. "the scheduler
    // has an outgoing message for me") on a SINGLE thread, via its own
    // WaitForSingleObject/WaitForMultipleObjects including the HANDLE
    // beginRead() returns. This is what lets one thread own a pipe's reads
    // AND its (occasional, brief) writes without a second thread ever
    // touching the handle -- see BeatShoreDesktop's PipeSessionOwner, built
    // specifically because splitting reads and writes across two threads on
    // the SAME pipe handle was the empirically-found hang documented at the
    // top of main.cpp. This does not reintroduce that: every ReadFile and
    // WriteFile on a given handle still comes from exactly one thread,
    // this just lets that one thread avoid blocking indefinitely inside a
    // single readLine() call when it also has other work to notice.
    //
    // Contract: call beginRead() before waiting; after your wait returns
    // (for ANY reason -- this handle firing, a different one firing, or a
    // timeout), call pollRead(). It never blocks. It returns Pending if
    // this handle wasn't actually the reason your wait woke up (or the
    // line isn't complete yet -- keep the loop going, calling beginRead()
    // again on the next iteration is always safe, including while a read
    // is already pending, where it's a cheap no-op). Don't interleave this
    // pair with readLine() calls on the same instance -- pick one style per
    // OverlappedPipeIO object (they share the read buffer/event/OVERLAPPED
    // state, which assumes a single in-flight read model either way, not
    // both simultaneously).
    enum class PollResult { Pending, Ok, Closed, Error, LineTooLarge };

    HANDLE beginRead()
    {
        if (multiplexReadPending) return readEvent;

        size_t nl = buffer.find('\n');
        if (nl != std::string::npos)
        {
            // A full line is already sitting from a previous over-read --
            // no real I/O needed, just make the caller's wait fire
            // immediately so pollRead() can hand it over next.
            multiplexBufferedLineReady = true;
            multiplexReadPending = true;
            SetEvent(readEvent);
            return readEvent;
        }

        multiplexBufferedLineReady = false;
        multiplexImmediateOk = false;
        multiplexImmediateError = 0;
        ZeroMemory(&multiplexOv, sizeof(multiplexOv));
        multiplexOv.hEvent = readEvent;
        ResetEvent(readEvent);
        DWORD readCount = 0;
        BOOL ok = ReadFile(handle, multiplexChunk, sizeof(multiplexChunk), &readCount, &multiplexOv);
        if (ok)
        {
            // Completed synchronously -- readCount is already valid; the
            // event is not reliably self-signaled in this case (same
            // reasoning as readLine()'s ok==TRUE branch), so signal it
            // ourselves for the caller's wait.
            multiplexImmediateOk = true;
            multiplexImmediateCount = readCount;
            SetEvent(readEvent);
        }
        else if (GetLastError() != ERROR_IO_PENDING)
        {
            multiplexImmediateError = GetLastError();
            SetEvent(readEvent);
        }
        multiplexReadPending = true;
        return readEvent;
    }

    PollResult pollRead(std::string& outLine)
    {
        if (!multiplexReadPending) return PollResult::Pending;
        if (WaitForSingleObject(readEvent, 0) != WAIT_OBJECT_0) return PollResult::Pending; // this handle isn't why the wait woke up

        multiplexReadPending = false;

        if (multiplexBufferedLineReady)
        {
            outLine = takeLineFromBuffer();
            return PollResult::Ok;
        }

        DWORD transferred = 0;
        if (multiplexImmediateOk)
        {
            transferred = multiplexImmediateCount;
        }
        else if (multiplexImmediateError != 0)
        {
            return (multiplexImmediateError == ERROR_BROKEN_PIPE || multiplexImmediateError == ERROR_PIPE_NOT_CONNECTED || multiplexImmediateError == ERROR_OPERATION_ABORTED)
                ? PollResult::Closed : PollResult::Error;
        }
        else if (!GetOverlappedResult(handle, &multiplexOv, &transferred, FALSE))
        {
            DWORD err = GetLastError();
            return (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED)
                ? PollResult::Closed : PollResult::Error;
        }
        if (transferred == 0) return PollResult::Closed;
        if (buffer.size() + transferred > kMaxLineBytes)
        {
            buffer.clear();
            return PollResult::LineTooLarge;
        }

        buffer.append(multiplexChunk, transferred);
        if (buffer.find('\n') != std::string::npos)
        {
            outLine = takeLineFromBuffer();
            return PollResult::Ok;
        }
        return PollResult::Pending; // more bytes arrived, but still no full line -- caller should beginRead() again
    }

    // For giving up on a pending multiplexed read early (session shutdown),
    // symmetric with readLine()'s own timeout-cleanup: cancels and waits
    // for the cancellation to actually land before returning, so the
    // handle/OVERLAPPED struct are never touched again after.
    void cancelPendingRead()
    {
        if (multiplexReadPending && !multiplexBufferedLineReady && multiplexImmediateError == 0 && !multiplexImmediateOk)
        {
            CancelIoEx(handle, &multiplexOv);
            DWORD dummy = 0;
            GetOverlappedResult(handle, &multiplexOv, &dummy, TRUE);
        }
        multiplexReadPending = false;
    }

private:
    std::string takeLineFromBuffer()
    {
        size_t nl = buffer.find('\n');
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    HANDLE handle;
    HANDLE readEvent = nullptr;
    HANDLE writeEventHandle = nullptr;
    std::string buffer;

    bool multiplexReadPending = false;
    bool multiplexBufferedLineReady = false;
    bool multiplexImmediateOk = false;
    DWORD multiplexImmediateCount = 0;
    DWORD multiplexImmediateError = 0;
    OVERLAPPED multiplexOv{};
    char multiplexChunk[4096];
};
