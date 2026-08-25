// Integration test for the real BridgeClient class used by
// BeatShoreBridgeAudioProcessor -- see CMakeLists.txt. Requires a
// BeatShoreDesktop process already running and listening on
// \\.\pipe\BeatShoreBridge.v1.
#include <juce_core/juce_core.h>
#include "../../BeatShoreBridge/Source/BridgeClient.h"
#include <fstream>
#include <iostream>
#include <cstring>

static bool loadBsmRaw(const std::string& path, uint32_t& sr, uint32_t& ch, uint32_t& frames, std::vector<float>& interleaved)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4];
    in.read(magic, 4);
    if (std::memcmp(magic, "BSM1", 4) != 0) return false;
    in.read(reinterpret_cast<char*>(&sr), 4);
    in.read(reinterpret_cast<char*>(&ch), 4);
    in.read(reinterpret_cast<char*>(&frames), 4);
    interleaved.resize(size_t(frames) * ch);
    in.read(reinterpret_cast<char*>(interleaved.data()), std::streamsize(interleaved.size() * sizeof(float)));
    return bool(in) || in.eof();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: BridgeClientTest <bsmraw-audio-path> [kind]" << std::endl;
        return 1;
    }
    const std::string kind = argc > 2 ? argv[2] : "tempo";

    uint32_t sr, ch, frames;
    std::vector<float> interleaved;
    if (!loadBsmRaw(argv[1], sr, ch, frames, interleaved))
    {
        std::cerr << "failed to load audio from " << argv[1] << std::endl;
        return 1;
    }
    std::cout << "[test] loaded audio: " << frames << " frames, " << ch << " ch, " << sr << " Hz" << std::endl;

    BridgeClient client;
    client.start();

    std::cout << "[test] waiting for BridgeClient to connect to BeatShore desktop..." << std::endl;
    for (int i = 0; i < 100; ++i)
    {
        if (client.getStatus() == BridgeStatus::Connected) break;
        juce::Thread::sleep(100);
    }

    if (client.getStatus() != BridgeStatus::Connected)
    {
        std::cerr << "[test] FAILED: BridgeClient never reached Connected (status=" << int(client.getStatus()) << ")" << std::endl;
        return 1;
    }
    std::cout << "[test] BridgeClient connected." << std::endl;

    if (!client.requestAnalysis(interleaved, sr, ch, frames, kind, "file"))
    {
        std::cerr << "[test] FAILED: requestAnalysis() returned false" << std::endl;
        return 1;
    }
    std::cout << "[test] requested '" << kind << "' analysis, waiting for result..." << std::endl;

    BridgeAnalysisResult result;
    bool got = false;
    for (int i = 0; i < 200; ++i)
    {
        if (client.takeResult(result)) { got = true; break; }
        juce::Thread::sleep(50);
    }

    if (!got)
    {
        std::cerr << "[test] FAILED: no result within timeout" << std::endl;
        return 1;
    }

    std::cout << "[test] result: success=" << (result.success ? "true" : "false")
              << " kind=" << result.kind
              << " message=" << result.message
              << " hasNumericValue=" << (result.hasNumericValue ? "true" : "false")
              << " numericValue=" << result.numericValue
              << " audioSource=" << result.audioSource
              << " noteCount=" << result.noteCount
              << " midiPath=" << result.midiPath
              << " midiSha256=" << result.midiSha256
              << " midiWriteError=" << result.midiWriteError << std::endl;

    if (result.success) std::cout << "[test] PASSED" << std::endl;
    return result.success ? 0 : 1;
}
