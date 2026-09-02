#pragma once
#include <juce_dsp/juce_dsp.h>

// Real-time EQ + Compressor + Limiter, run directly in
// PluginProcessor::processBlock() on the live audio actually flowing
// through this plugin in the host -- a fundamentally different kind of
// feature from Analyze/Transcribe/Humanize (all of which send a request to
// the desktop broker and get a result back later). This is the plugin's
// own audio-thread signal processing, using JUCE's own production dsp
// module classes (juce::dsp::IIR::Filter, Compressor, Limiter) -- the same
// building blocks real mixing/mastering plugins use, not hand-rolled math.
//
// Chain order: 3-band EQ (low shelf / mid peak / high shelf, fixed centre
// frequencies) -> Compressor -> Limiter. Deliberately simplified relative
// to a fully parametric multi-node EQ or a compressor with exposed attack/
// release -- matches what the "Mix" page in the design blueprint actually
// shows (one EQ curve, one COMP knob, one LIMIT knob), not a claim that
// this is a complete mastering suite. See PluginProcessor.h/.cpp for how
// this is wired to real AudioProcessorParameters and a real bypass toggle.
class MixChain
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        chain.prepare(spec);
        updateEQ();
        chain.get<compIndex>().setAttack(15.0f);
        chain.get<compIndex>().setRelease(120.0f);
        chain.get<limiterIndex>().setRelease(50.0f);
    }

    void reset() { chain.reset(); }

    // bypass: true = passthrough, exactly the audio this plugin already
    // passed through before this feature existed -- a real bypass via
    // ProcessContextReplacing::isBypassed (every processor in the chain
    // still gets called, so its internal state/envelopes don't jump when
    // bypass is released, they just aren't applied to the signal), not
    // merely "set every gain to unity". Matches the design blueprint's own
    // Original/Reconstruction A/B toggle.
    void process(juce::dsp::AudioBlock<float>& block, bool bypass)
    {
        juce::dsp::ProcessContextReplacing<float> context(block);
        context.isBypassed = bypass;
        chain.process(context);
    }

    // Setters take real-world units (dB, ratio, ms) throughout, matching
    // what a real mixing plugin's own parameters would be in. Cheap enough
    // to call every block from processBlock() itself (see
    // PluginProcessor.cpp) -- recomputing IIR coefficients per block is
    // the standard, accepted cost of parameter-smoothing-free EQ; not
    // click-free under automation, but stable for a slider a user is
    // dragging by hand, which is this feature's actual scope.
    void setEqLowShelfGainDb(float db)  { eqLowGainDb = db;  updateEQ(); }
    void setEqMidPeakGainDb(float db)   { eqMidGainDb = db;  updateEQ(); }
    void setEqHighShelfGainDb(float db) { eqHighGainDb = db; updateEQ(); }
    void setCompressorThresholdDb(float db) { chain.get<compIndex>().setThreshold(db); }
    void setCompressorRatio(float ratio)    { chain.get<compIndex>().setRatio(juce::jmax(1.0f, ratio)); }
    void setLimiterThresholdDb(float db)    { chain.get<limiterIndex>().setThreshold(db); }

private:
    void updateEQ()
    {
        if (sampleRate <= 0.0) return; // not yet prepared -- setters may legitimately be called before prepare() (initial parameter sync)
        // Dereference-and-copy into the EXISTING shared Coefficients
        // object (not `chain.get<..>().state = newPtr`) -- the standard
        // JUCE idiom for updating IIR coefficients live: ProcessorDuplicator's
        // per-channel mono Filter instances each hold their own reference
        // to the SAME Coefficients object, so mutating its contents in
        // place is what makes the update actually reach already-prepared
        // channels; replacing the top-level Ptr would not.
        *chain.get<lowShelfIndex>().state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            sampleRate, 150.0f, 0.7f, juce::Decibels::decibelsToGain(eqLowGainDb));
        *chain.get<midPeakIndex>().state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            sampleRate, 1000.0f, 0.9f, juce::Decibels::decibelsToGain(eqMidGainDb));
        *chain.get<highShelfIndex>().state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            sampleRate, 6000.0f, 0.7f, juce::Decibels::decibelsToGain(eqHighGainDb));
    }

    using StereoFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    enum { lowShelfIndex, midPeakIndex, highShelfIndex, compIndex, limiterIndex };
    juce::dsp::ProcessorChain<StereoFilter, StereoFilter, StereoFilter,
                               juce::dsp::Compressor<float>, juce::dsp::Limiter<float>> chain;

    double sampleRate = 0.0;
    float eqLowGainDb = 0.0f, eqMidGainDb = 0.0f, eqHighGainDb = 0.0f;
};
