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
    std::string midiGeneratedAt; // ISO8601, empty if not reported
};
