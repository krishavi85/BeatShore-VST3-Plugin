#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BridgeClient.h"
#include <algorithm>
#include <limits>

BeatShoreBridgeAudioProcessor::BeatShoreBridgeAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    bridgeClient = std::make_unique<BridgeClient>();
    bridgeClient->setHostSnapshotSource(&hostSnapshot);
    bridgeClient->start();

    // Real DAW automation/MIDI-learn target, not just a UI button -- see
    // the header comment on triggerAnalysisParam. addParameter() transfers
    // ownership to the base AudioProcessor; the raw pointer here is just
    // for reading/resetting it.
    triggerAnalysisParam = new juce::AudioParameterBool(
        juce::ParameterID { "triggerAnalysis", 1 }, "Trigger Analysis", false);
    addParameter(triggerAnalysisParam);

    // Real EQ/Compressor/Limiter parameters (see MixChain.h and the header
    // comment on mixChain below) -- every default is a fully transparent
    // value (0dB gain, ratio 1:1, thresholds at 0dB) and mixEnabledParam
    // itself defaults to false, so a session that never opens the Mix page
    // gets audio identical to before this feature existed.
    mixEnabledParam = new juce::AudioParameterBool(
        juce::ParameterID { "mixEnabled", 1 }, "Mix Chain Enabled", false);
    addParameter(mixEnabledParam);
    eqLowShelfGainParam = new juce::AudioParameterFloat(
        juce::ParameterID { "eqLowShelfGain", 1 }, "EQ Low Shelf Gain", -12.0f, 12.0f, 0.0f);
    addParameter(eqLowShelfGainParam);
    eqMidPeakGainParam = new juce::AudioParameterFloat(
        juce::ParameterID { "eqMidPeakGain", 1 }, "EQ Mid Peak Gain", -12.0f, 12.0f, 0.0f);
    addParameter(eqMidPeakGainParam);
    eqHighShelfGainParam = new juce::AudioParameterFloat(
        juce::ParameterID { "eqHighShelfGain", 1 }, "EQ High Shelf Gain", -12.0f, 12.0f, 0.0f);
    addParameter(eqHighShelfGainParam);
    compThresholdParam = new juce::AudioParameterFloat(
        juce::ParameterID { "compThreshold", 1 }, "Compressor Threshold", -60.0f, 0.0f, 0.0f);
    addParameter(compThresholdParam);
    compRatioParam = new juce::AudioParameterFloat(
        juce::ParameterID { "compRatio", 1 }, "Compressor Ratio", 1.0f, 20.0f, 1.0f);
    addParameter(compRatioParam);
    limiterThresholdParam = new juce::AudioParameterFloat(
        juce::ParameterID { "limiterThreshold", 1 }, "Limiter Threshold", -12.0f, 0.0f, 0.0f);
    addParameter(limiterThresholdParam);

    startTimerHz(10); // services triggerAnalysisParam regardless of whether an editor window is open -- see timerCallback()
}

BeatShoreBridgeAudioProcessor::~BeatShoreBridgeAudioProcessor()
{
    stopTimer(); // explicit, not relying on ~Timer's own deregistration -- avoids a theoretical callback-during-destruction race against this derived class's members
}

BridgeStatus BeatShoreBridgeAudioProcessor::getBridgeStatus() const
{
    return bridgeClient->getStatus();
}

bool BeatShoreBridgeAudioProcessor::isAnalysisInFlight() const
{
    return bridgeClient->isRequestInFlight();
}

double BeatShoreBridgeAudioProcessor::getAnalysisProgress() const
{
    return bridgeClient->getProgress();
}

bool BeatShoreBridgeAudioProcessor::hasCapturedAudio() const
{
    return activeFramesWritten.load(std::memory_order_relaxed) > 0
        || frozenBufferIndex.load(std::memory_order_relaxed) != -1;
}

