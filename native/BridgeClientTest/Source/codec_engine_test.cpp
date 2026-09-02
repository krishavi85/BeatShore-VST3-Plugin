// Direct proof that PythonEngine.h drives a REAL neural codec model
// end-to-end through the actual IPC mechanism: spawns encodec_engine.py
// (or dac_engine.py, selected by CODEC_ENGINE_SCRIPT_PATH/CODEC_NAME at
// build time) in its own dedicated venv, sends a real ENCODE_DECODE
// request against a real synthetic WAV file, and checks the real
// response and the real output file it wrote -- not a mock, the actual
// pretrained model doing actual inference, driven the same way
// PluginProcessor would drive it in production.
//
// This complements python_engine_test.cpp (which proves the generic IPC
// mechanism works through Python) by proving the SPECIFIC production
// protocol (ENCODE_DECODE / ENCODE_DECODE_RESULT, file-path-based audio
// I/O) also works, through a real model, not just an echo script.
#ifndef CODEC_ENGINE_SCRIPT_PATH
#error "CODEC_ENGINE_SCRIPT_PATH must be defined by CMake to the engine script's path"
#endif
#ifndef CODEC_PYTHON_EXE_PATH
#error "CODEC_PYTHON_EXE_PATH must be defined by CMake to the venv's python.exe"
#endif
#ifndef CODEC_NAME
#define CODEC_NAME "codec"
#endif
#ifndef CODEC_SAMPLE_RATE
#define CODEC_SAMPLE_RATE 24000.0
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

    // Minimal 16-bit PCM mono WAV writer -- just enough to hand the
    // Python engine a real, valid input file (a 440Hz sine, same
    // reference signal the standalone Python smoke tests already used).
    void writeSineWav(const std::string& path, double sampleRate, double durationS, double freqHz)
    {
        const uint32_t numSamples = static_cast<uint32_t>(sampleRate * durationS);
        std::vector<int16_t> samples(numSamples);
        for (uint32_t i = 0; i < numSamples; ++i)
        {
            const double t = i / sampleRate;
            samples[i] = static_cast<int16_t>(0.5 * 32767.0 * std::sin(2.0 * 3.14159265358979323846 * freqHz * t));
        }

        std::ofstream f(path, std::ios::binary);
        const uint32_t dataSize = numSamples * sizeof(int16_t);
        const uint32_t sr = static_cast<uint32_t>(sampleRate);
        const uint32_t byteRate = sr * 2;
        const uint16_t blockAlign = 2;
        const uint16_t bitsPerSample = 16;
        const uint32_t chunkSize = 36 + dataSize;

        f.write("RIFF", 4);
        f.write(reinterpret_cast<const char*>(&chunkSize), 4);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        const uint32_t fmtSize = 16;
        f.write(reinterpret_cast<const char*>(&fmtSize), 4);
        const uint16_t audioFormat = 1;
        f.write(reinterpret_cast<const char*>(&audioFormat), 2);
        const uint16_t numChannels = 1;
        f.write(reinterpret_cast<const char*>(&numChannels), 2);
        f.write(reinterpret_cast<const char*>(&sr), 4);
        f.write(reinterpret_cast<const char*>(&byteRate), 4);
        f.write(reinterpret_cast<const char*>(&blockAlign), 2);
        f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
        f.write("data", 4);
        f.write(reinterpret_cast<const char*>(&dataSize), 4);
        f.write(reinterpret_cast<const char*>(samples.data()), dataSize);
    }

    std::string jsonEscape(const std::string& path)
    {
        std::string out;
        for (char c : path)
        {
            if (c == '\\') out += "\\\\";
            else out += c;
        }
        return out;
    }
}

int main()
{
    // Own input filename per codec (not shared) -- each codec asserts its
    // audio arrives at its own native sample rate (confirmed directly:
    // DAC's 44.1kHz model raises AssertionError on a 24kHz input, since
    // neither engine script resamples -- see their own header comments),
    // so a shared file at the wrong rate for one of them isn't just
    // inconvenient, it's actually invalid input for that model.
    const std::string inputPath = "codec_test_input_" CODEC_NAME ".wav";
    const std::string outputPath = "codec_test_output_" CODEC_NAME ".wav";
    writeSineWav(inputPath, CODEC_SAMPLE_RATE, 1.0, 440.0);

    std::cout << "\n=== " CODEC_NAME " end-to-end: real model, real request, real output file ===" << std::endl;
    PythonEngine engine;
    const bool started = engine.start(CODEC_PYTHON_EXE_PATH, CODEC_ENGINE_SCRIPT_PATH);
    check(started, CODEC_NAME " engine process started");

    std::string readyLine;
    // Model loading (downloading pretrained weights on first run) can
    // genuinely take a while -- a long deadline here is a real
    // reflection of that, not padding to avoid a flaky test.
    const auto readyResult = engine.readLine(120000, readyLine);
    std::cout << "[test] READY line: " << readyLine << std::endl;
    check(readyResult == OverlappedPipeIO::ReadResult::Ok, "READY arrives (model finished loading)");
    check(extractStringField(readyLine, "type") == "READY", "READY line has type=READY");

    const std::string request =
        R"({"type":"ENCODE_DECODE","requestId":"test-1","inputWavPath":")" + jsonEscape(inputPath) +
        R"(","outputWavPath":")" + jsonEscape(outputPath) + R"("})";
    const bool wrote = engine.writeLine(request);
    check(wrote, "ENCODE_DECODE request written");

    std::string resultLine;
    const auto resultResult = engine.readLine(60000, resultLine);
    std::cout << "[test] result line: " << resultLine << std::endl;
    check(resultResult == OverlappedPipeIO::ReadResult::Ok, "ENCODE_DECODE_RESULT arrives");
    check(extractStringField(resultLine, "type") == "ENCODE_DECODE_RESULT", "response has type=ENCODE_DECODE_RESULT");
    check(extractBoolField(resultLine, "success"), "response reports success=true");

    // The real proof this actually did something: a genuine output file,
    // non-trivially sized (not an empty/truncated write).
    std::ifstream outFile(outputPath, std::ios::binary | std::ios::ate);
    const bool outputExists = outFile.good();
    const auto outputSize = outputExists ? static_cast<long long>(outFile.tellg()) : 0;
    std::cout << "[test] output file size: " << outputSize << " bytes" << std::endl;
    check(outputExists, "output WAV file was actually written");
    check(outputSize > 10000, "output WAV file has real, non-trivial audio content (not an empty/truncated write)");

    engine.writeLine(R"({"type":"SHUTDOWN"})");

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
