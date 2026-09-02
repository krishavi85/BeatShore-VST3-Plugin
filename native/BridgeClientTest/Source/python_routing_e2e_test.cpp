// Definitive end-to-end proof that main.cpp's real request routing to the
// Python-based engines (DAC, EnCodec, MT3) actually works -- not through
// PythonEngine.h directly (python_engine_test.cpp/codec_engine_test.cpp/
// mt3_engine_test.cpp already proved that layer), but through the REAL
// desktop process: a real HELLO handshake over the real named pipe, a
// real ANALYSIS_REQUEST with real shared-memory audio (the exact
// mechanism the plugin itself uses via BridgeClient.h), routed by
// main.cpp's own kind-based dispatch to the correct dedicated queue and
// worker, producing a real result read back over the same pipe.
//
// Requires a REAL, already-running BeatShoreDesktop.exe built with this
// session's routing changes -- same precondition as load_boundary_test.cpp,
// which this file's connection/HELLO helpers are deliberately modeled on
// (kept as their own small copies here rather than a shared header, same
// reasoning that file's own header comment gives: this is a thin,
// direct raw client, not a second implementation of BridgeClient worth
// factoring out for two files).
// See ChildProcessEngine.h's own comment on the same NOMINMAX fix -- this
// file uses std::min directly and the protocol headers below pull in
// windows.h themselves, so this must come before ALL of them, not just
// before this file's own <windows.h>.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../protocol/NamedPipeIO.h"
#include "../../protocol/MiniJson.h"
#include "../../protocol/SharedAudioBuffer.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>

using namespace bsjson;

namespace
{
    const char* kPipeName = "\\\\.\\pipe\\BeatShoreBridge.v1";
    int failures = 0;

    void check(bool condition, const std::string& label)
    {
        std::cout << "[test] " << label << ": " << (condition ? "PASS" : "FAIL") << std::endl;
        if (!condition) ++failures;
    }

    HANDLE connectPipe()
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            HANDLE h = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) return h;
            if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
            WaitNamedPipeA(kPipeName, 2000);
        }
        return INVALID_HANDLE_VALUE;
    }

    bool doHello(PipeLineIO& io)
    {
        Value hello = Value::object();
        hello.set("type", "HELLO");
        hello.set("v", 1);
        hello.set("clientName", "python-routing-e2e-test");
        if (!io.writeLine(stringify(hello))) return false;
        auto resp = io.readLine();
        if (!resp) return false;
        Value msg = parse(*resp);
        return msg["type"].asString() == "CAPABILITIES";
    }

    // A real two-note melody (C4 then E4), same content/reasoning as the
    // other engine tests' own synthetic fixtures -- real, checkable
    // musical content, not silence or noise.
    std::vector<float> makeTwoNoteMelody(uint32_t sampleRate, double durationS)
    {
        const uint32_t frames = uint32_t(sampleRate * durationS);
        std::vector<float> samples(frames, 0.0f);
        const double half = durationS / 2.0;
        for (uint32_t i = 0; i < frames; ++i)
        {
            const double t = i / double(sampleRate);
            const double freq = (t < half) ? 261.63 : 329.63;
            const double localT = (t < half) ? t : (t - half);
            const double envelope = std::min(1.0, localT / 0.02) * std::min(1.0, (half - localT) / 0.3);
            samples[i] = float(0.5 * envelope * std::sin(2.0 * 3.14159265358979323846 * freq * t));
        }
        return samples;
    }

    // Runs one real ANALYSIS_REQUEST of the given kind against a fresh
    // connection, waits for its terminal response, and returns the raw
    // response line for the caller to check kind-specific fields on.
    // PipeLineIO::readLine() is a plain blocking call with no timeout
    // parameter of its own -- fine here without a client-side deadline,
    // since the SERVER side (runPythonRequest's own timeoutMs in
    // main.cpp) already guarantees a terminal response (even if just an
    // ERROR/TIMEOUT) within a bounded time regardless of what this test
    // does.
    bool runRequest(const std::string& kind, const std::string& requestId, std::string& outResponseLine)
    {
        HANDLE h = connectPipe();
        if (h == INVALID_HANDLE_VALUE) { std::cout << "[test] could not connect to BeatShoreDesktop -- is it running?" << std::endl; return false; }
        PipeLineIO io(h);
        if (!doHello(io)) { CloseHandle(h); return false; }

        const std::string shmName = "Local\\BeatShoreAudio." + std::to_string(GetCurrentProcessId()) + "." + requestId;
        SharedAudioBuffer buffer;
        const uint32_t sampleRate = 44100, channels = 1;
        auto samples = makeTwoNoteMelody(sampleRate, 4.0);
        if (!buffer.create(shmName, sampleRate, channels, uint32_t(samples.size())))
        {
            std::cout << "[test] failed to create shared memory " << shmName << std::endl;
            CloseHandle(h);
            return false;
        }
        buffer.writeSamples(samples.data(), uint32_t(samples.size()));

        Value req = Value::object();
        req.set("type", "ANALYSIS_REQUEST");
        req.set("requestId", requestId);
        req.set("kind", kind);
        req.set("audioSource", "file");
        Value audio = Value::object();
        audio.set("shm", shmName);
        req.set("audio", audio);
        io.writeLine(stringify(req));

        // Bounded by iteration count, not wall-clock -- each readLine()
        // call is itself bounded by the server's own internal timeouts
        // (see the comment on this function's declaration), so a hard
        // cap here is just a belt-and-suspenders guard against an
        // infinite loop, not this test's real timeout mechanism.
        for (int i = 0; i < 20; ++i)
        {
            auto line = io.readLine();
            if (!line) { CloseHandle(h); return false; }
            Value msg;
            try { msg = parse(*line); } catch (const std::exception&) { continue; }
            const std::string type = msg.has("type") ? msg["type"].asString() : "";
            if (type == "MIDI_RESULT" || type == "ENCODE_DECODE_RESULT" || type == "ERROR")
            {
                outResponseLine = *line;
                CloseHandle(h);
                return true;
            }
        }
        CloseHandle(h);
        return false;
    }

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

    bool extractBoolField(const std::string& json, const std::string& key)
    {
        for (const std::string& needle : { "\"" + key + "\":true", "\"" + key + "\": true" })
            if (json.find(needle) != std::string::npos) return true;
        return false;
    }

    int extractIntField(const std::string& json, const std::string& key)
    {
        for (const std::string& needle : { "\"" + key + "\":", "\"" + key + "\": " })
        {
            auto pos = json.find(needle);
            if (pos == std::string::npos) continue;
            pos += needle.size();
            return std::atoi(json.c_str() + pos);
        }
        return -1;
    }
}

