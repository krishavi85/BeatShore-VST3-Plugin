// Standalone test harness standing in for the plugin's IPC client, so the
// desktop process <-> protocol <-> Node engine round trip can be verified
// without rebuilding/reloading the actual VST3 in a DAW for every iteration.
// Once this proves the round trip, the same connect/HELLO/create-shm/
// ANALYSIS_REQUEST sequence gets built into BeatShoreBridgeAudioProcessor.
#include <windows.h>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include "../../protocol/NamedPipeIO.h"
#include "../../protocol/MiniJson.h"
#include "../../protocol/SharedAudioBuffer.h"

using namespace bsjson;

static const char* PIPE_NAME = "\\\\.\\pipe\\BeatShoreBridge.v1";

// Reads the same BSM1 raw layout the desktop/Node side use, from a file
// produced ahead of time (mirrors what would really be captured audio in
// the plugin).
static bool loadBsmRaw(const std::string& path, uint32_t& sr, uint32_t& ch, uint32_t& frames, std::vector<float>& samples)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4];
    in.read(magic, 4);
    if (std::memcmp(magic, "BSM1", 4) != 0) return false;
    in.read(reinterpret_cast<char*>(&sr), 4);
    in.read(reinterpret_cast<char*>(&ch), 4);
    in.read(reinterpret_cast<char*>(&frames), 4);
    samples.resize(size_t(frames) * ch);
    in.read(reinterpret_cast<char*>(samples.data()), std::streamsize(samples.size() * sizeof(float)));
    return bool(in) || in.eof();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: BeatShoreTestClient <bsmraw-audio-path> [kind] [role]" << std::endl;
        return 1;
    }
    std::string audioPath = argv[1];
    std::string kind = argc > 2 ? argv[2] : "tempo";
    std::string role = argc > 3 ? argv[3] : "";

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        pipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY)
        {
            std::cerr << "connect failed, error=" << err << std::endl;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (pipe == INVALID_HANDLE_VALUE) { std::cerr << "could not connect to " << PIPE_NAME << " after retries" << std::endl; return 1; }
    std::cout << "[client] connected" << std::endl;

    PipeLineIO io(pipe);

    Value hello = Value::object();
    hello.set("type", "HELLO");
    hello.set("v", 1);
    hello.set("pluginVersion", "0.1.0-testclient");
    hello.set("pid", int(GetCurrentProcessId()));
    hello.set("sessionId", "testclient");
    io.writeLine(stringify(hello));
    std::cout << "[client] -> HELLO" << std::endl;

    auto capsLine = io.readLine();
    if (!capsLine) { std::cerr << "no response to HELLO" << std::endl; return 1; }
    std::cout << "[client] <- " << *capsLine << std::endl;

    uint32_t sr, ch, frames;
    std::vector<float> samples;
    if (!loadBsmRaw(audioPath, sr, ch, frames, samples))
    {
        std::cerr << "failed to load audio from " << audioPath << std::endl;
        return 1;
    }
    std::cout << "[client] loaded audio: " << frames << " frames, " << ch << " ch, " << sr << " Hz" << std::endl;

    std::string shmName = "Local\\BeatShoreAudio.testclient.r1";
    SharedAudioBuffer shm;
    if (!shm.create(shmName, sr, ch, frames))
    {
        std::cerr << "failed to create shared memory " << shmName << std::endl;
        return 1;
    }
    shm.writeSamples(samples.data(), frames * ch);
    std::cout << "[client] wrote audio into shared memory " << shmName << std::endl;

    Value audio = Value::object();
    audio.set("shm", shmName);
    audio.set("sampleRate", int(sr));
    audio.set("channels", int(ch));
    audio.set("frames", int(frames));

    Value req = Value::object();
    req.set("type", "ANALYSIS_REQUEST");
    req.set("v", 1);
    req.set("requestId", "r1");
    req.set("kind", kind);
    if (!role.empty()) req.set("role", role);
    req.set("audio", audio);
    req.set("audioSource", "file"); // this harness always loads audio from a .bsmraw file, never a live DAW track -- see PROTOCOL.md
    req.set("tempo", 120.0);
    io.writeLine(stringify(req));
    std::cout << "[client] -> ANALYSIS_REQUEST kind=" << kind << std::endl;

    // read until we see a terminal message (ANALYSIS_RESULT / MIDI_RESULT / ERROR) for r1
    for (int i = 0; i < 50; ++i)
    {
        std::cout << "[client] iteration " << i << ": calling readLine..." << std::endl;
        auto line = io.readLine();
        std::cout << "[client] iteration " << i << ": readLine returned, hasValue=" << (line ? "true" : "false") << std::endl;
        if (!line) { std::cout << "[client] pipe closed" << std::endl; break; }
        std::cout << "[client] <- " << *line << std::endl;
        Value msg = parse(*line);
        std::string type = msg["type"].asString();
        if (type == "ANALYSIS_RESULT" || type == "MIDI_RESULT" || type == "AUDIO_RESULT" || type == "ERROR")
        {
            std::cout << "[client] DONE (" << type << ")" << std::endl;
            break;
        }
    }

    CloseHandle(pipe);
    return 0;
}
