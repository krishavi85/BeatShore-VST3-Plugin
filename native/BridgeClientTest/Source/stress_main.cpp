// Stress/correctness test for the REAL BeatShoreBridgeAudioProcessor
// capture buffer (native/BeatShoreBridge/Source/PluginProcessor.cpp,
// compiled unmodified into this target -- not a reimplementation). Hammers
// processBlock() from one thread with a known, verifiable ramp pattern
// while repeatedly taking snapshots from another thread via
// captureSnapshotForTest() (the same swap + read-out code path
// triggerTempoAnalysis() uses, minus the BridgeClient connection gate), and
// checks every snapshot for internal consistency: strictly increasing
// sample values in chronological order, with no jumps, no repeats, and no
// torn/corrupted floats. A genuine race in the swap (the exact class of bug
// the single-buffer + pause-flag design had before this rewrite) would show
// up here as either a corrupted value or an out-of-order jump -- not just a
// crash -- which is why this checks data content, not merely "didn't
// crash."
#include "../../BeatShoreBridge/Source/PluginProcessor.h"
#include <atomic>
#include <thread>
#include <iostream>
#include <cmath>

int main()
{
    BeatShoreBridgeAudioProcessor processor;
    processor.setPlayConfigDetails(2, 2, 48000.0, 256);
    processor.prepareToPlay(48000.0, 256);

    std::atomic<bool> stop { false };
    std::atomic<int> snapshotsOk { 0 };
    std::atomic<int> snapshotsEmpty { 0 };
    std::atomic<int> corruptionsFound { 0 };

    // The ramp writes ((runningSampleIndex) mod RAMP_MOD) into both
    // channels, scaled down so it's a plausible (non-clipping) audio value.
    // A correct chronological read-out must show consecutive frames whose
    // *unscaled* integer index differs by exactly 1 (mod RAMP_MOD) --
    // anything else means the read-out picked up stale/future/torn data.
    constexpr int64_t RAMP_MOD = 1000000;

    std::thread audioThread([&]
    {
        juce::MidiBuffer midi;
        int64_t index = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            juce::AudioBuffer<float> buffer(2, 256);
            for (int i = 0; i < 256; ++i)
            {
                const float v = float((index + i) % RAMP_MOD) * 0.0001f;
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, v);
            }
            processor.processBlock(buffer, midi);
            index += 256;
        }
    });

    std::thread triggerThread([&]
    {
        for (int iter = 0; iter < 3000; ++iter)
        {
            std::vector<float> interleaved;
            double sr = 0.0;
            if (!processor.captureSnapshotForTest(interleaved, sr))
            {
                snapshotsEmpty.fetch_add(1);
                continue;
            }

            bool ok = true;
            const size_t frames = interleaved.size() / 2;
            int64_t prevRaw = -1;
            for (size_t f = 0; f < frames && ok; ++f)
            {
                const float l = interleaved[f * 2 + 0];
                const float r = interleaved[f * 2 + 1];
                if (l != r) { ok = false; break; } // both channels were written identically -- any divergence is corruption

                const int64_t raw = int64_t(std::round(l / 0.0001f));
                if (prevRaw >= 0)
                {
                    const int64_t expected = (prevRaw + 1) % RAMP_MOD;
                    if (raw != expected) { ok = false; break; }
                }
                prevRaw = raw;
            }

            if (ok) snapshotsOk.fetch_add(1);
            else corruptionsFound.fetch_add(1);

            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    });

    triggerThread.join();
    stop.store(true);
    audioThread.join();

    std::cout << "[stress] snapshots ok: " << snapshotsOk.load()
              << ", empty (no audio yet / slot busy): " << snapshotsEmpty.load()
              << ", corrupted: " << corruptionsFound.load() << std::endl;

    if (corruptionsFound.load() > 0)
    {
        std::cerr << "[stress] FAILED: found " << corruptionsFound.load() << " corrupted/out-of-order snapshots" << std::endl;
        return 1;
    }
    if (snapshotsOk.load() == 0)
    {
        std::cerr << "[stress] FAILED: never got a single valid snapshot -- test isn't exercising anything" << std::endl;
        return 1;
    }

    std::cout << "[stress] PASSED" << std::endl;
    return 0;
}
