#pragma once
// Thin, line-buffered wrapper over a Win32 byte-mode named pipe HANDLE.
// Deliberately byte-mode + manual newline splitting rather than
// PIPE_TYPE_MESSAGE: NDJSON's own '\n' framing is the message delimiter,
// so there's no need to also lean on Windows' message-mode framing --
// one framing mechanism, not two overlapping ones.
#include <windows.h>
#include <string>
#include <optional>

class PipeLineIO
{
public:
    explicit PipeLineIO(HANDLE h) : handle(h) {}

    bool writeLine(const std::string& line)
    {
        std::string withNewline = line + "\n";
        DWORD written = 0;
        return WriteFile(handle, withNewline.data(), DWORD(withNewline.size()), &written, nullptr) != 0
            && written == withNewline.size();
    }

    // Blocks until a full line is available, the pipe closes, or an error
    // occurs. Returns std::nullopt on close/error.
    std::optional<std::string> readLine()
    {
        size_t nl;
        while ((nl = buffer.find('\n')) == std::string::npos)
        {
            char chunk[4096];
            DWORD readCount = 0;
            BOOL ok = ReadFile(handle, chunk, sizeof(chunk), &readCount, nullptr);
            if (!ok || readCount == 0) return std::nullopt;
            buffer.append(chunk, readCount);
        }
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    HANDLE raw() const { return handle; }

private:
    HANDLE handle;
    std::string buffer;
};
