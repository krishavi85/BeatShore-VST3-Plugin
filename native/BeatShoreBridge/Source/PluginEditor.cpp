#include "PluginEditor.h"

// ---------------------------------------------------------------------------
// Futuristic palette. Built AROUND the existing brand accent (0xff9184d9,
// the violet already used for section headings before this reskin) rather
// than replacing it -- a cyan/teal secondary accent marks "live/connected"
// state, everything else is the same violet-on-near-black identity this
// project already had, just with glow, gradient, and card panels added.
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t kBgTop      = 0xff0a0e1c;
    constexpr uint32_t kBgBottom   = 0xff141c33;
    constexpr uint32_t kPanelFill  = 0xd9161b2e; // ~85% alpha
    constexpr uint32_t kAccent     = 0xff9184d9; // unchanged brand violet
    constexpr uint32_t kAccentDim  = 0x669184d9;
    constexpr uint32_t kLive       = 0xff4dd9c9; // cyan-teal "connected/live" accent
    constexpr uint32_t kWarn       = 0xffffd166; // unchanged -- was already bridgeStatusLabel's default colour
    constexpr uint32_t kError      = 0xffff6b6b;
    constexpr uint32_t kTextPrime  = 0xffe9e9ed; // unchanged
    constexpr uint32_t kTextDim    = 0xff8a8d9a; // unchanged
    constexpr uint32_t kTextFaint  = 0xff55586b;

    void styleHeading(juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
        l.setColour(juce::Label::textColourId, juce::Colour(kAccent));
    }

    void styleValue(juce::Label& l)
    {
        l.setFont(juce::Font(juce::FontOptions(13.5f)));
        l.setColour(juce::Label::textColourId, juce::Colour(kTextPrime));
    }

    // Cheap glow approximation: stroke the same rounded rect several times
    // with growing size and shrinking alpha, rather than a real blur --
    // fast enough to redraw at the timer's 10Hz without any offscreen
    // buffering.
    void drawGlowRoundedRect(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize,
                              juce::Colour glowColour, float glowRadius, float coreAlpha = 0.9f)
    {
        for (float r = glowRadius; r > 0.0f; r -= 1.0f)
        {
            const float alpha = coreAlpha * (1.0f - r / glowRadius) * 0.35f;
            g.setColour(glowColour.withAlpha(alpha));
            g.drawRoundedRectangle(bounds.expanded(r), cornerSize + r, 1.0f);
        }
        g.setColour(glowColour.withAlpha(coreAlpha));
        g.drawRoundedRectangle(bounds, cornerSize, 1.2f);
    }

    // A custom LookAndFeel_V4 for every TextButton this editor uses --
    // rounded, gradient-filled, glowing on hover, dimmed when disabled.
    // getToggleState() picks a stronger, solid-filled variant, used only by
    // the sidebar nav buttons (see showPage()) to mark the active page --
    // every trigger button (Analyze Tempo, Transcribe, Key, etc.) is never
    // toggled, so this is a purely additive rendering path: their look is
    // completely unchanged from before the sidebar existed. Button
    // identity (onClick, text, enabled state) is set on the juce::TextButton
    // objects themselves in the constructor, exactly as before; this class
    // only changes how those same buttons are painted.
    class FuturisticLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                   bool isHighlighted, bool isDown) override
        {
            auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
            const float corner = 6.0f;

            if (!button.isEnabled())
            {
                g.setColour(juce::Colour(0xff1a1d2c));
                g.fillRoundedRectangle(bounds, corner);
                g.setColour(juce::Colour(0xff33374a));
                g.drawRoundedRectangle(bounds, corner, 1.0f);
                return;
            }

            const juce::Colour accent(kAccent);

            if (button.getToggleState())
            {
                // Active sidebar nav item: solid fill, not just a tinted
                // gradient -- reads as "you are here" at a glance among
                // nine other list-style buttons.
                g.setColour(accent.withAlpha(0.85f));
                g.fillRoundedRectangle(bounds, corner);
                drawGlowRoundedRect(g, bounds, corner, accent, 4.0f, 0.9f);
                return;
            }

            juce::ColourGradient fill(accent.withAlpha(isDown ? 0.35f : (isHighlighted ? 0.30f : 0.20f)),
                                       bounds.getTopLeft(),
                                       accent.darker(isDown ? 0.6f : 0.4f).withAlpha(0.55f),
                                       bounds.getBottomRight(), false);
            g.setGradientFill(fill);
            g.fillRoundedRectangle(bounds, corner);

            drawGlowRoundedRect(g, bounds, corner, accent, isHighlighted || isDown ? 5.0f : 2.5f,
                                 isDown ? 1.0f : (isHighlighted ? 0.85f : 0.55f));
        }

        void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
        {
            g.setFont(getTextButtonFont(button, button.getHeight()));
            g.setColour(!button.isEnabled() ? juce::Colour(kTextFaint)
                                             : (button.getToggleState() ? juce::Colour(0xff0a0e1c) : juce::Colour(kTextPrime)));
            g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(6, 0),
                              juce::Justification::centred, 2);
        }

        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
        {
            return juce::Font(juce::FontOptions(juce::jmin(14.0f, buttonHeight * 0.5f), juce::Font::bold));
        }
    };
}

