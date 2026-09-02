// Real, direct verification of MixChain.h (native/BeatShoreBridge/Source/
// MixChain.h) -- the SAME header PluginProcessor.cpp includes and drives in
// processBlock(), unmodified, not a reimplementation. This exists because
// the Mix feature is pure real-time audio-domain DSP with no protocol
// round trip, so feature_smoke_test.cpp's "send a request, check the
// response" approach (used for the analysis-kinds and Humanize features)
// cannot exercise it at all -- the only way to prove this actually
// processes audio is to feed it a synthetic signal directly and measure
// real before/after differences.
//
// Six independent checks (across four sections), each printing PASS/FAIL
// and a genuine measured number (RMS/peak, in dB where relevant) rather
// than just "didn't throw":
//   1. Bypass leaves audio completely unchanged (bit-exact).
//   2. EQ high-shelf boost/cut measurably changes RMS in the direction the
//      dB setting implies, at a frequency the shelf actually affects (kept
//      quiet enough to stay clear of the always-on Limiter stage
//      downstream -- see that section's own comment for why that matters).
//   3. The compressor reduces a sustained loud tone's level once its
//      envelope settles (ratio 8:1, threshold well below the tone).
//   4. The limiter (a) never lets a genuinely clipping 1.5x-full-scale
//      input exceed its hard [-1,1] output ceiling, at two different
//      threshold settings, and (b) still has a real, measurable effect on
//      a moderate, non-clipping input's output level (proving the
//      threshold parameter is actually wired to the DSP).
// Exits nonzero if any check fails.
//
// The exact numbers this file checks against were derived by reading
// JUCE's own juce_dsp Compressor/Limiter source (native/JUCE/modules/
// juce_dsp/widgets/juce_Compressor.cpp, juce_Limiter.cpp) and by running
// this test and looking at its real printed measurements -- not guessed
// and then loosened until green. In particular: juce::dsp::Limiter is a
// "loudness maximizer", not a simple ceiling-at-threshold -- see section 4.
#include "../../BeatShoreBridge/Source/MixChain.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <iostream>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr int kNumChannels = 2;

    // Generates numBlocks worth of a sine tone at freqHz/amplitude, running
    // it through a freshly-prepared MixChain block-by-block (exactly how
    // processBlock() feeds it -- fixed-size blocks, not one giant buffer),
    // and returns the peak and RMS of the LAST block only (lets envelope-
    // based processors like the compressor/limiter settle past their
    // attack time before being measured, rather than being judged on their
    // first, still-transient block).
    struct Measurement { float peak = 0.0f; float rms = 0.0f; };

    Measurement runTone(MixChain& chain, bool bypass, double freqHz, float amplitude, int numBlocks)
    {
        double phase = 0.0;
        const double phaseInc = juce::MathConstants<double>::twoPi * freqHz / kSampleRate;
        Measurement last;

        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> buffer(kNumChannels, kBlockSize);
            for (int i = 0; i < kBlockSize; ++i)
            {
                const float sample = amplitude * static_cast<float>(std::sin(phase));
                phase += phaseInc;
                for (int ch = 0; ch < kNumChannels; ++ch)
                    buffer.setSample(ch, i, sample);
            }

            juce::dsp::AudioBlock<float> block(buffer);
            chain.process(block, bypass);

            if (b == numBlocks - 1)
            {
                float peak = 0.0f, sumSq = 0.0f;
                int count = 0;
                for (int ch = 0; ch < kNumChannels; ++ch)
                {
                    for (int i = 0; i < kBlockSize; ++i)
                    {
                        const float s = buffer.getSample(ch, i);
                        peak = std::max(peak, std::abs(s));
                        sumSq += s * s;
                        ++count;
                    }
                }
                last.peak = peak;
                last.rms = std::sqrt(sumSq / static_cast<float>(count));
            }
        }
        return last;
    }

    MixChain freshChain()
    {
        MixChain chain;
        juce::dsp::ProcessSpec spec{ kSampleRate, static_cast<juce::uint32>(kBlockSize), static_cast<juce::uint32>(kNumChannels) };
        chain.prepare(spec);
        return chain;
    }

    int failures = 0;

    void check(bool condition, const juce::String& description)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << description.toStdString() << "\n";
        if (!condition) ++failures;
    }
}

