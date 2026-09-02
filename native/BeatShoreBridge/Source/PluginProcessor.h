#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "BridgeTypes.h"
#include "MixChain.h"
#include "MasterMeter.h"

class BridgeClient;

class BeatShoreBridgeAudioProcessor final : public juce::AudioProcessor,
                                             private juce::Timer
{
public:
    BeatShoreBridgeAudioProcessor();
    ~BeatShoreBridgeAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    // Real-time audio callback. Passthrough only — see the .cpp for why
    // nothing else belongs here yet.
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    // Versioned plugin state (plan section 2, point 5: "save a small,
    // versioned plugin state and reconnect after BeatShore restarts"). The
    // reconnect half needs the bridge protocol below; the save/restore half
    // is real today.
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Not every host reports this (TrackProperties::name is
    // std::optional<String> for exactly that reason) -- used only for
    // human-readable MIDI export filenames on the desktop side (see
    // BridgeClient.h's requestAnalysis()/midi-export.js), never for
    // anything that needs to be reliable. Called by the host off the audio
    // thread (a track rename is not a realtime event), so a plain mutex is
    // fine here unlike hostSnapshot's atomics.
    void updateTrackProperties(const juce::AudioProcessor::TrackProperties& properties) override;
    juce::String getHostTrackName() const;

    const HostSnapshot& getHostSnapshot() const { return hostSnapshot; }
    BridgeStatus getBridgeStatus() const;

    enum class CaptureTriggerResult
    {
        Started,          // handed off to BridgeClient; a result or error will arrive via takeAnalysisResult()
        NotConnected,
        AlreadyInFlight,  // a previous request (or snapshot swap) hasn't finished yet
        NoAudioCaptured,  // nothing captured yet, or the audio thread never got to service the swap (host not calling processBlock)
        SilentAudio        // captured audio exists but every sample is at/near zero
    };

    // Editor-facing analysis triggers: hand the audio thread's most recently
    // frozen capture buffer to BridgeClient. See the .cpp / processBlock for
    // the double-buffered swap this relies on to avoid ever reading a buffer
    // the audio thread is concurrently writing. Message-thread only. Both
    // are thin wrappers over the same triggerAnalysisOfKind() -- kept as
    // separate named methods (not one generic string-taking method exposed
    // to the editor) so the public API stays self-documenting about which
    // analysis kinds actually have a UI path today.
    CaptureTriggerResult triggerTempoAnalysis();
    CaptureTriggerResult triggerPolyphonicTranscription();

    // Same pattern as the two above -- thin wrappers over
    // triggerAnalysisOfKind(), each just naming a kind the desktop's own
    // analyze.js has always supported (see its SUPPORTED_KINDS) but that
    // had no UI path until now. Result shape on the wire is identical to
    // tempo's (a plain ANALYSIS_RESULT: a number or JSON-stringified value
    // in BridgeAnalysisResult::message) -- no protocol change needed for
    // these three.
    CaptureTriggerResult triggerKeyAnalysis();
    CaptureTriggerResult triggerChordAnalysis();
    CaptureTriggerResult triggerLoudnessAnalysis();

    // Same MIDI_RESULT shape as triggerPolyphonicTranscription() (noteCount,
    // midiPath, etc. in BridgeAnalysisResult) -- transcribeDrums and
    // transcribeMono already went through the identical desktop/engine code
    // path as transcribePolyphonic (see analyze.js), just never had a
    // trigger method. Bass/lead are two separate methods, not one with a
    // parameter, matching the pattern above of the public API staying
    // self-documenting about what's actually wired to the editor -- both
    // call triggerAnalysisOfKind("transcribeMono", ...) with a different
    // role ("bass" narrows the pitch range analyze.js searches; "lead"
    // uses the default range).
    CaptureTriggerResult triggerDrumTranscription();
    CaptureTriggerResult triggerBassTranscription();
    CaptureTriggerResult triggerLeadTranscription();

    // Same MIDI_RESULT shape again, this time from BeatShoreDesktop's
    // Python-routed MT3 worker (kind "transcribeMt3" -- see main.cpp's
    // runMt3Worker and mt3_engine.py) rather than analyze.js's own
    // basic-pitch path triggerPolyphonicTranscription() above uses. A
    // separate, genuinely different neural transcription model, not a
    // duplicate button for the same thing -- kept as its own named
    // trigger method for the same self-documenting-API reason as every
    // other trigger* method here.
    CaptureTriggerResult triggerMt3Transcription();

    // Cancels whatever analysis/transcription request is currently in
    // flight (any kind -- not MT3-specific). BridgeClient has supported
    // real cancellation since it was written (see its requestCancel()'s
    // own comment, which called this "a future Cancel button"); this is
    // simply that button's first real caller. Returns false immediately,
    // without sending anything, if nothing is in flight.
    bool cancelAnalysis();

    // Real MIDI preview: plays the most recently completed MIDI-producing
    // transcription's written .mid file back out through this plugin's
    // own MIDI output (producesMidi() -- see processBlock()). BeatShore
    // Bridge has no piano-roll widget of its own to "display notes
    // through" -- it's a thin bridge plugin hosted inside a real DAW,
    // and the DAW's own piano roll is the genuine existing MIDI path a
    // plugin like this has. Route this plugin's MIDI output to a synth
    // to hear it, or record-arm a downstream MIDI/instrument track to
    // capture it and get real editing in the host's own piano roll --
    // this plugin does not (and does not pretend to) implement either
    // playback synthesis or note editing itself.
    // An independent, free-running, looping clock -- NOT locked to the
    // host's transport position; there's no meaningful relationship
    // between a rolling ~10s capture buffer's contents and wherever the
    // host playhead happens to be right now. Message-thread only.
    void setMidiPreviewEnabled(bool shouldPlay);
    bool isMidiPreviewEnabled() const { return midiPreviewEnabled.load(); }
    bool hasMidiPreviewLoaded() const { return activeMidiPreview.load() != nullptr; }
    double getMidiPreviewLengthSeconds() const { return midiPreviewLengthSeconds.load(); }
    double getMidiPreviewPositionSeconds() const { return midiPreviewPositionSeconds.load(); }

    // Set from the editor's Humanize page; applied to whichever
    // MIDI-producing kind is triggered NEXT (not retroactively to an
    // already-completed transcription -- there is no live-editable note
    // buffer in this plugin, only "set your amounts, then transcribe").
    // Message-thread only, same as every trigger* method.
    void setHumanizeSettings(HumanizeSettings settings) { humanizeSettings = settings; }
    HumanizeSettings getHumanizeSettings() const { return humanizeSettings; }

    // Real host-automatable parameters (see the header comment on mixChain
    // below) -- exposed so the editor's Mix page can bind real
    // juce::SliderParameterAttachment/ButtonParameterAttachment objects
    // directly to them (the correct, idiomatic JUCE way to connect a UI
    // control to an AudioProcessorParameter -- proper thread-safety,
    // automation gesture recording, and bidirectional sync all handled by
    // JUCE itself, not reimplemented here) rather than PluginProcessor
    // exposing its own get/set wrapper methods the way HumanizeSettings
    // above does, since these genuinely are host parameters and
    // Humanize's amounts are not.
    juce::RangedAudioParameter& getMixEnabledParameter() { return *mixEnabledParam; }
    juce::RangedAudioParameter& getEqLowShelfGainParameter() { return *eqLowShelfGainParam; }
    juce::RangedAudioParameter& getEqMidPeakGainParameter() { return *eqMidPeakGainParam; }
    juce::RangedAudioParameter& getEqHighShelfGainParameter() { return *eqHighShelfGainParam; }
    juce::RangedAudioParameter& getCompThresholdParameter() { return *compThresholdParam; }
    juce::RangedAudioParameter& getCompRatioParameter() { return *compRatioParam; }
    juce::RangedAudioParameter& getLimiterThresholdParameter() { return *limiterThresholdParam; }

    // Real EBU R128 loudness + true-peak metering (see MasterMeter.h) of
    // whatever this plugin is actually about to hand back to the host --
    // after Mix, if Mix is enabled. Message-thread only (a Timer poll);
    // the atomics inside Snapshot are what make that safe to read while
    // the audio thread concurrently writes them every block.
    const MasterMeter::Snapshot& getMasterSnapshot() const { return masterSnapshot; }
    // Message-thread: just raises a flag processBlock() services on the
    // audio thread (same request/service pattern as the ring-buffer swap
    // above) -- resetting libebur128's internal state directly from here
    // would race a concurrent add_frames_float() call.
    void requestMasterMeterReset() { masterMeterResetRequested.store(true, std::memory_order_relaxed); }

    bool isAnalysisInFlight() const;
    double getAnalysisProgress() const; // 0..1, meaningful only while isAnalysisInFlight()

    // Cheap, non-blocking proxy for "is there audio worth analyzing right
    // now" -- true once the audio thread has captured at least a little
    // into the active buffer, or a swap has already produced an unconsumed
    // frozen one. Not a precise duration readout (see STATUS.md); just
    // enough for the UI to grey out a trigger button before the user wastes
    // a click on an obviously-empty capture.
    bool hasCapturedAudio() const;

    // Test-only: runs the identical swap + chronological read-out as
    // triggerTempoAnalysis() (see captureFrozenSnapshot()), but without the
    // BridgeClient connection gate, so native/BridgeClientTest's stress test
    // can exercise and verify the buffer's concurrency behavior against a
    // live processBlock() thread without needing a BeatShore desktop
    // process running. Not part of the plugin's real runtime call path.
    bool captureSnapshotForTest(std::vector<float>& outInterleaved, double& outSampleRate);

    // Message-thread only (editor Timer). See BridgeClient::takeResult.
    bool takeAnalysisResult(BridgeAnalysisResult& out);

    static constexpr int stateVersion = 1;
    static constexpr double captureSeconds = 10.0;

private:
    // Runs on this processor's own juce::Timer (started in the
    // constructor), independent of whether an editor window is open --
    // automation and MIDI-learned controls need to work with the plugin
    // window closed, same as any other host automation. Services a rising
    // edge on triggerAnalysisParam (see processBlock(), which is where the
    // edge is actually detected) by calling triggerTempoAnalysis() and
    // resetting the parameter back to off. This is what makes "automation
    // bridge" a real, host-visible capability rather than just a UI button
    // -- REAPER can MIDI-map a controller directly to this parameter
    // (right-click it in the FX chain -> "Learn").
    void timerCallback() override;

    enum class SnapshotOutcome { Ok, NoAudio, Timeout, Busy };
    // Shared by triggerTempoAnalysis() and captureSnapshotForTest() -- the
    // swap-request/wait/read-out sequence itself, with no knowledge of
    // BridgeClient. Message-thread only (waits, so never call from
    // processBlock).
    SnapshotOutcome captureFrozenSnapshot(std::vector<float>& outInterleaved, float& outPeak);

    // Shared by every trigger* method above -- capture-and-send,
    // parameterized by which analysis kind to ask BridgeClient for. role is
    // only ever non-empty for triggerBassTranscription()/
    // triggerLeadTranscription() -- see BridgeClient::requestAnalysis()'s
    // own comment on it.
    CaptureTriggerResult triggerAnalysisOfKind(const juce::String& kind, const juce::String& role = juce::String());

    // Parses a completed MIDI_RESULT's written file into a flat,
    // tempo-converted-to-seconds juce::MidiMessageSequence for
    // processBlock() to play back -- called only from takeAnalysisResult()
    // when a MIDI-producing kind's result arrives with a real midiPath.
    // Message-thread only.
    void loadMidiPreviewFile(const juce::String& midiPath);

    HostSnapshot hostSnapshot;
    HumanizeSettings humanizeSettings; // all-zero default -- see setHumanizeSettings()

    std::unique_ptr<BridgeClient> bridgeClient;

    // Double-buffered rolling capture: the audio thread only ever writes
    // into ringBuffers[activeBufferIndex]; triggerTempoAnalysis() only ever
    // reads ringBuffers[frozenBufferIndex]. Those are never the same buffer
    // at the same time -- the swap in processBlock atomically hands the
    // *entire* currently-active buffer over to the reader and starts
    // writing into the other one, so there is no window where a reader and
    // the audio thread touch the same sample data. Contrast with the
    // earlier single-buffer + "pause writes" flag design this replaced:
    // that flag only stopped the *next* write from starting, not an
    // in-progress one, which left a real (if narrow) torn-read window.
    // See processBlock() for the swap and the .cpp for the read-out.
    //
    // That correctness claim was, until now, only *proven by reasoning*
    // about the rest of the call graph (JUCE's message thread is single-
    // threaded, and captureFrozenSnapshot() refuses to request a second
    // swap while frozenBufferIndex != -1) -- nothing in the buffer-swap
    // code itself enforced it, so a future change elsewhere (a second
    // reader, a changed gating check) could silently violate it. bufferState
    // makes the invariant self-checking instead of externally assumed: each
    // slot has an explicit ownership state, transitions are validated in
    // the two places that mutate them, and processBlock's swap refuses --
    // logs and skips, never blocks or corrupts -- rather than reclaiming a
    // buffer that isn't Free. See processBlock() and captureFrozenSnapshot().
    enum class BufferState { Free, Writing, Ready, Reading };
    static constexpr int kSnapshotSlots = 2;
    juce::AudioBuffer<float> ringBuffers[kSnapshotSlots];
    std::atomic<BufferState> bufferState[kSnapshotSlots] { { BufferState::Writing }, { BufferState::Free } };
    std::atomic<int> activeBufferIndex { 0 };          // audio thread: which buffer it's writing into
    std::atomic<juce::int64> activeWritePos { 0 };      // audio thread only
    std::atomic<juce::int64> activeFramesWritten { 0 }; // audio thread only, capped at buffer capacity

    std::atomic<bool> swapRequested { false };   // message thread sets; audio thread consumes
    std::atomic<int> frozenBufferIndex { -1 };   // -1 = no unconsumed snapshot; else index into ringBuffers, owned by the reader until it resets this to -1
    std::atomic<juce::int64> frozenFrameCount { 0 };
    std::atomic<juce::int64> frozenWritePos { 0 };

    // Guards buffer *reallocation* (prepareToPlay) against a concurrent
    // frozen-buffer read-out (triggerTempoAnalysis). Never taken by
    // processBlock -- prepareToPlay is never called concurrently with
    // processBlock per the JUCE contract, so the audio thread doesn't need
    // this lock, only the rare prepareToPlay-during-an-active-read case does.
    std::mutex bufferStructureMutex;
    double captureSampleRate = 0.0;

    mutable std::mutex trackNameMutex;
    juce::String hostTrackName;

    // Real, host-automatable parameter (owned by juce::AudioProcessor's
    // parameter list, not just a UI control): processBlock() reads it
    // every block -- cheap, lock-free, real-time safe -- and detects a
    // false->true edge, which is the only thing that crosses to
    // pollParameterTriggeredAnalysis() (an atomic flag, not the parameter
    // object itself; the audio thread never touches BridgeClient or waits
    // on anything).
    juce::AudioParameterBool* triggerAnalysisParam = nullptr;
    bool lastTriggerParamValue = false;    // audio thread only
    std::atomic<bool> triggerRequestedByHost { false };

    // Real EQ/Compressor/Limiter run in processBlock() itself (see
    // MixChain.h) on this plugin's own live audio -- unlike every trigger*
    // method above, this isn't a request to the desktop broker, it's this
    // plugin's own signal processing. Every parameter here is a real,
    // host-automatable AudioParameterFloat/Bool (addParameter()'d in the
    // constructor, same pattern as triggerAnalysisParam above), read with
    // the same cheap lock-free .get() every block. mixEnabledParam defaults
    // to false and every gain/ratio/threshold defaults to a fully
    // transparent value, so a session that never touches the Mix page gets
    // audio identical to before this feature existed -- not just "close
    // enough", bit-for-bit: processBlock() skips calling mixChain.process()
    // entirely while mixEnabledParam is false, rather than relying on
    // MixChain's own internal bypass path to be a no-op.
    MixChain mixChain;
    juce::AudioParameterBool* mixEnabledParam = nullptr;
    juce::AudioParameterFloat* eqLowShelfGainParam = nullptr;
    juce::AudioParameterFloat* eqMidPeakGainParam = nullptr;
    juce::AudioParameterFloat* eqHighShelfGainParam = nullptr;
    juce::AudioParameterFloat* compThresholdParam = nullptr;
    juce::AudioParameterFloat* compRatioParam = nullptr;
    juce::AudioParameterFloat* limiterThresholdParam = nullptr;
    // Cached previous parameter values (audio thread only) -- MixChain's
    // setters recompute real IIR coefficients, cheap but not free; only
    // calling them when a value has actually changed since the last block
    // avoids redoing that work ~every block for a slider nobody is moving,
    // without needing any parameter-smoothing machinery this feature's
    // scope doesn't call for.
    float lastEqLowGain = 0.0f, lastEqMidGain = 0.0f, lastEqHighGain = 0.0f;
    float lastCompThreshold = 0.0f, lastCompRatio = 1.0f, lastLimiterThreshold = 0.0f;

    // Real EBU R128 loudness/true-peak metering for the Master page (see
    // MasterMeter.h) -- always fed, unlike MixChain above, since observing
    // audio costs far less than filtering/compressing it and (unlike Mix)
    // there's no "disabled" state that would make skipping it meaningful:
    // a meter with nothing to show is just idle numbers, not extra risk.
    // masterSnapshot is written by processBlock() (audio thread) and read
    // by the editor's Timer (message thread) -- safe because every field
    // is a std::atomic, the same cross-thread contract hostSnapshot uses.
    MasterMeter masterMeter;
    MasterMeter::Snapshot masterSnapshot;
    std::atomic<bool> masterMeterResetRequested { false }; // message thread sets; audio thread services (see requestMasterMeterReset())

    // ---- Real MIDI preview (see setMidiPreviewEnabled()) -----------
    // Every sequence ever loaded this session is kept alive here rather
    // than freed the instant a newer one replaces it in activeMidiPreview:
    // the audio thread only ever reads activeMidiPreview via a single
    // atomic load per block and never touches whatever it pointed at
    // beyond that block, but freeing the OLD pointee the moment a new one
    // is published would still race a block that grabbed the old pointer
    // microseconds earlier. Manually-triggered transcriptions are rare (at
    // most a handful per session) and each sequence is tiny (at most a few
    // hundred MIDI events) -- never freeing them is genuinely negligible
    // memory, not a real leak; a generation-counted reclaim scheme would
    // add real complexity for no measurable benefit here.
    std::vector<std::unique_ptr<juce::MidiMessageSequence>> retiredMidiPreviews; // message thread only
    std::atomic<juce::MidiMessageSequence*> activeMidiPreview { nullptr }; // raw, non-owning -- see retiredMidiPreviews above
    std::atomic<bool> midiPreviewEnabled { false };          // message thread sets; audio thread reads
    std::atomic<bool> midiPreviewStopRequested { false };    // message thread sets (on disable, or a new file replacing the one mid-playback); audio thread services by flushing one All-Notes-Off, then clears it
    std::atomic<double> midiPreviewLengthSeconds { 0.0 };
    std::atomic<double> midiPreviewPositionSeconds { 0.0 };  // audio thread owns; read-only everywhere else (UI progress readout)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessor)
};