BeatShoreBridgeAudioProcessorEditor::BeatShoreBridgeAudioProcessorEditor(BeatShoreBridgeAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    futuristicLookAndFeel = std::make_unique<FuturisticLookAndFeel>();
    setLookAndFeel(futuristicLookAndFeel.get());
    startTimeMs = juce::Time::getMillisecondCounterHiRes();

    // Same title text as before this reskin -- only its font/colour change.
    titleLabel.setText("BeatShore Bridge -- thin passthrough + host context reader", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.5f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(kTextPrime));
    addAndMakeVisible(titleLabel);

    // --- Persistent header: bridge connection status, every page -------
    styleValue(bridgeStatusLabel);
    bridgeStatusLabel.setColour(juce::Label::textColourId, juce::Colour(kWarn)); // same default as before -- timerCallback() overwrites with the live-status colour
    bridgeStatusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(bridgeStatusLabel);

    // --- Sidebar nav -----------------------------------------------------
    for (int i = 0; i < kNumPages; ++i)
    {
        const auto page = static_cast<Page>(i);
        navButtons[size_t(i)].setButtonText(pageName(page));
        navButtons[size_t(i)].setClickingTogglesState(false); // toggle state is driven by showPage(), not click-to-toggle
        navButtons[size_t(i)].onClick = [this, page] { showPage(page); };
        addAndMakeVisible(navButtons[size_t(i)]);
    }

    // --- Overview page ---------------------------------------------------
    styleHeading(hostSectionLabel, "HOST CONTEXT");
    addAndMakeVisible(hostSectionLabel);

    for (auto* label : { &sampleRateLabel, &blockSizeLabel, &tempoLabel, &timeSigLabel, &transportLabel, &playheadLabel })
    {
        styleValue(*label);
        addAndMakeVisible(*label);
    }

    // --- Transcribe page ---------------------------------------------------
    styleHeading(bridgeSectionLabel, "ANALYZE TEMPO");
    addAndMakeVisible(bridgeSectionLabel);

    styleValue(captureStatusLabel);
    captureStatusLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    captureStatusLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    addAndMakeVisible(captureStatusLabel);

    // Same button text, same onClick target -- analyzeButtonClicked() below
    // is byte-for-byte the same function that already called
    // processor.triggerTempoAnalysis() before this reskin.
    analyzeTempoButton.setButtonText("Analyze Tempo (last 10s captured)");
    analyzeTempoButton.onClick = [this] { analyzeButtonClicked(); };
    addAndMakeVisible(analyzeTempoButton);

    styleValue(analysisResultLabel);
    analysisResultLabel.setText("No analysis run yet.", juce::dontSendNotification);
    addAndMakeVisible(analysisResultLabel);

    styleHeading(quickAnalysisSectionLabel, "QUICK ANALYSIS");
    addAndMakeVisible(quickAnalysisSectionLabel);

    keyButton.setButtonText("Key");
    keyButton.onClick = [this] { keyButtonClicked(); };
    addAndMakeVisible(keyButton);

    chordsButton.setButtonText("Chords");
    chordsButton.onClick = [this] { chordsButtonClicked(); };
    addAndMakeVisible(chordsButton);

    loudnessButton.setButtonText("Loudness");
    loudnessButton.onClick = [this] { loudnessButtonClicked(); };
    addAndMakeVisible(loudnessButton);

    styleValue(quickResultLabel);
    quickResultLabel.setText("No analysis run yet.", juce::dontSendNotification);
    addAndMakeVisible(quickResultLabel);

    styleHeading(transcribeSectionLabel, "TRANSCRIPTION (PIANO / GUITAR / DRUMS / BASS)");
    addAndMakeVisible(transcribeSectionLabel);

    transcribeButton.setButtonText("Transcribe Piano/Guitar (last 10s captured)");
    transcribeButton.onClick = [this] { transcribeButtonClicked(); };
    addAndMakeVisible(transcribeButton);

    drumsButton.setButtonText("Transcribe Drums");
    drumsButton.onClick = [this] { drumsButtonClicked(); };
    addAndMakeVisible(drumsButton);

    bassButton.setButtonText("Transcribe Bass");
    bassButton.onClick = [this] { bassButtonClicked(); };
    addAndMakeVisible(bassButton);

    leadButton.setButtonText("Transcribe Lead");
    leadButton.onClick = [this] { leadButtonClicked(); };
    addAndMakeVisible(leadButton);

    styleValue(transcribeStatusLabel);
    transcribeStatusLabel.setText("No transcription run yet.", juce::dontSendNotification);
    addAndMakeVisible(transcribeStatusLabel);

    styleValue(transcribeDetailLabel);
    transcribeDetailLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    transcribeDetailLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    addAndMakeVisible(transcribeDetailLabel);

    openExportFolderButton.setButtonText("Open Export Folder");
    openExportFolderButton.setEnabled(false);
    openExportFolderButton.onClick = [this] { openExportFolderClicked(); };
    addAndMakeVisible(openExportFolderButton);

    // --- Shared "not built yet" page ---------------------------------
    styleHeading(comingSoonTitleLabel, "NOT BUILT YET");
    comingSoonTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(comingSoonTitleLabel);

    styleValue(comingSoonBodyLabel);
    comingSoonBodyLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    comingSoonBodyLabel.setJustificationType(juce::Justification::centred);
    comingSoonBodyLabel.setFont(juce::Font(juce::FontOptions(12.5f)));
    addAndMakeVisible(comingSoonBodyLabel);

    // --- Humanize page ---------------------------------------------
    styleHeading(humanizeSectionLabel, "HUMANIZE (APPLIES TO YOUR NEXT TRANSCRIPTION)");
    addAndMakeVisible(humanizeSectionLabel);

    styleValue(humanizeExplainerLabel);
    humanizeExplainerLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    humanizeExplainerLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    humanizeExplainerLabel.setText(
        "Real, parameterized randomization (not a learned model) applied to note timing, "
        "velocity, dynamics, and articulation. Set these, then trigger a transcription on the "
        "Transcribe page -- it is NOT retroactive to a result you already have.",
        juce::dontSendNotification);
    humanizeExplainerLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(humanizeExplainerLabel);

    auto setupHumanizeSlider = [this](juce::Slider& slider, juce::Label& label, const juce::String& name)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setRange(0.0, 100.0, 1.0);
        slider.setValue(0.0, juce::dontSendNotification);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
        slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(kAccent));
        slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(kAccentDim));
        slider.setColour(juce::Slider::thumbColourId, juce::Colour(kLive));
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colour(kTextPrime));
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.onValueChange = [this] { humanizeControlsChanged(); };
        addAndMakeVisible(slider);

        styleHeading(label, name);
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
    };
    setupHumanizeSlider(timingSlider, timingSliderLabel, "TIMING");
    setupHumanizeSlider(velocitySlider, velocitySliderLabel, "VELOCITY");
    setupHumanizeSlider(dynamicsSlider, dynamicsSliderLabel, "DYNAMICS");
    setupHumanizeSlider(articulationSlider, articulationSliderLabel, "ARTICULATION");

    preserveGrooveToggle.setButtonText("Preserve Groove (smaller, non-drifting timing jitter)");
    preserveGrooveToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(kTextPrime));
    preserveGrooveToggle.setColour(juce::ToggleButton::tickColourId, juce::Colour(kLive));
    preserveGrooveToggle.onClick = [this] { humanizeControlsChanged(); };
    addAndMakeVisible(preserveGrooveToggle);

    // Reflect whatever's actually in the processor right now -- covers
    // both a genuinely restored project (setStateInformation() having
    // just run before the editor was created) and the more common case
    // of simply closing and reopening this plugin's own window mid-
    // session, which constructs a brand new Editor against the SAME
    // still-alive Processor. Without this, both cases would silently
    // show all-zero sliders while the processor (and therefore whatever
    // transcription runs next) was still using the real, non-zero
    // amounts underneath -- the sliders would lie about the plugin's
    // actual state. dontSendNotification: this is populating the UI FROM
    // existing state, not a user edit that should push a value back into
    // setHumanizeSettings() (which would be a harmless no-op here anyway,
    // but sending notifications for four sliders + a toggle during
    // construction is needless work).
    {
        const auto restored = processor.getHumanizeSettings();
        timingSlider.setValue(restored.timing * 100.0, juce::dontSendNotification);
        velocitySlider.setValue(restored.velocity * 100.0, juce::dontSendNotification);
        dynamicsSlider.setValue(restored.dynamics * 100.0, juce::dontSendNotification);
        articulationSlider.setValue(restored.articulation * 100.0, juce::dontSendNotification);
        preserveGrooveToggle.setToggleState(restored.preserveGroove, juce::dontSendNotification);
    }

    // --- Mix page ---------------------------------------------------
    // A real 3-band EQ + Compressor + Limiter running in processBlock() on
    // the live audio through this plugin (see MixChain.h) -- the first Mix
    // page control set bound to genuine AudioProcessorParameters rather
    // than a plugin-local setting, so every control here is constructed
    // first and then handed to a *ParameterAttachment, which is what
    // actually sets its range and keeps it in sync with the host
    // parameter (see sendInitialUpdate() below) -- not slider.setRange()
    // by hand the way the Humanize knobs above are.
    styleHeading(mixSectionLabel, "MIX (EQ / COMPRESSOR / LIMITER -- LIVE ON THIS PLUGIN'S AUDIO)");
    addAndMakeVisible(mixSectionLabel);

    styleValue(mixExplainerLabel);
    mixExplainerLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    mixExplainerLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    mixExplainerLabel.setText(
        "Real juce::dsp EQ/Compressor/Limiter processing this plugin's own audio in the host, right "
        "now -- not a request to BeatShore Desktop like every other page. Fixed band centres (150 Hz "
        "/ 1 kHz / 6 kHz shelf+peak) and fixed compressor attack/release; every gain, threshold, and "
        "ratio below is a real host-automatable parameter.",
        juce::dontSendNotification);
    mixExplainerLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(mixExplainerLabel);

    mixEnabledToggle.setButtonText("Mix Enabled (unticked = bypass, original signal passes through unchanged)");
    mixEnabledToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(kTextPrime));
    mixEnabledToggle.setColour(juce::ToggleButton::tickColourId, juce::Colour(kLive));
    addAndMakeVisible(mixEnabledToggle);
    mixEnabledAttachment = std::make_unique<juce::ButtonParameterAttachment>(processor.getMixEnabledParameter(), mixEnabledToggle);
    mixEnabledAttachment->sendInitialUpdate();

    auto setupMixKnob = [this](juce::Slider& slider, juce::Label& label, const juce::String& name,
                                std::unique_ptr<juce::SliderParameterAttachment>& attachment, juce::RangedAudioParameter& param)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        slider.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(kLive));
        slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(kAccentDim));
        slider.setColour(juce::Slider::thumbColourId, juce::Colour(kAccent));
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colour(kTextPrime));
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(slider);

        styleHeading(label, name);
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);

        // Constructed AFTER the slider is fully styled and visible -- its
        // constructor reads param.getNormalisableRange()/getDefaultValue()
        // to set the slider's actual range/step/double-click-reset value,
        // so it must run after setSliderStyle()/setTextBoxStyle() (which
        // don't touch range) but is otherwise order-independent of styling.
        attachment = std::make_unique<juce::SliderParameterAttachment>(param, slider);
        attachment->sendInitialUpdate();
    };
    setupMixKnob(eqLowShelfSlider, eqLowShelfLabel, "LOW SHELF", eqLowShelfAttachment, processor.getEqLowShelfGainParameter());
    setupMixKnob(eqMidPeakSlider, eqMidPeakLabel, "MID PEAK", eqMidPeakAttachment, processor.getEqMidPeakGainParameter());
    setupMixKnob(eqHighShelfSlider, eqHighShelfLabel, "HIGH SHELF", eqHighShelfAttachment, processor.getEqHighShelfGainParameter());
    setupMixKnob(compThresholdSlider, compThresholdLabel, "COMP THRESH", compThresholdAttachment, processor.getCompThresholdParameter());
    setupMixKnob(compRatioSlider, compRatioLabel, "COMP RATIO", compRatioAttachment, processor.getCompRatioParameter());
    setupMixKnob(limiterThresholdSlider, limiterThresholdLabel, "LIMITER", limiterThresholdAttachment, processor.getLimiterThresholdParameter());

    // --- Master page ---------------------------------------------------
    // Real, read-only EBU R128 loudness/true-peak metering (see
    // MasterMeter.h, libebur128) of this plugin's own final output --
    // every readout below is overwritten every timerCallback() tick from
    // processor.getMasterSnapshot(), the same live-refresh pattern the
    // Overview page's host-context labels already use.
    styleHeading(masterSectionLabel, "MASTER (REAL EBU R128 LOUDNESS + TRUE PEAK METERING)");
    addAndMakeVisible(masterSectionLabel);

    styleValue(masterExplainerLabel);
    masterExplainerLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    masterExplainerLabel.setColour(juce::Label::textColourId, juce::Colour(kTextDim));
    masterExplainerLabel.setText(
        "Real libebur128 measurement of this plugin's own output (after Mix, if Mix is enabled) -- "
        "not a request to BeatShore Desktop. Integrated LUFS and the true-peak hold accumulate from "
        "when playback started (or the last Reset), same as a real mastering meter.",
        juce::dontSendNotification);
    masterExplainerLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(masterExplainerLabel);

    auto setupMeterReadout = [this](juce::Label& caption, juce::Label& value, const juce::String& captionText)
    {
        styleHeading(caption, captionText);
        caption.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(caption);

        value.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
        value.setColour(juce::Label::textColourId, juce::Colour(kLive));
        value.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(value);
    };
    setupMeterReadout(momentaryLufsCaption, momentaryLufsLabel, "MOMENTARY");
    setupMeterReadout(shortTermLufsCaption, shortTermLufsLabel, "SHORT-TERM");
    setupMeterReadout(integratedLufsCaption, integratedLufsLabel, "INTEGRATED");
    setupMeterReadout(truePeakCaption, truePeakLabel, "TRUE PEAK");

    resetMeterButton.setButtonText("Reset Meter");
    resetMeterButton.onClick = [this] { processor.requestMasterMeterReset(); };
    addAndMakeVisible(resetMeterButton);

    // Wider than the single-column layout this replaced -- room for the
    // sidebar alongside a page's content. Height sized for the taller of
    // the real pages (Transcribe); Overview/Humanize/the placeholder pages
    // simply don't fill it.
    setSize(760, 700);
    showPage(Page::Overview);
    startTimerHz(10);
    timerCallback();
}

