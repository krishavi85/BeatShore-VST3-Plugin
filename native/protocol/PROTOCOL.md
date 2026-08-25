# BeatShore Bridge Protocol v1

Connects the VST3 plugin (`BeatShoreBridge`, C++/JUCE) to the BeatShore
desktop process (`BeatShoreDesktop`, C++/JUCE, delegating actual analysis to
a Node.js child process that reuses the existing browser DSP/ML code
unmodified). Two channels:

- **Control** — a Windows named pipe, `\\.\pipe\BeatShoreBridge.v1`. The
  desktop process is the server and accepts multiple simultaneous plugin
  connections (one pipe instance per connection, each serviced by its own
  thread — see STATUS.md's "Multiple plugin instances"); the plugin is the
  client, and reconnects with backoff if the desktop process isn't running.
  Framing is newline-delimited JSON (NDJSON) — one JSON object per line,
  UTF-8. NDJSON over pure-text framing so both a C++ JSON writer and Node's
  `JSON.stringify` produce byte-identical, greppable line output; a
  length-prefix framing was considered and rejected as harder to debug for
  no benefit at this message rate (this is control-plane traffic, not the
  audio path).
- **Audio** — a named shared-memory (file-mapping) segment, created by the
  plugin per request and referenced by name in `ANALYSIS_REQUEST`. Audio
  never goes through the pipe as JSON/base64 — only its shared-memory
  segment name and a `{sampleRate, channels, frames}` header do.

Every message is a single-line JSON object with at least `type` and `v`
(protocol version, currently `1`). The real-time audio callback
(`processBlock`) never touches this protocol directly — see
`native/BeatShoreBridge/Source/PluginProcessor.cpp` for how *captured audio*
crosses from the audio thread to the (non-realtime) message thread that
actually speaks this protocol: a double-buffered ring (`ringBuffers[2]`)
where processBlock does an O(1) pointer swap, never a copy, and the message
thread only ever reads the buffer processBlock isn't currently writing to.
No lock is held by processBlock; the message-thread side takes a mutex only
to guard against a same-time buffer *reallocation* (a sample-rate change),
not against processBlock itself.

## Connection lifecycle

```
plugin                              desktop
  | --- connect (named pipe) ----->  |
  | --- HELLO ---------------------> |
  |                     <----------- CAPABILITIES
  | --- HOST_STATE (periodic) -----> |
  | --- HEARTBEAT (every 5s idle) -> |
  |                     <----------- HEARTBEAT_ACK
  | --- ANALYSIS_REQUEST ----------> |
  |                     <----------- ANALYSIS_PROGRESS (0..1, repeated)
  |                     <----------- ANALYSIS_RESULT | MIDI_RESULT | AUDIO_RESULT
  | --- CANCEL (optional) ---------> |
  |                     <----------- ERROR (any time, either direction)
  |                     <----------- BROKER_SHUTTING_DOWN (any time, desktop-initiated)
```

If the desktop process is absent, the plugin's connect attempt simply fails
and it retries on a timer — normal audio passthrough is completely
unaffected either way (see `BridgeStatus` in PluginProcessor.h: the plugin
is fully functional with the bridge in any state, including forever
disconnected).

## Messages

All fields beyond `type`/`v` are message-specific.

### `HELLO` (plugin -> desktop)
```json
{"type":"HELLO","v":1,"pluginVersion":"0.1.0","pid":1234,"sessionId":"a1b2c3"}
```

### `CAPABILITIES` (desktop -> plugin)
```json
{"type":"CAPABILITIES","v":1,"desktopVersion":"0.1.0",
 "analysis":["tempo","key","structure","chords","transcribeDrums","transcribeMono","transcribePolyphonic","timbre"],
 "export":["wav","midi","rpp"]}
```

### `HOST_STATE` (plugin -> desktop, sent on change and every ~2s)
```json
{"type":"HOST_STATE","v":1,"sampleRate":48000,"blockSize":512,
 "bpm":120.0,"timeSigNum":4,"timeSigDen":4,"isPlaying":false,"playheadSeconds":0.0}
```

### `ANALYSIS_REQUEST` (plugin -> desktop)
```json
{"type":"ANALYSIS_REQUEST","v":1,"requestId":"a1b2c3d4-...",
 "kind":"transcribeMono","role":"bass","audioSource":"live-captured",
 "audio":{"shm":"Local\\BeatShoreAudio.a1b2c3.r1","sampleRate":48000,"channels":1,"frames":960000},
 "tempo":120.0,"hostTrackName":"Piano Take 3"}
```
`kind` is one of the `analysis` capabilities `CAPABILITIES` advertised.
`audio.shm` names a file mapping the plugin already created and written
interleaved `float32` samples into (see Shared memory layout below).
`requestId` is a UUID (`juce::Uuid` on the plugin side), not a small
counter — request IDs must stay unique even once more than one plugin
instance can be connected at a time (not yet built, see STATUS.md).
`audioSource` says where the audio actually came from: `"live-captured"`
for `BeatShoreBridgeAudioProcessor`'s real double-buffered capture off a
DAW track (the only value the real plugin ever sends), `"file"` for test
harnesses that load a fixture file (`BeatShoreTestClient`,
`BridgeClientTest`). It exists specifically so a desktop log line can be
audited later for whether a given result came from real DAW-hosted audio or
a test fixture — don't report end-to-end verification based on a log
showing anything other than `"live-captured"`. `hostTrackName` is optional
and omitted entirely when absent — populated from JUCE's
`AudioProcessor::updateTrackProperties()` when the host reports a track
name (not every host does); used only for a human-readable `MIDI_RESULT`
export filename (see below), never for anything that needs to be reliable
or collision-proof.

### `ANALYSIS_PROGRESS` (desktop -> plugin, zero or more per request)
```json
{"type":"ANALYSIS_PROGRESS","v":1,"requestId":"a1b2c3d4-...","progress":0.42}
```

### `ANALYSIS_RESULT` (desktop -> plugin — generic/analysis-shaped results: tempo, key, structure, timbre, chords)
```json
{"type":"ANALYSIS_RESULT","v":1,"requestId":"a1b2c3d4-...","kind":"tempo","result":128.03,
 "algorithm":"beatshore-dsp.estimateTempo (spectral-flux autocorrelation)",
 "computeMs":12,"desktopTotalMs":47}
```
`algorithm` names the actual function that produced `result` (see
`ALGORITHMS` in `analyze.js`) — not a confidence score, because none of
v1's analysis functions compute one; don't infer confidence from anything
here. `computeMs` is pure DSP time measured inside the Node engine;
`desktopTotalMs` is the full desktop-side handling time (shared-memory
open + temp-file write + IPC + compute), added by `BeatShoreDesktop` itself
as it relays the terminal message.

### `MIDI_RESULT` (desktop -> plugin — transcription results)
```json
{"type":"MIDI_RESULT","v":1,"requestId":"a1b2c3d4-...",
 "algorithm":"beatshore-dsp.transcribeMono (monophonic pitch track)",
 "computeMs":34,"desktopTotalMs":81,"noteCount":92,
 "notes":[{"time":0.12,"dur":0.24,"midi":45,"vel":90}],
 "midiPath":"C:\\Users\\...\\Documents\\BeatShore\\Exports\\BeatShore_Piano Take 3_transcribeMono_a1b2c3d4-....mid",
 "sha256":"...","midiSizeBytes":1842,"midiGeneratedAt":"2026-08-24T12:34:56.789Z"}
```
`midiPath`/`sha256`/`midiSizeBytes`/`midiGeneratedAt` are present only when
`noteCount > 0` and the file write succeeded (see `midi-export.js`); a write
failure on a non-empty transcription instead sets `midiWriteError` and omits
those four — the transcription itself still succeeded even though no file
exists. `midiPath`'s filename embeds `hostTrackName` (sanitized, capped at
40 characters, `"Untitled"` if absent) and `kind` for readability, but the
`requestId` UUID it also always embeds — never truncated — is what actually
guarantees no two exports ever collide, regardless of what `hostTrackName`
is or whether it's present at all.

