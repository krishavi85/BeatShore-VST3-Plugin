#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Minimal read-only status view: what the host is telling this plugin, and
// whether it's actually talking to BeatShore (it isn't yet — see
// BridgeStatus in PluginProcessor.h). This is UI plumbing to prove the host
// context reads work, not a design for the eventual real editor.
class BeatShoreBridgeAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                   private juce::Timer
{
public:
    explicit BeatShoreBridgeAudioProcessorEditor(BeatShoreBridgeAudioProcessor&);
    ~BeatShoreBridgeAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    static juce::String bridgeStatusText(BridgeStatus);
    void analyzeButtonClicked();
    void transcribeButtonClicked();
    void openExportFolderClicked();
    void applyResult(const BridgeAnalysisResult&); // shared by both trigger buttons' polling

    BeatShoreBridgeAudioProcessor& processor;

    juce::Label titleLabel;
    juce::Label hostSectionLabel;
    juce::Label sampleRateLabel, blockSizeLabel, tempoLabel, timeSigLabel, transportLabel, playheadLabel;
    juce::Label bridgeSectionLabel;
    juce::Label bridgeStatusLabel;
    juce::Label captureStatusLabel;
    juce::TextButton analyzeTempoButton;
    juce::Label analysisResultLabel;

    juce::Label transcribeSectionLabel;
    juce::TextButton transcribeButton;
    juce::Label transcribeStatusLabel;   // in-flight progress / terminal success-or-error state
    juce::Label transcribeDetailLabel;   // note count, processing time, algorithm, source
    juce::TextButton openExportFolderButton;
    juce::String lastMidiPath; // empty until a MIDI_RESULT with a written file arrives

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessorEditor)
};