bool BeatShoreBridgeAudioProcessor::takeAnalysisResult(BridgeAnalysisResult& out)
{
    const bool got = bridgeClient->takeResult(out);
    // Auto-load the MIDI preview the moment a real result arrives -- the
    // same "load, don't autoplay" split setMidiPreviewEnabled() itself
    // documents: this only ever populates activeMidiPreview, it never
    // flips midiPreviewEnabled on by itself. Every MIDI-producing kind
    // (basic-pitch's transcribePolyphonic, transcribeDrums, transcribeMono,
    // and MT3's transcribeMt3) shares this identical result shape, so no
    // kind check is needed here -- an empty midiPath (0 notes found, or a
    // write failure already surfaced via midiWriteError) is the only thing
    // that skips this, same guard loadMidiPreviewFile() itself would need.
    if (got && out.success && !out.midiPath.empty())
        loadMidiPreviewFile(out.midiPath);
    return got;
}

void BeatShoreBridgeAudioProcessor::loadMidiPreviewFile(const juce::String& midiPath)
{
    juce::File file(midiPath);
    juce::FileInputStream stream(file);
    if (!stream.openedOk()) return; // stale/unreadable path -- the text-based result labels already reported the real outcome, nothing further to do here

    juce::MidiFile midiFile;
    if (!midiFile.readFrom(stream)) return; // not a MIDI file this JUCE build can parse -- silently skip preview, same reasoning as above

    // Converts every track's event timestamps from ticks to seconds in
    // place, honoring whatever tempo-map events the file itself carries
    // (midi-export.js/mt3_engine.py both write a fixed-tempo file, but
    // this doesn't assume that) -- the standard JUCE way to get a
    // wall-clock-relative sequence out of a MIDI file.
    midiFile.convertTimestampTicksToSeconds();

    auto merged = std::make_unique<juce::MidiMessageSequence>();
    for (int i = 0; i < midiFile.getNumTracks(); ++i)
        merged->addSequence(*midiFile.getTrack(i), 0.0);
    merged->sort();
    merged->updateMatchedPairs();

    const double length = merged->getEndTime();

    // Flush whatever the PREVIOUS sequence left held before swapping the
    // pointer out from under a possibly-still-preview-enabled processBlock()
    // -- same stop-and-flush this triggers on an explicit disable (see
    // setMidiPreviewEnabled()), just triggered by "a new result replaced
    // the old one" instead of "the user clicked Stop".
    midiPreviewStopRequested.store(true, std::memory_order_relaxed);
    midiPreviewLengthSeconds.store(length, std::memory_order_relaxed);
    midiPreviewPositionSeconds.store(0.0, std::memory_order_relaxed);
    activeMidiPreview.store(merged.get(), std::memory_order_release);
    retiredMidiPreviews.push_back(std::move(merged)); // keeps it alive for the rest of this session -- see the header comment on retiredMidiPreviews
}

bool BeatShoreBridgeAudioProcessor::cancelAnalysis()
{
    return bridgeClient->requestCancel();
}

void BeatShoreBridgeAudioProcessor::setMidiPreviewEnabled(bool shouldPlay)
{
    if (!shouldPlay && midiPreviewEnabled.load())
        midiPreviewStopRequested.store(true, std::memory_order_relaxed); // flush any notes currently held before going silent
    midiPreviewPositionSeconds.store(0.0, std::memory_order_relaxed); // "Preview" always means "from the top", not "resume" -- true whether this call is turning playback on or off
    midiPreviewEnabled.store(shouldPlay, std::memory_order_relaxed);
}

void BeatShoreBridgeAudioProcessor::timerCallback()
{
    if (!triggerRequestedByHost.exchange(false)) return;

    triggerTempoAnalysis(); // return value intentionally ignored here -- same as the editor button, the result (or lack of one) surfaces via takeAnalysisResult()

    // Reset the parameter so the host sees the trigger "release" -- without
    // this, a host-side momentary control (e.g. a MIDI-learned button)
    // couldn't fire a second time without an explicit off-then-on move.
    // setValueNotifyingHost (not the raw AudioParameterBool assignment) so
    // the host's own parameter display/automation lane stays in sync.
    triggerAnalysisParam->setValueNotifyingHost(0.0f);
}