BeatShoreBridgeAudioProcessorEditor::~BeatShoreBridgeAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

const char* BeatShoreBridgeAudioProcessorEditor::pageName(Page page)
{
    switch (page)
    {
        case Page::Overview:    return "Overview";
        case Page::Separate:    return "Separate";
        case Page::Repair:      return "Repair";
        case Page::Transcribe:  return "Transcribe";
        case Page::Reconstruct: return "Reconstruct";
        case Page::Humanize:    return "Humanize";
        case Page::SoundMatch:  return "Sound Match";
        case Page::Mix:         return "Mix";
        case Page::Master:      return "Master";
        case Page::Export:      return "Export";
    }
    return "Unknown";
}

bool BeatShoreBridgeAudioProcessorEditor::pageIsBuilt(Page page)
{
    return page == Page::Overview || page == Page::Transcribe || page == Page::Humanize || page == Page::Mix || page == Page::Master;
}

void BeatShoreBridgeAudioProcessorEditor::showPage(Page page)
{
    currentPage = page;
    for (int i = 0; i < kNumPages; ++i)
        navButtons[size_t(i)].setToggleState(static_cast<Page>(i) == currentPage, juce::dontSendNotification);

    if (!pageIsBuilt(page))
    {
        comingSoonBodyLabel.setText(
            juce::String(pageName(page)) + " is part of the BeatShore Reverse Studio design, not built in this "
            "plugin yet -- see STATUS.md's \"Eighteenth\" section for what that would actually take "
            "(most of it needs real trained ML models, not just UI wiring).",
            juce::dontSendNotification);
    }

    updateControlVisibility();
    resized();
    repaint();
}

