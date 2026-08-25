#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "BridgeTypes.h"

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

    // Shared by triggerTempoAnalysis()/triggerPolyphonicTranscription() --
    // capture-and-send, parameterized only by which analysis kind to ask
    // BridgeClient for.
    CaptureTriggerResult triggerAnalysisOfKind(const juce::String& kind);

    HostSnapshot hostSnapshot;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatShoreBridgeAudioProcessor)
};