### `AUDIO_RESULT` (desktop -> plugin — reserved for future reconstructed-audio results; not produced by v1's analysis set)
```json
{"type":"AUDIO_RESULT","v1":1,"requestId":"a1b2c3d4-...","audio":{"shm":"...","sampleRate":48000,"channels":2,"frames":96000}}
```

### `CANCEL` (plugin -> desktop)
```json
{"type":"CANCEL","v":1,"requestId":"a1b2c3d4-..."}
```
Genuine, not a best-effort label -- see STATUS.md's "Genuine cancellation"
section for the desktop-side architecture (a per-session pipe-owner thread,
a shared job queue with an explicit state machine, and a Node-transport
worker thread) that makes this real. The desktop always replies with a
single `ERROR` naming one of four outcomes via `errorCode`:
- **`CANCELLED`** -- the named request was `QUEUED` (not yet started) and
  is now cancelled immediately, or it was `RUNNING` and has now been force-
  stopped (the desktop kills and restarts its Node engine process, since
  an in-flight Node/TensorFlow computation can't be asked to abort
  cooperatively -- verified to fully recover: a request sent immediately
  after a hard-cancel succeeds normally through the freshly-restarted
  engine).
- **`CANCEL_REQUESTED`** -- sent *first*, immediately, only when the named
  request was genuinely `RUNNING` at the moment the `CANCEL` arrived, as an
  acknowledgement that cancellation is underway; the `CANCELLED` terminal
  message above always follows shortly after (in practice, well under a
  second — bounded by how long killing and restarting the Node process
  takes, not by the request's own kind-aware timeout). A `CANCEL` for a
  `QUEUED` (not yet running) request skips straight to `CANCELLED` — there
  is nothing in flight to acknowledge separately.
- **`ALREADY_COMPLETED`** -- the named request already reached a terminal
  state (a real result, an error, or an earlier cancellation) before this
  `CANCEL` arrived; `message` names which via `jobStateName()`.
- **`REQUEST_NOT_FOUND`** -- the `requestId` never existed (or aged out of
  the desktop's bounded job history — see `JobRegistry` in
  `AnalysisScheduler.h`), or it belongs to a *different* session. One
  plugin instance cannot cancel another's request; the desktop reports
  that exactly like "not found" rather than leaking that the request
  exists elsewhere.

### `HEARTBEAT` (plugin -> desktop, every 5s while idle; a missed reply means
the desktop is gone — the plugin disconnects and starts retrying the
connection on its usual timer)
```json
{"type":"HEARTBEAT","v":1,"heartbeatId":"a1b2c3d4-...","protocolVersion":1}
```

