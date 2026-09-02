#pragma once
#include <atomic>
#include <string>

// Shared between PluginProcessor.h and BridgeClient.h. Split into its own
// header (rather than living in either) purely to avoid those two headers
// including each other: PluginProcessor needs BridgeStatus/
// BridgeAnalysisResult for its public API, BridgeClient needs
// PluginProcessor's forward declaration for the same reason. No JUCE types
// here on purpose, so this header stays cheap to include anywhere.

// Bridge connection state to the BeatShore desktop process. Reflects
// BridgeClient's actual connection state -- see BridgeClient.h for the real
// connect/HELLO/ANALYSIS_REQUEST implementation.
enum class BridgeStatus
{
    Disconnected, // no desktop process reachable right now -- retrying on a timer
    Connecting,
    Connected,
    Error
};

// Host context snapshot, written from the audio thread (processBlock) and
// read from both the (non-realtime) editor thread and BridgeClient's IPC
// thread (to build HOST_STATE messages -- see PROTOCOL.md). Every field is
// a lock-free atomic -- no mutex, no allocation, nothing that could block
// the audio callback.
struct HostSnapshot
{
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<int> blockSize { 0 };
    std::atomic<double> bpm { 0.0 };
    std::atomic<int> timeSigNumerator { 4 };
    std::atomic<int> timeSigDenominator { 4 };
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> isRecording { false };
    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<bool> hostProvidesTransport { false };
};

// Humanization amounts set on the editor's Humanize page, applied to
// whichever MIDI-producing kind (transcribePolyphonic/transcribeDrums/
// transcribeMono) is triggered next -- see PluginProcessor::triggerAnalysisOfKind()
// and BridgeClient::requestAnalysis(). Values are 0..1 (a 0-100% slider /
// 100), matching what analyze.js's dsp.applyHumanization() itself expects;
// this struct crosses the BridgeClient boundary unmodified, no unit
// conversion happens on the way. All-zero (the default) means "not
// requested" -- a request built from a default-constructed one behaves
// exactly as it did before this feature existed.
struct HumanizeSettings
{
    float timing = 0.0f, velocity = 0.0f, dynamics = 0.0f, articulation = 0.0f;
    bool preserveGroove = false;
    bool isActive() const { return timing > 0.0f || velocity > 0.0f || dynamics > 0.0f || articulation > 0.0f; }
};

struct BridgeAnalysisResult
{
    std::string requestIdEcho;
    std::string kind;
    bool success = false;
    std::string message;         // error text, or a human-readable summary of the result
    bool hasNumericValue = false;
    double numericValue = 0.0;
    std::string errorCode;       // set (non-empty) only when !success -- e.g. "SHM_OPEN_FAILED", see PROTOCOL.md
    std::string algorithm;       // set (non-empty) only when success -- which function on the desktop produced this
    int computeMs = -1;          // -1 = not reported. Pure DSP time, measured on the desktop's Node engine.
    int desktopTotalMs = -1;     // -1 = not reported. Full desktop-side handling time (shm open + IPC + compute).
    std::string audioSource;     // echoed from the request that produced this result -- "live-captured" or "file"

    // MIDI_RESULT-specific (transcribeDrums/transcribeMono/transcribePolyphonic).
    // noteCount stays -1 for a plain ANALYSIS_RESULT (tempo/key/etc) so the
    // UI can tell "this wasn't a transcription" apart from "0 notes found".
    int noteCount = -1;
    std::string midiPath;        // empty if no file was written (e.g. 0 notes, or the write itself failed -- see midiWriteError)
    std::string midiSha256;
    std::string midiWriteError;  // non-empty only if notes were found but the file write failed -- the transcription itself still succeeded
    int midiSizeBytes = -1;      // -1 = not reported (e.g. midiPath is empty)
    bool humanizeApplied = false; // echoes analyze.js's own humanizeApplied -- true only if at least one HumanizeSettings amount was actually nonzero for this request
    std::string midiGeneratedAt; // ISO8601, empty if not reported
};
