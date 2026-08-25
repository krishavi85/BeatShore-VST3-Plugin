#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BridgeClient.h"
#include <algorithm>

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
    return bridgeClient->takeResult(out);
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

BeatShoreBridgeAudioProcessor::CaptureTriggerResult BeatShoreBridgeAudioProcessor::triggerAnalysisOfKind(const juce::String& kind)
{
    if (bridgeClient->getStatus() != BridgeStatus::Connected) return CaptureTriggerResult::NotConnected;
    if (bridgeClient->isRequestInFlight()) return CaptureTriggerResult::AlreadyInFlight;

    std::vector<float> interleaved;
    float peak = 0.0f;
    const auto outcome = captureFrozenSnapshot(interleaved, peak);
    if (outcome == SnapshotOutcome::Busy) return CaptureTriggerResult::AlreadyInFlight;
    if (outcome != SnapshotOutcome::Ok) return CaptureTriggerResult::NoAudioCaptured;
    if (peak < 1.0e-4f) return CaptureTriggerResult::SilentAudio;

    const bool started = bridgeClient->requestAnalysis(interleaved, uint32_t(captureSampleRate), 2, uint32_t(interleaved.size() / 2), kind, "live-captured", getHostTrackName());
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
void BeatShoreBridgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

void BeatShoreBridgeAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("BeatShoreBridgeState");
    state.setProperty("version", stateVersion, nullptr);
    // No user-configurable parameters exist yet — this is a placeholder
    // structure so a future version has somewhere to add them without
    // breaking the version-check path below.
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
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BeatShoreBridgeAudioProcessor();
}