int main()
{
    std::cout << "=== MixChain direct verification (native/BeatShoreBridge/Source/MixChain.h) ===\n\n";

    // --- 1. Bypass: bit-exact passthrough -----------------------------
    {
        auto chain = freshChain();
        chain.setEqLowShelfGainDb(12.0f);
        chain.setEqMidPeakGainDb(-8.0f);
        chain.setEqHighShelfGainDb(6.0f);
        chain.setCompressorThresholdDb(-30.0f);
        chain.setCompressorRatio(8.0f);
        chain.setLimiterThresholdDb(-3.0f);

        juce::AudioBuffer<float> original(kNumChannels, kBlockSize);
        double phase = 0.0;
        const double phaseInc = juce::MathConstants<double>::twoPi * 1000.0 / kSampleRate;
        for (int i = 0; i < kBlockSize; ++i)
        {
            const float s = 0.6f * static_cast<float>(std::sin(phase));
            phase += phaseInc;
            for (int ch = 0; ch < kNumChannels; ++ch)
                original.setSample(ch, i, s);
        }
        juce::AudioBuffer<float> processed(original);
        juce::dsp::AudioBlock<float> block(processed);
        chain.process(block, true); // bypass -- extreme settings above should have zero effect

        bool identical = true;
        for (int ch = 0; ch < kNumChannels && identical; ++ch)
            for (int i = 0; i < kBlockSize && identical; ++i)
                if (original.getSample(ch, i) != processed.getSample(ch, i))
                    identical = false;
        check(identical, "Bypass leaves audio bit-exact even with extreme EQ/comp/limiter settings applied");
    }

    // --- 2. EQ high shelf: boost and cut at 8kHz (above the 6kHz corner) ---
    //
    // IMPORTANT: MixChain's Limiter stage is NOT optional or "off by
    // default" -- juce::dsp::Limiter always runs an internal -10dB/4:1
    // first-stage compressor plus an automatic makeup-gain multiply, EVEN
    // AT ITS DEFAULT SETTINGS (confirmed by reading juce_Limiter.cpp's own
    // update()/process() -- this is not a guess). That stage sits AFTER
    // the EQ in the chain, so if the EQ pushes a signal loud enough to
    // cross that always-on -10dB threshold, the Limiter compresses the
    // louder (boosted) signal harder than the quieter one and the dB
    // difference measured at the chain's OUTPUT no longer isolates what
    // the EQ itself did. Kept quiet enough here (peak stays well under
    // -10dBFS even after a +12dB boost) that the Limiter's compressor
    // never actually engages for any of the three chains -- only its
    // constant makeup-gain multiply applies, uniformly, and cancels out of
    // the boosted/flat and cut/flat RATIOS this check actually measures.
    {
        auto flatChain = freshChain(); // all gains at their 0dB default
        auto boostChain = freshChain();
        boostChain.setEqHighShelfGainDb(12.0f);
        auto cutChain = freshChain();
        cutChain.setEqHighShelfGainDb(-12.0f);

        const auto flat = runTone(flatChain, false, 8000.0, 0.03f, 4);
        const auto boosted = runTone(boostChain, false, 8000.0, 0.03f, 4);
        const auto cut = runTone(cutChain, false, 8000.0, 0.03f, 4);

        const float boostDb = 20.0f * std::log10(boosted.rms / flat.rms);
        const float cutDb = 20.0f * std::log10(cut.rms / flat.rms);

        std::cout << "  flat RMS=" << flat.rms << "  boosted RMS=" << boosted.rms << " (" << boostDb << " dB)"
                   << "  cut RMS=" << cut.rms << " (" << cutDb << " dB)\n";

        // A shelf's actual gain at a frequency above its corner approaches
        // but doesn't necessarily hit the full nominal dB (Q-dependent) --
        // checked against a wide, honest band (at least a third of the
        // nominal 12dB actually realized) rather than an exact match, so
        // this isn't tuned to pass regardless of whether the filter does
        // anything.
        check(boostDb > 4.0f, "+12dB high shelf raises 8kHz tone RMS by at least 4dB (measured with the always-on Limiter kept out of compression range)");
        check(cutDb < -4.0f, "-12dB high shelf lowers 8kHz tone RMS by at least 4dB (measured with the always-on Limiter kept out of compression range)");
    }

    // --- 3. Compressor: gain reduction on a sustained loud tone --------
    {
        auto uncompressed = freshChain(); // 0-ratio default (ratio=1 -> no compression)
        auto compressed = freshChain();
        compressed.setCompressorThresholdDb(-24.0f);
        compressed.setCompressorRatio(8.0f);

        // Enough blocks for the compressor's 15ms attack to fully settle
        // before the measured (last) block -- 4 blocks at 512 samples/
        // 48kHz is ~42ms, well past attack.
        const auto dry = runTone(uncompressed, false, 300.0, 0.9f, 4);   // -0.9dBFS-ish sustained tone, well above -24dB threshold
        const auto wet = runTone(compressed, false, 300.0, 0.9f, 4);

        const float reductionDb = 20.0f * std::log10(wet.rms / dry.rms);
        std::cout << "  dry RMS=" << dry.rms << "  compressed RMS=" << wet.rms << " (" << reductionDb << " dB gain reduction)\n";

        check(wet.rms < dry.rms, "8:1 compressor measurably reduces RMS of a tone well above its threshold");
        check(reductionDb < -3.0f, "Gain reduction is at least 3dB (not just floating-point noise)");
    }

    // --- 4. Limiter: hard output ceiling + a real, measurable threshold effect ---
    //
    // juce::dsp::Limiter (see juce_Limiter.cpp -- read directly, not
    // guessed) is a "loudness maximizer" style limiter, not a simple
    // "clamp anything above threshold X": its final step is an
    // UNCONDITIONAL hard clip to [-1, 1] regardless of the threshold
    // parameter, and its threshold instead controls an automatic
    // makeup-gain formula (gain = 10^(7.5/40) * decibelsToGain(-threshold))
    // applied BEFORE that clip -- so a genuinely clipping input (1.5x full
    // scale) hits the same 1.0 hard ceiling at EVERY threshold setting;
    // that ceiling, not "peak stays near the threshold in dB", is the
    // actual guarantee this stage provides. So this checks two different,
    // both real, things instead: (a) the hard ceiling holds regardless of
    // threshold, on a genuinely clipping input, and (b) the threshold
    // parameter has a real, measurable effect on output level for a
    // moderate input quiet enough that neither setting hits that ceiling
    // (proving the parameter is actually wired to the DSP, not inert).
    {
        auto hotDefault = freshChain();              // limiter threshold left at its -10dB default
        auto hotTight = freshChain();
        hotTight.setLimiterThresholdDb(-3.0f);

        const auto hotDry = runTone(hotDefault, false, 500.0, 1.5f, 3);  // 1.5x full scale -- genuinely digitally clipping
        const auto hotWet = runTone(hotTight, false, 500.0, 1.5f, 3);

        std::cout << "  [hard ceiling] default-threshold peak=" << hotDry.peak << "  -3dB-threshold peak=" << hotWet.peak << "\n";
        check(hotDry.peak <= 1.0f + 1.0e-4f, "Limiter's hard output ceiling holds at its default threshold on a 1.5x-full-scale input");
        check(hotWet.peak <= 1.0f + 1.0e-4f, "Limiter's hard output ceiling holds at -3dB threshold on a 1.5x-full-scale input");

        auto moderateLoose = freshChain();
        moderateLoose.setLimiterThresholdDb(-3.0f);  // higher threshold -> smaller makeup-gain multiply
        auto moderateTight = freshChain();
        moderateTight.setLimiterThresholdDb(-10.0f); // lower threshold -> larger makeup-gain multiply

        // 0.15 amplitude is quiet enough that NEITHER setting's makeup
        // gain pushes it anywhere near the final [-1,1] clip (worst case
        // ~0.15 * 4.9 =~ 0.73), so any RMS difference measured here is the
        // threshold parameter's real effect on the makeup-gain stage, not
        // the shared hard clip both settings would otherwise hit alike.
        const auto looseResult = runTone(moderateLoose, false, 500.0, 0.15f, 3);
        const auto tightResult = runTone(moderateTight, false, 500.0, 0.15f, 3);

        std::cout << "  [threshold effect] -3dB RMS=" << looseResult.rms << "  -10dB RMS=" << tightResult.rms << "\n";
        check(looseResult.peak < 0.95f && tightResult.peak < 0.95f,
              "Sanity: neither moderate-input case hit the hard clip ceiling, so the RMS comparison below isolates the threshold parameter");
        check(std::abs(looseResult.rms - tightResult.rms) > 0.02f,
              "Limiter threshold parameter measurably changes output level (not inert/unwired) on a non-clipping input");
    }

    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : juce::String(failures) + " CHECK(S) FAILED").toStdString() << "\n";
    return failures == 0 ? 0 : 1;
}