void BeatShoreBridgeAudioProcessorEditor::updateControlVisibility()
{
    const bool isOverview = currentPage == Page::Overview;
    const bool isTranscribe = currentPage == Page::Transcribe;
    const bool isHumanize = currentPage == Page::Humanize;
    const bool isMix = currentPage == Page::Mix;
    const bool isMaster = currentPage == Page::Master;
    const bool isComingSoon = !isOverview && !isTranscribe && !isHumanize && !isMix && !isMaster;

    for (auto* c : { &hostSectionLabel, &sampleRateLabel, &blockSizeLabel, &tempoLabel, &timeSigLabel, &transportLabel, &playheadLabel })
        c->setVisible(isOverview);

    for (auto* c : std::initializer_list<juce::Component*>{
             &bridgeSectionLabel, &captureStatusLabel, &analyzeTempoButton, &analysisResultLabel,
             &quickAnalysisSectionLabel, &keyButton, &chordsButton, &loudnessButton, &quickResultLabel,
             &transcribeSectionLabel, &transcribeButton, &drumsButton, &bassButton, &leadButton,
             &transcribeStatusLabel, &transcribeDetailLabel, &openExportFolderButton })
        c->setVisible(isTranscribe);

    for (auto* c : std::initializer_list<juce::Component*>{
             &humanizeSectionLabel, &humanizeExplainerLabel,
             &timingSlider, &timingSliderLabel, &velocitySlider, &velocitySliderLabel,
             &dynamicsSlider, &dynamicsSliderLabel, &articulationSlider, &articulationSliderLabel,
             &preserveGrooveToggle })
        c->setVisible(isHumanize);

    for (auto* c : std::initializer_list<juce::Component*>{
             &mixSectionLabel, &mixExplainerLabel, &mixEnabledToggle,
             &eqLowShelfSlider, &eqLowShelfLabel, &eqMidPeakSlider, &eqMidPeakLabel,
             &eqHighShelfSlider, &eqHighShelfLabel, &compThresholdSlider, &compThresholdLabel,
             &compRatioSlider, &compRatioLabel, &limiterThresholdSlider, &limiterThresholdLabel })
        c->setVisible(isMix);

    for (auto* c : std::initializer_list<juce::Component*>{
             &masterSectionLabel, &masterExplainerLabel,
             &momentaryLufsCaption, &shortTermLufsCaption, &integratedLufsCaption, &truePeakCaption,
             &momentaryLufsLabel, &shortTermLufsLabel, &integratedLufsLabel, &truePeakLabel,
             &resetMeterButton })
        c->setVisible(isMaster);

    comingSoonTitleLabel.setVisible(isComingSoon);
    comingSoonBodyLabel.setVisible(isComingSoon);
}

void BeatShoreBridgeAudioProcessorEditor::humanizeControlsChanged()
{
    HumanizeSettings settings;
    settings.timing = float(timingSlider.getValue() / 100.0);
    settings.velocity = float(velocitySlider.getValue() / 100.0);
    settings.dynamics = float(dynamicsSlider.getValue() / 100.0);
    settings.articulation = float(articulationSlider.getValue() / 100.0);
    settings.preserveGroove = preserveGrooveToggle.getToggleState();
    processor.setHumanizeSettings(settings);
}

void BeatShoreBridgeAudioProcessorEditor::drawPanel(juce::Graphics& g, juce::Rectangle<float> bounds,
                                                      juce::Colour glowColour, float glowAmount) const
{
    g.setColour(juce::Colour(kPanelFill));
    g.fillRoundedRectangle(bounds, 8.0f);
    drawGlowRoundedRect(g, bounds, 8.0f, glowColour, glowAmount, 0.5f);
}

void BeatShoreBridgeAudioProcessorEditor::drawStatusDot(juce::Graphics& g, juce::Point<float> centre,
                                                           juce::Colour colour, float pulsePhase) const
{
    const float pulse = 0.5f + 0.5f * std::sin(pulsePhase);
    for (float r = 7.0f; r > 1.5f; r -= 1.0f)
    {
        const float alpha = (0.10f + 0.10f * pulse) * (1.0f - (r - 1.5f) / 5.5f);
        g.setColour(colour.withAlpha(alpha));
        g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(centre));
    }
    g.setColour(colour);
    g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(centre));
}

