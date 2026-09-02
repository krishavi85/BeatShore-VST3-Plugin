#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <memory>

// Futuristic reskin of the editor -- every control below is the SAME
// juce::Label/juce::TextButton this editor has always had, wired to the
// SAME PluginProcessor calls (triggerTempoAnalysis(), takeAnalysisResult(),
// getBridgeStatus(), etc.) that the connection/protocol/regression testing
// has already exercised. Nothing here changes what happens when a button is
// clicked or what a status label's text says -- only how it's drawn.
// PluginProcessor.h/.cpp, the protocol, and the desktop broker are
// untouched by this change.
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
    static juce::Colour bridgeStatusColour(BridgeStatus);
    void analyzeButtonClicked();
    void transcribeButtonClicked();
    void openExportFolderClicked();
    void applyResult(const BridgeAnalysisResult&); // shared by both trigger buttons' polling

    // Presentation-only helpers -- read state, draw pixels, touch nothing
    // in PluginProcessor beyond the same read-only accessors timerCallback()
    // already called before this reskin.
    void drawPanel(juce::Graphics&, juce::Rectangle<float> bounds, juce::Colour glowColour, float glowAmount) const;
    void drawStatusDot(juce::Graphics&, juce::Point<float> centre, juce::Colour colour, float pulsePhase) const;
    void drawBrandMark(juce::Graphics&, juce::Rectangle<float> bounds) const;

    BeatShoreBridgeAudioProcessor& processor;
    std::unique_ptr<juce::LookAndFeel_V4> futuristicLookAndFeel;
    double startTimeMs = 0.0; // wall-clock reference for the idle glow pulse -- purely decorative, no protocol/state meaning

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

    // Card-panel bounds computed once in resized(), read back in paint() to
    // draw the glowing background behind each section -- geometry only,
    // doesn't affect any control's actual bounds/hit-testing.
    juce::Rectangle<float> hostPanelBounds, bridgePanelBounds, transcribePanelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessorEditor)
};