### `HEARTBEAT_ACK` (desktop -> plugin, in reply to `HEARTBEAT`)
```json
{"type":"HEARTBEAT_ACK","v":1,"heartbeatId":"a1b2c3d4-...","protocolVersion":1}
```
`heartbeatId` must be echoed back exactly as received. A receiver waiting for
this ack must require `type == "HEARTBEAT_ACK"`, a matching `heartbeatId`,
and a supported `protocolVersion` — accepting *any* line as a valid ack (the
v1 implementation's original behavior) means an unrelated message arriving
at the same time, e.g. a late result for an already-abandoned request, gets
silently consumed as the ack instead of being routed or discarded on its own
terms via `requestId`.

### `ERROR` (either direction)
```json
{"type":"ERROR","v":1,"requestId":"a1b2c3d4-...","errorCode":"SHM_OPEN_FAILED","message":"could not open shared memory segment '...'"}
```
`requestId` is omitted for connection-level errors not tied to one request.
`errorCode` is a stable, machine-readable identifier for the failure —
current values: `SHM_OPEN_FAILED`, `SHM_CREATE_FAILED`, `NODE_WRITE_FAILED`,
`NODE_EXITED`, `NODE_UNAVAILABLE` (a worker's Node engine failed to (re)start
and hasn't recovered — every job it pops fails immediately with this rather
than hanging or crashing the desktop process), `TIMEOUT` (either side gave
up waiting on the other — desktop-on-Node or plugin-on-desktop; both sides
enforce this via a real, cancellable I/O deadline, not just a message-count
cap — see STATUS.md's "Overlapped I/O hardening"),
`TOO_MANY_MESSAGES_CLIENT_SIDE` (the rare backstop case: the desktop sent an
excessive number of messages within the time budget without ever producing
a terminal one), `PIPE_WRITE_FAILED`, `PIPE_READ_FAILED`, `CANCELLED`,
`CANCEL_REQUESTED`, `ALREADY_COMPLETED`, `REQUEST_NOT_FOUND` (the four
`CANCEL`-specific codes — see the `CANCEL` section above),
`UNSUPPORTED_KIND`, `AUDIO_LOAD_FAILED`, `ANALYSIS_FAILED`,
`BAD_REQUEST_JSON`, `TOO_MANY_ACTIVE_JOBS` (this session already has as
many active/queued jobs as it's allowed), `QUEUE_FULL` (the desktop's
global job queue is at capacity), `AUDIO_LIMITS_EXCEEDED` (the claimed
frame count, channel count, or sample rate is outside the accepted range,
or would overflow the size calculation), `SERVER_BUSY` (the desktop's
total in-flight-audio budget across every session is currently exhausted
— distinct from `QUEUE_FULL`, which is about job *count*, not the memory
those jobs' audio actually occupies), `RATE_LIMITED` (this session is
submitting `ANALYSIS_REQUEST`s faster than the desktop allows,
independent of how many are currently active). `message` is for logs/UI
display, not for branching logic — branch on `errorCode`.

### `BROKER_SHUTTING_DOWN` (desktop -> plugin, any time, not tied to a request)
```json
{"type":"BROKER_SHUTTING_DOWN","v":1,"reason":"user_requested","retryAfterMs":null}
```
Sent once, to every currently-connected session, when the desktop is about
to shut down on purpose (today: only ever a user-initiated "Quit" from the
tray menu) — a dedicated message type rather than an `ERROR` with a
`BROKER_SHUTTING_DOWN` `errorCode`, so a receiver doesn't have to sniff an
error's `errorCode` to tell "going away on purpose" apart from "something
actually broke." The desktop closes the pipe shortly after sending this
(see STATUS.md's "graceful tray shutdown"); a receiver should treat it as
an immediate, clean disconnect rather than waiting for the read that follows
to fail. `reason` is currently always `"user_requested"` — included now
so a future trigger (e.g. an installer-driven upgrade) doesn't need a
protocol version bump to add one. `retryAfterMs` is currently always
`null`: this desktop process does not auto-restart itself, so there is no
concrete number to give a reconnecting client that would mean anything —
a receiver should fall back to its own normal reconnect-on-timer behavior.
The plugin-side reference implementation (`BridgeClient.h`) disconnects
immediately on receiving this (both while idle, via `sendHeartbeat()`'s ack
wait, and mid-request, via `handleAnalysisRequest()`'s response loop) and
reports any in-flight request as interrupted with this same `errorCode` so
the UI can show a calm, non-alarming status instead of a generic error.

## Shared memory layout

Windows file mapping, backed by the page file (true shared memory, not a
disk-backed temp file), created by the desktop process with a name derived
from the session and request ID so it can't collide across concurrent
sessions. Layout is a fixed 16-byte header followed by raw interleaved
`float32` PCM:

```
offset 0:  uint32 magic       = 0x42534D31 ("BSM1")
offset 4:  uint32 sampleRate
offset 8:  uint32 channels
offset 12: uint32 frames
offset 16: float32[frames * channels] samples, interleaved
```

The plugin creates the mapping (`CreateFileMappingA`) — it's the side that
just captured the audio, so it naturally owns sizing and writing it — and
names it in the `ANALYSIS_REQUEST` it sends. The desktop process opens it by
that name (`OpenFileMappingA`), reads it, and is done with it once the
request completes (success or error); the mapping is
per-request, not a persistent ring buffer, since v1's analysis workflow is
"capture a buffer, then analyze it," not continuous real-time streaming (see
`BeatShoreBridgeAudioProcessor` — nothing in `processBlock` writes to shared
memory; capture happens into a pre-allocated lock-free FIFO on the audio
thread, drained to the shared-memory segment on a background thread once
capture is complete).

## Versioning

`v` is checked on every message. A side receiving a higher major version than
it understands responds with `ERROR` and disconnects rather than guessing at
an unknown schema — this mirrors the plugin's own state-versioning rule
(`PluginProcessor::stateVersion`): never guess at data from a newer version
you don't understand.