void BeatShoreBridgeAudioProcessorEditor::drawBrandMark(juce::Graphics& g, juce::Rectangle<float> bounds) const
{
    // Abstract waveform/chevron mark, drawn as vector paths -- no image
    // asset, so no new binary resource or CMake change needed for this
    // reskin.
    juce::Path bars;
    const float w = bounds.getWidth() / 5.0f;
    const float heights[3] = { 0.45f, 1.0f, 0.65f };
    for (int i = 0; i < 3; ++i)
    {
        const float h = bounds.getHeight() * heights[i];
        juce::Rectangle<float> bar(bounds.getX() + i * w * 1.7f, bounds.getBottom() - h, w, h);
        bars.addRoundedRectangle(bar, w * 0.3f);
    }
    g.setColour(juce::Colour(kAccent));
    g.fillPath(bars);
    g.setColour(juce::Colour(kLive).withAlpha(0.6f));
    g.strokePath(bars, juce::PathStrokeType(1.0f));
}

void BeatShoreBridgeAudioProcessorEditor::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    juce::ColourGradient bg(juce::Colour(kBgTop), bounds.getTopLeft(),
                             juce::Colour(kBgBottom), bounds.getBottomRight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    // Faint HUD-style grid texture -- very low alpha, purely decorative.
    g.setColour(juce::Colour(kAccent).withAlpha(0.035f));
    for (float x = 0; x < bounds.getWidth(); x += 24.0f)
        g.drawVerticalLine(juce::roundToInt(x), 0.0f, bounds.getHeight());
    for (float y = 0; y < bounds.getHeight(); y += 24.0f)
        g.drawHorizontalLine(juce::roundToInt(y), 0.0f, bounds.getWidth());

    g.setColour(juce::Colour(kAccent).withAlpha(0.25f));
    g.drawRect(getLocalBounds(), 1);

    drawBrandMark(g, juce::Rectangle<float>(14.0f, 12.0f, 22.0f, 20.0f));

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const float pulsePhase = static_cast<float>((nowMs - startTimeMs) / 1000.0 * juce::MathConstants<double>::twoPi * 0.5);

    const auto bridgeStatus = processor.getBridgeStatus();
    const auto liveColour = bridgeStatusColour(bridgeStatus);

    // Persistent header connection dot -- just left of bridgeStatusLabel,
    // colour-coded by the SAME BridgeStatus enum bridgeStatusText() already
    // switches on, visible regardless of currentPage.
    drawStatusDot(g, { bridgeStatusLabel.getBounds().getX() - 10.0f,
                        bridgeStatusLabel.getBounds().getCentreY() + 0.0f },
                  liveColour, pulsePhase);

    drawPanel(g, sidebarBounds, juce::Colour(kAccent), 1.5f);

    if (currentPage == Page::Overview)
    {
        drawPanel(g, hostPanelBounds, juce::Colour(kAccent), 2.0f);
    }
    else if (currentPage == Page::Transcribe)
    {
        drawPanel(g, bridgePanelBounds, liveColour, bridgeStatus == BridgeStatus::Connected ? 4.0f : 2.0f);
        drawPanel(g, quickAnalysisPanelBounds, juce::Colour(kAccent), 2.0f);
        drawPanel(g, transcribePanelBounds, juce::Colour(kAccent), 2.0f);
    }
    else if (currentPage == Page::Humanize)
    {
        drawPanel(g, humanizePanelBounds, juce::Colour(kAccent), 2.0f);
    }
    else if (currentPage == Page::Mix)
    {
        drawPanel(g, mixPanelBounds, juce::Colour(kLive), 2.0f);
    }
    else if (currentPage == Page::Master)
    {
        drawPanel(g, masterPanelBounds, juce::Colour(kLive), 2.0f);
    }
    else
    {
        drawPanel(g, comingSoonPanelBounds, juce::Colour(kTextFaint), 1.5f);
    }

    // Thin "in-flight" progress bar under the trigger buttons -- reads
    // processor.isAnalysisInFlight()/getAnalysisProgress(), the same
    // read-only accessors timerCallback() already calls; nothing new is
    // exposed from PluginProcessor for this. Only meaningful (and only
    // drawn) on the Transcribe page, since that's the only page anything
    // can be in flight from.
    if (currentPage == Page::Transcribe && processor.isAnalysisInFlight())
    {
        const float progress = juce::jlimit(0.0f, 1.0f, static_cast<float>(processor.getAnalysisProgress()));
        auto barArea = juce::Rectangle<float>(transcribePanelBounds.getX(), getHeight() - 6.0f,
                                               transcribePanelBounds.getWidth(), 3.0f);
        g.setColour(juce::Colour(kAccentDim));
        g.fillRoundedRectangle(barArea, 1.5f);
        g.setColour(juce::Colour(kLive));
        g.fillRoundedRectangle(barArea.withWidth(barArea.getWidth() * progress), 1.5f);
    }
}

