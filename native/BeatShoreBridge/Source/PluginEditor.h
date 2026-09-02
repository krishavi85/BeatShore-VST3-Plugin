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
    void applyResult(const BridgeAnalysisResult&); // shared by every trigger button's polling

    // Same pattern as analyzeButtonClicked()/transcribeButtonClicked() above
    // -- each just calls its own PluginProcessor::trigger*() method and
    // writes the immediate CaptureTriggerResult text into the label that
    // kind's eventual result lands in. Key/chords/loudness share
    // quickResultLabel (a plain ANALYSIS_RESULT, same shape as tempo's own);
    // drums/bass/lead share transcribeStatusLabel/transcribeDetailLabel --
    // the same MIDI_RESULT labels the existing Transcribe Piano/Guitar
    // button already writes to, since they're the identical result shape.
    void keyButtonClicked();
    void chordsButtonClicked();
    void loudnessButtonClicked();
    void drumsButtonClicked();
    void bassButtonClicked();
    void leadButtonClicked();

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

    // New: exposes three analysis kinds the desktop engine has always
    // supported (analyze.js's own SUPPORTED_KINDS) but that had no UI path
    // before this change. Same plain-ANALYSIS_RESULT shape as tempo, so
    // they share one result label rather than needing three -- clicking a
    // different one overwrites the last shown result, the same way
    // re-clicking Analyze Tempo already overwrites its own.
    juce::Label quickAnalysisSectionLabel;
    juce::TextButton keyButton, chordsButton, loudnessButton;
    juce::Label quickResultLabel;

    juce::Label transcribeSectionLabel;
    juce::TextButton transcribeButton;
    juce::TextButton drumsButton, bassButton, leadButton; // new -- same MIDI_RESULT shape/labels as transcribeButton above
    juce::Label transcribeStatusLabel;   // in-flight progress / terminal success-or-error state
    juce::Label transcribeDetailLabel;   // note count, processing time, algorithm, source
    juce::TextButton openExportFolderButton;
    juce::String lastMidiPath; // empty until a MIDI_RESULT with a written file arrives

    // Card-panel bounds computed once in resized(), read back in paint() to
    // draw the glowing background behind each section -- geometry only,
    // doesn't affect any control's actual bounds/hit-testing.
    juce::Rectangle<float> hostPanelBounds, bridgePanelBounds, quickAnalysisPanelBounds, transcribePanelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessorEditor)
};
