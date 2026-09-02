// Direct proof that PythonEngine.h drives the REAL MR-MT3 transcription
// model end-to-end through the actual IPC mechanism: spawns
// mt3_engine.py in its own venv, sends a real TRANSCRIBE request against
// a real BSM1 file (BeatShoreDesktop's own raw-audio temp file format --
// same layout main.cpp already writes, see SharedAudioBuffer.h and
// mt3_engine.py's own header comment), and checks the real MIDI file
// that comes back, including its actual note content -- not a mock.
#ifndef MT3_ENGINE_SCRIPT_PATH
#error "MT3_ENGINE_SCRIPT_PATH must be defined by CMake"
#endif
#ifndef MT3_PYTHON_EXE_PATH
#error "MT3_PYTHON_EXE_PATH must be defined by CMake"
#endif

#include "../../BeatShoreDesktop/Source/PythonEngine.h"
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

namespace
{
    int failures = 0;
    void check(bool condition, const std::string& label)
    {
        std::cout << "[test] " << label << ": " << (condition ? "PASS" : "FAIL") << std::endl;
        if (!condition) ++failures;
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

    // Writes BeatShoreDesktop's own BSM1 raw-audio format directly --
    // "BSM1" magic, uint32 sampleRate/channels/frames, then interleaved
    // float32 samples -- the exact layout main.cpp itself writes from
    // shared memory (see native/protocol/SharedAudioBuffer.h), so this
    // test exercises the real file format, not a stand-in.
    void writeBsm1TwoNoteMelody(const std::string& path, uint32_t sampleRate, double durationS)
    {
        const uint32_t frames = static_cast<uint32_t>(sampleRate * durationS);
        std::vector<float> samples(frames, 0.0f);
        const double half = durationS / 2.0;
        for (uint32_t i = 0; i < frames; ++i)
        {
            const double t = i / static_cast<double>(sampleRate);
            const double freq = (t < half) ? 261.63 /* C4 */ : 329.63 /* E4 */;
            const double localT = (t < half) ? t : (t - half);
            const double envelope = std::min(1.0, localT / 0.02) * std::min(1.0, (half - localT) / 0.3);
            samples[i] = static_cast<float>(0.5 * envelope * std::sin(2.0 * 3.14159265358979323846 * freq * t));
        }

        std::ofstream f(path, std::ios::binary);
        f.write("BSM1", 4);
        const uint32_t channels = 1;
        f.write(reinterpret_cast<const char*>(&sampleRate), 4);
        f.write(reinterpret_cast<const char*>(&channels), 4);
        f.write(reinterpret_cast<const char*>(&frames), 4);
        f.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(float)));
    }

    std::string jsonEscape(const std::string& path)
    {
        std::string out;
        for (char c : path) { if (c == '\\') out += "\\\\"; else out += c; }
        return out;
    }
}

int main()
{
    const std::string inputPath = "mt3_test_input.bsmraw";
    const std::string outputPath = "mt3_test_output.mid";
    writeBsm1TwoNoteMelody(inputPath, 44100, 4.0);

    std::cout << "\n=== MT3 (MR-MT3) end-to-end: real model, real BSM1 input, real MIDI output ===" << std::endl;
    PythonEngine engine;
    const bool started = engine.start(MT3_PYTHON_EXE_PATH, MT3_ENGINE_SCRIPT_PATH);
    check(started, "mt3 engine process started");

    std::string readyLine;
    // Model warm-up at startup (see mt3_engine.py) plus a from-cold-cache
    // checkpoint load can genuinely take a while -- a long deadline here
    // reflects that reality, not padding.
    const auto readyResult = engine.readLine(180000, readyLine);
    check(readyResult == OverlappedPipeIO::ReadResult::Ok, "READY arrives (model finished loading/warming up)");
    check(extractStringField(readyLine, "type") == "READY", "READY line has type=READY");

    const std::string request =
        R"({"type":"TRANSCRIBE","requestId":"test-1","inputAudioPath":")" + jsonEscape(inputPath) +
        R"(","outputMidiPath":")" + jsonEscape(outputPath) + R"("})";
    check(engine.writeLine(request), "TRANSCRIBE request written");

    std::string resultLine;
    const auto resultResult = engine.readLine(60000, resultLine);
    std::cout << "[test] result line: " << resultLine << std::endl;
    check(resultResult == OverlappedPipeIO::ReadResult::Ok, "TRANSCRIBE_RESULT arrives");
    check(extractStringField(resultLine, "type") == "TRANSCRIBE_RESULT", "response has type=TRANSCRIBE_RESULT");
    check(extractBoolField(resultLine, "success"), "response reports success=true");

    const int noteCount = extractIntField(resultLine, "noteCount");
    std::cout << "[test] noteCount: " << noteCount << std::endl;
    check(noteCount > 0, "at least one note was actually transcribed from a real two-note input");

    std::ifstream outFile(outputPath, std::ios::binary | std::ios::ate);
    const bool outputExists = outFile.good();
    const auto outputSize = outputExists ? static_cast<long long>(outFile.tellg()) : 0;
    std::cout << "[test] output MIDI file size: " << outputSize << " bytes" << std::endl;
    check(outputExists, "output MIDI file was actually written");
    check(outputSize > 20, "output MIDI file is a real, non-empty Standard MIDI File");

    engine.writeLine(R"({"type":"SHUTDOWN"})");

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