void BeatShoreBridgeAudioProcessorEditor::resized()
{
    auto full = getLocalBounds().reduced(16);

    // --- Persistent header: brand + title, bridge status on the right ---
    auto headerRow = full.removeFromTop(28);
    bridgeStatusLabel.setBounds(headerRow.removeFromRight(220).withTrimmedLeft(14));
    headerRow.removeFromRight(8);
    titleLabel.setBounds(headerRow.withTrimmedLeft(32));
    full.removeFromTop(12);

    // --- Sidebar nav, full remaining height ------------------------------
    auto sidebarArea = full.removeFromLeft(150);
    sidebarBounds = sidebarArea.toFloat();
    auto navArea = sidebarArea.reduced(8);
    for (int i = 0; i < kNumPages; ++i)
    {
        navButtons[size_t(i)].setBounds(navArea.removeFromTop(30));
        navArea.removeFromTop(3);
    }

    full.removeFromLeft(16);
    auto content = full; // everything below/right of the header and sidebar

    if (currentPage == Page::Overview)
    {
        auto hostArea = content.removeFromTop(180);
        hostPanelBounds = hostArea.toFloat();
        auto hostInner = hostArea.reduced(16, 12);
        hostSectionLabel.setBounds(hostInner.removeFromTop(16));
        hostInner.removeFromTop(6);
        sampleRateLabel.setBounds(hostInner.removeFromTop(20));
        blockSizeLabel.setBounds(hostInner.removeFromTop(20));
        tempoLabel.setBounds(hostInner.removeFromTop(20));
        timeSigLabel.setBounds(hostInner.removeFromTop(20));
        transportLabel.setBounds(hostInner.removeFromTop(20));
        playheadLabel.setBounds(hostInner.removeFromTop(20));
    }
    else if (currentPage == Page::Transcribe)
    {
        // --- Bridge + tempo card ---
        auto bridgeArea = content.removeFromTop(130);
        bridgePanelBounds = bridgeArea.toFloat();
        auto bridgeInner = bridgeArea.reduced(14, 10);
        bridgeSectionLabel.setBounds(bridgeInner.removeFromTop(16));
        captureStatusLabel.setBounds(bridgeInner.removeFromTop(16));
        bridgeInner.removeFromTop(6);
        analyzeTempoButton.setBounds(bridgeInner.removeFromTop(28));
        bridgeInner.removeFromTop(6);
        analysisResultLabel.setBounds(bridgeInner.removeFromTop(30));

        content.removeFromTop(14);

        // --- Quick analysis card (key / chords / loudness) ---
        auto quickArea = content.removeFromTop(120);
        quickAnalysisPanelBounds = quickArea.toFloat();
        auto quickInner = quickArea.reduced(14, 10);
        quickAnalysisSectionLabel.setBounds(quickInner.removeFromTop(16));
        quickInner.removeFromTop(6);
        {
            auto buttonRow = quickInner.removeFromTop(28);
            const int gap = 6;
            const int buttonWidth = (buttonRow.getWidth() - gap * 2) / 3;
            keyButton.setBounds(buttonRow.removeFromLeft(buttonWidth));
            buttonRow.removeFromLeft(gap);
            chordsButton.setBounds(buttonRow.removeFromLeft(buttonWidth));
            buttonRow.removeFromLeft(gap);
            loudnessButton.setBounds(buttonRow);
        }
        quickInner.removeFromTop(8);
        quickResultLabel.setBounds(quickInner.removeFromTop(30));

        content.removeFromTop(14);

        // --- Transcription card ---
        auto transcribeArea = content;
        transcribePanelBounds = transcribeArea.toFloat();
        auto transcribeInner = transcribeArea.reduced(14, 10);
        transcribeSectionLabel.setBounds(transcribeInner.removeFromTop(16));
        transcribeInner.removeFromTop(6);
        transcribeButton.setBounds(transcribeInner.removeFromTop(28));
        transcribeInner.removeFromTop(6);
        {
            auto buttonRow = transcribeInner.removeFromTop(28);
            const int gap = 6;
            const int buttonWidth = (buttonRow.getWidth() - gap * 2) / 3;
            drumsButton.setBounds(buttonRow.removeFromLeft(buttonWidth));
            buttonRow.removeFromLeft(gap);
            bassButton.setBounds(buttonRow.removeFromLeft(buttonWidth));
            buttonRow.removeFromLeft(gap);
            leadButton.setBounds(buttonRow);
        }
        transcribeInner.removeFromTop(6);
        transcribeStatusLabel.setBounds(transcribeInner.removeFromTop(30));
        transcribeDetailLabel.setBounds(transcribeInner.removeFromTop(64));
        transcribeInner.removeFromTop(6);
        openExportFolderButton.setBounds(transcribeInner.removeFromTop(28));
    }
    else if (currentPage == Page::Humanize)
    {
        humanizePanelBounds = content.toFloat();
        auto inner = content.reduced(16, 14);
        humanizeSectionLabel.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(6);
        humanizeExplainerLabel.setBounds(inner.removeFromTop(50));
        inner.removeFromTop(18);

        auto knobRow = inner.removeFromTop(110);
        const int knobCount = 4;
        const int knobWidth = knobRow.getWidth() / knobCount;
        auto layoutKnob = [&](juce::Slider& s, juce::Label& l)
        {
            auto cell = knobRow.removeFromLeft(knobWidth);
            l.setBounds(cell.removeFromTop(16));
            s.setBounds(cell.reduced(12, 0));
        };
        layoutKnob(timingSlider, timingSliderLabel);
        layoutKnob(velocitySlider, velocitySliderLabel);
        layoutKnob(dynamicsSlider, dynamicsSliderLabel);
        layoutKnob(articulationSlider, articulationSliderLabel);

        inner.removeFromTop(16);
        preserveGrooveToggle.setBounds(inner.removeFromTop(26));
    }
    else if (currentPage == Page::Mix)
    {
        mixPanelBounds = content.toFloat();
        auto inner = content.reduced(16, 14);
        mixSectionLabel.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(6);
        mixExplainerLabel.setBounds(inner.removeFromTop(64));
        inner.removeFromTop(10);
        mixEnabledToggle.setBounds(inner.removeFromTop(24));
        inner.removeFromTop(18);

        auto knobRow = inner.removeFromTop(110);
        const int knobCount = 6;
        const int knobWidth = knobRow.getWidth() / knobCount;
        auto layoutKnob = [&](juce::Slider& s, juce::Label& l)
        {
            auto cell = knobRow.removeFromLeft(knobWidth);
            l.setBounds(cell.removeFromTop(16));
            s.setBounds(cell.reduced(6, 0));
        };
        layoutKnob(eqLowShelfSlider, eqLowShelfLabel);
        layoutKnob(eqMidPeakSlider, eqMidPeakLabel);
        layoutKnob(eqHighShelfSlider, eqHighShelfLabel);
        layoutKnob(compThresholdSlider, compThresholdLabel);
        layoutKnob(compRatioSlider, compRatioLabel);
        layoutKnob(limiterThresholdSlider, limiterThresholdLabel);
    }
    else if (currentPage == Page::Master)
    {
        masterPanelBounds = content.toFloat();
        auto inner = content.reduced(16, 14);
        masterSectionLabel.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(6);
        masterExplainerLabel.setBounds(inner.removeFromTop(48));
        inner.removeFromTop(18);

        auto readoutRow = inner.removeFromTop(90);
        const int cellWidth = readoutRow.getWidth() / 4;
        auto layoutReadout = [&](juce::Label& caption, juce::Label& value)
        {
            auto cell = readoutRow.removeFromLeft(cellWidth).reduced(6, 0);
            caption.setBounds(cell.removeFromTop(16));
            cell.removeFromTop(4);
            value.setBounds(cell.removeFromTop(40));
        };
        layoutReadout(momentaryLufsCaption, momentaryLufsLabel);
        layoutReadout(shortTermLufsCaption, shortTermLufsLabel);
        layoutReadout(integratedLufsCaption, integratedLufsLabel);
        layoutReadout(truePeakCaption, truePeakLabel);

        inner.removeFromTop(16);
        resetMeterButton.setBounds(inner.removeFromTop(28).withSizeKeepingCentre(140, 28));
    }
    else
    {
        comingSoonPanelBounds = content.toFloat();
        auto inner = content.reduced(24);
        auto centred = inner.withSizeKeepingCentre(inner.getWidth(), 70);
        comingSoonTitleLabel.setBounds(centred.removeFromTop(22));
        centred.removeFromTop(8);
        comingSoonBodyLabel.setBounds(centred);
    }
}

