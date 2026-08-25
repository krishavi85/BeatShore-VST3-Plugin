# BeatShore VST3 Plugin

BeatShore is a native Windows VST3 plugin (`BeatShore Bridge`) paired with a
desktop broker process (`BeatShore Desktop`) that runs tempo analysis and
polyphonic (piano/guitar) transcription — via Spotify's Basic Pitch model
through `@tensorflow/tfjs-node` — entirely on the user's own machine, with
results exported as Standard MIDI files a DAW can import directly.

**Current status: controlled Windows beta candidate, not a public release.**
See [`native/BeatShoreBridge/RELEASE_STATUS.md`](native/BeatShoreBridge/RELEASE_STATUS.md)
for the current, short-form release status (capabilities, limitations, exact
artifact hashes, supported hosts, known issues, and the release acceptance
checklist). [`native/BeatShoreBridge/STATUS.md`](native/BeatShoreBridge/STATUS.md)
is the full chronological engineering log.

## Architecture

- **`native/BeatShoreBridge/`** — the VST3 plugin (JUCE), capturing audio in
  the host DAW and talking to the desktop broker over a named pipe.
- **`native/BeatShoreDesktop/`** — the desktop broker: owns the named pipe,
  shared-memory audio transfer, job scheduling, and a system tray UI.
- **`native/BeatShoreDesktop/engine/`** — the Node.js analysis engine
  (tempo DSP, Basic Pitch transcription, MIDI export).
- **`native/protocol/`** — the shared IPC protocol (overlapped named-pipe
  I/O, shared-memory audio buffers) used by both the plugin and the broker.
- **`native/installer/`** — the Inno Setup installer script and
  `build-release.ps1`, a reproducible release build script (rebuild from
  source, unconditional restage, full regression suite against the staged
  tree, then compile).

## Building

Requires MSVC (Visual Studio Build Tools), CMake, Ninja, Node.js 24 LTS, and
the vendored JUCE 9.0.1 / VST3 SDK 3.8.1 (not included in this repo — see
`.gitignore`'s own comments for why, and `STATUS.md` for the pinned
versions). See `native/installer/build-release.ps1` for the exact build
sequence this project uses to produce a release.

## License

BeatShore's own source is not yet under a published open-source license.
Third-party components (JUCE, the VST3 SDK, Node.js, TensorFlow, Basic
Pitch) are used under their own respective licenses — see
`native/installer/stage/Licenses/` (produced by a release build) for the
full attribution texts.