BeatShoreBridgeAudioProcessor::SnapshotOutcome BeatShoreBridgeAudioProcessor::captureFrozenSnapshot(std::vector<float>& outInterleaved, float& outPeak)
{
    // Busy, not silently folded into "no audio": a previous snapshot hasn't
    // been consumed yet. This function is only ever called from the message
    // thread (never concurrently with itself, since JUCE's message thread
    // is single-threaded), so this check + the matching state check in
    // processBlock() together are what make "the audio thread never
    // reclaims a buffer still being read" a proven invariant rather than an
    // assumed one -- see the header comment on bufferState.
    if (frozenBufferIndex.load(std::memory_order_acquire) != -1) return SnapshotOutcome::Busy;

    swapRequested.store(true, std::memory_order_release);

    // Wait for the audio thread to perform the swap on its next
    // processBlock() call -- a pointer/index swap, not a copy, so this is
    // normally sub-millisecond. Bounded: if the host has stopped calling
    // processBlock entirely (some hosts suspend it on a bypassed/muted
    // track), give up rather than hang the message thread forever.
    const auto deadlineMs = juce::Time::getMillisecondCounter() + 500;
    while (frozenBufferIndex.load(std::memory_order_acquire) == -1)
    {
        if (juce::Time::getMillisecondCounter() > deadlineMs)
        {
            swapRequested.store(false, std::memory_order_release);
            return SnapshotOutcome::Timeout;
        }
        juce::Thread::sleep(2);
    }

    std::lock_guard<std::mutex> structureLock(bufferStructureMutex);

    const int slot = frozenBufferIndex.load(std::memory_order_relaxed);

    // Ready -> Reading. Asserted, not just assumed: processBlock() only
    // ever publishes frozenBufferIndex after storing Ready into this same
    // slot (see the swap code below), so seeing anything else here would
    // mean the invariant was already broken elsewhere -- fail loudly in
    // debug builds rather than silently reading a buffer in an unexpected
    // state.
    jassert(bufferState[slot].load(std::memory_order_acquire) == BufferState::Ready);
    bufferState[slot].store(BufferState::Reading, std::memory_order_release);

    const juce::int64 available = frozenFrameCount.load(std::memory_order_relaxed);
    const juce::int64 endPos = frozenWritePos.load(std::memory_order_relaxed);
    const juce::int64 capacity = ringBuffers[slot].getNumSamples();

    if (available <= 0 || capacity <= 0)
    {
        bufferState[slot].store(BufferState::Free, std::memory_order_release);
        frozenBufferIndex.store(-1, std::memory_order_release);
        return SnapshotOutcome::NoAudio;
    }

    outInterleaved.resize(size_t(available) * 2);
    outPeak = 0.0f;
    const juce::int64 startPos = (endPos - available + capacity) % capacity;
    for (juce::int64 i = 0; i < available; ++i)
    {
        const juce::int64 srcIndex = (startPos + i) % capacity;
        const float l = ringBuffers[slot].getSample(0, int(srcIndex));
        const float r = ringBuffers[slot].getSample(1, int(srcIndex));
        outInterleaved[size_t(i) * 2 + 0] = l;
        outInterleaved[size_t(i) * 2 + 1] = r;
        outPeak = juce::jmax(outPeak, std::abs(l), std::abs(r));
    }

    // The copy above is this function's own private data now. Reading ->
    // Free happens BEFORE the frozenBufferIndex reset that lets a new swap
    // be requested, so the audio thread can never observe this slot as
    // reclaimable while a caller could still be mid-copy out of it.
    bufferState[slot].store(BufferState::Free, std::memory_order_release);
    frozenBufferIndex.store(-1, std::memory_order_release);
    return SnapshotOutcome::Ok;
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerAnalysisOfKind(const juce::String& kind, const juce::String& role)
{
    if (bridgeClient->getStatus() != BridgeStatus::Connected) return CaptureTriggerResult::NotConnected;
    if (bridgeClient->isRequestInFlight()) return CaptureTriggerResult::AlreadyInFlight;

    std::vector<float> interleaved;
    float peak = 0.0f;
    const auto outcome = captureFrozenSnapshot(interleaved, peak);
    if (outcome == SnapshotOutcome::Busy) return CaptureTriggerResult::AlreadyInFlight;
    if (outcome != SnapshotOutcome::Ok) return CaptureTriggerResult::NoAudioCaptured;
    if (peak < 1.0e-4f) return CaptureTriggerResult::SilentAudio;

    const bool started = bridgeClient->requestAnalysis(interleaved, uint32_t(captureSampleRate), 2, uint32_t(interleaved.size() / 2), kind, "live-captured", getHostTrackName(), role, humanizeSettings);
    return started ? CaptureTriggerResult::Started : CaptureTriggerResult::AlreadyInFlight;
}

void BeatShoreBridgeAudioProcessor::updateTrackProperties(const juce::AudioProcessor::TrackProperties& properties)
{
    std::lock_guard<std::mutex> lock(trackNameMutex);
    hostTrackName = properties.name.value_or(juce::String());
}

juce::String BeatShoreBridgeAudioProcessor::getHostTrackName() const
{
    std::lock_guard<std::mutex> lock(trackNameMutex);
    return hostTrackName;
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerTempoAnalysis()
{
    return triggerAnalysisOfKind("tempo");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerPolyphonicTranscription()
{
    return triggerAnalysisOfKind("transcribePolyphonic");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerKeyAnalysis()
{
    return triggerAnalysisOfKind("key");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerChordAnalysis()
{
    return triggerAnalysisOfKind("chords");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerLoudnessAnalysis()
{
    return triggerAnalysisOfKind("loudness");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerDrumTranscription()
{
    return triggerAnalysisOfKind("transcribeDrums");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerBassTranscription()
{
    return triggerAnalysisOfKind("transcribeMono", "bass");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerLeadTranscription()
{
    return triggerAnalysisOfKind("transcribeMono", "lead");
}

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerMt3Transcription()
{
    return triggerAnalysisOfKind("transcribeMt3");
}

bool BeatShoreBridgeAudioProcessor::captureSnapshotForTest(std::vector<float>& outInterleaved, double& outSampleRate)
{
    outSampleRate = captureSampleRate;
    float peak = 0.0f;
    return captureFrozenSnapshot(outInterleaved, peak) == SnapshotOutcome::Ok;
}

void BeatShoreBridgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    hostSnapshot.sampleRate.store(sampleRate);
    hostSnapshot.blockSize.store(samplesPerBlock);

    // Not concurrent with processBlock() (JUCE never calls both at once),
    // but can race triggerTempoAnalysis() reading a frozen buffer from the
    // message thread -- e.g. a sample-rate change fired while a result was
    // mid read-out. The mutex is uncontended in the overwhelmingly common
    // case (prepareToPlay before playback starts, no pending snapshot).
    std::lock_guard<std::mutex> lock(bufferStructureMutex);
    captureSampleRate = sampleRate;
    const int capacityFrames = juce::jmax(1, int(sampleRate * captureSeconds));
    for (auto& rb : ringBuffers)
    {
        rb.setSize(2, capacityFrames, false, true, true);
        rb.clear();
    }
    activeBufferIndex.store(0);
    activeWritePos.store(0);
    activeFramesWritten.store(0);
    frozenBufferIndex.store(-1);
    swapRequested.store(false);
    bufferState[0].store(BufferState::Writing);
    bufferState[1].store(BufferState::Free);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = juce::uint32(samplesPerBlock);
    spec.numChannels = juce::uint32(getTotalNumOutputChannels());
    mixChain.prepare(spec);
    // Force the cached-value change detection in processBlock() to push
    // every current parameter value into the freshly (re)prepared chain on
    // the very next block, rather than assuming a value unchanged since
    // the last prepareToPlay() doesn't need re-applying to a possibly-new
    // sample rate.
    lastEqLowGain = lastEqMidGain = lastEqHighGain = std::numeric_limits<float>::quiet_NaN();
    lastCompThreshold = lastCompRatio = lastLimiterThreshold = std::numeric_limits<float>::quiet_NaN();

    // A sample-rate/channel-count change genuinely invalidates any
    // in-progress loudness measurement (libebur128 has no "change
    // parameters mid-stream and keep history" path worth using here --
    // see MasterMeter::prepare()), so this always starts the Master
    // meter's Integrated LUFS and true-peak hold fresh, same as opening a
    // new session would.
    masterMeter.prepare(sampleRate, static_cast<int>(spec.numChannels));
}

void BeatShoreBridgeAudioProcessor::releaseResources()
{
}

bool BeatShoreBridgeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// Real-time audio callback. Deliberately minimal, per the plan's own
// "non-negotiable real-time rule": no allocation, no file I/O, no network
// requests, no JSON parsing, no inference. Everything here is either a
// direct buffer passthrough or a lock-free atomic store — both safe to call
// from the audio thread. Reading host transport/tempo/time-signature info
// via getPlayHead() is a JUCE-internal read with no I/O of its own, so it's
// safe here too; publishing it into the atomics is what lets the (non-
// realtime) editor read it a moment later without touching the audio thread.
// The one thing added since that rule was written -- real MIDI preview
// playback (see setMidiPreviewEnabled()) -- still honors it: it only ever
// reads an already-built juce::MidiMessageSequence via plain O(1) array
// accessors (no allocation) and writes into the host-owned midiMessages
// buffer already passed into this call, exactly the same "read atomics,
// touch only what's already allocated" shape as everything above it.
void BeatShoreBridgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Passthrough: this bridge doesn't process audio itself yet. Extra
    // input channels (if any) beyond the output bus are simply not copied.
    for (auto ch = getTotalNumOutputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // Automation/MIDI-learn bridge: triggerAnalysisParam->get() is a cheap,
    // lock-free read (real-time safe). Only a false->true edge matters --
    // this thread never calls triggerTempoAnalysis() itself (that waits and
    // touches BridgeClient, neither of which belongs on the audio thread);
    // it just raises a flag pollParameterTriggeredAnalysis() (message
    // thread) checks.
    {
        const bool current = triggerAnalysisParam->get();
        if (current && !lastTriggerParamValue)
            triggerRequestedByHost.store(true, std::memory_order_relaxed);
        lastTriggerParamValue = current;
    }

    // Service a pending snapshot swap before this block's capture write, so
    // a request made between two blocks is honored starting on the very
    // next one. O(1): publishes the currently-active buffer's fill state as
    // the frozen snapshot, then flips which buffer this thread writes into.
    // No data is copied here and this thread never waits on anything --
    // triggerTempoAnalysis() (message thread) is the one that waits for
    // frozenBufferIndex to change, not the other way around.
    if (swapRequested.exchange(false, std::memory_order_acq_rel))
    {
        const int oldActive = activeBufferIndex.load(std::memory_order_relaxed);
        const int newActive = 1 - oldActive;

        // This buffer must never be reclaimed while a reader could still be
        // touching it. captureFrozenSnapshot() only ever requests a second
        // swap after transitioning its slot back to Free, so this should
        // always be Free here -- but "should always be" is exactly the kind
        // of claim this check exists to stop trusting blindly. If it's ever
        // not Free (a future bug elsewhere), refuse the swap outright:
        // never wait (this is the audio thread), never overwrite -- just
        // leave the request pending for a later block, when the previous
        // cycle should have finished. No allocation, no logging here
        // (that's not real-time safe either); the message-thread side
        // observes this as a Timeout, which is diagnosable.
        if (bufferState[newActive].load(std::memory_order_acquire) != BufferState::Free)
        {
            swapRequested.store(true, std::memory_order_release);
        }
        else
        {
            frozenFrameCount.store(activeFramesWritten.load(std::memory_order_relaxed), std::memory_order_relaxed);
            frozenWritePos.store(activeWritePos.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bufferState[oldActive].store(BufferState::Ready, std::memory_order_release); // Writing -> Ready
            frozenBufferIndex.store(oldActive, std::memory_order_release); // published last: this is the reader's ready gate

            activeWritePos.store(0, std::memory_order_relaxed);
            activeFramesWritten.store(0, std::memory_order_relaxed);
            bufferState[newActive].store(BufferState::Writing, std::memory_order_release); // Free -> Writing
            activeBufferIndex.store(newActive, std::memory_order_relaxed);
        }
    }

    // Feed the rolling capture buffer currently marked active. The other
    // buffer, if frozen, is never touched here -- see the header comment on
    // ringBuffers for why that makes this race-free without any pause flag.
    {
        const int active = activeBufferIndex.load(std::memory_order_relaxed);
        const int capacity = ringBuffers[active].getNumSamples();
        if (capacity > 0)
        {
            const int numSamples = buffer.getNumSamples();
            const int numChannelsToCapture = juce::jmin(2, buffer.getNumChannels());
            juce::int64 pos = activeWritePos.load(std::memory_order_relaxed);

            for (int i = 0; i < numSamples; ++i)
            {
                const int destIndex = int((pos + i) % capacity);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float sample = ch < numChannelsToCapture ? buffer.getSample(ch, i) : 0.0f;
                    ringBuffers[active].setSample(ch, destIndex, sample);
                }
            }

            activeWritePos.store((pos + numSamples) % capacity, std::memory_order_relaxed);
            const juce::int64 newFrames = juce::jmin<juce::int64>(activeFramesWritten.load(std::memory_order_relaxed) + numSamples, capacity);
            activeFramesWritten.store(newFrames, std::memory_order_relaxed);
        }
    }

    // Real EQ/Compressor/Limiter -- applied AFTER the capture above, so
    // analysis (tempo/key/transcription/etc.) always sees the original,
    // unprocessed audio exactly as it did before this feature existed;
    // only the actual output sent back to the host is affected. Skipped
    // entirely (not even MixChain::process()'s own internal bypass path)
    // while mixEnabledParam->get() is false, so a session that never opens
    // the Mix page costs this processBlock() nothing extra at all -- not
    // "negligible", genuinely zero additional work.
    if (mixEnabledParam->get())
    {
        // Every AudioParameterFloat::get() below is the same cheap,
        // lock-free, real-time-safe read triggerAnalysisParam->get()
        // already used -- no allocation, no lock, safe on this thread.
        // Only actually pushed into MixChain (which recomputes real IIR
        // coefficients) when a value has changed since the last block --
        // see the header comment on the lastEq*/lastComp*/lastLimiter*
        // cache members for why.
        const float eqLow = eqLowShelfGainParam->get();
        const float eqMid = eqMidPeakGainParam->get();
        const float eqHigh = eqHighShelfGainParam->get();
        const float compThresh = compThresholdParam->get();
        const float compRatio = compRatioParam->get();
        const float limiterThresh = limiterThresholdParam->get();

        if (eqLow != lastEqLowGain) { mixChain.setEqLowShelfGainDb(eqLow); lastEqLowGain = eqLow; }
        if (eqMid != lastEqMidGain) { mixChain.setEqMidPeakGainDb(eqMid); lastEqMidGain = eqMid; }
        if (eqHigh != lastEqHighGain) { mixChain.setEqHighShelfGainDb(eqHigh); lastEqHighGain = eqHigh; }
        if (compThresh != lastCompThreshold) { mixChain.setCompressorThresholdDb(compThresh); lastCompThreshold = compThresh; }
        if (compRatio != lastCompRatio) { mixChain.setCompressorRatio(compRatio); lastCompRatio = compRatio; }
        if (limiterThresh != lastLimiterThreshold) { mixChain.setLimiterThresholdDb(limiterThresh); lastLimiterThreshold = limiterThresh; }

        juce::dsp::AudioBlock<float> block(buffer);
        mixChain.process(block, false);
    }

    // Real EBU R128 loudness/true-peak metering (see MasterMeter.h) of
    // exactly what's about to go back to the host -- reads `buffer` AFTER
    // the Mix block above, not before, so Master reflects this plugin's
    // actual output when Mix is engaged, not the pre-Mix signal.
    // getArrayOfReadPointers() returns a pointer into JUCE's own
    // already-allocated per-channel array -- no allocation on this thread.
    if (masterMeterResetRequested.exchange(false, std::memory_order_acq_rel))
        masterMeter.reset();
    masterMeter.process(buffer.getArrayOfReadPointers(), buffer.getNumSamples(), masterSnapshot);

    // Real MIDI preview output (see setMidiPreviewEnabled()/
    // loadMidiPreviewFile()): an independent, free-running, looping clock
    // -- NOT locked to the host's own transport position, since there is
    // no meaningful relationship between a rolling ~10s capture buffer's
    // contents and wherever the host playhead happens to be right now.
    // Added into midiMessages rather than replacing it, so this never
    // discards whatever MIDI (if any) is already flowing through this
    // track from the host -- consistent with the audio passthrough above
    // only ever adding processing, never silently dropping input that
    // isn't this feature's concern.
    // getNumEvents()/getEventPointer() are plain O(1) accessors into an
    // already-built juce::MidiMessageSequence -- no allocation, safe here.
    if (midiPreviewEnabled.load(std::memory_order_relaxed))
    {
        if (auto* seq = activeMidiPreview.load(std::memory_order_acquire))
        {
            const double sr = getSampleRate();
            const int numSamples = buffer.getNumSamples();
            if (sr > 0.0 && numSamples > 0)
            {
                const double sequenceLength = juce::jmax(0.05, midiPreviewLengthSeconds.load(std::memory_order_relaxed));
                double pos = midiPreviewPositionSeconds.load(std::memory_order_relaxed);
                double remaining = numSamples / sr;
                double blockOffsetSeconds = 0.0;

                // Bounded loop-wrap count: only a pathologically short
                // (sub-block-length) sequence could wrap more than once
                // within a single block: capped so a future bug here
                // can't spin the audio thread forever.
                for (int guard = 0; guard < 64 && remaining > 1.0e-9; ++guard)
                {
                    const double windowEnd = juce::jmin(pos + remaining, sequenceLength);
                    for (int i = 0; i < seq->getNumEvents(); ++i)
                    {
                        const auto& msg = seq->getEventPointer(i)->message;
                        const double t = msg.getTimeStamp();
                        if (t >= pos && t < windowEnd)
                        {
                            const int samplePos = juce::jlimit(0, numSamples - 1,
                                int(std::round((blockOffsetSeconds + (t - pos)) * sr)));
                            midiMessages.addEvent(msg, samplePos);
                        }
                    }
                    const double consumed = windowEnd - pos;
                    remaining -= consumed;
                    blockOffsetSeconds += consumed;
                    pos = windowEnd;

                    if (pos >= sequenceLength - 1.0e-9)
                    {
                        // Loop wrap: flush every channel's held notes right
                        // at the wrap point so a note that was still
                        // sounding at the sequence's end can never hang
                        // past it into the next lap.
                        const int flushSamplePos = juce::jlimit(0, numSamples - 1, int(std::round(blockOffsetSeconds * sr)));
                        for (int ch = 1; ch <= 16; ++ch)
                            midiMessages.addEvent(juce::MidiMessage::allNotesOff(ch), flushSamplePos);
                        pos = 0.0;
                    }
                }
                midiPreviewPositionSeconds.store(pos, std::memory_order_relaxed);
            }
        }
    }

    // Services a stop request from setMidiPreviewEnabled(false) or a new
    // result replacing the sequence mid-playback (see
    // loadMidiPreviewFile()) -- one All-Notes-Off flush per request, not
    // gated on midiPreviewEnabled being true right now, so a note held
    // from the OLD state still gets silenced even if playback was just
    // switched off (or swapped) in the same block that would otherwise
    // have skipped the block above entirely.
    if (midiPreviewStopRequested.exchange(false, std::memory_order_acq_rel))
        for (int ch = 1; ch <= 16; ++ch)
            midiMessages.addEvent(juce::MidiMessage::allNotesOff(ch), 0);

    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            hostSnapshot.hostProvidesTransport.store(true);
            hostSnapshot.isPlaying.store(position->getIsPlaying());
            hostSnapshot.isRecording.store(position->getIsRecording());

            if (const auto bpm = position->getBpm())
                hostSnapshot.bpm.store(*bpm);

            if (const auto timeSig = position->getTimeSignature())
            {
                hostSnapshot.timeSigNumerator.store(timeSig->numerator);
                hostSnapshot.timeSigDenominator.store(timeSig->denominator);
            }

            if (const auto seconds = position->getTimeInSeconds())
                hostSnapshot.playheadSeconds.store(*seconds);
        }
        else
        {
            hostSnapshot.hostProvidesTransport.store(false);
        }
    }
    else
    {
        hostSnapshot.hostProvidesTransport.store(false);
    }
}

juce::AudioProcessorEditor* BeatShoreBridgeAudioProcessor::createEditor()
{
    return new BeatShoreBridgeAudioProcessorEditor(*this);
}

// Was a placeholder that saved nothing ("No user-configurable parameters
// exist yet") from back before Mix/Master/Humanize existed, and was never
// updated when they were added -- a real, confirmed bug, not a REAPER
// quirk: found via a live REAPER acceptance test where a render's output
// kept reverting to unprocessed audio despite the Mix page visibly
// showing real, non-default values immediately beforehand. That's exactly
// what happens when a host builds a fresh/offline plugin instance for a
// render and restores it via getStateInformation()/setStateInformation()
// -- with the old placeholder, the restored instance had nothing to
// restore, so it silently fell back to every parameter's default
// (Mix Enabled = false). The same root cause would have broken REAPER
// project save/reload for these parameters too, not just rendering.
//
// Fixed generically rather than by hand-listing the 7 Mix parameters:
// walks getParameters() and saves each one's real ParameterID (from
// RangedAudioParameter::getParameterID(), stable across reordering,
// unlike array index) and its normalized 0..1 value -- so this
// automatically covers every current AudioProcessorParameter (including
// mixEnabledParam and all six Mix float params) and any added later,
// with no further change needed here. triggerAnalysisParam is included
// too for the same genericity, even though as a momentary trigger it
// should almost always be saved as false.
//
// humanizeSettings is NOT an AudioProcessorParameter (see its own header
// comment on why -- it's a plugin-local setting, not a host-automatable
// one), so it's serialized separately, by hand, alongside the parameters.
void BeatShoreBridgeAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("BeatShoreBridgeState");
    state.setProperty("version", stateVersion, nullptr);

    juce::ValueTree params("Parameters");
    for (auto* param : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
        {
            juce::ValueTree p("Param");
            p.setProperty("id", ranged->getParameterID(), nullptr);
            p.setProperty("value", ranged->getValue(), nullptr); // normalized 0..1 -- range-independent, survives a future range change
            params.appendChild(p, nullptr);
        }
    }
    state.appendChild(params, nullptr);

    juce::ValueTree humanize("Humanize");
    humanize.setProperty("timing", humanizeSettings.timing, nullptr);
    humanize.setProperty("velocity", humanizeSettings.velocity, nullptr);
    humanize.setProperty("dynamics", humanizeSettings.dynamics, nullptr);
    humanize.setProperty("articulation", humanizeSettings.articulation, nullptr);
    humanize.setProperty("preserveGroove", humanizeSettings.preserveGroove, nullptr);
    state.appendChild(humanize, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void BeatShoreBridgeAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        auto state = juce::ValueTree::fromXml(*xml);
        const int loadedVersion = state.getProperty("version", 0);
        if (loadedVersion > stateVersion)
        {
            // Saved by a newer plugin version than this one understands.
            // Ignore rather than guess — do not silently corrupt state.
            juce::Logger::writeToLog("BeatShoreBridge: state version " + juce::String(loadedVersion)
                                      + " is newer than this build (" + juce::String(stateVersion) + "); ignoring saved state.");
            return;
        }

        if (auto params = state.getChildWithName("Parameters"); params.isValid())
        {
            for (const auto& p : params)
            {
                const juce::String id = p.getProperty("id", juce::String());
                const float value = p.getProperty("value", 0.0f);
                for (auto* param : getParameters())
                {
                    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param); ranged != nullptr && ranged->getParameterID() == id)
                    {
                        // setValueNotifyingHost(), not setValue() directly --
                        // this is the standard JUCE way to programmatically
                        // change a parameter from inside the processor (as
                        // opposed to a UI gesture) while still keeping the
                        // host and any UI attachments correctly in sync.
                        ranged->setValueNotifyingHost(value);
                        break;
                    }
                }
            }
        }

        if (auto humanize = state.getChildWithName("Humanize"); humanize.isValid())
        {
            humanizeSettings.timing = humanize.getProperty("timing", 0.0f);
            humanizeSettings.velocity = humanize.getProperty("velocity", 0.0f);
            humanizeSettings.dynamics = humanize.getProperty("dynamics", 0.0f);
            humanizeSettings.articulation = humanize.getProperty("articulation", 0.0f);
            humanizeSettings.preserveGroove = humanize.getProperty("preserveGroove", false);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BeatShoreBridgeAudioProcessor();
}