juce::String BeatShoreBridgeAudioProcessorEditor::bridgeStatusText(BridgeStatus status)
{
    switch (status)
    {
        case BridgeStatus::Disconnected: return "Not connected -- no BeatShore desktop process found. Retrying...";
        case BridgeStatus::Connecting:   return "Connecting...";
        case BridgeStatus::Connected:    return "Connected";
        case BridgeStatus::Error:        return "Error";
    }
    return "Unknown";
}

juce::Colour BeatShoreBridgeAudioProcessorEditor::bridgeStatusColour(BridgeStatus status)
{
    switch (status)
    {
        case BridgeStatus::Disconnected: return juce::Colour(kTextFaint);
        case BridgeStatus::Connecting:   return juce::Colour(kWarn);
        case BridgeStatus::Connected:    return juce::Colour(kLive);
        case BridgeStatus::Error:        return juce::Colour(kError);
    }
    return juce::Colour(kTextFaint);
}

namespace
{
    juce::String captureTriggerResultText(BeatShoreBridgeAudioProcessor::CaptureTriggerResult r)
    {
        using Result = BeatShoreBridgeAudioProcessor::CaptureTriggerResult;
        switch (r)
        {
            case Result::Started:         return "Analyzing...";
            case Result::NotConnected:    return "Not connected to BeatShore desktop.";
            case Result::AlreadyInFlight: return "An analysis is already running.";
            case Result::NoAudioCaptured: return "No audio captured yet -- play some audio through this track first.";
            case Result::SilentAudio:     return "Captured audio is silent -- nothing to analyze.";
        }
        return "Unknown";
    }
}

