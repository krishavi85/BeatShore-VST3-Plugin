#include "PluginEditor.h"

namespace
{
    void styleHeading(juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        l.setColour(juce::Label::textColourId, juce::Colour(0xff9184d9));
    }

    void styleValue(juce::Label& l)
    {
        l.setFont(juce::Font(juce::FontOptions(14.0f)));
        l.setColour(juce::Label::textColourId, juce::Colour(0xffe9e9ed));
    }
}

BeatShoreBridgeAudioProcessorEditor::BeatShoreBridgeAudioProcessorEditor(BeatShoreBridgeAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    titleLabel.setText("BeatShore Bridge -- thin passthrough + host context reader", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    styleHeading(hostSectionLabel, "HOST CONTEXT");
    addAndMakeVisible(hostSectionLabel);

    for (auto* label : { &sampleRateLabel, &blockSizeLabel, &tempoLabel, &timeSigLabel, &transportLabel, &playheadLabel })
    {
        styleValue(*label);
        addAndMakeVisible(*label);
    }

    styleHeading(bridgeSectionLabel, "BEATSHORE DESKTOP BRIDGE");
    addAndMakeVisible(bridgeSectionLabel);

    styleValue(bridgeStatusLabel);
    bridgeStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xffffd166));
    addAndMakeVisible(bridgeStatusLabel);

    styleValue(captureStatusLabel);
    captureStatusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    captureStatusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8a8d9a));
    addAndMakeVisible(captureStatusLabel);

    analyzeTempoButton.setButtonText("Analyze Tempo (last 10s captured)");
    analyzeTempoButton.onClick = [this] { analyzeButtonClicked(); };
    addAndMakeVisible(analyzeTempoButton);

    styleValue(analysisResultLabel);
    analysisResultLabel.setText("No analysis run yet.", juce::dontSendNotification);
    addAndMakeVisible(analysisResultLabel);

    styleHeading(transcribeSectionLabel, "TRANSCRIPTION (PIANO / GUITAR)");
    addAndMakeVisible(transcribeSectionLabel);

    transcribeButton.setButtonText("Transcribe Piano/Guitar (last 10s captured)");
    transcribeButton.onClick = [this] { transcribeButtonClicked(); };
    addAndMakeVisible(transcribeButton);

    styleValue(transcribeStatusLabel);
    transcribeStatusLabel.setText("No transcription run yet.", juce::dontSendNotification);
    addAndMakeVisible(transcribeStatusLabel);

    styleValue(transcribeDetailLabel);
    transcribeDetailLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    transcribeDetailLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8a8d9a));
    addAndMakeVisible(transcribeDetailLabel);

    openExportFolderButton.setButtonText("Open Export Folder");
    openExportFolderButton.setEnabled(false);
    openExportFolderButton.onClick = [this] { openExportFolderClicked(); };
    addAndMakeVisible(openExportFolderButton);

    setSize(440, 560);
    startTimerHz(10);
    timerCallback();
}

BeatShoreBridgeAudioProcessorEditor::~BeatShoreBridgeAudioProcessorEditor()
{
    stopTimer();
}

void BeatShoreBridgeAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff161826));
    g.setColour(juce::Colour(0xff2a2c36));
    g.drawRect(getLocalBounds(), 1);
}

void BeatShoreBridgeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    titleLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(10);

    hostSectionLabel.setBounds(area.removeFromTop(16));
    sampleRateLabel.setBounds(area.removeFromTop(18));
    blockSizeLabel.setBounds(area.removeFromTop(18));
    tempoLabel.setBounds(area.removeFromTop(18));
    timeSigLabel.setBounds(area.removeFromTop(18));
    transportLabel.setBounds(area.removeFromTop(18));
    playheadLabel.setBounds(area.removeFromTop(18));

    area.removeFromTop(12);
    bridgeSectionLabel.setBounds(area.removeFromTop(16));
    bridgeStatusLabel.setBounds(area.removeFromTop(20));
    captureStatusLabel.setBounds(area.removeFromTop(16));

    area.removeFromTop(8);
    analyzeTempoButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    analysisResultLabel.setBounds(area.removeFromTop(36));

    area.removeFromTop(12);
    transcribeSectionLabel.setBounds(area.removeFromTop(16));
    area.removeFromTop(6);
    transcribeButton.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    transcribeStatusLabel.setBounds(area.removeFromTop(36));
    transcribeDetailLabel.setBounds(area.removeFromTop(54));
    area.removeFromTop(6);
    openExportFolderButton.setBounds(area.removeFromTop(28));
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

void BeatShoreBridgeAudioProcessorEditor::openExportFolderClicked()
{
    if (lastMidiPath.isEmpty()) return;
    juce::File(lastMidiPath).revealToUser();
}

void BeatShoreBridgeAudioProcessorEditor::applyResult(const BridgeAnalysisResult& result)
{
    const bool isTempo = juce::String(result.kind) == "tempo";
    juce::Label& statusLabel = isTempo ? analysisResultLabel : transcribeStatusLabel;

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
        if (!isTempo) transcribeDetailLabel.setText("", juce::dontSendNotification);
        return;
    }

    if (isTempo)
    {
        juce::String text = result.hasNumericValue
                                 ? "Tempo: " + juce::String(result.message) + " BPM"
                                 : juce::String(result.message);
        if (result.desktopTotalMs >= 0)
            text << " (" << result.desktopTotalMs << " ms)";
        statusLabel.setText(text, juce::dontSendNotification);
        return;
    }

    // Polyphonic transcription (MIDI_RESULT).
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

    const bool inFlight = processor.isAnalysisInFlight();
    const bool canTrigger = bridgeStatus == BridgeStatus::Connected && !inFlight;
    analyzeTempoButton.setEnabled(canTrigger);
    transcribeButton.setEnabled(canTrigger);
    captureStatusLabel.setText(processor.hasCapturedAudio() ? "Capture: audio buffered, ready to analyze" : "Capture: nothing buffered yet",
                                juce::dontSendNotification);

    if (inFlight)
    {
        const int pct = juce::roundToInt(processor.getAnalysisProgress() * 100.0);
        // Only one request can be in flight at a time -- show the progress
        // on whichever section's status label most recently said
        // "Analyzing..." is close enough without threading the in-flight
        // request's kind through the UI; both labels update together here
        // since the reader can't tell which one started it, but only the
        // one that actually shows "Analyzing..." reads as meaningfully "in
        // progress" to the user.
        const juce::String progressText = "Analyzing... " + juce::String(pct) + "%";
        if (analysisResultLabel.getText() == "Analyzing..." || analysisResultLabel.getText().startsWith("Analyzing... "))
            analysisResultLabel.setText(progressText, juce::dontSendNotification);
        if (transcribeStatusLabel.getText() == "Analyzing..." || transcribeStatusLabel.getText().startsWith("Analyzing... "))
            transcribeStatusLabel.setText(progressText, juce::dontSendNotification);
    }

    BridgeAnalysisResult result;
    if (processor.takeAnalysisResult(result))
        applyResult(result);

    openExportFolderButton.setEnabled(!lastMidiPath.isEmpty());
}
