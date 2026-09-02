#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <memory>
#include <array>

// Sidebar-navigated reorganization of the editor, structurally modeled on
// the "BeatShore Reverse Studio" design blueprint the user shared (see
// STATUS.md's "Eighteenth"/"Nineteenth" sections) -- WITHOUT pretending any
// of that blueprint's unbuilt features (stem separation, spectral repair,
// sound matching, AI chat) exist. Five of the ten sidebar sections have
// real content: Overview (host context readouts), Transcribe (every
// analysis/transcription trigger this plugin actually has), Humanize
// (real, honest, non-ML parameterized randomization applied to whichever
// MIDI-producing kind is triggered next -- see dsp.applyHumanization() in
// beatshore-dsp.js), Mix (a real EQ + Compressor + Limiter -- see
// MixChain.h -- running live on this plugin's own audio in processBlock(),
// the first page whose controls are genuine host AudioProcessorParameters
// rather than a plugin-local setting), and Master (real EBU R128
// loudness/true-peak metering -- see MasterMeter.h, libebur128 -- of
// whatever this plugin is actually about to hand back to the host). The
// other five show an explicit, honest "not built yet" state -- never a
// working-looking control wired to nothing.
//
// Every juce::Label/juce::TextButton below is the SAME control this editor
// has always had, wired to the SAME PluginProcessor calls
// (triggerTempoAnalysis(), takeAnalysisResult(), getBridgeStatus(), etc.)
// the connection/protocol/regression testing has already exercised.
// Reorganizing which page a control appears on, and moving bridge
// connection status into a persistent header (visible regardless of page,
// matching how the blueprint keeps its own connection-style indicators in
// its header rather than buried in one tab), does not change what happens
// when a button is clicked or what a status label's text means.
// PluginProcessor.h/.cpp, BridgeClient.h, the protocol, and the desktop
// broker are untouched by the reorg itself; Mix and Master are the two
// pages that DO add new real state to PluginProcessor (MixChain + 6
// parameters; MasterMeter + a Snapshot), both entirely in processBlock()'s
// own audio path, not the analysis/broker path at all.
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

    // Called on every Humanize-page slider/toggle change -- reads all four
    // sliders + the toggle and pushes them to processor.setHumanizeSettings()
    // in one call, so the processor's copy is always exactly what the
    // sliders currently show (no partial-update drift between controls).
    void humanizeControlsChanged();

    // ---- Sidebar navigation -----------------------------------------
    // Overview/Transcribe are real pages; the other eight are the
    // blueprint's own remaining sections, each showing the shared
    // "not built yet" panel rather than any faked control. Order matches
    // the blueprint's own sidebar order.
    enum class Page { Overview, Separate, Repair, Transcribe, Reconstruct, Humanize, SoundMatch, Mix, Master, Export };
    static constexpr int kNumPages = 10;
    static const char* pageName(Page);
    static bool pageIsBuilt(Page); // true for Overview/Transcribe/Humanize/Mix/Master
    void showPage(Page);
    void updateControlVisibility(); // sets setVisible() on every page's controls to match currentPage -- geometry stays whatever resized() last computed for the active page

    Page currentPage = Page::Overview;
    std::array<juce::TextButton, kNumPages> navButtons;
    juce::Rectangle<float> sidebarBounds;

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

    // ---- Persistent header (every page) ------------------------------
    // Bridge connection status used to live inside a page-specific card;
    // moved here so it's visible no matter which sidebar section is
    // selected -- you need to know if BeatShore Desktop is reachable
    // regardless of what you're doing, the same reasoning the blueprint's
    // own header status indicators follow.
    juce::Label bridgeStatusLabel;

    // ---- Overview page ------------------------------------------------
    juce::Label hostSectionLabel;
    juce::Label sampleRateLabel, blockSizeLabel, tempoLabel, timeSigLabel, transportLabel, playheadLabel;

    // ---- Transcribe page -----------------------------------------------
    juce::Label bridgeSectionLabel; // kept on this page: "ready to trigger analysis" framing belongs with the triggers, not the persistent header
    juce::Label captureStatusLabel; // also belongs here, not Overview -- it's about whether a trigger button will do anything right now
    juce::TextButton analyzeTempoButton;
    juce::Label analysisResultLabel;

    juce::Label quickAnalysisSectionLabel;
    juce::TextButton keyButton, chordsButton, loudnessButton;
    juce::Label quickResultLabel;

    juce::Label transcribeSectionLabel;
    juce::TextButton transcribeButton;
    juce::TextButton drumsButton, bassButton, leadButton;
    juce::Label transcribeStatusLabel;   // in-flight progress / terminal success-or-error state
    juce::Label transcribeDetailLabel;   // note count, processing time, algorithm, source
    juce::TextButton openExportFolderButton;
    juce::String lastMidiPath; // empty until a MIDI_RESULT with a written file arrives

    // ---- Humanize page ---------------------------------------------
    // Real, parameterized note-level randomization (see
    // dsp.applyHumanization()'s own comment for exactly what each slider
    // does) -- NOT a learned humanization model, and NOT applied
    // retroactively to a result already on the Transcribe page: there is
    // no live-editable note buffer in this plugin, only "set your amounts
    // here, then go trigger a transcription."
    juce::Label humanizeSectionLabel, humanizeExplainerLabel;
    juce::Slider timingSlider, velocitySlider, dynamicsSlider, articulationSlider;
    juce::Label timingSliderLabel, velocitySliderLabel, dynamicsSliderLabel, articulationSliderLabel;
    juce::ToggleButton preserveGrooveToggle;

    // ---- Mix page -------------------------------------------------------
    // The first Mix-page feature that's a REAL AudioProcessorParameter, not
    // a plugin-local setting like Humanize's sliders -- so these bind via
    // juce::SliderParameterAttachment/ButtonParameterAttachment (declared in
    // juce_audio_processors' juce_ParameterAttachments.h, plain juce
    // namespace -- not juce::dsp, despite what the DSP chain itself lives
    // in) directly to the RangedAudioParameter&s PluginProcessor exposes
    // (getEqLowShelfGainParameter() etc.), the idiomatic JUCE mechanism:
    // host automation, undo, and value-changed sync are all handled by the
    // attachment object itself (it even pulls the slider's range/step from
    // the parameter's own NormalisableRange in its constructor), not
    // reimplemented with a slider listener and a manual
    // setHumanizeSettings()-style push like the Humanize page uses. See
    // MixChain.h for what these six numbers actually drive.
    juce::Label mixSectionLabel, mixExplainerLabel;
    juce::ToggleButton mixEnabledToggle;
    juce::Slider eqLowShelfSlider, eqMidPeakSlider, eqHighShelfSlider, compThresholdSlider, compRatioSlider, limiterThresholdSlider;
    juce::Label eqLowShelfLabel, eqMidPeakLabel, eqHighShelfLabel, compThresholdLabel, compRatioLabel, limiterThresholdLabel;
    std::unique_ptr<juce::ButtonParameterAttachment> mixEnabledAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> eqLowShelfAttachment, eqMidPeakAttachment, eqHighShelfAttachment,
                                                      compThresholdAttachment, compRatioAttachment, limiterThresholdAttachment;

    // ---- Master page -----------------------------------------------
    // Real EBU R128 loudness + true-peak metering (see MasterMeter.h,
    // libebur128) of this plugin's own final output -- read-only, unlike
    // every other real page: nothing here is a control the user sets,
    // it's live measurement of audio actually flowing through, refreshed
    // every timerCallback() tick the same way the Overview page's host
    // readouts are. resetMeterButton is the one interactive control:
    // clears Integrated LUFS and the true-peak hold back to "no
    // measurement yet" so a user can start a fresh reading mid-session.
    juce::Label masterSectionLabel, masterExplainerLabel;
    juce::Label momentaryLufsCaption, shortTermLufsCaption, integratedLufsCaption, truePeakCaption;
    juce::Label momentaryLufsLabel, shortTermLufsLabel, integratedLufsLabel, truePeakLabel;
    juce::TextButton resetMeterButton;

    // ---- Shared "not built yet" page (Separate/Repair/Reconstruct/
    // Sound Match/Export) -----------------------------------
    juce::Label comingSoonTitleLabel, comingSoonBodyLabel;

    // Card-panel bounds computed once in resized(), read back in paint() to
    // draw the glowing background behind each section -- geometry only,
    // doesn't affect any control's actual bounds/hit-testing.
    juce::Rectangle<float> hostPanelBounds, bridgePanelBounds, quickAnalysisPanelBounds, transcribePanelBounds, humanizePanelBounds, mixPanelBounds, masterPanelBounds, comingSoonPanelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessorEditor)
};
