#pragma once
#include "third_party/libebur128/ebur128.h"
#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>

// Real EBU R128 loudness + true-peak metering for the Master page, using
// libebur128 (native/BeatShoreBridge/Source/third_party/libebur128, MIT,
// vendored unmodified -- see that directory's VENDORED.md) on this
// plugin's own audio in processBlock() -- the same "process the plugin's
// own live audio for real" pattern MixChain.h already established, not a
// request to the desktop broker. Unlike MixChain, this never alters the
// signal; it only observes whatever processBlock() is about to hand back
// to the host (after Mix, if Mix is enabled) and publishes real measured
// numbers.
//
// Threading: libebur128's query functions (ebur128_loudness_momentary
// etc.) are not documented as safe to call concurrently with
// ebur128_add_frames_float() from another thread. So every read here
// happens on the SAME thread that just fed it -- the audio thread, inside
// processBlock() -- immediately after add_frames_float(), and the results
// are published into atomic doubles a UI timer on the message thread can
// read lock-free. Same cross-thread pattern PluginProcessor's own
// HostSnapshot already uses for sampleRate/tempo/etc.
class MasterMeter
{
public:
    // Display sentinel for "no measurable loudness yet / true silence" --
    // libebur128 itself reports -HUGE_VAL (negative infinity) for that
    // case (see ebur128_loudness_momentary's own doc comment), which isn't
    // a useful number to store in an atomic<double> a UI reads on a timer
    // (comparisons/formatting on an actual -inf are needlessly fragile);
    // -100.0 LUFS/dBTP is well below anything EBU R128 ever reports for
    // real audio, so the UI can treat it as "show '-inf'" unambiguously.
    static constexpr double kNegInf = -100.0;

    struct Snapshot
    {
        std::atomic<double> momentaryLufs { kNegInf };
        std::atomic<double> shortTermLufs { kNegInf };
        std::atomic<double> integratedLufs { kNegInf };
        std::atomic<double> truePeakDb { kNegInf };
    };

    ~MasterMeter() { destroy(); }

    void prepare(double sampleRate, int numChannels)
    {
        destroy();
        channels = numChannels;
        state = ebur128_init(static_cast<unsigned int>(numChannels),
                              static_cast<unsigned long>(sampleRate),
                              EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    }

    // Re-initializes the underlying ebur128_state from scratch -- the
    // library has no "clear accumulated history" call short of this (see
    // ebur128_change_parameters()'s own doc comment: it's a EBUR128_ERROR_
    // NO_CHANGE no-op when channels/samplerate are unchanged, which they
    // always are here), so Integrated LUFS and the true-peak hold both
    // genuinely restart from zero, not just visually reset in the UI.
    void reset()
    {
        if (state == nullptr) return;
        const auto sr = state->samplerate;
        const auto ch = state->channels;
        destroy();
        state = ebur128_init(ch, sr, EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    }

    // channelData/numSamples: JUCE's own per-channel buffer layout.
    // libebur128 requires interleaved frames, so this interleaves into a
    // reusable scratch buffer (grown, never shrunk -- no per-block heap
    // allocation once primed at the session's largest block size) before
    // calling add_frames_float().
    void process(const float* const* channelData, int numSamples, Snapshot& snapshot)
    {
        if (state == nullptr || numSamples <= 0 || channels <= 0) return;

        const size_t needed = static_cast<size_t>(numSamples) * static_cast<size_t>(channels);
        if (interleaveScratch.size() < needed) interleaveScratch.resize(needed);

        for (int i = 0; i < numSamples; ++i)
            for (int ch = 0; ch < channels; ++ch)
                interleaveScratch[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(ch)] = channelData[ch][i];

        if (ebur128_add_frames_float(state, interleaveScratch.data(), static_cast<size_t>(numSamples)) != EBUR128_SUCCESS)
            return;

        double v = 0.0;
        if (ebur128_loudness_momentary(state, &v) == EBUR128_SUCCESS)
            snapshot.momentaryLufs.store(std::isfinite(v) ? v : kNegInf, std::memory_order_relaxed);
        if (ebur128_loudness_shortterm(state, &v) == EBUR128_SUCCESS)
            snapshot.shortTermLufs.store(std::isfinite(v) ? v : kNegInf, std::memory_order_relaxed);
        if (ebur128_loudness_global(state, &v) == EBUR128_SUCCESS)
            snapshot.integratedLufs.store(std::isfinite(v) ? v : kNegInf, std::memory_order_relaxed);

        // Session true-peak hold: the max across every channel since the
        // last prepare()/reset(), which is exactly what ebur128_true_peak()
        // itself already tracks per channel (it's cumulative, not
        // per-block -- see its own doc comment) -- just taking the max
        // across channels here since the UI shows one number.
        double maxTruePeakLinear = 0.0;
        for (int ch = 0; ch < channels; ++ch)
        {
            double peak = 0.0;
            if (ebur128_true_peak(state, static_cast<unsigned int>(ch), &peak) == EBUR128_SUCCESS)
                maxTruePeakLinear = std::max(maxTruePeakLinear, peak);
        }
        snapshot.truePeakDb.store(maxTruePeakLinear > 0.0 ? 20.0 * std::log10(maxTruePeakLinear) : kNegInf, std::memory_order_relaxed);
    }

private:
    void destroy() { if (state != nullptr) ebur128_destroy(&state); state = nullptr; }

    ebur128_state* state = nullptr;
    int channels = 0;
    std::vector<float> interleaveScratch; // audio thread only, grown lazily, never freed until destruction
};