void BeatShoreBridgeAudioProcessorEditor::analyzeButtonClicked()
{
    analysisResultLabel.setText(captureTriggerResultText(processor.triggerTempoAnalysis()), juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::transcribeButtonClicked()
{
    transcribeStatusLabel.setText(captureTriggerResultText(processor.triggerPolyphonicTranscription()), juce::dontSendNotification);
    transcribeDetailLabel.setText("", juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::keyButtonClicked()
{
    quickResultLabel.setText(captureTriggerResultText(processor.triggerKeyAnalysis()), juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::chordsButtonClicked()
{
    quickResultLabel.setText(captureTriggerResultText(processor.triggerChordAnalysis()), juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::loudnessButtonClicked()
{
    quickResultLabel.setText(captureTriggerResultText(processor.triggerLoudnessAnalysis()), juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::drumsButtonClicked()
{
    transcribeStatusLabel.setText(captureTriggerResultText(processor.triggerDrumTranscription()), juce::dontSendNotification);
    transcribeDetailLabel.setText("", juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::bassButtonClicked()
{
    transcribeStatusLabel.setText(captureTriggerResultText(processor.triggerBassTranscription()), juce::dontSendNotification);
    transcribeDetailLabel.setText("", juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::leadButtonClicked()
{
    transcribeStatusLabel.setText(captureTriggerResultText(processor.triggerLeadTranscription()), juce::dontSendNotification);
    transcribeDetailLabel.setText("", juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::openExportFolderClicked()
{
    if (lastMidiPath.isEmpty()) return;
    juce::File(lastMidiPath).revealToUser();
}

namespace
{
    // MIDI_RESULT-shaped kinds (noteCount/midiPath/etc, same as
    // transcribePolyphonic) vs everything else (a plain ANALYSIS_RESULT: a
    // number or JSON-stringified value in result.message). See
    // BridgeClient.h's own type == "MIDI_RESULT" branch, which this mirrors
    // -- kept as an explicit kind list here (not e.g. "noteCount != -1")
    // because a *failed* request never gets a noteCount at all, and errors
    // still need to route to the right label.
    bool isMidiProducingKind(const juce::String& kind)
    {
        return kind == "transcribePolyphonic" || kind == "transcribeDrums" || kind == "transcribeMono";
    }
}

void BeatShoreBridgeAudioProcessorEditor::applyResult(const BridgeAnalysisResult& result)
{
    const juce::String kind(result.kind);
    const bool isTempo = kind == "tempo";
    const bool isMidiKind = isMidiProducingKind(kind);
    // Three destinations, not two: tempo keeps its own label (unchanged
    // from before this feature set), key/chords/loudness share
    // quickResultLabel, and every MIDI-producing kind (piano/guitar, drums,
    // bass, lead) shares transcribeStatusLabel -- the same routing rule
    // Transcribe Piano/Guitar already used, just no longer hardcoded to
    // "not tempo means MIDI".
    juce::Label& statusLabel = isTempo ? analysisResultLabel : (isMidiKind ? transcribeStatusLabel : quickResultLabel);

    if (!result.success)
    {
        // The broker shutting down on purpose (a user-initiated Quit,
        // typically) is not the same category of event as a genuine
        // failure -- shown without the "Error: ... [CODE]" framing every
        // other errorCode gets, since there's nothing wrong to alarm the
        // user about and BridgeClient's own reconnect-on-timer loop will
        // pick back up automatically once a new broker process is
        // reachable.
        juce::String text = result.errorCode == "BROKER_SHUTTING_DOWN"
                                 ? juce::String(result.message)
                                 : "Error: " + juce::String(result.message);
        if (!result.errorCode.empty() && result.errorCode != "BROKER_SHUTTING_DOWN")
            text << " [" << result.errorCode << "]";
        statusLabel.setText(text, juce::dontSendNotification);
        if (isMidiKind) transcribeDetailLabel.setText("", juce::dontSendNotification);
        return;
    }

    if (!isMidiKind)
    {
        // tempo, key, chords, or loudness -- all the same plain
        // ANALYSIS_RESULT shape. hasNumericValue is only ever true for the
        // genuinely numeric kinds (tempo, loudness); key ({"key":"C",
        // "mode":"major"}) and chords (a segment list) arrive as
        // BridgeClient's own JSON-stringified fallback in result.message --
        // shown as-is rather than hand-parsed here, matching this editor's
        // existing "trust the message string" approach to every other
        // result kind.
        juce::String text = result.hasNumericValue
                                 ? (isTempo ? "Tempo: " + juce::String(result.message) + " BPM"
                                            : juce::String(result.message))
                                 : juce::String(result.message);
        if (result.desktopTotalMs >= 0)
            text << " (" << result.desktopTotalMs << " ms)";
        statusLabel.setText(text, juce::dontSendNotification);
        return;
    }

    // MIDI_RESULT: piano/guitar polyphonic transcription, drums, or mono
    // bass/lead -- identical shape regardless of which one produced it.
    statusLabel.setText(result.noteCount > 0
                             ? juce::String(result.noteCount) + " notes found"
                             : "No notes detected in the captured audio.",
                         juce::dontSendNotification);

    juce::StringArray detailLines;
    detailLines.add("Algorithm: " + juce::String(result.algorithm.empty() ? "--" : result.algorithm));
    detailLines.add("Source: " + juce::String(result.audioSource.empty() ? "--" : result.audioSource));
    if (result.computeMs >= 0) detailLines.add("Processing time: " + juce::String(result.computeMs) + " ms");
    if (!result.midiPath.empty())
    {
        detailLines.add("MIDI: " + juce::File(result.midiPath).getFileName()
                         + (result.midiSizeBytes >= 0 ? " (" + juce::String(result.midiSizeBytes) + " bytes)" : ""));
        lastMidiPath = result.midiPath;
    }
    else if (!result.midiWriteError.empty())
    {
        detailLines.add("MIDI file write failed: " + juce::String(result.midiWriteError));
    }
    transcribeDetailLabel.setText(detailLines.joinIntoString("\n"), juce::dontSendNotification);
}

void BeatShoreBridgeAudioProcessorEditor::timerCallback()
{
    const auto& snap = processor.getHostSnapshot();

    sampleRateLabel.setText("Sample rate: " + juce::String(snap.sampleRate.load(), 1) + " Hz", juce::dontSendNotification);
    blockSizeLabel.setText("Block size: " + juce::String(snap.blockSize.load()) + " samples", juce::dontSendNotification);

    if (snap.hostProvidesTransport.load())
    {
        tempoLabel.setText("Tempo: " + juce::String(snap.bpm.load(), 2) + " BPM", juce::dontSendNotification);
        timeSigLabel.setText("Time signature: " + juce::String(snap.timeSigNumerator.load()) + " / " + juce::String(snap.timeSigDenominator.load()),
                              juce::dontSendNotification);
        transportLabel.setText(juce::String(snap.isPlaying.load() ? "Playing" : "Stopped")
                                    + (snap.isRecording.load() ? " (recording)" : ""),
                                juce::dontSendNotification);
        const auto secs = snap.playheadSeconds.load();
        const int mins = static_cast<int>(secs) / 60;
        const double rem = secs - mins * 60.0;
        playheadLabel.setText("Playhead: " + juce::String(mins) + ":" + juce::String(rem, 2).paddedLeft('0', 5),
                               juce::dontSendNotification);
    }
    else
    {
        tempoLabel.setText("Tempo: host did not report position info", juce::dontSendNotification);
        timeSigLabel.setText("Time signature: --", juce::dontSendNotification);
        transportLabel.setText("Transport: --", juce::dontSendNotification);
        playheadLabel.setText("Playhead: --", juce::dontSendNotification);
    }

    const auto bridgeStatus = processor.getBridgeStatus();
    bridgeStatusLabel.setText(bridgeStatusText(bridgeStatus), juce::dontSendNotification);
    bridgeStatusLabel.setColour(juce::Label::textColourId, bridgeStatusColour(bridgeStatus));

    const bool inFlight = processor.isAnalysisInFlight();
    const bool canTrigger = bridgeStatus == BridgeStatus::Connected && !inFlight;
    for (auto* button : { &analyzeTempoButton, &transcribeButton, &keyButton, &chordsButton, &loudnessButton,
                           &drumsButton, &bassButton, &leadButton })
        button->setEnabled(canTrigger);
    captureStatusLabel.setText(processor.hasCapturedAudio() ? "Capture: audio buffered, ready to analyze" : "Capture: nothing buffered yet",
                                juce::dontSendNotification);

    if (inFlight)
    {
        const int pct = juce::roundToInt(processor.getAnalysisProgress() * 100.0);
        // Only one request can be in flight at a time -- show the progress
        // on whichever label most recently said "Analyzing..." is close
        // enough without threading the in-flight request's kind through the
        // UI; all three update together here since the reader can't tell
        // which one started it, but only the one that actually shows
        // "Analyzing..." reads as meaningfully "in progress" to the user.
        const juce::String progressText = "Analyzing... " + juce::String(pct) + "%";
        for (auto* label : { &analysisResultLabel, &transcribeStatusLabel, &quickResultLabel })
            if (label->getText() == "Analyzing..." || label->getText().startsWith("Analyzing... "))
                label->setText(progressText, juce::dontSendNotification);
    }

    BridgeAnalysisResult result;
    if (processor.takeAnalysisResult(result))
        applyResult(result);

    openExportFolderButton.setEnabled(!lastMidiPath.isEmpty());

    // Master page: read the real MasterMeter::Snapshot atomics processBlock()
    // wrote this same tick's worth of blocks into. formatLufs() renders the
    // -100.0 sentinel (see MasterMeter::kNegInf) as "-inf", matching how a
    // real loudness meter shows true silence -- not a literal "-100.0".
    {
        auto formatLufs = [](double v, const char* unit)
        {
            return v <= MasterMeter::kNegInf + 0.001
                       ? juce::String("-inf ") + unit
                       : juce::String(v, 1) + " " + unit;
        };
        const auto& masterSnap = processor.getMasterSnapshot();
        momentaryLufsLabel.setText(formatLufs(masterSnap.momentaryLufs.load(), "LUFS"), juce::dontSendNotification);
        shortTermLufsLabel.setText(formatLufs(masterSnap.shortTermLufs.load(), "LUFS"), juce::dontSendNotification);
        integratedLufsLabel.setText(formatLufs(masterSnap.integratedLufs.load(), "LUFS"), juce::dontSendNotification);
        truePeakLabel.setText(formatLufs(masterSnap.truePeakDb.load(), "dBTP"), juce::dontSendNotification);
    }

    repaint(); // drives the status-dot pulse and in-flight progress bar in paint()
}
