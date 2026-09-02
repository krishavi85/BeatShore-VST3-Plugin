// Direct proof that getStateInformation()/setStateInformation() actually
// round-trip real plugin state -- the bug this test exists to catch was
// found live, in REAPER: getStateInformation() was a placeholder that
// saved nothing (a leftover from before any real parameters existed),
// so an offline render (which builds/restores a fresh plugin instance)
// silently reverted to every parameter's default (Mix Enabled = false)
// no matter what the live editing instance's knobs showed. That would
// have broken REAPER project save/reload identically, for the same
// reason -- this test exercises exactly that save-into-a-fresh-instance
// path, not just "does it compile".
//
// Compiles the REAL PluginProcessor.cpp/PluginEditor.cpp (unmodified),
// same pattern as BridgeStressTest -- not a reimplementation of the
// state logic.
#include "../../BeatShoreBridge/Source/PluginProcessor.h"
#include <iostream>

namespace
{
    int failures = 0;
    void check(bool condition, const juce::String& description)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << description.toStdString() << "\n";
        if (!condition) ++failures;
    }

    bool approxEqual(float a, float b, float tolerance = 0.01f)
    {
        return std::abs(a - b) <= tolerance;
    }
}

int main()
{
    std::cout << "=== State round-trip verification (getStateInformation/setStateInformation) ===\n\n";

    // Set up a real processor, give it non-default values on every real
    // parameter plus humanizeSettings, save its state, then apply that
    // saved state to a SECOND, freshly-constructed instance -- exactly
    // what a host does for an offline render or a project reload (a new
    // instance, not the live one, receiving setStateInformation()).
    BeatShoreBridgeAudioProcessor original;

    original.getMixEnabledParameter().setValueNotifyingHost(1.0f); // AudioParameterBool: 1.0 = true
    // setValueNotifyingHost takes normalized 0..1; convert real-world
    // target values through each parameter's own range so this test
    // doesn't hardcode the ranges MixChain/PluginProcessor.cpp already
    // define -- if those ever change, this test keeps testing "does a
    // real, distinctive value survive", not a stale hardcoded number.
    auto setReal = [](juce::RangedAudioParameter& param, float realValue)
    {
        param.setValueNotifyingHost(param.convertTo0to1(realValue));
    };
    setReal(original.getEqLowShelfGainParameter(), 2.0f);
    setReal(original.getEqMidPeakGainParameter(), -1.5f);
    setReal(original.getEqHighShelfGainParameter(), 3.0f);
    setReal(original.getCompThresholdParameter(), -18.0f);
    setReal(original.getCompRatioParameter(), 2.5f);
    setReal(original.getLimiterThresholdParameter(), -3.0f);

    HumanizeSettings humanize;
    humanize.timing = 0.7f;
    humanize.velocity = 0.4f;
    humanize.dynamics = 0.6f;
    humanize.articulation = 0.3f;
    humanize.preserveGroove = true;
    original.setHumanizeSettings(humanize);

    juce::MemoryBlock savedState;
    original.getStateInformation(savedState);
    check(savedState.getSize() > 0, "getStateInformation() produces non-empty data");

    // A second, independent instance -- NOT the one whose knobs were just
    // turned. This is the part the old placeholder implementation could
    // never have passed: there was nothing in savedState to restore.
    BeatShoreBridgeAudioProcessor restored;
    restored.setStateInformation(savedState.getData(), static_cast<int>(savedState.getSize()));

    check(restored.getMixEnabledParameter().getValue() > 0.5f, "Mix Enabled restored to true on a fresh instance");
    check(approxEqual(restored.getEqLowShelfGainParameter().convertFrom0to1(restored.getEqLowShelfGainParameter().getValue()), 2.0f),
          "EQ Low Shelf Gain restored to 2.0dB");
    check(approxEqual(restored.getEqMidPeakGainParameter().convertFrom0to1(restored.getEqMidPeakGainParameter().getValue()), -1.5f),
          "EQ Mid Peak Gain restored to -1.5dB");
    check(approxEqual(restored.getEqHighShelfGainParameter().convertFrom0to1(restored.getEqHighShelfGainParameter().getValue()), 3.0f),
          "EQ High Shelf Gain restored to 3.0dB");
    check(approxEqual(restored.getCompThresholdParameter().convertFrom0to1(restored.getCompThresholdParameter().getValue()), -18.0f),
          "Compressor Threshold restored to -18.0dB");
    check(approxEqual(restored.getCompRatioParameter().convertFrom0to1(restored.getCompRatioParameter().getValue()), 2.5f),
          "Compressor Ratio restored to 2.5");
    check(approxEqual(restored.getLimiterThresholdParameter().convertFrom0to1(restored.getLimiterThresholdParameter().getValue()), -3.0f),
          "Limiter Threshold restored to -3.0dB");

    const auto restoredHumanize = restored.getHumanizeSettings();
    check(approxEqual(restoredHumanize.timing, 0.7f), "Humanize timing restored to 0.7");
    check(approxEqual(restoredHumanize.velocity, 0.4f), "Humanize velocity restored to 0.4");
    check(approxEqual(restoredHumanize.dynamics, 0.6f), "Humanize dynamics restored to 0.6");
    check(approxEqual(restoredHumanize.articulation, 0.3f), "Humanize articulation restored to 0.3");
    check(restoredHumanize.preserveGroove == true, "Humanize preserveGroove restored to true");

    // Sanity check the OTHER direction too: a completely fresh instance
    // that never received setStateInformation() at all must still show
    // real defaults (Mix Enabled = false), not accidentally "always on"
    // from some static/global state leaking between instances.
    BeatShoreBridgeAudioProcessor untouched;
    check(untouched.getMixEnabledParameter().getValue() < 0.5f, "A fresh instance with no restored state still defaults Mix Enabled to false (no cross-instance leakage)");

    std::cout << "\n" << (failures == 0 ? "ALL CHECKS PASSED" : juce::String(failures) + " CHECK(S) FAILED").toStdString() << "\n";
    return failures == 0 ? 0 : 1;
}