int main()
{
    // --- MT3, through the real desktop, real pipe, real routing ---
    {
        std::cout << "\n=== transcribeMt3 via a real ANALYSIS_REQUEST to a real, running BeatShoreDesktop.exe ===" << std::endl;
        std::string response;
        const bool got = runRequest("transcribeMt3", "e2e-mt3-1", response);
        check(got, "received a terminal response");
        if (got)
        {
            std::cout << "[test] response: " << response << std::endl;
            check(extractStringField(response, "type") == "MIDI_RESULT", "response type is MIDI_RESULT (the SAME shape transcribeDrums/transcribeMono already use -- zero plugin-side changes needed)");
            check(extractBoolField(response, "success"), "success=true");
            check(extractIntField(response, "noteCount") > 0, "noteCount > 0 -- real notes from a real two-note input, routed through the real desktop process");
        }
    }

    // --- DAC, through the real desktop, real pipe, real routing ---
    {
        std::cout << "\n=== encodeDecodeDac via a real ANALYSIS_REQUEST to a real, running BeatShoreDesktop.exe ===" << std::endl;
        std::string response;
        const bool got = runRequest("encodeDecodeDac", "e2e-dac-1", response);
        check(got, "received a terminal response");
        if (got)
        {
            std::cout << "[test] response: " << response << std::endl;
            check(extractStringField(response, "type") == "ENCODE_DECODE_RESULT", "response type is ENCODE_DECODE_RESULT");
            check(extractBoolField(response, "success"), "success=true");
        }
    }

    // --- EnCodec, through the real desktop, real pipe, real routing ---
    {
        std::cout << "\n=== encodeDecodeEncodec via a real ANALYSIS_REQUEST to a real, running BeatShoreDesktop.exe ===" << std::endl;
        std::string response;
        const bool got = runRequest("encodeDecodeEncodec", "e2e-encodec-1", response);
        check(got, "received a terminal response");
        if (got)
        {
            std::cout << "[test] response: " << response << std::endl;
            check(extractStringField(response, "type") == "ENCODE_DECODE_RESULT", "response type is ENCODE_DECODE_RESULT");
            check(extractBoolField(response, "success"), "success=true");
        }
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
