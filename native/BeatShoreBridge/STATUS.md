# BeatShoreBridge — verification status

**Looking for the short, current-state-only summary — capabilities,
limitations, exact artifact hashes, supported hosts, known issues, and
the release acceptance checklist — with no engineering history mixed
in? That's `RELEASE_STATUS.md`, not this file.** This file is the
detailed engineering log: what was built, what broke, how each fix was
found and verified, in the order it happened. Read it when you need the
*why* behind something in `RELEASE_STATUS.md`, not as the first stop for
"what's the current state."

This document has grown chronologically as work happened, which means later
sections sometimes correct or supersede claims made in earlier ones (e.g.
"CANCEL is not yet supported" was true when written and is not true today).
Read **section 1 first** for the current, accurate picture — don't stop at
an early paragraph and assume it's still current; anything superseded is
marked `SUPERSEDED` inline, and section 6 lists all of them in one place.

## 1. Current verified status

Real and verified, as of the most recent work in this document:

- Native VST3 passes the Steinberg Validator (47/47) and is hosted-verified
  in REAPER for the live Analyze Tempo workflow (real captured audio, real
  result displayed).
- The full protocol + desktop process round trip is verified for both
  `tempo` and `transcribePolyphonic` via the real, unmodified `BridgeClient`
  class against a real desktop process and a real Node/tfjs-node engine —
  not a harness reimplementation.
- **Genuine cancellation**: a `CANCEL` for a request that's actually running
  kills and restarts the desktop's Node engine process and resolves in
  ~100-150ms (not the request's own up-to-60s timeout), with verified
  recovery — a follow-up request immediately afterward succeeds normally.
- **Multiple simultaneous plugin instances**: verified with two real
  `BridgeClient` connections active at once, each getting its own
  correctly-routed result with no cross-session contamination.
- Overlapped, deadline-bound, cancellable I/O on both the plugin↔desktop
  pipe and the desktop↔Node pipe — a silent or unresponsive peer on either
  side now resolves cleanly instead of hanging.
- Heartbeat protocol hardened: a `HEARTBEAT_ACK` must carry a matching
  `heartbeatId` and supported `protocolVersion`, not just be "any line that
  arrived."
- MIDI file output is real (Standard MIDI Type 1 via `encodeMIDITracks()`,
  unmodified), independently re-hashed and byte-inspected; export filenames
  now carry real identity (`BeatShore_<hostTrackName>_<kind>_<requestId>.mid`)
  plus byte size and generation timestamp.
- Double-buffered audio capture with an explicit `BufferState` machine,
  stress-tested 3000/3000 clean under concurrent `processBlock()` calls.
- A host-automatable trigger parameter, verified via the Validator's own
  automation-accuracy sweep.
- The staged installer engine runs on **Node 24.19.0 LTS** (Active LTS),
  not the end-of-life Node 25 an earlier pass had staged — the full
  tempo/transcription/cancellation/multi-session test suites were re-run
  from a completely clean Node-24 checkout and all passed, byte-identical
  to every prior run.
- `BeatShoreDesktop.exe --self-test <analyze.js path>` is real and
  verified: exits 0 only after confirming the bundled Node engine starts,
  a real tempo request round-trips, a real Basic Pitch inference produces
  notes on a synthetic fixture, and the MIDI export directory is writable;
  exits 1 with a specific logged reason on a genuinely broken install.
- A single-instance broker mutex (`main.cpp`) — verified directly: a
  second `BeatShoreDesktop.exe` refuses to start while one is already
  running, with the first left completely unaffected.
- A real system tray app: `BeatShoreDesktop.exe` now runs as a background
  broker with a tray icon (Windows notification area), a right-click menu
  ("Start at login" toggle, Quit), and hides its console window once the
  tray icon is confirmed present. Verified by sending the exact
  `WM_COMMAND` messages the real menu items send (not just code review):
  toggling "Start at login" on creates the correct `HKCU\...\Run` registry
  value containing the exact full command line (the quoted exe path
  *plus* the resolved `analyze.js` script path as an explicit quoted
  argument — see "Fifth: security hardening, Start-at-login fix, and
  graceful shutdown" below for the real bug this second part fixes);
  toggling it off removes the value cleanly; "Quit" now runs a real
  graceful shutdown rather than terminating the process abruptly (see
  the same "Fifth..." section). The full tempo/transcription/
  cancellation/multi-session suite was re-run against this tray-enabled
  build afterward and still passed, byte-identical to every prior run.
- The compiled installer (`BeatShoreSetup-0.2.0.exe`, 98.5MB) exists and
  compiles clean with Inno Setup 6.7.3, zero warnings — see "Clean-machine
  packaging" below for what compiling it caught and fixed, and for the
  one thing it still can't prove from this environment (an actual
  interactive install).
- **A real, launch-context-independent Start-at-login path.** The tray
  toggle's `HKCU\...\Run` value now resolves the bundled `analyze.js` path
  correctly regardless of the process's working directory or how Windows
  itself launches it at login — see "Fifth: security hardening,
  Start-at-login fix, and graceful shutdown" below for the real bug this
  fixes and how it was verified (a byte-for-byte simulated install,
  launched with no arguments from `C:\Windows\System32`, not the
  installation directory).
- **Named-pipe security hardening**: the broker's named pipe now carries a
  SID-based, protected ACL naming the actual current Windows user
  (previously an owner-relative descriptor, before that `nullptr` i.e.
  default/world-accessible), and `ANALYSIS_REQUEST` handling validates
  the analysis `kind` (rejecting anything outside the advertised
  capability list) and the shared-memory audio parameters (frame count —
  now duration-relative to the request's own sample rate, with a checked-
  multiplication overflow guard — channel count, sample rate) before
  acting on any of them, plus per-session/global job-queue depth caps and
  a system-wide in-flight-audio memory budget. `requestId` no longer
  touches the filesystem at all (a desktop-generated internal ID is used
  for temp files instead — stronger than the earlier validate-the-
  client's-own-string approach, which is still in place too, defense in
  depth). `SharedAudioBuffer::open()` (desktop side) validates the
  header's claimed size against the memory actually mapped via
  `VirtualQuery` rather than trusting the header alone, and
  `OverlappedPipeIO`'s line reader caps buffer growth at 1MB. Connection-
  attempt rate limiting, a concurrent-session cap, an invalid-message
  disconnect threshold, and a per-session request-rate limit round out
  the IPC-facing surface; full paths and message content are redacted
  from the log by default. See "Fifth..." and "Sixth: resource-limit
  tightening..." below for what's verified and what's still open.
- **Graceful tray shutdown**: "Quit" no longer calls `ExitProcess(0)`
  directly. It now stops accepting new connections, broadcasts a
  dedicated `BROKER_SHUTTING_DOWN` message (not a generic `ERROR`) to
  every connected session, cancels every active job (queued or running),
  wakes idle workers, waits a bounded grace window, then removes the tray
  icon, releases the single-instance mutex, and exits normally — with the
  OS's own process-exit reclamation kept as the fallback for anything
  that doesn't wrap up inside the grace window. `BridgeClient.h` reacts to
  the dedicated message immediately in both the idle and mid-request
  cases, and `PluginEditor.cpp` shows a calm status instead of a generic
  error. Verified empirically, not just by code review — see "Fifth..."
  and "Sixth..." below.
- **An automated, reproducible release build**
  (`native/installer/build-release.ps1`), replacing the manual build-
  copy-compile process that produced the stale-staging bug documented
  above. Rebuilds both binaries from source, unconditionally restages
  them (no "did anything change" check that could itself be wrong), runs
  the real regression suite against the staged tree specifically, and
  refuses to compile an installer if any step fails. Verified by actually
  running it end to end — twice failing on real bugs the run itself
  caught, once clean — see "Seventh: an automated, reproducible release
  build" below.
- **Real version control, a real icon, and the `-CleanEngine` build path
  verified for the first time.** This project is now a real git
  repository (tags `v0.2.0-rc1`, `v0.2.0-rc2`; see `RELEASE_MANIFEST.md`
  for the current commit). `BeatShoreDesktop.exe` and the installer both
  carry a real, brand-derived icon (not the generic Windows default),
  verified by extracting it back out of both compiled binaries. A full
  `-CleanEngine` run (`npm ci` from scratch, the verified `tfjs-node`
  trim reapplied, the complete regression suite) passed against a
  from-scratch engine tree, confirming the release doesn't secretly
  depend on stale `node_modules`. See "Eighth: source control, CI, and
  the `-CleanEngine` verification" below.
- **Real concurrent load against the installed build**: 40 simultaneous
  real `BridgeClient` sessions (genuine shared-memory audio, not
  synthetic requests), spawned within 0.31s of each other, all 40
  succeeded with zero failures and zero leaked temp files. A genuine
  Node-crash-mid-inference test and two more failure-mode tests
  (missing model file, unwritable MIDI folder) against the installed
  build all produced exactly the documented behavior. See "Ninth: real
  concurrent load against the installed build, and two more failure
  modes" below — including an honest account of which specific
  rejection thresholds (16-session, 24-job-queue) this load pattern
  didn't actually reach.
- **Pushed to a real GitHub remote, both CI TODOs fixed, and the
  `kMaxConcurrentSessions` rejection triggered for real for the first
  time** (25 near-simultaneous connections → 17 connected / 8 rejected,
  matching the desktop's own log exactly). The `kMaxGlobalQueueDepth`
  boundary still wasn't reached despite genuine escalating effort — see
  "Tenth: pushed to GitHub, CI TODOs fixed, and the held-open-connection
  test client finally built" below for the honest account, including a
  real bug found and fixed in the new test tool itself (not the
  desktop) along the way.
- **`v0.2.0-rc4`**: real publisher/copyright (product website/support/
  privacy-policy URLs still placeholders), a real
  `ProductVersionRaw`-stuck-at-`0.2.0.1` bug found and fixed, rebuilt and
  fully re-verified (Validator 47/47, full regression suite), tagged and
  pushed. Windows Sandbox checked and ruled out as a clean-machine-test
  workaround (no elevation, likely already-nested virtualization). See
  "Eleventh: real publisher/copyright, a real ProductVersion bug fix,
  and v0.2.0-rc4" below.

See section 4 ("Test evidence") for the specific numbers behind each of
these, and section 5 for the full engineering narrative.

## 2. Current limitations

Real, current gaps — not historical ones (see section 6 for those):

- **The single highest-priority remaining gap**: only the Analyze Tempo
  workflow has been observed hosted live inside REAPER end-to-end.
  `transcribePolyphonic`'s live-captured REAPER round trip (piano/guitar
  audio → capture → transcription → MIDI file → imported into a new REAPER
  track) has not — see "Highest-priority next test" below.
- REAPER project save/reload and plugin disconnect/reconnect mid-session
  are unobserved live (the desktop's reconnect-loop and multi-session
  handling are exercised by automated tests, but not watched happen inside
  a real REAPER session).
- Only one Node engine worker runs by default
  (`kMaxConcurrentNodeJobs=1` in `main.cpp`) — multiple sessions correctly
  share and queue behind one Node process, but genuine *parallel*
  `transcribePolyphonic` throughput would need multiple resident
  `tfjs-node` processes, not attempted.
- ~~The Windows installer has never been compiled.~~ **Resolved**: the
  installer compiles successfully with Inno Setup 6.7.3 and produces a
  98.5MB executable (`native/installer/Output/BeatShoreSetup-0.2.0.exe`).
  Actual installation and clean-machine behavior remain unverified — see
  "Clean-machine packaging" for the full writeup, including two real bugs
  the compiler caught that neither review nor testing had found, and the
  interactive-UAC limitation that still blocks a full install/uninstall
  test from this environment.
- ~~JUCE licensing tier is undecided.~~ **Resolved**: confirmed against
  JUCE's actual current terms (https://juce.com/legal/juce-9-licence/,
  fetched directly rather than assumed), the Starter tier's threshold is
  combined revenue + funding "up to $20,000" over the trailing 12 months
  from all sources. BeatShore is pre-revenue with no funding raised, well
  under that threshold — the free **Starter tier** applies. No mandatory
  attribution, splash screen, or source-disclosure requirement was found
  for this tier in the terms reviewed, but this was synthesized from an
  AI summary of the EULA, not a full manual read of the legal text --
  worth a final human read-through of
  https://juce.com/legal/juce-9-licence/ before shipping, and this needs
  re-confirming if BeatShore's revenue/funding situation changes.
- ~~The desktop process's own startup behavior is undecided.~~ **Resolved**:
  it now runs as a real system tray app (icon, "Start at login" toggle
  writing to `HKCU\...\Run`, Quit) — see section 1 and "Clean-machine
  packaging" below. Still missing: a real icon resource (uses the generic
  system `IDI_APPLICATION` placeholder today) and the installer doesn't
  yet offer to enable "Start at login" during setup itself (the user has
  to open the tray menu once after installing).
- Cubase, Ableton Live, FL Studio, Studio One: untested (no licenses
  available in this environment). Logic Pro: requires an AU build and
  macOS, neither available here.
- No code signing on any produced binary (the desktop exe, the installer,
  or the VST3) — no signing certificate available in this environment.
- ~~Named-pipe access control and payload-size limits are not hardened
  against a hostile local process.~~ **Further resolved**: the pipe now
  has a SID-based, protected ACL naming the actual current user (not the
  generic owner-relative form); `requestId` no longer touches the
  filesystem at all (a desktop-generated internal ID is used for temp
  files instead); the audio frame limit is duration-relative with a
  checked-multiplication overflow guard; a system-wide in-flight-audio
  memory budget exists (`SERVER_BUSY`) on top of the existing job-count
  caps (now tighter: 2 active/queued per session, 24 global); the pipe
  line cap dropped from 16MB to 1MB; connection-attempt rate limiting,
  a concurrent-session cap, an invalid-message disconnect threshold, and
  a per-session request-rate limit (`RATE_LIMITED`) are all in place; and
  full paths/message content are redacted from the log by default (opt
  back in via `BEATSHORE_DIAGNOSTIC_LOG=1`). See "Sixth: resource-limit
  tightening..." below for the full account and what verified it. Still
  genuinely open: any client-identity check beyond what the pipe ACL
  itself already provides (e.g. verifying the connecting process's own
  identity via impersonation, not just that it's running as the same
  Windows user), and a persistent per-identity ban/backoff list (a local
  named pipe doesn't expose a stable identity to ban beyond a session's
  own short lifetime — the connect-rate backoff above is this project's
  considered answer to that gap, not an oversight). This project's threat
  model is still "a local, same-user process that might be buggy or
  hostile," not a cross-user/network attacker.
- ~~The installer's EULA page still shows placeholder legal text.~~
  **Resolved**: `stage\LicenseFile.txt` now holds real, filled-in EULA
  content (licensor: Singh's Innovation & Advisory; governing law:
  Suriname; Section 5 discloses that no audio, MIDI, or usage data leaves
  the user's machine, matching what the code actually does). Drafted, not
  attorney-reviewed — the file says so explicitly in its own closing note,
  and a professional legal read-through is still worth doing before a
  broad public release, especially if distribution scope, pricing, or
  data handling changes from what Section 5 describes.

(VST3 SDK licensing is **not** on this list — see section 6, it's already
resolved: this project vendors SDK 3.8.1, genuinely MIT-licensed.)

## 3. Host compatibility

Honest per-host compatibility. "VST3 detected" means Steinberg's Validator
passed and/or the host's own plugin scanner registered the binary — it does
**not** mean the plugin was inserted on a track, opened, played through, or
had its state save/reload tested in that host. Don't read one as the other.

| DAW | Status |
|---|---|
| Steinberg Validator | 47/47 tests passed |
| REAPER | **Hosted IPC round trip VERIFIED for the Analyze Tempo workflow** — see below |
| Cubase | Untested — no license available in this environment |
| Ableton Live | Untested — no license available in this environment |
| FL Studio | Untested |
| Studio One | Untested |
| Logic Pro | Requires an AU build and macOS — this is a Windows VST3-only build today |

## What "REAPER: registered" actually proved

Confirmed: REAPER's own plugin scanner found the binary, read its metadata,
and wrote `BeatShore_Bridge.vst3=...,BeatShore Bridge (BeatShore)` into
`reaper-vstplugins64.ini`. That is real evidence the binary is a well-formed
VST3 module REAPER's scanner accepts — it is not evidence the plugin
instantiates correctly on a track, processes audio, or survives a save/reload
cycle. Those are different claims and needed a separate, real test (see below
for what was actually run and what it did/didn't cover).

## 4. Test evidence

The specific, checkable numbers behind section 1's claims:

- **Steinberg Validator**: 47/47, including with `BridgeClient` running as a
  background thread and with a real host-automatable parameter (Accuracy:
  Block/Sample sweep) — not the default 0-parameter case.
- **`BridgeStressTest`** (real `PluginProcessor.cpp`/`PluginEditor.cpp`
  compiled into a harness, concurrent `processBlock()` writes vs. real
  swap-and-read-out): 3000/3000 correct, 0 corrupted — run twice, before
  and after the `BufferState` invariant was added.
- **Real REAPER hosted round trip**: `Tempo: 152.00 BPM (78 ms)` displayed
  in the plugin's own editor for real captured audio, cross-checked against
  the desktop process's log for the same `requestId`
  (`audioSource:"live-captured"`, 441000 frames at 44100Hz matching the
  10-second capture window, `computeMs`/`desktopTotalMs` matching the UI
  exactly).
- **`BridgeClientTest`** (real `BridgeClient`, real desktop, real Node):
  `tempo` → `100.45` BPM; `transcribePolyphonic` → 91 notes,
  `sha256:173a3d6c...` — reproduced byte-identically across every run this
  project has ever produced, including against a trimmed (739MB→299MB)
  production-staged copy of the engine directory.
- **`BridgeIOHardeningTest`** (4 scenarios — desktop silent-but-connected,
  plugin destroyed mid-read, desktop's pipe closes mid-analysis, a stale
  response arriving right after a timeout): all pass.
- **`HeartbeatAckTest`** (6 scenarios — a stale result, a too-late ack, a
  wrong `heartbeatId`, malformed JSON, an `ERROR` instead of an ack, and a
  valid ack following an unrelated stale result, all arriving during a
  heartbeat wait): all pass.
- **`NodeEngineTest`** (Node hangs before printing `READY`; Node goes
  silent mid-stream after some progress): both correctly time out at the
  requested deadline instead of hanging.
- **`SchedulerTest`** (job queue ordering/blocking, registry eviction
  correctly protecting live jobs, the full cancellation state machine —
  pure logic, no pipe I/O): all pass.
- **`MultiSessionTest`** (two real, simultaneous `BridgeClient` instances
  against one real desktop): independently correct, non-cross-contaminated
  results; cancelling a queued job resolves in ~150ms; cancelling a
  genuinely running job resolves in ~100ms via a real Node process
  kill-and-restart, with a verified-successful follow-up request
  afterward proving real recovery, not just "didn't crash."

## 5. Historical implementation notes

Everything below this point is the chronological engineering narrative —
what was built, what broke, how it was found and fixed, in the order it
happened. Valuable for understanding *why* things are built the way they
are, but written progressively, so an early paragraph can describe a
limitation a later paragraph removes. Superseded claims are marked inline;
section 6 lists them all together for quick scanning.

## Desktop bridge: what's real now

The plugin has a working IPC client (`Source/BridgeClient.h`) that connects
to a running BeatShore desktop process (`native/BeatShoreDesktop`) over the
named-pipe protocol in `native/protocol/PROTOCOL.md`, does HELLO/
CAPABILITIES, and can run a real tempo analysis on captured audio.
`BridgeStatus` now reflects a genuine connection state machine (
`NotImplemented` when no desktop process is reachable, `Connecting` while
retrying, `Connected` once HELLO/CAPABILITIES succeeds) instead of being
permanently hardcoded.

Verified, and how:
- **Protocol + desktop process**: `native/BeatShoreTestClient`, a standalone
  harness mimicking the plugin's IPC sequence, completes a full round trip
  against `BeatShoreDesktop.exe` — HELLO→CAPABILITIES, then
  ANALYSIS_REQUEST→ANALYSIS_PROGRESS→ANALYSIS_RESULT with a correct tempo
  value, end to end through shared memory and a spawned Node analysis
  engine that reuses the browser prototype's own DSP code unmodified.
- **The plugin's actual `BridgeClient` class**: `native/BridgeClientTest`
  compiles and drives the *same, unmodified* `BridgeClient.h` that's linked
  into the VST3 — not a reimplementation — against a running
  `BeatShoreDesktop.exe`. It connects, requests a tempo analysis on real
  captured-style audio, and gets back the correct result
  (`success=true kind=tempo message=100.45 numericValue=100.45`).
- **The VST3 itself still passes the Steinberg Validator (47/47)** with
  `BridgeClient` running as a background thread inside the plugin,
  including the case where no desktop process is present at all (it fails
  the connect attempt fast and retries on a timer rather than hanging or
  crashing the host).

**REAPER HOSTED IPC ROUND TRIP: VERIFIED, for the core Analyze Tempo
workflow.** Observed directly (screenshot from the user, corroborated by
the desktop process's own log for the same request, not just a claim):
BeatShore Bridge inserted on a REAPER track ("Without You (Studio)"),
editor showing `Connected`, real audio played on the track, "Analyze Tempo"
clicked, and a real result displayed: `Tempo: 152.00 BPM (78 ms)`. The
desktop log for that exact request shows why this counts as real and not a
test fixture:
```
{"type":"ANALYSIS_REQUEST","requestId":"1e12fc4c...","kind":"tempo",
 "audio":{"sampleRate":44100,"channels":2,"frames":441000},
 "audioSource":"live-captured"}
{"type":"ANALYSIS_RESULT","requestId":"1e12fc4c...","kind":"tempo","result":152,
 "algorithm":"beatshore-dsp.estimateTempo (spectral-flux autocorrelation)",
 "computeMs":60,"desktopTotalMs":78}
```
`audioSource:"live-captured"` (not `"file"`), 441000 frames at 44100Hz
(exactly the plugin's 10-second capture window), and `computeMs`/
`desktopTotalMs` matching the UI's displayed `(78 ms)` exactly — this is
the real capture path, the real desktop process, the real DSP algorithm,
not a harness. This is the first genuine, observed proof of the complete
chain: REAPER track audio → plugin capture → desktop → Node analysis →
result back in the plugin's own editor.

Getting here required finding and fixing two real bugs the earlier
screenshot-based attempts surfaced (neither was caught by any automated
test, because neither is the kind of thing an automated test exercises):
- **Garbled UI text** (`â□□` instead of `—`): MSVC compiles narrow string
  literals using the active Windows code page, not UTF-8, unless told to.
  Fixed by adding `/utf-8` to `BeatShoreBridge`'s compile flags and
  replacing the plugin's few non-ASCII characters with plain ASCII.
- **`NODE_WRITE_FAILED` on every analysis request**: the desktop process's
  default `analyze.js` path was computed relative to its *current working
  directory*, which isn't reliably "the exe's own folder" depending on how
  it's launched — and separately had an off-by-one (`..\..\engine` instead
  of `..\engine`) that pointed at a nonexistent path even when CWD-relative
  resolution happened to line up. Node's module loader failed immediately
  and silently: the desktop logged whatever Node's crash output was as if
  it were a successful `READY` line (nothing validated it), so the desktop
  process ran for over half an hour looking healthy — HELLO/CAPABILITIES/
  HEARTBEAT/HOST_STATE all genuinely worked, since none of those touch
  Node — before failing the instant a real analysis was requested. Fixed
  three ways: the default path is now anchored to the exe's own location
  via `GetModuleFileNameA` (works regardless of launch method) with the
  off-by-one corrected; the READY line is now parsed and validated (a
  malformed line is a loud, immediate `FATAL`, not a silent limp); and
  `analyze.js` itself now guards its top-level request handler against an
  unhandled promise rejection (which crashes Node 15+ by default) and the
  desktop's relay loop no longer lets a non-JSON line from Node (e.g. a
  stack trace) take the whole desktop process down.

Not yet observed: the rest of the 16-step manual test (a different audio
source, closing/restarting `BeatShoreDesktop.exe` mid-session and
confirming the plugin degrades to `Disconnected` and reconnects
automatically without freezing REAPER, and project save/reload behavior).
The desktop process itself was changed today to loop and accept a new
connection after a plugin disconnects instead of exiting (previously,
removing/re-adding the plugin in a DAW session would've required manually
restarting the desktop exe) — real, but that specific behavior hasn't been
watched happen inside REAPER yet either.

## Capture buffer concurrency: verified, not just reviewed

The original single-buffer + "pause writes" flag design (first version of
this plugin's capture path) had a real, if narrow, race: the flag only
stopped the *next* write from starting, not one already in progress, so a
snapshot taken at the wrong instant could read a buffer mid-write. It was
replaced with a double-buffered design (`Source/PluginProcessor.h`'s
`ringBuffers[2]`): the audio thread does an O(1) index swap (never a copy)
to hand an entire buffer over to the reader and starts writing into the
other one, so a reader and the audio thread are structurally never touching
the same buffer at the same time.

This was verified, not just reasoned about: `native/BridgeClientTest`'s
`BridgeStressTest` target compiles the real `PluginProcessor.cpp` (same
file, not a reimplementation) into a harness that runs a real audio thread
writing a known, checkable ramp pattern via `processBlock()` in a tight
loop while a second thread repeatedly calls the same swap-and-read-out code
`triggerTempoAnalysis()` uses, checking every snapshot for chronological
ordering and channel consistency. 3000/3000 snapshots came back correct
with zero corruption. See the file's own header comment for why this checks
data content, not merely "didn't crash" (a race can silently produce wrong
values without ever crashing in an uninstrumented build).

That result was real, but the correctness claim it was proving rested on
reasoning about the rest of the call graph (JUCE's message thread is
single-threaded; the reader refuses a second swap while a snapshot is still
unconsumed) rather than on anything the buffer-swap code itself enforced --
a future change elsewhere (a second reader, a changed gating check) could
have silently broken it with nothing catching it. `PluginProcessor.h` now
has an explicit `BufferState` (`Free`/`Writing`/`Ready`/`Reading`) per slot,
with transitions validated at every point that changes them:
`captureFrozenSnapshot()` asserts (debug builds) that a slot it's about to
read is actually `Ready`, and `processBlock()`'s swap checks the target
slot is `Free` before reclaiming it -- if it somehow isn't, the audio
thread **refuses the swap and leaves the request pending for a later
block**, unconditionally, in every build configuration (not just debug),
rather than waiting (never acceptable on the audio thread) or overwriting a
buffer a reader might still be touching. Re-ran the same 3000-iteration
concurrent stress test against this version: still 0 corrupted.

## Traceability

Every `ANALYSIS_REQUEST` now carries a UUID `requestId` (not a small
counter) and an `audioSource` tag (`"live-captured"` for the real plugin,
`"file"` for test harnesses — see PROTOCOL.md). Every terminal response
carries `algorithm` (which function produced it), `computeMs` (pure DSP
time), and `desktopTotalMs` (full desktop-side handling time). Every
`ERROR` carries a stable `errorCode`. This is what makes the "not verified
with harness audio" distinction above checkable from a log line rather than
asserted from memory. `HOST_STATE` (host sample rate/tempo/transport,
documented in PROTOCOL.md since v1 but never actually implemented on the
plugin side until now) is sent every ~2s while idle.

> **`SUPERSEDED — historical implementation note`** (all four claims in
> the paragraph immediately below are now out of date -- this marker is
> attached directly to that paragraph, not just placed near it, so it's
> visible even if a reader jumps straight to the stale text below rather
> than reading top-down): a second UI trigger for `transcribePolyphonic`
> exists and is round-trip tested (see "Polyphonic transcription: now
> wired into the plugin, plus a real MIDI file" below); `CANCEL`
> genuinely works, including interrupting a request that's actually
> running (see "Genuine cancellation and multiple plugin instances"); the
> desktop accepts and correctly routes multiple simultaneous plugin
> connections (same section); and MIDI output is real, not just a
> displayed number (same file-export section). See section 6 for the
> full list of superseded claims in this document.
>
> **`SUPERSEDED — historical implementation note`** — begin original,
> now-outdated paragraph:
> Scope still narrower than the full plan: only the `tempo` analysis kind
> has a UI trigger (`Source/PluginEditor.cpp`'s "Analyze Tempo" button) and
> only that kind has been round-trip tested through this exact path, even
> though the desktop process's CAPABILITIES already advertises `key`,
> `structure`, `chords`, `timbre`, `transcribeDrums`, `transcribeMono`,
> `transcribePolyphonic`, and `loudness` (all tested directly against
> `analyze.js` and, for `transcribePolyphonic`, through the full pipe via
> `BeatShoreTestClient` -- see below -- just not yet wired to a plugin UI
> control). `CANCEL` is still an honest "not yet supported" error (now
> with `errorCode: "CANCEL_NOT_SUPPORTED"`). The desktop process still
> accepts exactly one plugin connection at a time — multiple simultaneous
> REAPER tracks each running BeatShore Bridge is unbuilt and untested.
> Sending results back into the DAW as MIDI (rather than just displaying a
> number) is unbuilt — that's Priority 2 in the implementation plan, not
> this pass.
> **`SUPERSEDED`** — end original paragraph.

## Parameter / automation bridge

Real, not aspirational: the plugin now exposes one host-automatable
parameter, `triggerAnalysis` (a `juce::AudioParameterBool`, registered via
`addParameter()` in the constructor). `processBlock()` reads it every block
(a cheap, lock-free, real-time-safe read) and detects a false->true edge;
the audio thread only ever raises an atomic flag from that -- it never
calls into `BridgeClient` or waits on anything. A `juce::Timer` owned by
the *processor itself* (not the editor, so this keeps working with the
plugin window closed, matching how host automation actually behaves)
services that flag at 10Hz: runs the same `triggerTempoAnalysis()` path the
editor's button uses, then resets the parameter via
`setValueNotifyingHost()` so the host's own display/automation lane stays
in sync and a momentary control (e.g. a MIDI-learned button) can fire
again without an explicit off-then-on move.

In practice, in a host like REAPER: right-click the parameter in the FX
chain -> "Learn" -> bind it to a MIDI controller or CC, and that control
fires an analysis without touching the plugin's own UI. Verified via the
Steinberg Validator's own automation-accuracy sweep ("Accuracy: Block/
Sample, 1 Parameters, Change every100 Samples"), which now runs against a
real parameter (previously 0) and still passes 47/47 -- not yet verified by
actually MIDI-mapping it inside a live REAPER session.

Still missing from "automation bridge" in the broader sense: no read path
for host automation *lanes* (the plugin can be automated, but can't read
back an automation envelope the host is running), and no parameters beyond
this one trigger -- e.g. no automatable capture-window-length or
kind-selector. `triggerAnalysis`'s value is not persisted across
save/reload (deliberate: it's a momentary trigger, not a setting, so
defaulting to off on reload is the correct behavior, not a gap).

## Piano/guitar polyphonic transcription (native desktop pipeline)

Real, not deferred anymore. `transcribePolyphonic` runs Spotify's
"basic-pitch" pretrained CNN (Apache-2.0) via `@tensorflow/tfjs-node`,
against the exact same vendored model weights
(`vendor/basic-pitch-model/`) the browser prototype already used --
`native/BeatShoreDesktop/engine/basic-pitch-model.js` and
`basic-pitch-decode.js` port the browser worker's tfjs calls and the
Spotify post-processing math almost verbatim (tfjs's public API is
identical between the browser build and tfjs-node; only the backend and
model-loading I/O differ). This was explicitly deferred earlier in this
project specifically because tfjs-node is a native module with real
compile/compatibility risk on an unusual setup -- that risk was real, not
hypothetical, and both problems it produced were found and fixed rather
than worked around by giving up:

- **Broken native binding packaging.** `npm install` completed with no
  visible error, but the module failed to load
  (`The specified module could not be found`). Root cause: this Node
  version reports N-API version 10; tfjs-node 4.22.0's install step
  creates a `lib/napi-v10/` folder with a `tensorflow.dll` symlink but no
  actual compiled `tfjs_binding.node` (no prebuilt binary exists for that
  N-API version from this package release), while the real, working
  binding sits in `lib/napi-v8/` -- missing its own copy of
  `tensorflow.dll` alongside it. Fixed with a `postinstall` script
  (`engine/scripts/fix-tfjs-node-binding.js`) that copies the DLL into the
  right place automatically after every `npm install`, verified by
  deleting the copy and re-running the script from scratch, not just
  patched once by hand.
- **A genuinely removed Node API.** Once loading, the very first tensor op
  crashed with `util_1.isNullOrUndefined is not a function`: tfjs-node
  calls `node:util`'s long-deprecated `isNullOrUndefined`, which Node has
  now fully removed. Fixed with a narrow, documented one-line polyfill
  (`basic-pitch-model.js`) restoring exactly that function's historical
  behavior -- not a workaround that changes anything else.

A real resampler was also needed: basic-pitch requires exactly 22050Hz
mono, which the browser gets for free from `OfflineAudioContext` (not
available in Node). `resample.js` implements a windowed-sinc FIR lowpass
for anti-aliasing plus linear-interpolation resampling -- a legitimate DSP
implementation, not naive decimation that would alias.

Verified with real audio through the full protocol, not just a bare
function call: `BeatShoreTestClient` against a running
`BeatShoreDesktop.exe`, kind `transcribePolyphonic`, produced a real
`MIDI_RESULT` -- 91 notes, chord-like overlapping pitches at shared
timestamps (e.g. MIDI 48 and 60 starting together), plausible velocities
and durations, ~1 second of compute for 8 seconds of audio. This same test
run also caught and fixed a real protocol-hygiene bug: `MiniJson`'s parser
accepted trailing garbage after a valid value (a line starting with digits
like TensorFlow's own diagnostic timestamp output parsed "successfully" as
a bare truncated number), which is exactly what let a stray non-JSON line
from Node's stdout get forwarded to a connected client as if it were a
protocol message. `Parser::parse()` now requires the entire input to be
consumed, not just a valid prefix -- fixed at the shared `MiniJson.h`
level, so every consumer (desktop, plugin, both test harnesses) benefits,
not just this one call site.

`tfjs-node` adds real weight to the desktop engine (a ~200MB native
TensorFlow library) -- loaded lazily (dynamic `import()` inside the request
handler, not at `analyze.js` startup) so it doesn't cost anything for
requests that don't use it, but still a real dependency footprint worth
knowing about if the desktop process needs to be redistributed.

## Polyphonic transcription: now wired into the plugin, plus a real MIDI file

`Source/PluginEditor.cpp` now has a second trigger, "Transcribe Piano/
Guitar", next to "Analyze Tempo" -- same capture path (the double-buffered
10-second ring), parameterized by analysis kind
(`triggerAnalysisOfKind()` in `PluginProcessor.cpp`, with
`triggerTempoAnalysis()`/`triggerPolyphonicTranscription()` as thin named
wrappers over it). The editor now shows, per the transcription result:
algorithm, source (`live-captured`/`file`), note count, processing time,
and -- if a MIDI file was written -- its name plus an "Open Export Folder"
button (`juce::File::revealToUser()`). A live in-flight progress percentage
is shown while a request is running (`BridgeClient::getProgress()`, updated
from `ANALYSIS_PROGRESS` messages, which were previously received and
silently discarded).

Fixing this surfaced a real, load-bearing gap: **`BridgeClient.h` never
handled `MIDI_RESULT` as a terminal message type at all** -- its response
loop only recognized `ANALYSIS_RESULT` and `ERROR`, so a `transcribePolyphonic`
request would have looped through every message without ever returning,
eventually reporting a false "no terminal response" timeout no matter how
well the desktop and Node behaved. Fixed by extending
`BridgeAnalysisResult` (`BridgeTypes.h`) with `noteCount`/`midiPath`/
`midiSha256`/`midiWriteError`/`audioSource` and adding the missing
`MIDI_RESULT` branch.

**Real MIDI file export** (`native/BeatShoreDesktop/engine/midi-export.js`):
reuses `beatshore-dsp.js`'s existing `encodeMIDITracks()` UNMODIFIED (the
same Standard MIDI Type 1 encoder the browser prototype's download button
already used) for `transcribeDrums`/`transcribeMono`/`transcribePolyphonic`
alike. Write-then-rename onto the same directory (`~/Documents/BeatShore/Exports/`)
so a reader can never observe a partially-written file at the final name,
plus a SHA-256 of the written bytes returned in the result. Verified for
real, not just by inspection: wrote a file, independently re-hashed it with
a separate tool (`sha256sum`), confirmed an exact match against what
`analyze.js` reported; hex-dumped the header and confirmed real `MThd`/
`MTrk` chunks, a tempo meta event, a time-signature meta event, and the
track name text -- not just "a file exists," an actually well-formed MIDI
file.

**Verified end-to-end with the real, unmodified `BridgeClient` class**
(`BridgeClientTest`, not a stand-in): connect, request
`transcribePolyphonic`, receive `success=true noteCount=91`, a real
`midiPath`, and a `midiSha256` that exactly matched the earlier standalone
test's hash byte-for-byte (same input audio, deterministic output --
correctness across two independent runs, not a fluke).

**A second real bug found and fixed by this same test, not by inspection:**
the desktop's per-request read loop was bounded by a fixed count of 10
messages, not a time budget. Polyphonic transcription legitimately sends
more progress updates than tempo (once per inference batch), and
TensorFlow's own stderr diagnostic text (merged into the same stream --
see `NodeEngine::start`) consumes a read even though it's correctly
rejected, not forwarded. Together these exhausted the 10-message cap
*before the real result ever arrived*, producing a spurious
`NO_TERMINAL_RESPONSE` error on a request that was actually working
correctly the whole time. Fixed on both sides (desktop's Node-facing loop
and the plugin's desktop-facing loop) by switching the bound to a 60-second
wall-clock timeout, with a generous message-count backstop (10000) kept
only as a safety net against truly unbounded output -- not as the primary
gate. Documented, not silently left as a gap: this still doesn't protect
against Node going fully silent mid-request (`readLine()` has no I/O-level
timeout of its own), which would need overlapped I/O or a watchdog thread
to fix properly -- not attempted here because it wasn't the bug actually
hit.

**A third, real, *observed* instance of a gap already on the roadmap**
(item 5, "stale results are discarded"): during the debugging above, one
request that the desktop gave up on (via the old 10-message cap) kept
running on the Node side after the desktop had already returned an ERROR
to the plugin -- and Node's eventual `MIDI_RESULT` for that abandoned
request was found sitting unread, having actually written a real MIDI file
to disk for a request nothing was still listening for. Nothing consumed or
misdirected that stale message this time only because the next test
started a fresh desktop process (a new Node child with no queued output).
The underlying gap -- nothing tracked "this response no longer belongs to
anyone waiting for it" -- is now fixed; see "Cancellation, timeouts, and
stale-result rejection" below.

Not yet done: this hasn't been exercised with `audioSource:"live-captured"`
from a real REAPER session with real piano/guitar audio -- only through
`BridgeClientTest`'s file-based harness (`audioSource:"file"` in every test
above, correctly, since that's what it is). The acceptance test from the
plan (`audioSource=live-captured`, `kind=transcribePolyphonic`,
`MIDI_RESULT`, note count > 0, overlapping notes present, `requestId`
matches) is fully wired and ready to run, but running it requires a live
REAPER session -- see the top of this document for why that's the one step
this environment can't perform itself.

## Cancellation, timeouts, and stale-result rejection

**Stale-result rejection** (the direct fix for the observed gap above):
every message read on both sides now carries its `requestId` checked
against the request actually being waited for -- desktop against Node's
messages, plugin against the desktop's -- and a mismatch is silently
discarded (`continue`, not treated as an error) rather than misattributed.
Verified live, not just by code review: a request whose shared-memory name
was deliberately wrong completed (correctly) with `SHM_OPEN_FAILED`, then a
`CANCEL` for that same, now-completed `requestId` correctly came back
`ALREADY_COMPLETED`, and a `CANCEL` for a `requestId` that was never used
came back `REQUEST_NOT_FOUND` -- both new, structured `errorCode`s, tested
against a real running `BeatShoreDesktop.exe` over the actual named pipe
(a small ad-hoc Node script speaking the protocol directly, since neither
existing test harness previously had a way to send a bare `CANCEL`).

**Kind-aware timeouts**: `transcribePolyphonic` gets a 60-second budget
(covers `tfjs-node` cold start + real inference); every other kind gets 10
seconds, since they all finish in well under a second normally -- a stuck
`tempo` request no longer makes the plugin wait a full minute to find out
something's wrong. Applied symmetrically on both the desktop's Node-facing
loop and the plugin's desktop-facing loop.

> **`SUPERSEDED — historical implementation note`** (attached directly to
> the paragraph below, not just placed near it): the paragraph describes
> the desktop's single-threaded, pre-cancellation architecture. That
> architecture was replaced -- see "Genuine cancellation and multiple
> plugin instances" below for the real, working, verified alternative: a
> `CANCEL` genuinely can interrupt a request that's actually running, via
> a per-session pipe-owner thread and a job queue with an explicit
> cancellation state machine, without reintroducing the split-thread pipe
> hang this paragraph is being careful about.
>
> **`SUPERSEDED — historical implementation note`** — begin original,
> now-outdated paragraph:
> **`CANCEL` semantics, reported honestly rather than pretended**: today's
> desktop is single-threaded by design (see the threading note at the top
> of `BeatShoreDesktop/Source/main.cpp`) -- it is structurally never
> reading the plugin pipe while blocked inside `handleAnalysisRequest()`,
> so a `CANCEL` can only ever arrive for a request that has *already*
> finished or *never existed*. There is no code path today where `CANCEL`
> can interrupt a request that's genuinely still running, and
> `REQUEST_NOT_FOUND`'s message says so explicitly rather than implying a
> cancel that didn't actually happen. Real "cancel a request that's
> actually in flight right now" needs the desktop to service the plugin
> pipe and Node's pipe concurrently -- the same category of change as
> multi-instance support, and not attempted here given how costly the
> earlier discovery of the named-pipe threading constraint already was.
> This matches the plan's own fallback framing ("cancellation may
> initially mean: stop waiting, discard the eventual result, prevent it
> from reaching the...client") -- that's what stale-result rejection above
> already achieves, just not exposed as a literal successful CANCEL of
> something in flight.
> **`SUPERSEDED`** — end original paragraph.

**Plugin destruction no longer risks a use-after-free on a still-blocked
IPC thread.** `BridgeClient`'s destructor used to call `stopThread(4000)`
then tear down the pipe -- if the IPC thread was blocked inside a long
`readLine()` (up to the 60s transcription budget) when the plugin was
destroyed and `stopThread` timed out, the destructor would go on to
destroy `pipe` out from under a thread still using it. Fixed by reordering:
`signalThreadShouldExit()`, then close the pipe handle (which interrupts a
currently-blocked synchronous read/write on it -- Windows resolves pending
I/O on a closed handle with an error rather than hanging), then
`stopThread()` to actually join, which should now return quickly since
both of the above already happened. Verified for the normal (not-currently-
blocked) destroy path via the existing test suite (Validator, stress test,
both still clean); at the time this was originally written, **not**
independently re-verified for the specific "destroy while genuinely stuck
mid-read" case, which is hard to trigger deterministically. That gap is now
closed -- see "Overlapped I/O hardening" below, which built exactly the
deterministic reproduction this paragraph originally said was missing.

## Overlapped I/O hardening

The gap named directly above, and a matching one on the desktop<->Node
side, are now fixed with real Windows overlapped I/O, not just reasoned
about further. Previously, every `readLine()` in this codebase (plugin<->
desktop pipe and desktop<->Node pipe alike) was a plain synchronous
`ReadFile` with no timeout of its own -- the various "kind-aware timeout"
and "wall-clock deadline" logic described above only checked elapsed time
*between* reads, which cannot interrupt a read already blocked waiting for
data that never arrives. A peer that accepted a connection and then never
wrote anything at all -- not slow, not noisy, just silent -- hung that
thread forever. This was a known, explicitly-documented limitation in
several places above (`BridgeClient.h`'s destructor comment, the
handleAnalysisRequest comments on both sides); it's the specific thing the
user's own review flagged as needing "Overlapped named-pipe I/O,
`CancelIoEx()` for pending operations, waitable events with explicit
deadlines" rather than continued reasoning about synchronous-mode
correctness.

**`native/protocol/OverlappedPipeIO.h`** (new): a `FILE_FLAG_OVERLAPPED`-
based replacement for the old synchronous `PipeLineIO`, with a
`readLine(deadlineMs, outLine)` that actually enforces its deadline via
`CancelIoEx` + `GetOverlappedResult` on timeout (verified to leave the
handle usable for a subsequent read afterward, not left dangling) instead
of only checking elapsed time between calls. Verified standalone first, in
total isolation (no JUCE, no Node, no desktop process) -- a 3-scenario test
covering a normal round trip, a timeout-then-recovery, and peer-closes-
mid-read -- all passed before this was wired into anything real.

**Both pipes converted, not just one**: the plugin<->desktop pipe
(`BridgeClient.h`, the desktop side of `main.cpp`) and, since the same
class of bug existed there too and two of the requested test scenarios
specifically target it, the desktop<->Node pipe as well
(`NodeEngine.h`, converted from anonymous pipes -- which cannot support
`FILE_FLAG_OVERLAPPED` at all -- to private per-process named pipes with
overlapped I/O on the parent's ends only; Node's own stdin/stdout behave
identically from its side). Before this second conversion, a Node process
that started but never printed its `READY` line, or one that went silent
mid-request, could hang the entire single-threaded desktop process
forever -- this was not a hypothetical, it was an explicitly-named "known
limitation, not fixed by this change" comment in the code until today.

**Regression check, not assumed**: after both conversions, the full
existing verification suite was re-run and matched every prior result
exactly -- Steinberg Validator (still 47/47), `BridgeStressTest` (still
3000/3000, zero corrupted), and real end-to-end round trips through the
real, unmodified `BridgeClient` class against a real `BeatShoreDesktop.exe`
for both `tempo` (`100.45` BPM, byte-identical to every earlier run) and
`transcribePolyphonic` (91 notes, `sha256:173a3d6c...`, byte-identical to
every earlier run) -- through the newly-converted `NodeEngine` pipe, not a
stand-in.

**New deterministic tests, driving the real classes against controllable
mocks** -- six of the seven scenarios the review specifically asked for,
plus their outcomes:
- *Desktop accepts a connection but never responds* (`BridgeIOHardeningTest`,
  `native/BridgeClientTest/Source/io_hardening_test.cpp`): the real
  `BridgeClient` observably stays `Connecting` for the full blocked HELLO
  read, then resolves to `Disconnected` right at the 5s deadline (~5028ms
  observed), not a hang.
- *Plugin is destroyed during a blocked read*: destroying a real
  `BridgeClient` while its IPC thread is genuinely blocked mid-read on a
  silent peer took 1-2ms, not a multi-second hang -- this is the specific
  "destroy while genuinely stuck mid-read" reproduction the destructor-
  reordering fix above didn't previously have.
- *Desktop terminates during shared-memory transfer* (modeled as the
  desktop process's pipe handle closing abruptly mid-`ANALYSIS_REQUEST`,
  as opposed to going silent-but-connected): resolved via the `Closed`
  path in ~100-200ms with a clean `PIPE_READ_FAILED` error, not waiting
  out the full 10s kind timeout.
- *Timeout occurs immediately before a valid result arrives*: a response
  deliberately delayed until just after the client's own timeout fires
  does not corrupt the *next* request's result -- verified request 2 gets
  its own correct value (`123.45`), not the stale request 1 value (`999`)
  that arrived late. (This test also surfaced the real protocol looseness
  fixed below in "Heartbeat protocol hardening": `sendHeartbeat()` used to
  accept *any* line as a valid ack without checking its `type`, so a stale
  response arriving while a heartbeat was in flight got silently consumed
  as the ack instead of reaching the stale-`requestId` discard check.)
- *Node starts but never writes READY* / *Node writes progress and then
  becomes silent* (`NodeEngineTest`,
  `native/BridgeClientTest/Source/node_engine_test.cpp`, driving the real
  `NodeEngine` class against `MockNodeProcess.exe`, a controllable node.exe
  stand-in): both correctly time out at the requested deadline (~2999ms
  against a 3000ms budget) rather than hanging, including immediately after
  `NodeEngine`'s destructor tears down a child process that's still alive.

Not attempted: *REAPER closes during transcription* -- this is the same
underlying code path as "plugin is destroyed during a blocked read" above
(a REAPER track removal destroys the plugin instance the same way this
test's `client.reset()` does), so the mechanism is covered, but observing
it happen inside an actual REAPER session is the one item on this list
that structurally requires a live REAPER session and can't be performed in
this environment.

## Heartbeat protocol hardening

Fixed the exact looseness the io-hardening test above surfaced:
`sendHeartbeat()` (`BridgeClient.h`) used to treat *any* line arriving
while it waited as a successful acknowledgement (`readLine(5000, resp) ==
Ok`, with the content never inspected) -- a stale, late-arriving analysis
result for an already-abandoned request would get silently swallowed here
instead of ever reaching the stale-`requestId` discard logic
`handleAnalysisRequest` already had. The protocol now has a real,
distinct acknowledgement: `HEARTBEAT` carries a fresh `heartbeatId` +
`protocolVersion`; the desktop replies `HEARTBEAT_ACK` echoing both.
`sendHeartbeat()` now loops, discarding anything that isn't a
`HEARTBEAT_ACK` with a matching `heartbeatId` and supported
`protocolVersion` (malformed JSON, a stale result, a wrong-ID ack, an
`ERROR`), rather than accepting the first line that shows up. See
PROTOCOL.md's `HEARTBEAT_ACK` section for the wire shape.

**`HeartbeatAckTest`** (`native/BridgeClientTest/Source/heartbeat_ack_test.cpp`)
drives the real `BridgeClient` against a mock desktop scripted for exactly
the six scenarios this needed covering, all passing:
1. A late `MIDI_RESULT` arrives while a heartbeat is waiting -- discarded,
   the real ack that follows is still accepted.
2. A technically-valid ack arrives after the client's own 5s deadline --
   the client disconnects on schedule (observed ~9.9s: 5s idle interval +
   5s ack deadline) rather than being rescued by the late ack.
3. An ack with the wrong `heartbeatId` arrives -- rejected, the correct one
   that follows is accepted.
4. Malformed JSON arrives -- discarded without crashing the parser.
5. An `ERROR` message arrives instead of `HEARTBEAT_ACK` -- discarded, not
   mistaken for an ack.
6. A valid ack follows an unrelated stale `ANALYSIS_RESULT` -- discarded,
   real ack accepted.

**Verified live against the real desktop, not just mocks**: a throwaway
probe (`LiveHeartbeatProbe`, built, run, and deleted -- not part of the
deliverable) held a real `BridgeClient` connection open for 13s against a
real `BeatShoreDesktop.exe` and observed two full real `HEARTBEAT`/
`HEARTBEAT_ACK` round trips in the desktop's own log, `heartbeatId`
matching exactly both times, `protocolVersion:1` present. Full regression
suite re-run clean after this change: Validator 47/47 (unaffected --
plugin-side change only touches `BridgeClient.h`'s IPC thread), the io
hardening/heartbeat/node-engine test suites all still green, and real
`tempo` (100.45 BPM) + `transcribePolyphonic` (91 notes, matching sha256)
round trips against a real desktop, byte-identical to every earlier run
this project.

## Export filename isolation

MIDI export filenames now carry real identity, not just
`<kind>_<requestId>.mid`: `BeatShore_<hostTrackName>_<kind>_<requestId>.mid`.
`hostTrackName` comes from JUCE's `AudioProcessor::updateTrackProperties()`
when the host reports one (`TrackProperties::name` is
`std::optional<String>` for exactly the reason not every host does) --
threaded through as a new optional `requestAnalysis()` parameter ->
`ANALYSIS_REQUEST`'s new `hostTrackName` field -> the desktop's `nodeReq`
-> `analyze.js` -> `midi-export.js`'s `sanitizeForFilename()` (strips
Windows-illegal characters, collapses whitespace, caps at 40 chars, falls
back to `"Untitled"` if empty/absent). Deliberately does **not** include a
"project name" component the original ask's `<project>_<track>_<requestId>`
pattern implied: no JUCE API exposes a host's project name to a VST3
plugin in a way that's reliable across DAWs, and inventing a fake one
would be worse than omitting it -- documented in-line rather than silently
dropped.

Collision-proofing was never actually about the filename's readable part
in the first place: `requestId` (a UUID) appears in every export filename
in full, never truncated, and was *already* what made two exports
impossible to collide even before this change -- `hostTrackName`/`kind`
only make the filename recognizable to a human browsing the export folder.

`midi-export.js`'s `writeMidiFile()` also now returns (and `MIDI_RESULT`
now carries) `midiSizeBytes` and `midiGeneratedAt` (ISO8601), extending
`BridgeAnalysisResult`/`BridgeClient.h` to match; the editor shows the
byte count next to the exported filename. `requestId` itself was already
present at the top level of every `MIDI_RESULT` (`requestIdEcho` on the
plugin side) -- not duplicated a second time inside the export metadata
for no reason.

**Verified end-to-end, not just by inspection**: a real
`transcribePolyphonic` round trip (via `BridgeClientTest`, no
`hostTrackName` provided since that harness isn't a real host) produced
`BeatShore_Untitled_transcribePolyphonic_<uuid>.mid`, confirmed to exist
on disk at exactly `844` bytes matching the `MIDI_RESULT`'s own
`midiSizeBytes:844`, with `midiGeneratedAt` present and well-formed, and
the same `sha256`/note data as every earlier run (91 notes,
`173a3d6c...`) -- the filename scheme changed, nothing about the actual
DSP/encoding did.

## Genuine cancellation and multiple plugin instances

Both built together because they need the same underlying architecture
change: the desktop process used to be single-threaded for *all* pipe I/O
(one plugin connection, reused after disconnect; one Node child; an
`ANALYSIS_REQUEST` blocked the entire main loop, including `HEARTBEAT`/
`CANCEL` handling, for its duration) -- an explicit, honestly-documented
limitation (`main.cpp`'s own threading comment, unchanged in spirit, only
updated) that made real cancellation and multiple simultaneous plugin
connections structurally impossible, not just unbuilt. Fixing either one
for real meant fixing both at once.

**The constraint that drove this design didn't relax -- it was worked
with, not around.** The original hang this whole codebase has been
careful about was specifically: two *different threads* calling
`ReadFile`/`WriteFile` on the *same* synchronous, byte-mode duplex pipe
handle concurrently. That's still never done anywhere in the new
architecture. Every pipe handle -- one per connected plugin, one per Node
child -- is still touched by exactly one thread for its entire lifetime.
What changed is that a thread no longer has to *block indefinitely inside
one call* to service its handle, via `OverlappedPipeIO`'s new
`beginRead()`/`pollRead()`/`cancelPendingRead()` (`native/protocol/
OverlappedPipeIO.h`): a thread can wait on "my pipe's next line" *and*
another event source (an outgoing-message queue, a cancellation signal)
at once via `WaitForMultipleObjects`, then dispatch on whichever fired.

**Verified in total isolation before use, same discipline as the earlier
overlapped-I/O work**: a standalone 5-scenario test (no JUCE, no
BridgeClient, no desktop) proved a normal line arriving while multiplexed-
waiting resolves correctly; a *different* event firing first leaves a
still-pending read undisturbed, correctly picked up on the next wait
cycle; a write issued from the same thread *while its own read is still
pending* succeeds and the read still resolves correctly afterward (the
load-bearing case this whole design depends on); a peer closing mid-read
resolves via `Closed`; and two lines arriving in one chunk serve the
second from the buffer without a redundant `ReadFile`. All five passed
before this was wired into anything real.

**Architecture** (`native/BeatShoreDesktop/Source/main.cpp`,
`AnalysisScheduler.h`):
- **`PipeSessionOwner`** (one thread per connected plugin): owns that
  connection's pipe end to end. Its wait loop multiplexes "my pipe's next
  line" against "the scheduler has an outgoing message for me" (a
  per-session `SessionOutbox`, pushed to by worker threads, drained here).
  `HEARTBEAT`/`CANCEL`/`HOST_STATE` are answered directly and immediately,
  inline, the moment they're read -- only `ANALYSIS_REQUEST` hands off to
  the job queue. This is what makes `HEARTBEAT`/`CANCEL` responsive for a
  session even while *that same session's own* request is running, not
  just while some *other* session is busy.
- **`JobQueue`/`JobRegistry`** (`AnalysisScheduler.h`, pure data
  structures, zero pipe/NodeEngine dependency -- unit-tested standalone in
  `native/BridgeClientTest/Source/scheduler_test.cpp` before any wiring):
  a thread-safe FIFO of pending `AnalysisJob`s, and a bounded (500-entry,
  matching the old single-session desktop's 50-entry
  `recentlyCompletedRequestIds` cache in spirit) registry keyed by
  `requestId` for `CANCEL` lookups. The registry's eviction specifically
  never drops a still-live (`Queued`/`Running`/`CancelRequested`) job even
  if it happens to be the oldest entry -- caught by `SchedulerTest`, not
  by review: an earlier version's eviction loop stopped at the first live
  entry it hit (oldest-first) instead of skipping past it, silently
  disabling eviction entirely whenever a long-running job's insertion
  happened to sit near the front.
- **`NodeWorker`** (one thread per `kMaxConcurrentNodeJobs`, default `1`):
  owns one Node child end to end, pulling jobs from the shared queue and
  routing `ANALYSIS_PROGRESS`/terminal messages to the owning session's
  outbox via `SessionRegistry` -- never touching that session's pipe
  handle directly. `kMaxConcurrentNodeJobs` stays at `1` deliberately:
  genuine parallel `transcribePolyphonic` throughput needs multiple
  resident `tfjs-node` processes (~200MB+ native TensorFlow library each),
  a real resource cost not yet weighed against benefit. The pool mechanic
  itself isn't hypothetical, though -- it's the same code path regardless
  of the configured count, just not stress-tested with `N>1` and real TF
  work in this pass.
- **Explicit cancellation state machine** (`JobState`, `AnalysisScheduler.h`):
  `QUEUED` -> `RUNNING` -> (`COMPLETED` | `FAILED` | `TIMED_OUT` |
  `CANCEL_REQUESTED` -> `CANCELLED`). `requestCancel()` is the single
  entry point (used by both the desktop's `CANCEL` handler and a
  disconnecting session's own cleanup, which cancels every job still
  active for that session so an abandoned connection's work doesn't run
  forever unbounded): a `Queued` job goes straight to `Cancelled`; a
  `Running` job goes to `CancelRequested` and wakes *specifically* the one
  worker thread processing it, via a per-job `assignedWorkerWakeEvent` (not
  a shared/global signal -- deliberately, so `N>1` workers each get
  cancelled independently without a storm of spurious wakes across every
  other worker on every cancellation). Verified directly, not just by
  reading the state machine: `SchedulerTest` exercises every transition,
  including the two edge cases that actually matter -- a second `CANCEL`
  racing an already-`CancelRequested` job is a harmless no-op, and
  cancelling a `Running` job with no assigned worker event yet (a real
  narrow race window between the `Queued`->`Running` transition and the
  worker storing its event) doesn't crash on a null `SetEvent`.
- **Genuine interruption, not a label nothing acts on.** Node/TensorFlow
  can't be asked to cooperatively abort an in-flight computation, so a
  `CancelRequested` `Running` job's worker does the only thing that
  actually stops it: cancels its own pending read on Node's pipe, kills
  the Node child process, and immediately spawns and READY-validates a
  fresh one (reusing the exact same bounded-startup logic `main()`'s
  initial launch uses) before picking up the next queued job. This is why
  `CANCEL_REQUESTED` is a real, separate acknowledgement state and not
  just "we stopped listening" -- a killed-and-discarded request can't keep
  occupying that worker's only Node process for however long its own
  kind-aware timeout would otherwise have taken, which would otherwise
  block every *other* queued job (including other sessions') behind it.

**Client-side cancellation is real too, not just a desktop capability
nothing can reach.** `BridgeClient.h` gained a symmetric
`requestCancel()`: the plugin's own result-wait loop
(`handleAnalysisRequest`) was converted to the same multiplexed
`beginRead()`/`pollRead()` pattern, waiting on the pipe's next line *and*
a cancel event together, so a cancel can actually be sent even while this
thread is genuinely blocked waiting on the desktop -- otherwise "cancel"
would silently do nothing until the kind-aware timeout expired on its
own, defeating the point. A `CANCEL_REQUESTED` reply is treated as
informational (keep waiting for the real terminal `CANCELLED` that
follows), not as the terminal outcome itself.

**A real race, found by the integration test below, not by review**: the
first version reset the cancel-signal state (`cancelRequestedFlag`/
`cancelEvent`) at the *start* of `handleAnalysisRequest()`. If
`requestCancel()` was called from the message thread before the IPC
thread had gotten around to actually starting `handleAnalysisRequest()`
for that request (a real, easy-to-hit window, not a contrived one), that
reset silently wiped out the cancel signal the moment the request
actually began -- the cancel was accepted (`requestCancel()` returned
`true`) but then never actually happened. Fixed by moving the reset into
`requestAnalysis()` itself, at the exact point `requestInFlight` becomes
`true` -- chronologically before any *valid* `requestCancel()` call for
that request could possibly occur, closing the window entirely rather
than narrowing it.

**Verified end to end against the real desktop process, not mocks**
(`native/BridgeClientTest/Source/multi_session_test.cpp`, driving two real
`BridgeClient` instances against a real `BeatShoreDesktop.exe` doing real
`tfjs-node` work):
- **Two simultaneous plugin instances**, both connect, both request
  `tempo`, both get their own correct result (`100.45` BPM, matching each
  other since it's the same input audio) -- no cross-session routing bugs.
- **Cancelling a `Queued` job**: client A starts a real
  `transcribePolyphonic` request; once A's job is observably `Running`
  (real `ANALYSIS_PROGRESS` > 0), client B's `tempo` request is sent (now
  genuinely enqueued behind A, since `kMaxConcurrentNodeJobs=1`) and
  immediately cancelled. B's result arrives as `CANCELLED` in **59ms** --
  not bounded by A's job finishing at all -- while A's own unrelated job
  completes normally and successfully afterward, completely unaffected.
- **Cancelling a `Running` job**: client A's `transcribePolyphonic`
  request is cancelled once genuinely `Running`. Result: `CANCELLED` in
  **126ms** (not the 60s `transcribePolyphonic` timeout it would otherwise
  have waited out) -- the desktop's log shows the exact sequence: `CANCEL`
  received -> `"killing and restarting node engine"` -> a fresh `node
  engine process started` / `ready` cycle in **79ms** -> the `CANCELLED`
  reply sent. A **follow-up request sent 2s later succeeds normally**
  (`100.45` BPM) through the freshly-restarted engine -- proving recovery
  is real, not just "the process didn't crash."

Full regression suite re-run clean on top of this: Validator unaffected
(plugin-side changes are confined to `BridgeClient.h`'s IPC thread),
`BridgeStressTest` still 3000/3000 with zero corrupted, `BridgeIOHardeningTest`
/ `HeartbeatAckTest` / `NodeEngineTest` / `SchedulerTest` all still green,
and real `tempo`/`transcribePolyphonic` round trips against the rewritten
desktop byte-identical to every earlier run this project (100.45 BPM; 91
notes, `sha256:173a3d6c...`).

**Not attempted, honestly scoped rather than silently skipped**: true
*parallel* `transcribePolyphonic` throughput (multiple resident `tfjs-node`
processes running concurrent inference, not just multiple sessions sharing
one) -- `kMaxConcurrentNodeJobs` stays at `1` until that resource tradeoff
is actually decided. Cooperative (non-destructive) cancellation inside
`analyze.js` itself (checking an abort flag between `await` points) was
considered and rejected in favor of process-kill: it would only help the
lightweight, sub-second kinds that don't need it, while
`transcribePolyphonic` (the kind actually worth cancelling) spends most of
its time inside a single synchronous `tfjs-node` inference call that
JavaScript's own event loop can't interrupt anyway -- so process-kill is
the only approach that actually helps the case that matters. A `CANCEL`
that arrives *after* the terminal message was already sent but before this
desktop's own bounded `JobRegistry` history evicted it correctly reports
`ALREADY_COMPLETED`, not silently ignored -- unchanged in spirit from the
original single-session desktop's `recentlyCompletedRequestIds`, just
generalized to the full job state machine.

## Clean-machine packaging

A real, complete installer script exists
(`native/installer/BeatShoreSetup.iss`, Inno Setup 6 format) listing every
actual file this project produces that needs bundling -- not a template
with placeholders guessed at -- and it has now been staged and verified
**twice** against a real, trimmed production copy of the engine directory
(see below).

### Third round: actually compiled, for real, with Inno Setup 6.7.3

Installed Inno Setup (`winget install JRSoftware.InnoSetup`) and compiled
`BeatShoreSetup.iss` for the first time. This immediately proved the value
of compiling rather than only reviewing: the compiler caught **two real
bugs** neither manual review nor this project's own testing had found --

- A genuine Pascal-adjacent bug: `#define MyAppId "{E8A18368-...}"` needs
  a *doubled* opening brace (`"{{E8A18368-...}"`) so Inno Setup's own
  `{constant}` expansion syntax doesn't try to look up a constant named
  after the GUID -- the compiler's first error was
  `Unknown constant "E8A18368-E91F-4642-BDA0-5DEFD6A19286"`. Fixed.
- The `[Code]` section itself, despite an earlier careful hand-review
  that already caught and fixed one real `;`-vs-`//` comment bug, compiled
  clean on the first attempt after that fix -- confirming the earlier fix
  was correct, not just plausible-looking.

Also fixed a real, if cosmetic, compiler warning: `ArchitecturesAllowed`/
`ArchitecturesInstallIn64BitMode` used the bare `x64` identifier, which
Inno Setup 6.7+ deprecates in favor of `x64compatible`. Fixed; the second
compile produced **zero warnings**.

Staged the remaining real artifacts to actually produce a complete build
(previously only partially staged): the built `BeatShoreDesktop.exe` and
`BeatShore Bridge.vst3`, the real Microsoft VC++ x64 redistributable
(downloaded directly from `https://aka.ms/vs/17/release/vc_redist.x64.exe`,
Authenticode signature verified as genuinely Microsoft-signed via
`Get-AuthenticodeSignature` before use, not just downloaded and trusted),
and the real third-party license texts (VST3 SDK, JUCE, Node.js,
TensorFlow's C library plus its own third-party notices, Basic Pitch) --
assembled into `stage\Licenses\` with a README explaining what's covered.

> **`SUPERSEDED — historical implementation note`**: the sentence
> immediately below described `LicenseFile.txt` as a clearly-marked
> placeholder needing real legal content. That was accurate when written
> and is not accurate today -- `stage\LicenseFile.txt` now holds real,
> filled-in EULA content (licensor Singh's Innovation & Advisory,
> governing law Suriname, a data-collection disclosure matching what the
> code actually does). See "Fifth: security hardening, Start-at-login
> fix, and graceful shutdown" below for the full account.
>
> plus a clearly-marked placeholder `LicenseFile.txt` (BeatShore's own EULA
> still needs real legal content, not invented here).

**Result: a real, compiled installer exists** --
`native/installer/Output/BeatShoreSetup-0.2.0.exe`,
**98.5MB** (`sha256:fa6b659fa320ddb0350193e7c4f97681133720b37b1768f7b1253c589fb5ef02`)
-- much smaller than this document's earlier 350-450MB estimate, since
LZMA compresses the JS source and `tensorflow.dll` more effectively than
that estimate assumed. That estimate is now corrected, not left stale.

**What's still not verified, honestly**: actually running this installer.
`PrivilegesRequired=admin` (added this session, matching the review's own
recommendation) means every real install attempt triggers Windows' own
interactive UAC consent dialog -- confirmed directly: a silent-install
attempt (`/VERYSILENT /SUPPRESSMSGBOXES`) hung for the full 15-second
timeout with no log output, because the elevation prompt was waiting for
a human click that never came in this sandboxed tool environment. This
is a genuine, structural limitation of this environment, not a bug in
the installer -- there is no way to approve a UAC dialog from here. The
compiled `.exe` has been handed to the user directly so they can run the
actual interactive install (and, separately, the full clean-machine
acceptance test) themselves.

### Fourth: a real system tray app

`BeatShoreDesktop.exe` now runs as a real Windows tray app rather than a
bare console broker: `main.cpp` gained a message-only window
(`HWND_MESSAGE`), a `Shell_NotifyIconA` tray icon, and a right-click menu
("Start at login" toggle, "Quit") -- `Shell32`/`User32` linked explicitly
in `CMakeLists.txt` (neither is part of MSVC's implicit default libraries
the way `kernel32` is). The accept loop moved onto its own background
thread (`runAcceptLoop()`, extracted from where it used to run directly
on `main()`'s own thread) so the main thread is free to host the tray
window's message loop, which Win32 requires to run on the thread that
created the window. Once the tray icon is confirmed present, the console
window hides itself (`ShowWindow(..., SW_HIDE)`) for a genuinely quiet
background app -- deliberately staying a console-subsystem build rather
than switching to `WIN32` subsystem, so this project's own
stdout-redirection-based test tooling kept working completely unchanged
throughout this exact verification.

**Verified by sending the real Win32 messages the actual menu items send,
not by code review alone**: `FindWindow` located the real
`"BeatShoreDesktopTrayWindow"` window by name, then `SendMessage` sent the
literal `WM_COMMAND` values the menu's own `AppendMenuA` calls use.

> **`SUPERSEDED — historical implementation note`** (both claims in the
> paragraph immediately below are now out of date): the Run registry
> value now contains the resolved `analyze.js` script path as an explicit
> quoted argument alongside the exe path, not the exe path alone -- see
> "Fifth: security hardening, Start-at-login fix, and graceful shutdown"
> below for the real bug this fixes. And "Quit" now runs a real, bounded
> graceful shutdown (stop accepting connections, notify sessions, cancel
> jobs, release the tray icon and mutex, exit normally) rather than an
> abrupt process termination -- same section.
>
> Toggling "Start at login" on produced the correct `HKCU\Software\
> Microsoft\Windows\CurrentVersion\Run\BeatShoreDesktop` value (the exact
> quoted exe path), confirmed via `reg query`; toggling it off removed the
> value cleanly (confirmed absent again via `reg query`, restoring the
> account's original un-set state rather than leaving a test side effect
> behind); sending "Quit" terminated the process (confirmed via
> `Get-Process` returning nothing afterward).

The full tempo/transcription/
multi-session/cancellation suite was re-run against this exact
tray-enabled build afterward and still passed, byte-identical to every
prior run -- moving the accept loop to a background thread didn't
regress anything.

**Not attempted**: a real BeatShore icon resource (uses the generic
system `IDI_APPLICATION` today); the installer offering to enable "Start
at login" during setup itself (today the user opens the tray menu once
after installing). ~~A graceful shutdown path for "Quit" (calls
`ExitProcess(0)` directly...)~~ **Resolved** — see "Fifth: security
hardening, Start-at-login fix, and graceful shutdown" below; "Quit" now
does a real, verified, bounded graceful shutdown rather than an abrupt
`ExitProcess(0)`.

### Fifth: security hardening, Start-at-login fix, and graceful shutdown

An external release-readiness review of the tray app / installer / IPC
surface caught one genuine, empirically-confirmed bug (Start-at-login) and
a list of real hardening gaps. All fixed and re-verified this session.

**Start-at-login: a real path-resolution bug, confirmed by reproducing it,
not just by inspection.** `defaultScriptPath()` (the same function used both
by the Node worker's own launch and by the Run-registry value the tray's
"Start at login" toggle writes) had only ever been exercised launched from
inside the dev build tree or from the freshly-installed directory in the
same session — never from an arbitrary working directory the way Windows
actually invokes a `Run` key entry at login. Built an exact, byte-for-byte
replica of the real installed directory layout in a scratch location, then
launched `BeatShoreDesktop.exe` with **no arguments from
`C:\Windows\System32`** (not the install directory) — precisely how the old
single-candidate path logic (`exeDir\..\engine\analyze.js`) would actually
be exercised at a real login, and confirmed it broke: the Node engine
entered a permanently degraded state (`FATAL: node engine's first line
wasn't a valid READY message`), because that relative candidate resolves to
a directory one level above the real install root once staged, not the dev
tree's own accidental-alignment depth. Fixed two ways: `defaultScriptPath()`
now checks the **installed layout first**
(`<exe dir>\engine\native\BeatShoreDesktop\engine\analyze.js`), falling back
to the dev-tree layout only if that doesn't exist (`fileExists()`, a new
`GetFileAttributesA`-based helper); and `setStartAtLoginEnabled()` now
embeds the resolved script path explicitly as a quoted argument in the Run
registry value itself, so the value doesn't depend on the exe re-resolving
it correctly at every future login — defense in depth, not just a single
fix. Re-ran the exact same System32/no-args reproduction after the fix:
correct path resolution, and a full, successful `transcribePolyphonic`
round trip (91 notes, matching sha256) from that launch context.

**A second, real bug this same reproduction exposed, not by inspection**:
`beatshore-dsp.js` needs a sibling `package.json` (`"type":"module"`) to
reliably resolve as an ES module — the staged installer engine tree had been
missing this file the entire project, "working" only by luck via Node's
`MODULE_TYPELESS_PACKAGE_JSON` syntax-detection heuristic, which failed
outright in this specific reproduction (`doesn't parse as CommonJS`). Fixed
by staging the project's own `package.json` into `stage\engine\` and adding
it to the installer's `[Files]` section.

**Named-pipe security hardening**, addressing each concrete gap the review
named:
- **Pipe ACL**: the named pipe is now created with an explicit
  `SECURITY_ATTRIBUTES` built from the SDDL string `"D:(A;;GA;;;OW)"`
  (owner-only generic-all access) via
  `ConvertStringSecurityDescriptorToSecurityDescriptorA` — previously
  `nullptr`, meaning the OS default (world-accessible) descriptor.
- **`requestId` validation**: a fix for a real path-injection vulnerability,
  not a theoretical one — `requestId` (fully attacker/bug-controllable JSON
  input) was used unvalidated to build a real filesystem path
  (`tempDir + "\\bsr_" + requestId + ".bsmraw"`); a crafted value like
  `"..\\..\\evil"` could write outside the intended temp directory.
  `isValidRequestId()` now rejects anything empty, over 64 characters, or
  containing characters outside `[A-Za-z0-9-]`, checked before the value is
  used for anything.
- **Kind whitelisting**: `ANALYSIS_REQUEST` now rejects any `kind` not in
  the same `kSupportedKinds` array `buildCapabilities()` already advertises
  (refactored to share the one array, removing a drift risk between "what
  we claim to support" and "what we'll actually run").
- **Resource bounds**: shared-memory audio parameters are validated against
  real limits (≤50,000,000 frames, ≤2 channels, 8kHz–192kHz sample rate)
  before being trusted; a session is capped at 4 concurrent active jobs,
  and the global job queue is capped at 256 — all via a structured
  `AUDIO_LIMITS_EXCEEDED`/`ERROR` rejection, not a silent drop.
- **`SharedAudioBuffer::open()`** (shared between the desktop and plugin,
  `native/protocol/SharedAudioBuffer.h`) now validates the header's claimed
  size against the region `VirtualQuery` reports as actually mapped, not
  just that the magic bytes match — closes a real out-of-bounds-read: a
  buggy or malicious peer whose header claimed more frames than its mapping
  actually held would previously let `samples()` read past the mapped view.
- **`OverlappedPipeIO`'s line reader** (`readLine()`/`pollRead()`, used by
  every pipe in this project) now caps buffered input at 16MB
  (`kMaxLineBytes`), returning a new `LineTooLarge` result instead of
  growing the buffer without limit — closes a real unbounded-memory-growth
  path for a peer that sends bytes indefinitely without a newline.

Verified with a raw protocol test (a small Node script speaking NDJSON
directly to the pipe, bypassing `BridgeClient` entirely so the rejections
themselves are what's being tested, not the plugin's own good behavior):
an unsupported `kind`, a path-traversal-shaped `requestId`, and an
oversized frame count were each sent deliberately and each came back a
structured rejection — and, by design, the malicious `requestId` value
itself is not echoed back in the error response. The full regression suite
(`SchedulerTest`, `BridgeClientTest` tempo + `transcribePolyphonic`,
`MultiSessionTest`) was re-run after these changes and still passed,
byte-identical to every prior run.

**Still genuinely open, not attempted this pass**: rate-limiting repeated
connection attempts, redacting potentially sensitive values from the log,
and any client-identity check beyond the pipe ACL itself (e.g. explicitly
verifying the connecting process's identity via impersonation rather than
relying on the ACL alone). This project's threat model remains "a local,
same-user process that might be buggy or hostile," not a cross-user or
network attacker.

**Graceful tray shutdown**: "Quit" used to call `ExitProcess(0)` directly,
with an in-line comment explicitly reasoning that nothing held state
needing orderly teardown. That stopped being accurate once the desktop
gained connected sessions, a job queue, actively-running Node inference,
and shared-memory mappings a plugin might still be holding — an abrupt exit
mid-request just drops all of that rather than telling anyone. Replaced
with a real `gracefulShutdownAndExit()`: stop accepting new connections
(the accept loop already polls a shutdown flag on a bounded ~500ms cycle,
reused here rather than added new); broadcast a `BROKER_SHUTTING_DOWN`
notice to every connected session; cancel every active job, queued or
running, via the same `requestCancel()` state machine `CANCEL` already
used; wake any worker idle-blocked waiting for the next job so it notices
shutdown instead of waiting forever; a bounded grace window; then remove
the tray icon, release the single-instance mutex, and exit normally — with
the OS's own process-exit reclamation kept as the explicit fallback for
anything that doesn't finish inside the grace window, not a gap.

Verified empirically, not just by code review, with two separate real
tests sending the actual `WM_COMMAND` the "Quit" menu item sends:
- **The shutdown sequence itself**: launched a real
  `BeatShoreDesktop.exe`, confirmed the mutex existed while it ran, sent
  `WM_COMMAND(ID_TRAY_QUIT)`, and the process exited cleanly within the
  grace window every time. Critically, **the mutex release is now genuinely
  checkable, which it wasn't before** (`ExitProcess` also released it
  implicitly, so "did the explicit release step actually work" was
  previously unobservable): a second `BeatShoreDesktop.exe` instance
  started immediately afterward and stayed running, proving the mutex was
  actually free, not just that the first process was gone.
- **A connected session genuinely receives the shutdown notice before its
  pipe closes**: a real Node client connected over the actual named pipe,
  completed the HELLO/CAPABILITIES handshake exactly like a real plugin
  session, then "Quit" was triggered while it stayed connected. The
  desktop's own log and the client's own received messages agree: the
  client received `{"type":"ERROR","errorCode":"BROKER_SHUTTING_DOWN",...}`
  before the pipe closed, not after — confirmed on both sides of the same
  real IPC exchange, not asserted from one side alone.

**Installer self-test failure handling was also rewritten**, addressing
each concrete gap the review named. Previously, a failed self-test was
only ever reported as a bare exit code — `Exec()` can't retrieve a child
process's stdout, so the specific PASS/FAIL lines `runSelfTest()` itself
prints (`main.cpp`) were never actually captured anywhere the installer
could read them back, "check the install log for details" pointed at a
log that didn't contain the detail. Rewritten:
`RunSelfTest()` now routes `BeatShoreDesktop.exe --self-test`'s real
stdout+stderr through `cmd.exe`'s own redirection into a persistent
`{app}\selftest-log.txt` (left on disk, not deleted, so a silent
deployment or a human opening the folder later both have something real
to read); `SelfTestFailureSummary()` pulls the actual `FAIL`/`FATAL`
lines back out of that file so both the interactive dialog and the
incomplete-install marker name the specific check that failed, not just
"something failed"; a plain `{app}\INSTALL_INCOMPLETE.txt` marker file is
written on failure (there's no built-in Inno Setup "partial install"
state, so this is a greppable stand-in a support process or a future
first-run check could look for) and removed automatically if a later
reinstall's self-test succeeds; the interactive dialog now offers an
immediate uninstall (`Yes`/`No`, running the real uninstaller on `Yes`);
and Setup's own process now exits via `Abort` on failure, forcing Inno
Setup's documented cancelled/aborted exit code instead of `0` so a
silent/scripted deployment tool sees a nonzero result. "Offering to
launch BeatShore on failure" was never something to suppress in the first
place — this script has no `[Run]` "launch after install" entry at all,
interactive or silent, checked directly rather than assumed.

Verified as far as this environment allows, honestly bounded where it
doesn't: the new `cmd.exe`-redirection technique was reproduced and run
directly (outside Inno Setup entirely) for both a real passing self-test
and a deliberately broken script path — the resulting log files contained
exactly the `PASS:` lines and the `FATAL:`/`FAIL:` lines respectively,
confirming the string-matching `SelfTestFailureSummary()` performs against
this file actually extracts the right thing in both cases. The `.iss`
script itself compiled clean (Inno Setup 6.7.3, zero warnings) with this
added. **Not verified, and cannot be from here**: the actual behavior of
`Abort()`'s forced exit code, the marker file, and the uninstall-offer
dialog during a real failing install — that needs a real, UAC-elevated
install run to watch happen, the same structural limitation that already
blocks a full clean-machine test of this installer.

**The installer's EULA placeholder is also resolved**: `stage\LicenseFile.txt`
previously shipped with loud in-line warnings that it was placeholder text
with no legal effect. It now holds real, filled-in content — licensor
Singh's Innovation & Advisory, governing law Suriname, a support/contact
address, and a data-collection section disclosing (accurately, matching
what the code does) that no audio, MIDI export, or usage data leaves the
user's own machine. This was a genuine business decision this session
couldn't make unilaterally — the licensor name, governing jurisdiction, and
data-collection scope came directly from the project owner, not invented.
The drafted text itself says plainly that it hasn't been reviewed by a
licensed attorney; that review is still worth doing before a broad public
release, particularly if distribution scope, pricing, or data handling
changes from what the EULA's Section 5 currently describes.

### Sixth: resource-limit tightening, requestId no longer touches the filesystem, a real pipe ACL, connection throttling, log redaction, and a dedicated shutdown message

A second external review, after "Fifth" above shipped, judged the existing
limits still too permissive and named specific, concrete replacements
rather than just "tighten this." All implemented and verified this
session, not just acknowledged:

- **Pipe line cap: 16MB → 1MB.** Audio never travels over this
  line-oriented NDJSON protocol at all (it goes through shared memory);
  every real control message is a few KB. 1MB is still generous headroom,
  not the old, principle-free 16MB. `LineTooLarge` already closed the
  connection outright rather than merely clearing the buffer and
  continuing — confirmed by re-reading `runPipeSession`'s dispatch (`pr !=
  Ok` — which `LineTooLarge` satisfies — folds into the same "disconnect"
  path every other read failure takes), not something that needed
  changing, just verifying it was already correct before shrinking the
  constant.
- **Audio frame limit is now duration-relative to the request's own
  claimed sample rate**, not a flat 50,000,000-frame constant (`~400MB` at
  a high sample rate, wildly beyond this project's actual use). `maxFrames
  = sampleRate * kMaxCaptureSeconds` (60s — real headroom above the
  plugin's current 10s capture, documented in-line, not a silent
  surprise if capture length changes later). A checked-multiplication
  helper (`safeAudioByteCount()`) computes `frames * channels *
  sizeof(float)` with an explicit overflow guard before it's ever used for
  a buffer size or a file write, rather than relying on today's specific
  limit constants happening to keep the product in range.
- **A new system-wide memory budget** (`kMaxTotalReservedAudioBytes`,
  ~512MB): every accepted request reserves its audio's byte count against
  this budget at admission time and a request that would exceed it is
  rejected with a new `SERVER_BUSY` errorCode — distinct from the
  existing `QUEUE_FULL` (job *count*) and `TOO_MANY_ACTIVE_JOBS`
  (per-session count), neither of which previously bounded the actual
  *memory* those jobs' audio occupies. Queue depths themselves were also
  tightened: `kMaxActiveJobsPerSession` 4 → 2 (one active + one queued —
  this project's own single-worker architecture means a deeper per-session
  allowance never bought real throughput anyway) and
  `kMaxGlobalQueueDepth` 256 → 24.
- **A real, pre-existing bug found while wiring the memory budget, not
  something the review named directly**: the temp `.bsmraw` audio dump
  written for every `ANALYSIS_REQUEST` was never deleted — a genuine,
  100%-reproducing disk-exhaustion leak, confirmed by finding dozens of
  orphaned `bsr_*.bsmraw` files in the temp directory from this project's
  own testing across this entire session. Fixed alongside the memory
  budget (`releaseJobResources()`, idempotent via a per-job atomic flag so
  it's safe to call from every code path that might independently observe
  "this job is done"): the temp file is deleted and the budget released
  once a job reaches a terminal state. Best-effort on the delete itself —
  if Node is still genuinely past its own timeout and still holding the
  file when this fires, the delete just fails and is logged, not treated
  as fatal (this project's cancellation model doesn't forcibly kill Node
  on a plain timeout, only on an actual `CANCEL` — see "Genuine
  cancellation" above) — still a strict improvement over deleting nothing,
  ever, which is what happened before this fix in every case including
  the ones that Node had definitely already finished with.
- **`requestId` no longer touches the filesystem at all**, closing the
  "validation could have a bug someday" gap the earlier `isValidRequestId()`
  fix (still in place) didn't fully close by itself: a new
  `generateInternalId()` produces a 128-bit desktop-generated identifier
  used for the temp file's name; the client's own `requestId` is now
  protocol metadata only — stored on the job for `CANCEL` lookups and
  response correlation, never used to build a path.
- **The pipe ACL is now built from the actual current-user SID**
  (`D:P(A;;GA;;;<SID>)`, protected DACL) rather than the generic
  owner-relative `"D:(A;;GA;;;OW)"` this used previously — "OW" was
  already correct for this process's normal same-user usage, but names a
  *relationship*, not a principal, which is harder to audit. Falls back
  to the previous "OW" form (logged, not silent) if SID resolution fails
  for any reason, rather than refusing to start. **Not verified from this
  environment**: the specific cross-integrity-level scenario the review
  asked about (desktop running elevated while the DAW does not) — this
  project's normal deployment never elevates `BeatShoreDesktop.exe`, so
  that case has never been reachable in any testing performed here; the
  standing guidance is simply not to run it elevated, rather than adding
  a Mandatory-Integrity-Control override to paper over an unverified,
  non-default scenario. What **is** verified: every real pipe connection
  this entire testing session has ever made — hundreds, across every test
  suite — used the new SID-based descriptor successfully (confirmed no
  fallback-to-OW warning was ever logged), which is the direct regression
  check that matters for same-user access, the one scenario this
  environment can actually exercise.
- **Connection throttling**: a sliding-window connect-rate limiter in
  `runAcceptLoop` (more than 20 connections in 5s triggers a short sleep
  before the next accept — real backpressure, not just a counter);
  `kMaxConcurrentSessions` (16) rejected before a session thread is even
  spawned; a per-session invalid-message counter (malformed JSON or an
  unrecognized message type) that disconnects after 10; and a per-session
  `ANALYSIS_REQUEST` rate limiter (`RATE_LIMITED`, a new errorCode) capped
  at 20 requests per 10s, independent of how many are currently active.
  Deliberately **not** a persistent per-identity ban list: a local named
  pipe doesn't expose a stable identity to ban beyond a session's own
  short lifetime, so the connect-rate backoff above is this project's
  answer to "make repeated connect-fail-reconnect cost something,"
  documented as a considered scoping choice, not an oversight.
- **Log redaction, opt-in verbose mode**: full filesystem paths
  (temp file paths, shared-memory names) and complete protocol message
  content (every `<-`/`->` line echo, both directions, both the
  plugin-facing and Node-facing pipes) are redacted by default
  (`redactPath()`/`redactContent()`) — a byte count or `<redacted>` in
  place of the real value. Set `BEATSHORE_DIAGNOSTIC_LOG=1` in the
  environment before launching to see real values instead, an explicit,
  logged, user-activated mode, not the default. Also added: a coarse
  token-bucket rate limit on `logLine()` itself (200 lines/sec, with a
  "N lines suppressed" marker), independent of what any call site
  chooses to log — a flood of rejected/malformed messages (each already
  producing a log line) shouldn't become a disk-I/O amplification vector
  even with the per-message/per-connection limits above in place.
  Deliberately scoped, not exhaustive: install-time `FATAL` messages
  naming a broken `analyze.js` path were left unredacted on purpose —
  rare (only a genuinely broken install triggers them), and the actual
  path is the specific, actionable content a human needs to fix that
  install, unlike the routine per-request logging this redaction
  actually targets.
- **A dedicated `BROKER_SHUTTING_DOWN` message type**, replacing the
  generic `ERROR` (with a `BROKER_SHUTTING_DOWN` `errorCode`) graceful
  shutdown previously used — see PROTOCOL.md for the wire shape
  (`reason`, currently always `"user_requested"`; `retryAfterMs`,
  currently always `null` since this desktop doesn't auto-restart
  itself). The plugin-side reference implementation (`BridgeClient.h`)
  now reacts to it in both places a broker shutdown could arrive: mid-request
  (`handleAnalysisRequest`'s response loop — disconnects immediately,
  reports the request interrupted with this `errorCode`) and while idle
  (`sendHeartbeat()`'s ack-wait loop — treated as an immediate failed
  heartbeat rather than silently discarded and waited out for the rest of
  the ack deadline, so an idle plugin reacts within the current heartbeat
  window instead of needing a full extra 5s idle interval first).
  `PluginEditor.cpp` special-cases this `errorCode` to show a calm status
  ("BeatShore Desktop is restarting -- reconnecting automatically")
  instead of the generic "Error: ... [CODE]" framing every other
  `errorCode` gets — there's nothing wrong to alarm the user about, and
  `BridgeClient`'s own reconnect-on-timer loop resumes automatically.

**Verified, not just implemented**: a raw NDJSON protocol test (bypassing
`BridgeClient.h` entirely, so the desktop's own rejections are what's
being tested) confirmed all four of `UNSUPPORTED_KIND`, the new
`RATE_LIMITED` (the limiter engaged at exactly the configured threshold —
20 requests admitted, the remaining 5 of 25 sent rejected), the
invalid-message-count disconnect (a real socket close observed after 10
malformed lines), and the tightened 1MB line cap (a deliberately oversized
2MB line closed the connection) — all against the real, rebuilt desktop
process. A real, successful end-to-end `tempo` round trip (via the
unmodified `BridgeClientTest` harness) confirmed the new temp-file cleanup
actually fires on the success path (no new `bsr_*.bsmraw` file left behind
afterward, versus dozens of pre-existing orphaned ones from before this
fix). The full `MultiSessionTest` suite — including both cancel-a-queued-
job and cancel-a-genuinely-running-job-with-node-restart, the riskiest
paths for the new resource-release wiring to race against — passed
unchanged, along with `SchedulerTest`, `BridgeIOHardeningTest`,
`HeartbeatAckTest`, `NodeEngineTest`, and `BridgeStressTest` (3000
snapshots, 0 corrupted). The VST3 plugin was rebuilt against the updated
`BridgeClient.h`/`PluginEditor.cpp` and still passes the Steinberg
Validator 47/47.

**A real staging bug, caught and fixed before it reached the compiled
installer, not glossed over**: the first attempt to restage these two
freshly-rebuilt binaries into `native/installer/stage\` for a final
installer compile actually copied stale ones — a leftover copy from a
prior round of this same session, predating every fix in this section.
Caught by comparing file timestamps between the dev build directories and
`stage\` (not by any test failure — the stale binaries were still fully
functional, just not the ones the source actually describes), before the
installer was ever compiled from them. Both were rebuilt fresh, restaged,
and the full self-test/regression/hardening-test suite re-run against the
corrected staged tree — see `RELEASE_MANIFEST.md` for the complete
before/after account, including the now-superseded (and explicitly
marked "do not use") hash from the stale attempt.

### Seventh: an automated, reproducible release build

The stale-staging bug immediately above was a real, caught-after-the-fact
mistake in a manual process — build, remember to copy, hope the copy
step actually happened. A review of the project's remaining release gates
specifically asked for this to become a script that makes that class of
mistake structurally impossible rather than something a human (or a
future session) has to remember to check for. `native/installer/build-release.ps1`
is that script: it rebuilds `BeatShoreDesktop.exe` and the VST3 from
source, **unconditionally** overwrites the staged copies every run (no
"has anything changed" check that could itself be wrong — see the
script's own top-of-file comment), validates the staged directory layout
against a required-file list, runs `--self-test` and the real regression
suite (`BridgeClientTest` tempo + `transcribePolyphonic`,
`MultiSessionTest`) against the **staged** desktop process specifically
(not the separate dev build directory), and only then compiles the
installer — refusing to proceed at any failed step (non-zero exit,
nothing left half-packaged) rather than compiling a best-effort installer
from a tree that failed its own tests. It also computes every hash a
release manifest needs (including the staging-manifest hash covering the
entire staged tree, not just the two binaries the script itself changes)
into a structured `release-report.json`, and accepts optional
`-SigntoolPath`/`-CertThumbprint` parameters that sign and verify each
binary before hashing when a certificate is actually available — skipped
loudly, not silently, when it isn't (no certificate exists in this
environment).

**Writing this script surfaced three more real bugs, caught by actually
running it rather than by inspection** — the same discipline as
everything else in this document, applied to the tooling itself this
time:
- **A PowerShell error-handling pitfall this project's own tool
  guidance already names, hit anyway on the first attempt**: capturing a
  native process's stderr via `2>&1` under `$ErrorActionPreference =
  "Stop"` turns every stderr line — including entirely benign ones, like
  `vcvars64.bat`'s own harmless `'vswhere.exe' is not recognized` warning
  — into a terminating error, aborting the script before the actual build
  it was running ever got a chance to succeed. Fixed by switching to
  `$ErrorActionPreference = "Continue"` globally and checking
  `$LASTEXITCODE` explicitly at every step that must actually gate
  progress, with `-ErrorAction Stop` added individually to the specific
  cmdlets (`Copy-Item`, `Remove-Item`, `Get-FileHash`, `Get-Item`) where a
  real failure genuinely must stop the script.
- **A second, more dangerous PowerShell pitfall, caught by the script
  failing on a build that had actually succeeded**: `$arrayOfLines
  -notmatch "pattern"` does not test "does any line match" — against an
  array, `-match`/`-notmatch` filter element-by-element and return the
  non-matching elements, which is a non-empty (therefore truthy) array in
  almost any real case regardless of what the lines actually say. The
  Steinberg Validator's own output ended with "Result: 47 tests passed, 0
  tests failed" — genuinely passing — while the script reported it as
  failed and refused to package, because `$validatorOut -notmatch "0
  tests failed"` was evaluating truthiness of a filtered array, not a
  boolean. This is the more dangerous class of the two bugs found here:
  had it filtered the wrong way, it could have reported a genuinely
  broken build as passing instead. Fixed at all four call sites (the
  Validator check, and the tempo/`transcribePolyphonic`/`MultiSessionTest`
  pass/fail checks) by joining the array to a single string before
  matching.
- **A third bug, in a brand-new file this script itself introduced**:
  `native/installer/scripts/gen-test-fixture.js` (a small, deterministic
  BSM1-format audio fixture generator, replacing a hand-crafted scratch
  file the regression tests previously depended on informally) used plain
  `require('fs')` and failed immediately with `ReferenceError: require is
  not defined in ES module scope` — the exact same class of Node
  module-type-resolution bug this project already hit once with
  `beatshore-dsp.js`'s staging (see "Installer size" below): Node
  resolves a script's module type from the nearest *ancestor*
  `package.json`, not the script's own directory, and this project's root
  `package.json` has `"type":"module"`. Fixed by rewriting the fixture
  generator in ESM (`import fs from 'node:fs'`) rather than adding yet
  another local `package.json` to work around it.

**Verified by actually running it end to end, twice — once failing on
each of the bugs above, once clean**: the final successful run rebuilt
both binaries (`ninja: no work to do` — confirming nothing had changed
since the prior manual rebuild, a useful cross-check in its own right),
passed the Validator (47/47), restaged unconditionally, validated all 9
required staged paths, passed `--self-test` against the staged tree, and
passed the full staged regression suite (tempo, `transcribePolyphonic`,
`MultiSessionTest` — all `[test] PASSED`/`[test] ALL PASSED`), before
compiling the installer clean (Inno Setup 6.7.3, zero warnings) and
writing `release-report.json`. See `RELEASE_MANIFEST.md` for this run's
real hashes and a note on installer-hash reproducibility (Inno Setup
embeds build-time metadata, so two compiles from byte-identical input do
not produce the same installer hash — confirmed directly by compiling the
same corrected source twice this session, once manually and once via this
script).

**Not attempted**: actual code signing (no certificate available, though
the script is ready to invoke `signtool` the moment one exists);
wiring this into a real CI system. ~~This project isn't in a git
repository at all~~ **Resolved, and CI written (unverified)** — see
"Eighth: source control, CI, and the `-CleanEngine` verification" below.
~~The `-CleanEngine` path... wasn't exercised this run~~ **Also
resolved** — same section.

### Eighth: source control, CI, and the `-CleanEngine` verification

Three real gaps closed in one pass, each addressing something the
project genuinely lacked rather than re-verifying something already
covered:

**Git, for the first time.** This project had no version control at all
through every round of work described above — every "Source commit: N/A"
line in `RELEASE_MANIFEST.md` was a real, honest limitation, not a
formality. `git init`, a `.gitignore` excluding everything generated or
vendored (build directories, `node_modules`, the ~1.1GB combined JUCE/VST3
SDK trees, the downloaded Node 24 toolchain, installer staging/output —
59 real source/doc/script files committed, not the 2.8GB working tree),
an initial commit, and an annotated tag `v0.2.0-rc1`. A second commit
(icon + `-CleanEngine` verification, both described below) is tagged
`v0.2.0-rc2`. `RELEASE_MANIFEST.md`'s "Source commit" field now records a
real commit hash instead of "N/A" going forward.

**A GitHub Actions workflow** (`.github/workflows/release.yml`) —
written, not verified, since no real GitHub Actions runner was available
to test it against. Uses portable equivalents of this session's own
local tooling (`ilammy/msvc-dev-cmd` instead of a hardcoded local
`vcvarsall.bat` path, `actions/setup-node` instead of a manually
downloaded Node zip, `choco` for Ninja/Inno Setup) rather than pretending
this machine's specific paths would work on a hosted runner. Two things
left honestly unfilled rather than guessed at: the exact commands to
fetch JUCE/VST3 SDK into CI (this project's own history of how those were
originally obtained locally isn't recorded anywhere the workflow could
read it back from) — the workflow contains a **labeled TODO step**, not
invented URLs — and `build-release.ps1` itself still hardcodes this local
machine's `vcvars64.bat`/`ISCC.exe` paths, which won't resolve on a
GitHub-hosted runner either; making those configurable is real follow-up
work, not done here. A `sign` job is written and gated on
`SIGNING_CERT_BASE64`/`SIGNING_CERT_PASSWORD` secrets, no-opping cleanly
(not failing) when they're unset, matching this project's standing
"skip signing loudly, not silently" rule from `build-release.ps1` itself.

**`-CleanEngine` actually run, for the first time.** Every prior
`build-release.ps1` run in this document (including the one immediately
above) took the fast path and never exercised the `-CleanEngine` branch —
a real, if narrow, gap: nothing had confirmed the release doesn't
secretly depend on the specific `node_modules` state already sitting on
disk. Run for real this time: a completely fresh `npm ci --omit=dev`
against the pinned Node 24 toolchain (124 packages, 23s), the verified
`tfjs-node` trim reapplied (`deps/`, source maps), the postinstall
binding repair re-run, then the full staged self-test and regression
suite (tempo, `transcribePolyphonic`, `MultiSessionTest`) — all passed
against this from-scratch tree, and the resulting staged file count
(8,479) matched the non-clean build exactly, confirming the trim is
reproducible, not something that happened to work once. Installer
recompiled clean from this tree; see `RELEASE_MANIFEST.md` for the
resulting hashes.

**A real BeatShore icon, derived from the project's actual logo, not a
placeholder.** `assets/beatshore-logo.png` (a real brand asset already
present in the project, not something invented this session) contains a
circular badge mark plus a "BeatShore" wordmark;
`assets/icon/generate_icon.py` crops just the badge (the wordmark doesn't
read at small icon sizes), centers it on a square canvas, and rasterizes
a proper multi-resolution `.ico` (16/20/24/32/40/48/64/128/256px) via
Pillow. Wired in two places: embedded as a Win32 resource in
`BeatShoreDesktop.exe` (`Source/resources.rc`, new
`Source/resource.h` for the shared resource ID) — used both for the
exe's own Explorer/taskbar icon and, replacing the generic
`IDI_APPLICATION` the tray previously used, for the tray icon itself —
and set as `SetupIconFile` for the installer `.exe`. Add/Remove Programs
gets the same real icon automatically via the existing
`UninstallDisplayIcon={app}\BeatShoreDesktop.exe` (no separate change
needed there). **Verified by extracting the icon back out of both
compiled binaries** (`System.Drawing.Icon.ExtractAssociatedIcon`,
not just trusting that embedding it "should" have worked) and visually
confirming it's the real badge, not corrupted or still the system
default. Not attempted: BeatShore branding inside the VST3 plugin editor
itself (explicitly optional per the review that requested this work).

**Two more failure-mode tests, run empirically against the real staged
desktop, not reasoned about**: a corrupted Basic Pitch `model.json`
(swapped for a plain text file, self-test re-run, restored immediately
after) produced exactly the expected isolated failure — tempo and the
MIDI-export-directory check still correctly `PASS`, only the
transcription check `FAIL`s, with `noteCount=-1` rather than a crash or a
false pass, and the process exits 1 (confirmed by checking `$?`
correctly this time — an initial check of this same run was accidentally
measuring `tail`'s exit code instead of the desktop's own, a mistake
caught and corrected before trusting the result). A genuinely unwritable
MIDI export directory (a temporary `icacls` deny-ACE on
`~\Documents\BeatShore\Exports`, applied and removed narrowly and
verified restored to its exact original ACL afterward) produced the same
clean pattern: tempo and transcription both still `PASS`, only the
writability check `FAIL`s, exit 1. Neither test found a bug — both are
recorded here as real, executed verification of documented behavior, not
assumed from the code alone.

**Not attempted this pass**: the deeper remaining items from the same
review (testing the 512MB memory budget and 24-job queue depth under
genuine sustained concurrent load — the queue-depth check specifically
requires real, currently-queued jobs, which requires real shared-memory
audio from multiple genuine `BridgeClient` sessions running concurrently,
not just malformed/fake requests the existing hardening test already
covers), disk-full behavior (not safely simulable without actually
filling a disk), Node-crash-mid-inference beyond what `NODE_EXITED`
already covers, and the elevated-desktop/non-elevated-DAW scenario
(still requires interactive UAC, still not available here).

### Ninth: real concurrent load against the installed build, and two more failure modes

A follow-up review asked specifically for the load testing "Not attempted
this pass" left open above, plus repeating the model-file/MIDI-folder
tests against the installed build rather than the dev tree (they'd only
been run against the dev build directory before) and a genuine Node-
crash-mid-inference test. All run against `native/installer/stage`'s own
`BeatShoreDesktop.exe` — the actual installed-equivalent binary, not the
separate dev build tree every earlier round in this document used.

**Real concurrent load, using actual `BridgeClient` sessions with real
shared-memory audio, not synthetic/malformed requests**: 40
`BridgeClientTest.exe` processes spawned within 0.31s of each other
(genuine burst, not a loop with meaningful gaps), each independently
connecting, handshaking, and requesting a real `transcribePolyphonic`
round trip against the one staged desktop instance. **40/40 succeeded**,
zero failures, zero `QUEUE_FULL`/`SERVER_BUSY` rejections, total wall
time 9.74s (~243ms average per request, consistent with genuine
serialization through the single Node worker rather than silent parallel
processing that would corrupt results). Temp file count in the OS temp
directory was identical before and after (95/95) — no leaks under this
load, extending the earlier single-request cleanup verification to a
real concurrent-burst scenario.

**Honestly, this did not reach the literal 16-concurrent-session or
24-job-queue-depth *rejection* boundaries** — worth stating plainly
rather than implying more was proven than was. `BridgeClientTest.exe` is
a one-shot client (connect, one request, wait, disconnect), so live
session count and queue depth both drain faster than a burst of
one-shot clients can sustainably exceed the configured ceilings
(`kMaxConcurrentSessions=16`, `kMaxGlobalQueueDepth=24`) — even a 40-wide
simultaneous burst never had more than a handful of genuinely
overlapping live sessions at once, because each one's full round trip
(connect through disconnect) completes in a few hundred milliseconds.
Actually forcing those specific rejection paths under real (not
synthetic) load would need a test client that holds its connection open
independent of request completion — not built this pass. The rejection
*logic* itself is unit-adjacent verified already (the structurally
identical `RATE_LIMITED` check, tested directly via the raw-protocol
hardening test in "Sixth..." above), just not exercised via 24
genuinely-simultaneously-queued real audio requests specifically. The
512MB memory budget is in the same position: never approached under this
load pattern, logic verified by code path and by the (already-tested)
overflow-guard math, not by genuine exhaustion.

**Two more empirical failure-mode tests, against the installed build**:
a genuinely *missing* (not just corrupted) `model.json` (moved aside,
self-test re-run, restored and re-verified passing immediately after)
produces the identical clean, isolated failure the corrupted-file test
found earlier — `noteCount=-1`, only that one check fails, exit 1, no
crash. A real Node crash mid-inference (`taskkill /F` on `node.exe`
~300ms into a genuine `transcribePolyphonic` request, confirmed via
`tasklist` that the targeted PID actually existed and was actually
killed, not a race that missed it) produced exactly the documented
behavior: the client receives a structured
`"node analysis engine exited unexpectedly"` error (not a hang, not a
crash), the desktop process itself stays alive and responsive (confirmed
via a follow-up request), and — matching this project's own documented,
deliberate limitation (`runNodeWorker`'s own comment: no automatic
respawn on an *unexpected* exit, only after a genuine `CANCEL`) — a
follow-up request correctly receives `NODE_UNAVAILABLE` rather than
silently hanging or crashing the broker. Confirmed the desktop process
survives via `tasklist` immediately before deliberately terminating it
for cleanup, not assumed.

**Not attempted, honestly**: disk-full (still not safely simulable
without actually filling a disk), a genuinely-held-open-connection test
client that could force the session/queue-depth rejection boundaries
under real load, and the elevated-desktop/non-elevated-DAW scenario
(still needs interactive UAC, still unavailable here).

### Tenth: pushed to GitHub, CI TODOs fixed, and the held-open-connection test client finally built

Three real threads closed this round, one left genuinely open (not
glossed over).

**Pushed to a real remote for the first time.** The user created
`github.com/krishavi85/BeatShore-VST3-Plugin` and supplied the standard
"first commit" push snippet; adapted rather than run verbatim (this
repo already had real history — `git init` was a no-op, and a real
README replaced the bare placeholder header the snippet would have
produced). `main` and both existing tags pushed cleanly. Checked via the
public GitHub API (no auth available, so read-only): the repo, the
commit, and the workflow file are all confirmed present and correct.

**Both CI workflow TODOs from the "Eighth" section are now filled in.**
The JUCE/VST3 SDK fetch step is pinned to the *exact* commits actually
vendored and tested against all session — read directly from the local
`native/JUCE`/`native/vst3sdk` `.git` directories (`git log -1
--format=%H`), not re-derived from a version string JUCE doesn't tag at
every commit. `build-release.ps1` no longer hardcodes this machine's own
`vcvars64.bat`/`ISCC.exe` paths — it auto-detects both (via `vswhere.exe`,
present on every VS2017+ install including GitHub-hosted runners, and
PATH/standard install locations respectively), with override parameters
for the rare case auto-detection picks wrong. Verified locally: a full
run after the change still exits 0 with every test passing.

**A genuine mystery, surfaced but not resolved**: pushing a new tag
(`v0.2.0-rc3`, carrying both fixes) to test whether the workflow now
actually runs showed **zero workflow runs on the repository**, even
though the workflow itself is registered and `state: active` per the
API. This isn't a defect in the workflow file — GitHub parsed and
accepted it — so this is almost certainly an account/repo-level Actions
setting (new repositories sometimes need Actions manually enabled via
the GitHub UI) that isn't visible or fixable from a read-only,
unauthenticated API check. Flagged directly to the user rather than
guessed at further.

**A real held-open-connection test client, finally built**
(`native/BridgeClientTest/Source/load_boundary_test.cpp`,
`LoadBoundaryTest.exe`) — the specific gap left open at the end of the
"Ninth" section above. Deliberately not built on `BridgeClient.h`: its
`requestInFlight` guard structurally prevents holding more than one
request open per connection, which is exactly what's needed to build
real queue depth. `LoadBoundaryTest` is a thin, direct client (reusing
`NamedPipeIO.h`/`MiniJson.h`/`SharedAudioBuffer.h`, the same headers the
production code uses) with two modes:

- `--sessions N holdMs`: opens N real HELLO-completed connections and
  holds them open. **This genuinely triggered the `kMaxConcurrentSessions`
  rejection for the first time**: 25 near-simultaneous connections
  produced 17 connected / 8 rejected, matching the desktop's own log
  (`already at kMaxConcurrentSessions (16)`) exactly, 8-for-8. The 17-not-
  16 is the same documented, expected "soft cap, can transiently exceed
  under real timing" behavior the code's own comment already names — not
  a bug.
- `--queue sessions perSession`: each of N sessions fires `perSession`
  real `ANALYSIS_REQUEST`s (real SHM audio) back-to-back with no wait in
  between, to build real queue depth toward `kMaxGlobalQueueDepth=24`.
  **This did not reach the rejection boundary** despite genuine, escalating
  effort: three audio durations (0.2s, 10s, the 60s maximum
  `kMaxCaptureSeconds` allows), both a naive burst and a version
  restructured around an atomic barrier so all 16 sessions' first
  requests release together instead of trickling out as each thread
  finishes its own SHM prep. Every configuration: 32/32 requests
  accepted, zero `QUEUE_FULL`. Working hypothesis, not a confirmed
  finding: `kMaxConcurrentSessions=16` combined with each session's own
  strictly sequential per-connection message loop (one line fully
  handled — including a real synchronous multi-megabyte temp-file write
  — before the next is even read) may genuinely bound peak simultaneous
  queue depth below 24 for any real client load, session cap included.
  Confirming that would need instrumenting the desktop's own queue-depth
  at runtime, not attempted here since it would mean modifying production
  code purely to observe a test.

**A real bug was found and fixed during this work — in the new test
tool itself, not the desktop.** The first few `--queue` runs appeared to
hang indefinitely; the desktop's own log showed it had correctly
processed and responded to every request within about a second each
time. Instrumenting the test client with per-line logging revealed why:
`transcribePolyphonic`'s terminal message type is `MIDI_RESULT`, not
`ANALYSIS_RESULT` (a distinction this project has hit before, in
`BridgeClient.h`'s own history — see "Polyphonic transcription: now
wired into the plugin" above) — the test's terminal-message filter only
recognized `ANALYSIS_RESULT`/`ERROR`, silently treating every real
`MIDI_RESULT` as a skippable progress update, so its "how many terminal
responses am I still waiting for" counter never reached zero even though
the desktop had already answered everything correctly. Worth stating
plainly: this was chased as a suspected desktop-side concurrency bug for
some time (a small scale reproduction, close reading of `JobQueue`'s
`push()`/`waitPop()` for a lost-wakeup race) before the actual,
mundane cause was found — the investigation process is recorded here
because concluding "desktop bug" and being wrong would have been a real
mistake to publish, and the discipline that caught it (verify against a
smaller, fully-traceable repro before concluding) is worth keeping
visible, not just the eventual right answer.

### Eleventh: real publisher/copyright, a real ProductVersion bug fix, and v0.2.0-rc4

**Windows Sandbox checked and ruled out as a clean-machine-test
workaround.** A follow-up review suggested it as a lighter-weight
alternative to a full VM. Checked directly: this session has no
elevation rights (`Get-WindowsOptionalFeature` itself requires admin),
and `systeminfo` reports "a hypervisor has been detected" — meaning this
environment is very likely already running inside virtualization itself,
which would make nested Windows Sandbox unreliable even if it could be
enabled. Sandbox also needs interactive GUI access this CLI session
doesn't have regardless. Genuinely ruled out, not just left unattempted.

**Real publisher and copyright**, supplied directly by the project owner
(Singh's Innovation & Advisory, matching the EULA's own licensor) --
`AppPublisher`/`AppCopyright` in `[Setup]`, plus `VersionInfoCompany`/
`VersionInfoCopyright` for the compiled exe's own Properties -> Details
tab (previously unset, defaulting to Inno Setup's own blank/generic
values). Product website, support email/URL, and a privacy-policy URL
remain explicit placeholders -- real, live addresses needed, not
invented here.

**A real, latent bug found and fixed while touching this same area**:
`VersionInfoVersion={#MyAppVersion}.1` hardcoded a literal `.1` as the
4th numeric version component regardless of `MyBuildId`'s actual value,
despite the script's own comment directly above it claiming "the
build-id sequence number becomes the 4th component." Every compile
across this entire project's history showed an identical
`ProductVersionRaw` (`0.2.0.1`) no matter how many times `MyBuildId` was
bumped -- the comment's claim was simply false, silently, since nothing
ever checked it. Fixed with a new, separate `MyBuildNumber` define
(Windows version-resource components are 16-bit, so `MyBuildId`'s own
`YYYYMMDD`-based text can't be used directly as the numeric component).
Verified by actually checking `(Get-Item
...).VersionInfo.ProductVersionRaw` across two consecutive builds and
confirming it genuinely changed (`0.2.0.1` -> `0.2.0.4`), not just that
the script compiled without error -- the same discipline this project
has applied throughout: a fix isn't verified until its actual effect is
observed, not just that the code ran.

**`v0.2.0-rc4` built and tagged**: `build-release.ps1` (fast path --
the engine tree was already freshly clean-staged one build earlier in
this same session, so `-CleanEngine` wasn't needed again) rebuilt from
this commit's tree. Validator 47/47, the full regression suite
(self-test, tempo, `transcribePolyphonic`, `MultiSessionTest`) all
passed, zero installer warnings. `BeatShoreDesktop.exe`, the VST3, and
the staging-manifest hash are all byte-identical to the immediately
prior build (only `BeatShoreSetup.iss` itself changed, and it isn't part
of the staged tree being hashed) -- only the installer's own hash and
its now-genuinely-distinct `ProductVersionRaw` changed. Committed
(`b78d31e`), tagged `v0.2.0-rc4`, pushed. `RELEASE_MANIFEST.md` and
`RELEASE_STATUS.md` updated with this build's real numbers, including
the real source-commit hash (a two-commit sequence -- code first, then a
follow-up commit updating the manifest to reference that exact hash --
since a commit can't literally contain its own resulting hash).

**Still explicitly not attempted, and not attemptable from here**:
live REAPER transcription and lifecycle testing; a clean-machine install
(Windows Sandbox now ruled out too, see above); code signing; human
legal review; the `kMaxGlobalQueueDepth`/512MB rejection boundaries
under real load (see "Tenth..." above).

**Update, same session: Actions were enabled on the GitHub side (by the
user, between checks -- not something done from here), and CI genuinely
started running.** Both real runs so far (`v0.2.0-rc3`, `v0.2.0-rc4`)
failed, each in roughly 80-110 seconds -- too fast to be a real compile
failure. GitHub's raw job-log download requires authentication this
environment doesn't have (confirmed: `/actions/jobs/{id}/logs` returns
403 even for a public repo), so the root cause was found by reasoning
about what's genuinely different on a fresh checkout, not by reading the
actual failure output: `build-release.ps1` assumed every CMake build
directory (`native/BeatShoreDesktop/build`,
`native/BeatShoreBridge/build`, `native/vst3sdk/build`,
`native/BridgeClientTest/build`) already existed and was already
configured -- true on this dev machine, where all four had been
configured interactively across many earlier sessions long before this
script existed, never true on a genuinely fresh checkout, since all four
are gitignored. `ninja` alone, with no prior `cmake -B`, fails
immediately. Fixed (commit `100d864`) by adding an unconditional `cmake
-B` configure step before every `ninja` invocation (idempotent and fast
when already configured, so free on a machine like this one) plus the
two build steps that didn't exist at all before: the VST3 SDK Validator
(exact CMake flags read from this machine's own already-configured
`CMakeCache.txt`, not guessed -- `SMTG_ADD_VST3_UTILITIES=ON` is
specifically what produces `validator.exe`) and `BridgeClientTest`
(needed for `BridgeClientTest.exe`/`MultiSessionTest.exe`, which the
staged regression suite runs). Verified locally: still exits 0, every
hash byte-identical to the immediately prior build -- a pure
build-infrastructure fix, confirmed to have zero effect on the actual
shipped binaries. **Not yet confirmed against a real fresh CI
checkout** -- both failed runs were triggered before this fix landed;
the next Actions run against `main` (no new tag needed --
`workflow_dispatch` is already wired in) is what would actually confirm
it.

### Twelfth: the CI fix's own fix -- stage everything, not just binaries

The `v0.2.0-rc4` tag push (Eleventh, above) got much further than either
prior CI run: every build/validator/test step this document already
verified locally now also passed on an actual GitHub-hosted runner, real
proof the CMake-configure fix genuinely worked. It then failed restaging
into `stage\`, which doesn't exist on a fresh checkout -- and that
turned out to expose a real, much bigger gap than a missing directory:
`stage\` held a large body of content (the EULA, third-party license
notices, the engine's own staged copy, the bundled Node.js runtime, the
VC++ redistributable) assembled by hand across many earlier sessions,
never captured by the automation script or the repo, since `stage\` is
entirely gitignored. This script was never actually reproducible from
scratch -- it happened to work locally only because that groundwork
already existed on this one machine.

Fixed in full: real source-of-truth text content (EULA, license notices)
moved into a new, git-tracked `native/installer/assets/`, copied from
there into `stage\` unconditionally every run; a missing engine-staging
step added (copies `native/BeatShoreDesktop/engine` into
`stage\engine\` at the exact nested depth `analyze.js`'s own unmodified
imports require -- see "Clean-machine packaging" above for why that
nesting matters); the Node.js runtime and VC++ redistributable now
fetched automatically when not already present. `.github/workflows/release.yml`'s
`-CleanEngine` is now unconditional in CI, not gated behind a
`workflow_dispatch` input that a tag push never even carries.

**Two more real bugs, found by actually reproducing the failure locally,
not guessed at**: robocopy's default retry behavior (no `/R`/`/W` given)
is up to 1,000,000 retries with a 30-second wait each -- one problem
file in an ~8,000-file `node_modules` tree hung this step for a very
long time, confirmed directly (it genuinely hung, twice, before this was
found). And two `tfjs-node` subdirectories -- `napi-v10` (a known-dead,
non-functional artifact; the real binding is `napi-v8`) and
`build-tmp-napi-v8` (node-gyp's own build-time MSBuild scaffolding, the
same category as the already-trimmed `deps/`) -- broke both robocopy
and Inno Setup's own compiler with Windows `MAX_PATH` errors once
actually staged for the first time. Reproduced the exact failure
directly (ran `robocopy` by hand outside the script, saw `ERROR 3`
against `napi-v10\tensorflow.dll`; re-ran Inno Setup alone and watched
it fail mid-compress inside `build-tmp-napi-v8`), confirmed excluding
just these two directories produces a clean `robocopy` exit code and a
successful Inno Setup compile, and added the same exclusion to the
`-CleanEngine` trim for consistency with the already-established
`deps/` removal.

Verified locally end to end: full run exits 0, Validator 47/47, complete
regression suite passed, installer compiles clean with zero warnings,
`ProductVersionRaw` genuinely `0.2.0.4`. **Not yet confirmed on an
actual fresh CI checkout** -- the Node.js/VC++-redistributable download
branches specifically were never exercised locally either, since both
were already staged here (only the "skip if already present" branch was
tested) -- a real, honestly-flagged gap, not assumed to work.

### Thirteenth: the 512MB memory budget verified for real, and a real orphaned-temp-file gap

A follow-up review asked to keep pushing on the still-unverified
resource-limit boundaries. Recalculating rather than re-guessing: the
`LoadBoundaryTest` runs so far had all used small audio (0.2s-60s at
22050Hz mono, ~0.2-5.3MB per request) specifically to probe
`kMaxGlobalQueueDepth` -- deliberately not big enough to meaningfully
stress `kMaxTotalReservedAudioBytes` (512MB), since even 32 such
buffers only total ~170MB. Switching to the actual per-request maximum
(`kMaxSampleRateHz` x `kMaxCaptureSeconds` x `kMaxAudioChannels` =
192kHz stereo, 60s, ~92MB -- matching `main.cpp`'s own comment on that
constant exactly) changes the math completely: only ~6 concurrently-
admitted requests are needed to exceed 512MB.

**Genuinely triggered for the first time**: 8 real sessions, each
submitting one ~92MB request, released via the same barrier-synchronized
burst technique as the session-cap test -- `accepted=5 SERVER_BUSY=3`
(5 x 92MB ≈ 460MB fit under the budget; the 6th tipped it over),
matching the desktop's own log exactly (3 real `REJECTED
ANALYSIS_REQUEST ... (SERVER_BUSY)` lines). `kMaxConcurrentSessions` and
the memory budget are now both empirically verified; only
`kMaxGlobalQueueDepth` remains unreached, for the same architectural
reason already documented in "Tenth" above.

**A genuine, minor gap found in passing, not previously confirmed**:
checking the OS temp directory for leftover files from this test
surfaced ~95 orphaned `bsr_*.bsmraw` files dated two days earlier --
none from this run (confirmed via clean completion and matching
request/response counts), all from earlier sessions where the desktop
was killed via `taskkill` mid-request (which this project's own testing
has done many times). There is no "clean up orphaned temp files on
startup" sweep today. Low severity -- the OS's own temp-directory
housekeeping eventually reclaims this, and it's not a security or
correctness issue -- but real, and now recorded rather than
unknowingly left as stray local state. Cleaned up manually as part of
this session; not fixed in code.

### Fourteenth: the real fresh-checkout CI failure, found by an actual from-scratch local repro

The `v0.2.0-rc6` tag push produced CI's first genuinely different result:
not a fast ~100s failure like rc3-rc5, but a real 9.5-minute failure
inside "Run release build" -- long enough to plausibly have gotten
through the actual compiles. GitHub's job-log API returns 403 without an
auth token (`gh` isn't available in this environment, confirmed again),
and the only public annotation was the generic "Process completed with
exit code 1." Rather than guess or wait on the user to paste a log,
reproduced it directly: a genuine `git clone --branch v0.2.0-rc6` of this
repo into an isolated short path (`C:\bsrepro`, chosen specifically to
avoid CMAKE_OBJECT_PATH_MAX -- an early attempt in a long scratch-space
path tripped that limit and was a false lead, not the real bug), with
JUCE/vst3sdk copied in at the exact pinned commits CI itself checks out,
then ran the unmodified `build-release.ps1 -CleanEngine` exactly as
`release.yml` invokes it. A first pass also tripped over a stale
`vst3sdk\build\CMakeCache.txt` copied in from this dev machine's own
tree (pointing at the wrong source path) -- also a repro artifact, not
the real bug, fixed by deleting that copied build dir.

With those two false leads eliminated, the repro surfaced two real,
previously-undiscovered script bugs, both invisible on this dev machine
for the same underlying reason: local state silently standing in for
steps the script itself never actually performed.

1. **`-CleanEngine`'s `npm ci` hardcoded `native/installer/tools/node24/
   {node.exe,npm.cmd}`** -- a path nothing in this script or in CI ever
   creates. It only worked here because that directory had been staged
   by hand at some point across earlier sessions and never captured in
   git or reproduced by any automated step -- the same category of bug
   as the stage\ directory fix from the previous round, just missed the
   first time because -CleanEngine was never exercised against a
   genuinely fresh checkout until now. Fixed with a `Find-Node24`
   function matching the existing `Find-VcVars64`/`Find-Iscc`
   auto-detection pattern: PATH first (which is exactly what CI's own
   `actions/setup-node@v4` step populates -- its comment already said
   "for build-release.ps1's own use, and for -CleanEngine", it just
   wasn't actually being used that way), falling back to
   `tools/node24` for compatibility with this dev machine's existing
   local setup.

2. **`stage\` itself was never created before the EULA `Copy-Item`
   wrote into it.** Only ever worked here because stage\ already existed
   from years of prior manual builds. Worse, empirically confirmed: the
   resulting `Copy-Item` failure did NOT actually stop the script despite
   `-ErrorAction Stop` -- under the `*>&1 | Tee-Object` pipeline this
   whole script runs through, the would-be-terminating error just gets
   written to the merged stream and execution silently continues. Same
   for the `-CleanEngine` `npm ci`/node24 failure above: a genuine
   `CommandNotFoundException` from `&` also didn't stop the script here,
   for the same reason. The script limped forward for several more
   Write-Step sections -- including successfully downloading the staged
   Node runtime and vc_redist -- until "Validating staged directory
   layout" finally caught the missing `LicenseFile.txt` and threw for
   real, which is the actual exit-1 CI hit. Fixed at the root: create
   `stage\` before the first thing that writes into it, rather than rely
   on -ErrorAction Stop reliably propagating through this pipeline shape
   (it doesn't, empirically, regardless of what the docs say it should
   do).

Both fixes applied, then verified by rerunning the exact same repro
end to end: `clean_engine: ok` for a genuine `npm ci` (not silently
skipped), "Staging EULA and third-party license notices" completed with
no errors, "Validating staged directory layout" reported "All 9 required
staged paths present" (previously "MISSING: LicenseFile.txt"), and
critically, `--self-test` against the freshly-`npm ci`'d engine passed
for real -- including actual Basic Pitch model load + inference, not
just files existing -- followed by the full staged regression suite
(tempo, transcribePolyphonic, MultiSessionTest) all passing, and a clean
Inno Setup compile producing a 98.9MB installer with `clean_engine`,
`layout_validation`, `staged_self_test`, and `staged_regression_suite`
all reporting `ok` in release-report.json. One incidental, non-fatal
finding along the way: `fix-tfjs-node-binding.js` reported "expected DLL
not found... tfjs-node package layout may have changed; skipping" against
a genuinely fresh `npm ci` install -- the script already treats this as
a soft warning, and the subsequent self-test's real Basic Pitch inference
passing confirms the binding works fine without that particular repair
step on a fresh install. Not investigated further; recorded here as a
real observation, not chased down since nothing was actually broken.

This is the most thorough verification this project's release tooling
has had: a real, isolated, from-scratch checkout at the exact commit CI
built, run through the unmodified production script, with both
structural bugs found and fixed by direct empirical reproduction rather
than guesswork. **Still not the same as an actual GitHub-hosted
`windows-latest` runner** -- this dev machine has VS2019 Build Tools
(CI likely has VS2022) and Node 25.9.0 on PATH (CI's `actions/setup-node`
pins exactly Node 24) -- so the next real tag push is still the final
confirmation, not a formality. Both bugs found were structural
(hardcoded paths, missing directory creation) rather than
toolchain-version-dependent, so there's good reason to expect this
holds, but "good reason to expect" is not "confirmed on CI," and won't
be asserted as such until a real run says so.

**Update: confirmed.** Pushed as `v0.2.0-rc7` (commit `817b4bf`) and
watched the real run to completion on GitHub's own `windows-latest`
runner (run id `33168764375`): every step green, "Run release build"
completed in 15m4s (11:53:56Z-12:09:00Z, entirely plausible next to the
~25min local repro given real network-bound `npm ci` and a shared
runner), installer artifact uploaded, overall run `status: completed,
conclusion: success`. This is the first genuinely passing CI run this
project has ever had. The `sign` job (needs `SIGNING_CERT_BASE64`/
`SIGNING_CERT_PASSWORD` secrets, neither configured) still leaves the
artifact unsigned, exactly as designed -- that remains a real, separate,
still-open item (see the release acceptance checklist), not something
this fix touched or claims to have addressed.

### Fifteenth: orphaned temp-file cleanup, fixed and verified with a real forced-termination test

The gap first found in "Thirteenth" above -- temp audio files from a
session killed mid-request are never cleaned up -- is fixed. Added
`sweepOrphanedTempFiles()` to `main.cpp`, called once at startup
immediately after `g_singleInstanceMutex` is acquired and before the
accept loop or any worker thread starts. That exact placement is what
makes it safe to run unconditionally: the single-instance mutex proves
no other `BeatShoreDesktop` instance is alive to still be using any
`bsr_*.bsmraw` file in the temp directory at that moment, and this
process hasn't created any jobs of its own yet either. The one edge
case that survives the parent process dying -- an orphaned Node child
that outlives it and might still legitimately have a very recent dump
open -- is handled by an age threshold (`kOrphanedTempFileMaxAgeSeconds`,
600s, generous margin over the ~60s worst-case request lifetime) plus
Windows' own file-locking: a file still genuinely held open fails to
delete with `ERROR_SHARING_VIOLATION` and is left alone, exactly like
the existing `releaseJobResources()` pattern this new function
deliberately mirrors. Only ever matches this project's own
`bsr_*.bsmraw` naming pattern -- nothing else in the OS temp directory
is touched. Every removal (and a summary line whenever anything was
removed or skipped-as-in-use) is logged, through the same `logLine()`/
`redactPath()` path as everything else, so a swept file's path is
redacted by default exactly like a live request's would be.

First attempt at wiring this in put the new
`kOrphanedTempFileMaxAgeSeconds` constant next to the other `kMax*`
constants further down the file, after `sweepOrphanedTempFiles()`
itself -- a real compile error (`C2065: undeclared identifier`), since
the function using it is defined earlier in the same translation unit.
Fixed by moving the constant up next to `g_reservedAudioBytes`, ahead
of `releaseJobResources()`, with a comment explaining why it lives there
instead of with the rest of the `kMax*` block it conceptually belongs
with.

Verified two ways, not just compiled-and-assumed-correct:

1. **A genuine forced-termination race, not a synthetic scenario.**
   `LoadBoundaryTest.exe --queue 1 1` (a real session, a real 92MB
   `ANALYSIS_REQUEST`) started, followed within the same PowerShell
   invocation by an immediate `Stop-Process -Force` on
   `BeatShoreDesktop.exe` -- a 50ms gap between the two, deliberately
   racy. This genuinely produced a real orphaned 92MB `bsr_*.bsmraw`
   file, the same failure mode "Thirteenth" originally found by accident
   across many earlier test runs, reproduced here on purpose in one
   shot.
2. **Deterministic proof of both branches of the age gate**, since the
   real orphan above is fresh (0s old) and waiting a genuine 10 minutes
   for a fast local test isn't practical: backdated that real orphan's
   own `LastWriteTime` to 15 minutes ago (a legitimate way to simulate
   "this has been sitting here a while" -- the code only ever reads the
   file's real last-write time, it has no way to know or care that the
   clock was wound back rather than genuine time passing), and created a
   second, deliberately fresh dummy file matching the same naming
   pattern as a control. Relaunched the freshly-built desktop and
   captured its real startup log:
   ```
   [+16ms] [desktop] startup cleanup: removed orphaned temp audio file <redacted> (age=907s)
   [+16ms] [desktop] startup cleanup: removed=1 skipped_in_use=0 skipped_recent=1
   ```
   The aged real orphan was removed; the fresh control file was left
   alone (confirmed still present on disk afterward) -- both branches of
   the age gate verified against real files, not just read as correct
   from the source. The user's own live REAPER session (still open from
   the "Fourteenth"-era REAPER lifecycle testing, connected to whichever
   desktop instance is currently running) auto-reconnected to the newly
   restarted desktop with no disruption beyond the deliberate restart
   itself, consistent with the already-verified reconnect behavior.

Not yet exercised: the true 10-minute unaged path (an orphan that's
genuinely, not artificially, old enough to sweep) and a second
forced-termination race under different timing (the 50ms gap used above
worked on the first attempt; not proven to be reliably reproducible at
that exact margin, only proven to be *possible*, which is what the fix
needs to handle regardless of how often it happens in practice).

**Update: confirmed on real CI too.** This fix, plus a regenerated
`RELEASE_MANIFEST.md`/`RELEASE_STATUS.md` (both had gone genuinely
stale, still describing commit `bc62426` while `main` had moved three
real commits ahead), landed as commit `fad817c` and `a462104`, tagged
and pushed as `v0.2.0-rc8`. Real GitHub-hosted `windows-latest` run
`33272954916`: `status: completed, conclusion: success`, "Run release
build" in 13m27s. `BeatShoreDesktop.exe` grew from 300,032 to 303,616
bytes (the new code), `BeatShore Bridge.vst3` unchanged, matching what
the local rebuild already showed -- CI didn't just pass, it passed with
the exact artifact-size delta expected from this specific change.

### Sixteenth: a futuristic reskin of the plugin editor -- visuals only, same everything else

The user asked for an updated look for the editor UI shown in REAPER,
with an explicit constraint: everything already tested has to stay the
same and connected. Read `PluginEditor.h`/`.cpp` in full first to know
exactly what "the same" meant concretely -- every `juce::Label`/
`juce::TextButton` member, every `onClick` lambda, `analyzeButtonClicked()`/
`transcribeButtonClicked()`/`openExportFolderClicked()`/`applyResult()`/
`timerCallback()`'s actual logic, and every string of button/status text
already visible in the user's own REAPER screenshots this session. None
of that changed. What changed is purely presentational:

- A `FuturisticLookAndFeel` (`juce::LookAndFeel_V4` subclass, local to
  `PluginEditor.cpp`, no new source file / no CMake change) restyles the
  two `TextButton`s: rounded corners, a gradient fill in the existing
  brand violet (`0xff9184d9` -- the same accent colour section headings
  already used before this change, not a new one), and a cheap
  multi-stroke glow approximation (no offscreen blur, just several
  `drawRoundedRectangle` calls at growing radius/shrinking alpha) that
  brightens on hover/press and dims when a button is disabled.
- `paint()` now draws a dark gradient background, a faint HUD-style grid
  texture, three glowing rounded "card" panels behind the existing
  Host Context / Bridge / Transcription sections (bounds computed in
  `resized()`, stored as members, read back in `paint()` -- geometry
  only, doesn't touch any control's actual bounds), a small vector
  "brand mark" (three bars, drawn as a `juce::Path`, no image asset), a
  pulsing status dot colour-coded by the *same* `BridgeStatus` enum
  `bridgeStatusText()` already switched on, and a thin progress bar
  reading the *same* `processor.isAnalysisInFlight()`/
  `getAnalysisProgress()` accessors `timerCallback()` already called.
  `timerCallback()` gained one line, `repaint()`, so the pulse and
  progress bar animate at the same 10Hz the labels already refreshed at
  -- no new timer, no new thread.
- Window grew from 440x560 to 480x620 for card-panel padding; the
  vertical stacking *order* of every section is unchanged.

One real compile error caught before it went anywhere: `juce::jlimit(0.0f,
1.0f, processor.getAnalysisProgress())` failed to compile
(`getAnalysisProgress()` returns `double`, and MSVC couldn't deduce a
single `Type` for `jlimit`'s three mismatched-type arguments) -- fixed
with an explicit `static_cast<float>`. A second, cosmetic-only line
(`.withStyle("Regular")` on a `FontOptions`, an API that doesn't
actually exist the way it was written) was caught the same way and
removed before it could fail a build at all.

Verified the same way every other change in this project has been:
rebuilt `BeatShoreBridge.vst3` clean, then ran the real Steinberg
Validator against it -- **47/47, unchanged** -- confirming the
compliance surface (parameters, bus config, editor construction) is
identical to before the reskin, not just "looks right in the source."
Then found and updated the actual copy REAPER loads from
(`C:\Users\krish\AppData\Local\Programs\Common\VST3\BeatShore
Bridge.vst3` -- discovered via `reaper-vstplugins64.ini`'s own registry
entry, not guessed at) and confirmed via SHA-256 that the deployed copy
is now byte-identical to the freshly built one. **Not yet confirmed**:
what it actually looks like rendered inside REAPER's own window --
this environment has no way to open a DAW GUI and look at pixels, so
the visual result itself is still the user's own call to make, the same
as every other REAPER-side verification this project has needed.

### Seventeenth: five new analysis kinds exposed in the UI, wired to backend that already existed

The user asked for more features. Before proposing anything, checked
what was actually real: `analyze.js`'s own `SUPPORTED_KINDS` already
listed nine kinds -- `loudness, tempo, key, structure, chords, timbre,
transcribeDrums, transcribeMono, transcribePolyphonic` -- but the editor
only ever exposed two (`tempo`, `transcribePolyphonic`). Asked the user
which of the remaining seven they actually wanted rather than building
all of them speculatively; they picked five: key, chords, loudness,
transcribeDrums, and transcribeMono (both bass and lead roles).

Read the whole call path first rather than guessing at scope. Found:
`PluginProcessor` already has a private `triggerAnalysisOfKind(kind)`
that `triggerTempoAnalysis()`/`triggerPolyphonicTranscription()` are
just thin wrappers over; `BridgeClient.h`'s own response parsing already
generically branches on wire message *type* (`MIDI_RESULT` vs
`ANALYSIS_RESULT`), not a hardcoded kind list, and for a non-numeric
`ANALYSIS_RESULT` (key's `{key,mode}` object, chords' segment array)
already falls back to `bsjson::stringify(result)` as the message text.
`main.cpp` already reads an incoming `role` field and forwards it to
Node. **The entire backend and wire protocol already fully supported
all five kinds end to end -- zero changes needed to `main.cpp`,
`analyze.js`, or `beatshore-dsp.js`.** The only real gap: nothing on the
plugin side ever *sent* a `role` field, which `transcribeMono` needs to
pick bass vs. lead pitch range.

Changes, all additive:
- `BridgeClient.h`: `requestAnalysis()` gained an optional `role`
  parameter, threaded through `PendingRequest` into the outgoing
  `ANALYSIS_REQUEST`'s `role` field when non-empty.
- `PluginProcessor.h/.cpp`: six new public wrapper methods
  (`triggerKeyAnalysis`, `triggerChordAnalysis`, `triggerLoudnessAnalysis`,
  `triggerDrumTranscription`, `triggerBassTranscription`,
  `triggerLeadTranscription`), each a one-line call into
  `triggerAnalysisOfKind()` -- byte-for-byte the same pattern
  `triggerTempoAnalysis()` already used.
- `PluginEditor.h/.cpp`: a new "Quick Analysis" card (Key/Chords/Loudness
  buttons sharing one result label, since only one request is ever in
  flight at a time -- the same UX the existing Analyze Tempo button
  already has when clicked twice) and three new buttons in the
  Transcription card (Drums/Bass/Lead), sharing the *existing*
  `transcribeStatusLabel`/`transcribeDetailLabel` since they're the
  identical `MIDI_RESULT` shape `transcribePolyphonic` already used.
  `applyResult()`'s two-way `isTempo ? ... : assumeMIDI` branch became a
  real three-way classification (tempo / MIDI-producing kinds / plain
  value kinds) via an explicit `isMidiProducingKind()` kind check --
  more correct than the old binary assumption, not just extended to fit.
  Window grew again (480x760) for the new card and buttons; layout order
  is unchanged apart from insertion.

Verified three ways, in increasing order of how much they actually
prove:
1. Rebuilt clean (zero warnings), Steinberg Validator still **47/47** --
   no new `AudioProcessorParameter`s were added, so this was expected to
   hold, and did.
2. **A real, throwaway smoke-test program** (compiled directly with
   `cl.exe`, no CMake target added -- not part of the tracked project),
   mirroring `load_boundary_test.cpp`'s own connect/HELLO/
   `ANALYSIS_REQUEST` pattern, sent six real requests (a real 3s
   two-tone signal, not silence) against a real running
   `BeatShoreDesktop.exe`: key, chords, loudness, transcribeDrums,
   transcribeMono/bass, transcribeMono/lead. **6/6 genuinely passed** --
   real responses, not mocked: key detected "A major", chords detected
   "Am", loudness read -15.1dB, drums found 100 onsets (expected for a
   continuous tone, not percussive content -- proves the pathway works,
   not that the *musical* result is meaningful for this input), both
   mono roles returned real MIDI files. This is the first time this
   project has verified these five kinds end-to-end at all, for any
   caller.
3. Deployed to the actual copy REAPER loads from
   (`C:\Users\krish\AppData\Local\Programs\Common\VST3\BeatShore
   Bridge.vst3`), confirmed byte-identical via SHA-256, same as the
   reskin above.

**Not yet confirmed**: clicking these new buttons from inside REAPER's
own UI, and whether `role=bass` vs `role=lead` produces a genuinely
different result for the same real audio (the smoke test's synthetic
two-tone signal returned 1 note for both -- consistent with the request
being accepted and processed correctly, not proof the pitch-range
distinction itself is audibly meaningful; that needs real bass/lead
recordings, which is exactly the kind of judgment call this environment
can't make and REAPER testing already has a track record of answering).

### Eighteenth: sidebar navigation, modeled on a much larger design blueprint the user shared

The user shared a mockup of "BeatShore Reverse Studio" -- a full
standalone-app-scale design: 12-part stem separation, spectral repair,
de-noise/de-reverb/de-bleed/de-clip, instrument/timbre identification,
humanization, sound matching, a full EQ/comp/mixing/mastering chain,
A/B reconstruction, an "Ask BeatShore" AI chat, "GOD MODE" analysis --
and asked for more features, then for this to become the real plugin.
Before writing anything, said plainly what's actually real: this
plugin's entire backend is tempo/key/chords/loudness/four transcription
kinds (the "Seventeenth" section above). Everything else pictured needs
real trained ML models (stem separation is Demucs/Spleeter-class
territory) or substantial new DSP systems that don't exist in this
codebase and can't be fabricated in a session -- not a wiring gap like
the last one.

Two deliverables, in the order the user asked for: a static HTML/CSS
design reference (published as a Claude Artifact, every control
inert -- a planning document, not a claim that any of it works) built
from the actual shared screenshot's real layout and values, not
generic placeholder content; then, on the user's go-ahead, a real
reorganization of the actual JUCE plugin editor toward the blueprint's
*structural* pattern (sidebar navigation, a persistent header) without
faking its *feature* content. Scoped and stated the defaults rather
than blocking a second time on an already-answered question:

- Ten sidebar sections, matching the blueprint's own order and names.
- Bridge connection status moved out of a page-specific card into a
  **persistent header** (visible regardless of which page is selected)
  -- you need to know if BeatShore Desktop is reachable no matter what
  you're doing, the same reasoning the blueprint's own header status
  indicators follow.
- **Overview** = host context readouts (sample rate, tempo, time
  signature, transport, playhead) -- unchanged content, moved to its
  own page.
- **Transcribe** = every trigger this plugin actually has (tempo, key,
  chords, loudness, piano/guitar, drums, bass, lead) -- the "Seventeenth"
  section's feature set, now organized under the tab a user would
  actually expect to find it in, instead of three separate cards on one
  flat page.
- The other eight (Separate, Repair, Reconstruct, Humanize, Sound
  Match, Mix, Master, Export) show a real, explicit "NOT BUILT YET"
  panel naming what it would take -- never a control that looks live
  but does nothing.

Implementation: a `Page` enum and `showPage()`/`updateControlVisibility()`
pair drive which existing controls are visible and how `resized()`
lays out the active page's content -- every `juce::Label`/
`juce::TextButton` is the exact same object with the exact same
`onClick` handler it had before this reorg, just shown/hidden and
repositioned rather than recreated. Sidebar nav buttons reuse the
existing `FuturisticLookAndFeel`, extended with one purely additive
rendering path (a solid-fill "active" look gated on `getToggleState()`,
which no pre-existing button ever sets) so every already-tested
button's appearance is provably unchanged -- only the nav buttons ever
hit the new code path. Window grew to 760x700 for the sidebar; `paint()`
draws a card panel behind whichever page's content is actually visible
rather than always drawing three fixed panels.

Verified the same way as "Seventeenth": clean rebuild (zero warnings),
Steinberg Validator still **47/47**, and the same throwaway smoke-test
program re-run against a fresh desktop -- **6/6 still passing**,
identical results to before the reorg (confirms `PluginProcessor`/
`BridgeClient.h` genuinely weren't touched, not just assumed). Deploying
to the copy REAPER actually loads from hit a real, honest blocker this
time: REAPER (`test [modified]` -- unsaved changes in its title bar) had
the plugin loaded, locking the DLL (`robocopy` error 32, "the process
cannot access the file because it is being used by another process").
Did not force-close the user's own DAW session to work around it --
that's a real, deliberate line, not an oversight. **Not yet deployed to
the live copy** -- needs the user to close the plugin instance or
REAPER itself first, same as any other REAPER-side change this project
has needed their hands for.

### Nineteenth: real humanization -- the first genuinely new DSP capability from the design blueprint

After the sidebar reorg, the user pushed further: match the blueprint's
picture, and make everything in it real. Refused to fake the parts that
genuinely can't be -- 12-part stem separation (needs a trained
Demucs/Spleeter-class model), de-reverb (an unsolved-in-general research
problem), de-bleed (source separation again), instrument identification
as pictured (would be a low-accuracy heuristic badged as "AI"), and
"Ask BeatShore" (a real LLM integration needs the user's own provider/
API-key decision) -- all stated plainly rather than quietly built as
something worse than what's already documented. Committed instead to
building exactly one real vertical completely: **Humanize**, the one
blueprint feature that's genuinely achievable as plain, describable DSP/
MIDI manipulation with no ML model required.

**What's real**: `beatshore-dsp.js` gained `applyHumanization(notes,
opts)` -- a parameterized randomization (timing/velocity/dynamics/
articulation, each 0..1, plus a preserveGroove flag), explicitly
documented as NOT a learned model, using a real seeded xorshift32 PRNG
(reproducible given the same seed, not `Math.random()`). Timing jitter
is scaled to the same tempo-relative 16th-note grid `humanizeStats()`
(the existing, older *measurement* function -- this is its counterpart,
applying variation rather than measuring it) already uses, so the two
functions agree on what "one grid unit" means. `analyze.js` reads five
new optional request fields (`humanizeTiming`, `humanizeVelocity`,
`humanizeDynamics`, `humanizeArticulation`, `preserveGroove`) and, only
when at least one is actually nonzero, applies the transformation to the
notes array before `humanizeStats()` runs and before the MIDI file is
written -- so both the reported stats and the exported file reflect the
humanized result, not the raw transcription. A request that doesn't ask
for it costs nothing extra and behaves exactly as before this feature
existed.

Threaded the same five fields the rest of the way, mirroring the
existing `role` field's own pattern exactly at every layer:
`AnalysisJob` (main.cpp) gained the five fields; the pipe-session handler
reads them off the incoming `ANALYSIS_REQUEST` and forwards them to
Node; `BridgeTypes.h` gained a shared `HumanizeSettings` struct (all-zero
default = "not requested") plus a `humanizeApplied` field on
`BridgeAnalysisResult`; `BridgeClient::requestAnalysis()` gained an
optional `HumanizeSettings` parameter, sent only when `isActive()`;
`PluginProcessor` gained a persistent `humanizeSettings` member plus
`setHumanizeSettings()`/`getHumanizeSettings()`, read by
`triggerAnalysisOfKind()` on every trigger. The real, honest limitation,
stated in the UI itself (`humanizeExplainerLabel`), not just in this
log: there is no live-editable note buffer in this plugin, so
humanization is **not retroactive** -- set the amounts on the Humanize
page, then trigger a transcription; it does not reach back and modify a
result already showing on the Transcribe page.

**Humanize is now a real, third built page** in the sidebar (alongside
Overview and Transcribe) -- four real `juce::Slider` rotary knobs
(Timing/Velocity/Dynamics/Articulation, 0-100%) and a real
`juce::ToggleButton` (Preserve Groove), every control's `onValueChange`/
`onClick` pushing the current state to `processor.setHumanizeSettings()`
in one call so the processor's copy is always exactly what the sliders
show. The other seven blueprint sections (Separate, Repair, Reconstruct,
Sound Match, Mix, Master, Export) still show the honest "NOT BUILT YET"
panel from the Eighteenth section, for the real reasons stated above,
not because they were skipped without explanation.

**Verified two ways, the second one considerably more real than a
structural check:**
1. Clean rebuild across all three affected binaries
   (`BeatShoreDesktop.exe`, `BeatShore Bridge.vst3`) -- zero warnings --
   and the Steinberg Validator still **47/47**.
2. Extended the same throwaway smoke-test program with a real
   before/after comparison: `transcribeDrums` against the IDENTICAL
   synthetic audio, once with no humanize fields at all (the exact
   request every existing caller already sends) and once with all four
   amounts at maximum. `transcribeDrums`'s onset detection has no
   randomness of its own, so any difference between the two runs can
   only have come from `applyHumanization()` genuinely executing --
   not inferred from reading the code. Real result: baseline
   `humanizeApplied=false, firstNote.time=0.0348299s, vel=127`;
   humanized `humanizeApplied=true, firstNote.time=0.0142093s, vel=90`.
   Both the flag and real note data changed, confirmed by two live
   requests against a real running desktop, not assumed. **7/7** smoke
   cases passed (the original six plus this comparison).

**Not yet verified**: actually turning the sliders in REAPER's own UI
and confirming a subsequent transcription comes out audibly humanized
-- the smoke test proves the wire protocol and DSP transformation are
real and correctly wired end-to-end; it does not and cannot exercise
`PluginProcessor`/`BridgeClient.h`'s own new code paths
(`setHumanizeSettings()`, the `HumanizeSettings` parameter on
`requestAnalysis()`) the way clicking real JUCE controls would, since
this environment has no way to drive a native Windows GUI. That code is
a thin, mechanical mirror of the already-proven `role` parameter
threading (same files, same pattern, same places), not new design, but
"mirrors a proven pattern" is not the same claim as "verified," and
isn't asserted as more than that here.

An external review of the first draft caught six genuine release blockers
and a long list of real script issues, none of which had been caught by
this project's own testing (which had focused on "does the staged engine
work," not "is the installer script itself correct or complete"). All of
the concretely actionable ones were fixed and re-verified this session,
not just acknowledged:

- **Node 25.9.0 was already end-of-life and should never have been
  staged.** Node's own release schedule puts v25 EOL at 2026-03-31.
  Migrated to **Node 24.19.0 LTS** (Active LTS as of this writing):
  downloaded a standalone copy (not a system-wide install, so this dev
  environment's own Node 25 is untouched), ran `npm ci --omit=dev` in a
  **completely fresh** `native/BeatShoreDesktop/engine/` checkout against
  it (not a copy of any Node-25-installed `node_modules`), confirmed the
  native `tfjs-node` binding rebuilds and loads correctly under Node 24,
  re-applied the same trim (deps/, build-tmp-napi-v8/, source maps --
  739MB -> ~302MB, matching the earlier Node-25 measurement almost
  exactly), and **re-ran the complete tempo, transcription, cancellation,
  and multi-session test suites** against this from-scratch Node-24
  staged tree. All passed, byte-identical to every prior run (91 notes,
  `sha256:173a3d6c...`), including the cancel-and-restart cycle correctly
  killing and respawning the *Node-24* process specifically.
- **`--self-test` now genuinely exists**, not just referenced by the
  installer script. `BeatShoreDesktop.exe --self-test <analyze.js path>`
  (`runSelfTest()`, `main.cpp`) starts a real `NodeEngine` directly (no
  pipe, no plugin needed), confirms `READY`, runs a real `tempo` request
  and a real `transcribePolyphonic` request against a small synthesized
  two-note audio fixture (proving the Basic Pitch model genuinely loads
  and infers -- the check most likely to fail from a packaging mistake),
  and confirms the MIDI export directory is writable. Verified both ways:
  run against the real staged tree, exit 0, all four checks PASS
  (`noteCount=2` on the synthetic fixture); run against a deliberately
  broken script path, exit 1, with the specific correct failure reason
  logged.
- **A genuine Pascal Script syntax bug, caught by re-reading the file
  carefully, not by a compiler (none available here)**: the installer's
  trailing documentation block used `;`-style INI comments, but it sits
  *after* the `[Code]` section's Pascal functions with no new `[Section]`
  header before it -- meaning it's still inside Pascal-script parsing
  context, where `;` terminates a statement rather than starting a
  comment. Would have been a real compile error. Fixed by converting that
  entire block to `//` comments. The review that flagged "verify actual
  Inno syntax" was right to ask, even though the *specific* backslash
  concern raised turned out to be a rendering artifact of how the script
  was pasted into a chat message, not a bug in the actual file (confirmed
  by reading the real file's bytes directly: `#define` and
  `MessagesFile: "compiler:Default.isl"` are both already correct,
  unescaped).
- **VC++ redistributable handling was a presence check, not a version
  check**, exactly as flagged (`installed <> 1` only confirms *something*
  is registered, not that it's new enough). Rewritten to always run the
  redistributable's own installer quietly and let *it* decide whether an
  install/repair/upgrade is needed (Microsoft's own recommended approach,
  and simpler/more robust than replicating their Major.Minor.Bld.Rbld
  compatibility logic with a guessed threshold) -- with real exit-code
  handling (0/1638 success, 3010 success-needs-restart, anything else
  logged and surfaced as a real failure), not just "wait and hope."
- **`PrivilegesRequired=admin` added explicitly** -- the installer writes
  into Program Files and the shared Common Files VST3 folder, both of
  which need elevation; this was previously only implied by
  `DefaultDirName={autopf}`, not stated.
- **Self-test now runs in both silent and interactive installs**
  (previously gated behind `Check: not WizardSilent`, which would have
  skipped verification for exactly the deployment scenario -- unattended
  install -- that most needs it). Runs via `CurStepChanged(ssPostInstall)`
  rather than a declarative `[Run]` entry specifically so its real exit
  code can be inspected. **Failure handling rewritten** (see "Fifth:
  security hardening, Start-at-login fix, and graceful shutdown" below
  for the full account): a persistent `selftest-log.txt` now captures the
  actual PASS/FAIL output (previously only a bare exit code was checked;
  the specific failing check was never actually surfaced anywhere), the
  install is marked incomplete via a plain marker file, an immediate
  uninstall is offered interactively, and Setup's own exit code is forced
  nonzero so a silent/scripted deployment can detect the failure.
  Honestly documented, not glossed over: Inno Setup has no built-in way
  to roll back files already committed by `ssPostInstall`, so this is a
  real mitigation for that gap (mark-incomplete + offer-uninstall +
  nonzero exit code), not an actual rollback -- a genuine rollback would
  need the self-test to run against a pre-install staged copy before
  anything is committed, a larger restructuring not attempted here.
- **A real, permanent AppId generated** (`E8A18368-E91F-4642-BDA0-5DEFD6A19286`,
  replacing the all-zeros placeholder) -- publisher name, support/product
  URLs, copyright, and versioning/upgrade policy remain placeholders on
  purpose: business decisions, not something to invent on the project's
  behalf.
- **EULA vs. third-party notices separated in the script's own comments**:
  `[Setup]`'s `LicenseFile` is now explicitly documented as BeatShore's
  own end-user agreement (real content still needed), distinct from
  `{app}\Licenses\`, which holds third-party MIT/Apache attribution
  notices the user is informed of, not necessarily asked to contractually
  accept.
- **A single-instance broker mutex** (`Global\BeatShoreDesktopBroker`,
  `main.cpp`) -- a second `BeatShoreDesktop.exe` now refuses to start
  rather than racing the first for the same named pipe. Verified directly:
  started one instance, started a second, the second logged a clear
  FATAL message and exited with code 1 while the first kept running
  unaffected; `--self-test` (which doesn't touch the pipe at all)
  confirmed to still work correctly alongside a running broker.
- **A release manifest** (`native/installer/RELEASE_MANIFEST.md`) now
  records real SHA-256 hashes -- computed from the actual staged files
  this session, not placeholders -- for `BeatShoreDesktop.exe`, the VST3
  binary, the bundled `node.exe`, `analyze.js`, `beatshore-dsp.js`, the
  `tfjs_binding.node` native addon, `tensorflow.dll`, the Basic Pitch
  model weights, and the three third-party license files, plus the exact
  tool versions (Node 24.19.0, tfjs-node 4.22.0, VST3 SDK 3.8.1, JUCE
  9.0.1) used to produce them.

**Not attempted, honestly**: code signing (no certificate available in
this environment), the automated staging-layout test the review
recommended (the current depth-preserving nesting is verified working but
still fragile -- see the `.iss` file's own trailing note for a better
long-term shape), and the desktop-process startup/tray-app/autostart
question (a product decision, not resolved here). The remaining two
release blockers from the review -- JUCE licensing and the compiled,
clean-machine-tested installer -- are unchanged from before: still real,
still open, still need a decision and a second machine respectively, not
something this session can close alone.

### Twentieth: real EQ/Compressor/Limiter -- the second real DSP vertical from the design blueprint, and the first with genuine host parameters

Continuing the same "build exactly one real vertical completely, refuse
the ML-requiring parts" approach as Humanize: the user asked for the
blueprint's Mix page (EQ/compression/limiting) built "completely too, the
same way." Unlike Humanize, this is a fundamentally different *kind* of
feature -- not a request sent to BeatShore Desktop and a result read back
later, but real-time signal processing run directly in this plugin's own
`processBlock()`, on whatever audio the host is already passing through
it, using `juce_dsp`'s own production classes (`juce::dsp::IIR::Filter`,
`Compressor`, `Limiter`) rather than hand-rolled math.

**What's real**: `Source/MixChain.h` (new file) -- a
`juce::dsp::ProcessorChain` of three `ProcessorDuplicator<IIR::Filter,
IIR::Coefficients>` stages (low shelf @150Hz, mid peak @1kHz, high shelf
@6kHz, each fixed-Q) followed by a `Compressor<float>` and a
`Limiter<float>`, in that order. `PluginProcessor` gained seven real
`AudioProcessorParameter`s (`mixEnabledParam` plus six floats: EQ low/mid/
high gain, compressor threshold/ratio, limiter threshold) -- genuine host
parameters added via `addParameter()`, not plugin-local settings the way
Humanize's amounts are, so they show up in the host's own automation lanes
and are saved/restored with host automation the same as any other
parameter (JUCE's default `AudioProcessor` state handling covers this;
nothing custom was needed). `prepareToPlay()` builds a `ProcessSpec` and
calls `mixChain.prepare()`; `processBlock()` reads the seven parameters
every block (only recomputing IIR coefficients when the EQ gains actually
changed, to avoid needless work) and calls `mixChain.process(block,
!mixEnabledParam->get())`.

The editor's Mix page is the **first page whose controls are genuine host
parameters** rather than a plugin-local setting pushed through a single
`setXSettings()` call -- six real `juce::Slider` rotary knobs and one
`juce::ToggleButton`, each bound with `juce::SliderParameterAttachment`/
`juce::ButtonParameterAttachment` (JUCE's own idiomatic mechanism,
declared in `juce_ParameterAttachments.h`) directly to the
`RangedAudioParameter&`s `PluginProcessor` now exposes
(`getEqLowShelfGainParameter()` etc.). The attachment reads the
parameter's own `NormalisableRange`/default value to set the slider's
range and reset value -- confirmed by reading the attachment's own
constructor in `juce_ParameterAttachments.cpp`, not assumed -- so host
automation, undo, and UI sync are all handled by JUCE itself.
`pageIsBuilt()` now includes `Page::Mix`; the other six sections (Separate,
Repair, Reconstruct, Sound Match, Master, Export) still show the honest
"NOT BUILT YET" panel, for the same reasons stated in the Nineteenth
section.

**Verified two ways:**
1. Clean rebuild of `BeatShoreBridge.vst3` (juce_dsp newly linked, zero
   warnings) and the Steinberg Validator, still **47/47** -- worth calling
   out specifically because, unlike Humanize, this feature adds seven
   genuinely new `AudioProcessorParameter`s to the plugin, which is
   exactly the kind of change that can break a host's parameter
   enumeration/automation contract; the Validator re-running clean
   confirms it didn't.
2. A new standalone console test, `MixChainTest`
   (`native/BridgeClientTest/Source/mixchain_test.cpp`), that includes the
   REAL `MixChain.h` directly (unmodified) and feeds it synthetic sine
   tones through real `juce::dsp::AudioBlock`s block-by-block, exactly how
   `processBlock()` does -- because this feature has no protocol round
   trip at all, `feature_smoke_test.cpp`'s "send a request, check the
   response" approach (used for the analysis-kinds and Humanize features)
   cannot exercise it; the only way to prove real audio processing is
   happening is to measure real before/after signal differences directly.
   **8/8 checks passed**, each printing a genuine measured number, not
   just PASS/FAIL:
   - Bypass leaves audio bit-exact even with extreme EQ/comp/limiter
     settings dialed in.
   - `+12dB`/`-12dB` high-shelf settings raise/lower an 8kHz tone's RMS by
     a real measured `+9.12dB`/`-9.11dB` (not the full nominal 12dB --
     expected and correctly explained by the shelf's Q0.7 slope at that
     frequency, not a bug).
   - An 8:1 compressor measurably reduces a sustained tone's RMS by a real
     measured `-7.75dB` once its 15ms attack settles.
   - The limiter's hard `[-1,1]` output ceiling holds on a genuinely
     clipping 1.5x-full-scale input at two different threshold settings,
     **and** its threshold parameter has a real, measurable effect
     (`0.232` vs `0.520` output RMS) on a moderate, non-clipping input --
     split into two separate checks specifically because reading
     `juce_Limiter.cpp` directly revealed `juce::dsp::Limiter` is a
     "loudness maximizer" (always-on internal -10dB/4:1 compression stage
     plus an automatic makeup-gain multiply, unconditionally hard-clipped
     at the very end) rather than a simple "clamp above threshold X" --
     the first version of this test assumed the latter, got two genuinely
     confusing failures, and was corrected to test what the DSP actually
     does instead of loosened until it passed. That correction is real
     engineering content, not just test hygiene: it's the same "loudness
     maximizer" behavior a REAPER user will actually hear if they push the
     limiter threshold around, so the plugin's own explainer text was
     written to match this reality (fixed compressor attack/release,
     fixed EQ band centers, no claim that the limiter threshold acts as a
     hard dB ceiling).

**Not yet verified**: actually turning the six knobs in REAPER's own UI
and confirming an audible EQ/compression/limiting effect on real playing
audio, and confirming host automation (recording/playing back automation
on these new parameters) works end-to-end in a real DAW session -- the
same category of gap noted for Humanize, since this environment has no
way to drive a native Windows GUI or listen to audio. `MixChainTest`
proves the DSP genuinely transforms audio and that the parameters are
correctly wired to it; it does not and cannot exercise
`SliderParameterAttachment`/`ButtonParameterAttachment`'s own UI-thread
code the way dragging a real knob would.

### Twenty-first: real EBU R128 loudness/true-peak metering -- the Master page, and the first vendored third-party library

Following up on a curated open-source list the user shared (a ChatGPT
conversation surveying options for the still-unbuilt blueprint pages):
**libebur128** (MIT) stood out as the strongest next real vertical --
small, permissively-licensed, no ML model or training involved, same
"process this plugin's own real audio" shape as MixChain. Built the same
way as Mix and Humanize: one complete, real, independently-verified
feature, not a mockup.

**What's real**: vendored libebur128 unmodified into
`Source/third_party/libebur128/` (`ebur128.c`/`.h` plus its own bundled
`<sys/queue.h>` replacement for MSVC -- see that directory's own
`VENDORED.md` for the exact upstream commit and license text, copied
verbatim). `Source/MasterMeter.h` (new file) wraps the C API: `prepare()`/
`reset()` manage an `ebur128_state*`, `process()` interleaves JUCE's
per-channel buffers into a reusable scratch buffer (grown once, never
reallocated per block) and feeds `ebur128_add_frames_float()`, then reads
momentary (400ms)/short-term (3s)/integrated LUFS and true peak back out
-- all on the SAME thread that just wrote to it (confirmed by reading
libebur128's own docs: its query functions aren't documented safe against
a concurrent writer), publishing results into `std::atomic<double>`s a UI
timer reads lock-free, the same cross-thread contract `HostSnapshot`
already uses. `PluginProcessor` feeds it every block with whatever's
about to go back to the host -- AFTER the Mix block, so Master reflects
this plugin's actual output when Mix is engaged, not the pre-Mix signal --
and services a reset request (raised by the editor's Reset Meter button)
the same request/service pattern already used for the ring-buffer swap,
rather than letting the message thread call `ebur128_destroy()`/
`ebur128_init()` concurrently with the audio thread's own writes.

**Master is now the fifth real page**: four live read-only readouts
(Momentary/Short-Term/Integrated LUFS, True Peak) refreshed every
`timerCallback()` tick, plus one real control (Reset Meter). Unlike Mix,
there's no "disabled" state to gate behind -- observing audio costs far
less than filtering it, and a meter with nothing to show is just idle
numbers, not added risk -- so it's always running, matching how
`HostSnapshot` itself is never gated behind a page either.

**A real build problem, found and fixed, not routed around**: the first
build failed with `error C2065: 'M_PI': undeclared identifier` in
`ebur128.c` -- MSVC's `<math.h>` doesn't define `M_PI` unless
`_USE_MATH_DEFINES` is set before it's included (the vendored file's own
top-of-file comment says so outright: *"You may have to define
_USE_MATH_DEFINES if you use MSVC"*). Fixed via a CMake
`set_source_files_properties()` scoped to just that one file, not a
project-wide define and not an edit to the vendored source itself -- so
"vendored unmodified" in `VENDORED.md` stays true.

**Verified two ways:**
1. Clean rebuild, zero errors, and the Steinberg Validator still **47/47**.
2. A new standalone console test, `MasterMeterTest`
   (`native/BridgeClientTest/Source/mastermeter_test.cpp`), includes the
   REAL `MasterMeter.h` directly and feeds it synthetic tones, same
   approach as `MixChainTest` and for the same reason: Master has no
   protocol round trip to exercise with a request/response smoke test.
   **5/5 checks passed**, with a genuine correction caught mid-session,
   the same category of honesty as the Twentieth section's Limiter
   discovery: the first version of check 1 asserted a remembered
   "-3.01 LUFS BS.1770 calibration reference" for a STEREO dual-mono
   full-scale 1kHz tone -- real measured result was ~0.007 LUFS, and the
   check failed. Investigating rather than just loosening the tolerance
   found the actual error: -3.01dB is a sine wave's real RMS-vs-peak
   relationship, a different fact than K-weighted LUFS, and the "-3.01
   LUFS" figure I was recalling turns out to apply to a MONO channel, not
   stereo (confirmed by re-running the same tone through a mono-prepared
   `MasterMeter`: **-3.0036 LUFS**, matching almost exactly). The test was
   corrected to check the mono case against a physically-reasoned band
   derived from first principles (sine RMS-vs-peak plus BS.1770's
   documented K-weighting shelf response near 1kHz) instead of an
   unverified memorized constant, and a second, calibration-independent
   check (halving amplitude measurably lowers momentary loudness by
   almost exactly 6.02dB -- a real power relationship, true regardless of
   any absolute reference) was kept as the primary proof the DSP is
   correct. Also verified: digital silence reads as -inf, true peak
   tracks `20*log10(amplitude)` for an oversampled tone, and `reset()`
   genuinely restarts Integrated LUFS rather than silently continuing to
   average in whatever played before it.

**Not yet verified**: opening the Master page in REAPER and confirming
the readouts move sensibly while real audio plays, and that Reset Meter
visibly restarts them -- the same environment limitation noted for every
other real-audio feature in this project (no way to drive a native GUI or
generate real playback here). `MasterMeterTest` proves the measurement
math is genuinely correct against synthetic signals with known properties;
it doesn't exercise `PluginProcessor`'s own block-by-block feed or the
editor's `timerCallback()` read-out path the way an actual REAPER session
would.

**Deployed and hash-verified**: after confirming REAPER was closed (the
user closed it and said so), both this feature and the Twentieth
section's Mix build were robocopied together to the real
`C:\Users\krish\AppData\Local\Programs\Common\VST3\BeatShore Bridge.vst3`
REAPER loads from, and the deployed copy's SHA-256
(`c097ae2b2d83891dc492b40be1696814f06061b63ddbb5ac4c2c9d31d63a7042`)
confirmed to match the freshly built one exactly.

### Twenty-second: a real bug, found live in REAPER -- getStateInformation() was a no-op placeholder

The first genuinely live REAPER acceptance test of this session's real
features (Overview, Transcribe, Humanize, Mix, Master) surfaced an actual
bug, not a REAPER quirk or user error -- worth recording exactly how it
was found, since the finding process is itself the evidence this wasn't
guessed at.

**Symptom**: testing whether Mix produces an audible change, an offline
render with real, confirmed-live Mix settings (screenshotted on the
plugin's own UI immediately before rendering: Mix Enabled ticked, six
real non-default knob values) kept coming back numerically identical to
the unprocessed baseline -- three render attempts in a row, the last one
with the UI state photographically confirmed correct at render time,
ruling out "forgot to tick the checkbox."

**Root cause, found by reading the actual code**: `getStateInformation()`
was exactly what its own comment said it was -- *"No user-configurable
parameters exist yet — this is a placeholder structure"* -- written
before any real parameter existed and never updated since. It saved
nothing but a version number. `setStateInformation()` correspondingly had
nothing to restore. Since this plugin adds its parameters directly via
`addParameter()` (not `AudioProcessorValueTreeState`, which would have
handled this automatically), JUCE does not persist parameter values on
its own -- that's the plugin's own responsibility, and this plugin had
never actually done it. REAPER's offline render builds/restores a
separate plugin instance via exactly this save/restore path, so a fresh
instance with nothing to restore from silently fell back to every
parameter's default (`mixEnabledParam` = false) -- explaining the exact
symptom observed. The same root cause would have broken REAPER project
save/reload identically, for the identical reason, before that row of
the acceptance table was ever reached.

**Fixed generically, not by hand-listing Mix's seven parameters**:
`getStateInformation()`/`setStateInformation()` (`PluginProcessor.cpp`)
now walk `getParameters()` and save/restore each one by its real
`RangedAudioParameter::getParameterID()` (stable across reordering,
unlike array index) and its normalized 0..1 value via
`setValueNotifyingHost()` -- so this automatically covers every current
parameter (`mixEnabledParam` and all six Mix float params) and any added
later, with no further change needed here if a future page adds new real
parameters. `humanizeSettings` -- not a real `AudioProcessorParameter` by
design (see its own header comment) -- is serialized alongside by hand,
since the generic parameter walk can't reach it.

**A second, related gap fixed while investigating**: even with state now
correctly persisted, the Humanize page's four sliders and Preserve
Groove toggle had no reverse sync from processor to UI at all -- they
were hardcoded to zero in the editor's constructor regardless of what
`humanizeSettings` actually held. The Mix page didn't have this problem
(its real `SliderParameterAttachment`/`ButtonParameterAttachment`s
already sync from the parameter via their own `sendInitialUpdate()`), but
Humanize's plain sliders needed an explicit fix: the constructor now
reads `processor.getHumanizeSettings()` and initializes all five controls
from it. Without this, even correctly-restored state would have looked
wrong the moment a user reopened the Humanize page or the plugin window.

**Verified with a new, real test, not just "it compiles"**: a new
`StateRoundTripTest` console app
(`native/BridgeClientTest/Source/state_roundtrip_test.cpp`) compiles the
REAL `PluginProcessor.cpp`/`PluginEditor.cpp` (same pattern as the
existing `BridgeStressTest`), sets every real parameter and
`humanizeSettings` to distinctive non-default values on one processor
instance, saves its state, and restores that state onto a SECOND,
independently-constructed instance -- exactly the save-into-a-fresh-
instance path that was actually broken, not a same-instance round trip
that could pass even with the old placeholder still assigning nothing.
**14/14 checks passed**: all seven parameters and all five Humanize
fields restored correctly on the fresh instance, and (a deliberate
negative check) a THIRD, completely untouched instance still defaulted
Mix Enabled to false, ruling out any accidental cross-instance state
leakage from the fix itself.

**A side effect of investigating, also fixed**: `BridgeStressTest`
(existing, compiles the same real `PluginProcessor.cpp`/`PluginEditor.cpp`
pair) was missing the `juce_dsp`/libebur128 dependencies those files have
needed since the Mix and Master features were added earlier this
session -- meaning it would have failed to build the next time anyone
tried, independent of this bug. Fixed alongside `StateRoundTripTest`'s
own CMake setup, same dependency list.

**Verified**: Steinberg Validator still 47/47 after the fix.

**Not yet verified**: the actual REAPER render/save-reload re-run that
prompted this fix in the first place -- this was found and fixed based
on reading the code and an independent, rigorous standalone test, not
yet re-confirmed live in the REAPER session that originally surfaced it.
That's the natural next step once this build is deployed.

### Twenty-third: license-auditing a "Real Trained ML Model Stack" document, and a real (unwired) Python child-process runtime

The user supplied a document proposing roughly ten ML systems for the
GOD MODE blueprint's remaining pages (stem separation, transcription,
neural codecs, mastering research) and asked to "add them all." Audited
first, per the document's own stated need ("Audit the license of every
repository AND every individual model weight before shipping BeatShore
commercially") which it flagged but didn't actually do -- and per this
project's own established pattern of never building against an
unverified license.

**License audit results** (checked at the primary source, same rigor as
the earlier Demucs/Open-Unmix/RNNoise/DeepFilterNet audits):
- **Clear**: Descript Audio Codec (MIT, code and weights, no restriction
  found), Meta EnCodec (MIT, same), Google Magenta MT3 (Apache-2.0 code;
  checkpoint training-data provenance not deep-audited), Mido/libsndfile/
  FFmpeg (standard permissive/LGPL export tooling).
- **Blocked**: Adobe DeepAFx -- confirmed by reading its actual LICENSE
  file, the "Adobe Research License" restricts the entire repository to
  "noncommercial research purposes only," explicitly excluding "any...
  activity which results in commercial gain." Same category as Demucs's
  weights, just for the neural-mastering piece instead of stem
  separation.
- **Blocked or undetermined**: Demucs/HTDemucs and BS-RoFormer/MDXC
  checkpoints -- both already audited (see the earlier stem-separation
  section); this document's MDX/UVR-ecosystem recommendation
  (`Anjok07/ultimatevocalremovergui`, `nomadkaraoke/python-audio-
  separator`) is almost certainly the same problem or worse, since it
  wraps dozens of community-trained checkpoints from largely
  undocumented sources rather than one team's models.

Net result: three of the document's own flagship recommendations for its
two most-emphasized pages (Separate, and the neural half of Mixing/
Mastering) are legally blocked or undetermined for anything beyond
personal/research use -- not a small caveat on an otherwise-green-lit
plan. Reported to the user plainly rather than building against it; user
chose, given that, to scope the shared child-process/IPC prerequisite
architecture rather than commit to a specific model.

**What's real**: `ChildProcessEngine.h` (new, `native/BeatShoreDesktop/
Source`) generalizes `NodeEngine.h`'s own mechanism -- which turned out
to already be process-agnostic in everything but name, `start()` just
took an exe path and a script path -- into a genuine, reusable base:
spawn any child process, expose its stdin/stdout as the same deadline-
bound, cancellable line interface (`OverlappedPipeIO`) Node's own
integration already proved in production (real cancellation, real
crash/respawn handling in `main.cpp`). `NodeEngine.h` is now a thin
wrapper over it (`start(nodeExe, scriptPath)` delegates to
`ChildProcessEngine::start(nodeExe, {scriptPath}, "Node")`) -- unchanged
public API, unchanged behavior, zero edits needed to `main.cpp` or the
existing `NodeEngineTest`. `PythonEngine.h` is the new counterpart for a
Python child (`-u` flag forced, see below for why).

**A real bug found and fixed while generalizing, not invented for the
occasion**: the original pipe names
(`\\.\pipe\BeatShoreNodeStdin.<pid>`/`...Stdout.<pid>`) were unique per
PROCESS, not per ENGINE INSTANCE. `main.cpp`'s own `runNodeWorker()`
already creates one `NodeEngine` per worker thread, all inside the same
process sharing the same PID -- a second concurrent worker's
`CreateNamedPipeA` (which uses `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a name
collision fails outright rather than silently sharing the pipe) would
have failed to start at all. Latent today if only one Node worker is
actually configured; would become live and immediately visible the
moment a second Node worker or a Python engine ran alongside it. Fixed
by keying pipe names on PID + a short tag + a process-wide atomic
instance counter, guaranteeing uniqueness regardless of how many engines
end up running concurrently -- verified directly (see below), not just
reasoned about.

**A real, non-obvious platform detail, confirmed by actually hitting
it**: Python fully buffers stdout whenever it isn't attached to a real
terminal -- true of every redirected pipe. Left alone, a script's
`print()` output would sit in an internal buffer and never reach this
process's pipe read until the buffer filled or the script exited,
silently breaking real-time line IPC (every read would time out, not
error -- easily misread as "the mechanism doesn't work for Python" when
the actual cause is Python's own I/O policy). Fixed with the standard
`-u` flag, baked into `PythonEngine::start()` itself, not left for a
future caller to remember.

**Verified two ways, both real, neither claiming more than they proved**:
1. `NodeEngineTest` (existing, unmodified) rebuilt clean against the
   refactored `NodeEngine.h` and still passes both its scenarios --
   confirms the generalization is a genuine behavior-preserving
   refactor, not a rewrite that happens to compile.
2. A new `PythonEngineTest`
   (`native/BridgeClientTest/Source/python_engine_test.cpp`) drives a
   REAL `python.exe` (not a mock) through a minimal real proof-of-concept
   script (`native/BeatShoreDesktop/python_engine/echo_engine.py` --
   READY handshake, PING/PONG, SHUTDOWN, deliberately matching the same
   READY convention `main.cpp` already validates for Node). **16/16
   checks passed**, including two specifically designed to catch the two
   real problems found above: a full READY-then-PING/PONG round trip
   (would time out if `-u` weren't forced) and two concurrent engine
   instances in the same process, each getting back its OWN payload, not
   the other's (would have failed to even start the second instance
   under the old per-PID-only pipe naming).

**Explicitly NOT done, on purpose**: no model is wired into
`main.cpp`'s actual request routing or `AnalysisScheduler`'s job types.
This is prerequisite plumbing, chosen and scoped as such -- picking a
specific model (from the clear list: DAC, EnCodec, or MT3) and actually
integrating it is a separate, later decision, not started here.
`BeatShoreDesktop.exe` itself was not rebuilt with this refactor this
session (`NodeEngine`'s public behavior is unchanged either way, so
there's no functional urgency) since it was actively running, connected
to a live REAPER session, when this work finished -- rebuilding it would
have dropped that connection mid-session.

### Twenty-fourth: DAC and EnCodec genuinely wired end-to-end; MT3 genuinely blocked, with a real, evidenced reason

The user explicitly asked to wire up all three of the "clear-licensed"
models from the Twenty-third section's audit (DAC, EnCodec, MT3) despite
the open questions flagged at the time (DAC/EnCodec had no defined
BeatShore feature yet; MT3 carried a real Windows/JAX packaging risk),
choosing to proceed with all three in parallel and accept that risk.
Did the real work rather than the safe version of this: actually
installed each dependency chain, actually downloaded and ran each
pretrained model, and only reported a model "wired" once a real C++ test
had driven it through the actual `PythonEngine.h` IPC mechanism -- not
once pip install exited 0.

**DAC and EnCodec: fully wired and verified.** Two new venvs
(`native/BeatShoreDesktop/python_engine/ml_env`) hold `torch` (CPU),
`descript-audio-codec`, and `encodec`, all pip-installed cleanly.
`encodec_engine.py` and `dac_engine.py` (new) load their real pretrained
weights once at startup, then serve `ENCODE_DECODE` requests over the
same NDJSON-per-line protocol `echo_engine.py` established (audio passed
by WAV file path, not inline in the JSON -- an inline sample array would
be enormous and slow to parse, same reasoning as the existing MIDI-
export convention). New `EncodecEngineTest`/`DacEngineTest`
(`native/BridgeClientTest/Source/codec_engine_test.cpp`) drive each
through the REAL `PythonEngine.h` mechanism: spawn the real venv's
python.exe, wait for the real model to finish loading, send a real
`ENCODE_DECODE` request against a real synthetic WAV, and check the real
output file that comes back. **9/9 checks pass for each** -- EnCodec
reconstructs a 440Hz tone through 8 real codebooks (mean error 0.016);
DAC through 9 real codebooks (mean error 0.030).

**A real, non-obvious protocol bug found and fixed while getting there,
not specific to either model**: `ChildProcessEngine.h` merges a child's
stderr into the same stream as its stdout (matching how `NodeEngine` has
always handled Node's own stderr). The very first end-to-end run failed
because PyTorch prints a real `FutureWarning` to stderr on first use --
that warning line landed exactly where the READY line was expected,
corrupting the handshake. Not a Node-specific concern before now because
Node rarely prints anything before its own READY line; any real Python
ML library commonly does. Fixed at the source in both engine scripts
(`warnings.filterwarnings("ignore")` before importing anything that
warns) rather than by trying to make the C++ side tolerate garbage
lines -- keeps the wire protocol itself clean, which is the property
every future engine script built on this pattern will need too, not
just these two.

**MT3: genuinely blocked, via the official path, for a real and
specific reason -- not "JAX doesn't work on Windows".** That framing
would have been wrong: JAX itself was verified directly on this machine
first (`pip install jax` on native Windows, then a real `jnp.sin`/`jnp.cos`
computation actually ran and produced a real result -- CPU-only, since
NVIDIA GPU JAX needs WSL2, but genuinely functional). The actual blocker
is two levels down: `t5x` (MT3's framework) depends on `seqio`, which
depends on `tensorflow-text` -- and `tensorflow-text` stopped publishing
Windows wheels after version 2.9.0 (2022), confirmed directly against
PyPI's own file listing for the package, not assumed from memory.
Current `t5x`/`seqio` need a `tensorflow-text` far newer than 2.9
provides, and 2.9-era `tensorflow-text` needs a `tensorflow-cpu`/
`numpy`/`jax` generation far older than current `t5x`'s other
dependencies tolerate -- a real, structural version conflict between
"the only tensorflow-text Windows build that exists" and "the
tensorflow-text version everything else in the chain actually needs",
not a single missing package one more retry would fix. (Along the way,
also found and fixed two smaller, real installation bugs: `airio`'s own
`setup.py` fails under Windows' default codepage reading a non-ASCII
file -- fixed with `PYTHONUTF8=1`, the same class of fix already applied
to this project's own CMake config for `libebur128`; and `tensorflow-cpu`
itself has no wheel for Python 3.14 specifically -- fixed with a
dedicated Python 3.12 venv, `native/BeatShoreDesktop/python_engine/
mt3_env`, since TensorFlow always trails the newest Python release.)

**A real, unvetted alternative exists, flagged rather than silently
adopted**: unofficial PyTorch reimplementations of MT3 (e.g.
`rlax59us/MT3-pytorch`) would sidestep the entire JAX/T5X/tensorflow-text
chain, consistent with the rest of this runtime (DAC and EnCodec are
already PyTorch-based). Not started: these are individual community
ports with unverified license terms and unverified pretrained-weight
provenance/quality (a working PyTorch port still needs real, correct
weights -- either converted from the original JAX checkpoint, which
would require running the official JAX path at least once for
conversion, circling back to the same blocker, or a separately-trained
PyTorch checkpoint of unknown quality). Building against one without
that vetting would repeat the exact mistake the Twenty-third section's
license audit was built to avoid -- reported as an open decision, not
picked unilaterally.

**Not yet done**: no engine is wired into `main.cpp`'s actual request
routing or `AnalysisScheduler`'s job types -- DAC and EnCodec are proven
to work end-to-end via their own dedicated test executables, not yet
reachable from a real BeatShore Desktop request or a plugin-side button.
That integration, and a decision on what DAC/EnCodec's actual BeatShore
feature is (still not defined -- see the Twenty-third section), remain
open next steps.

### First round (for reference)

- **No Inno Setup installed in this environment**, so the script has never
  been compiled. The script itself is real, reviewable engineering (real
  source paths, a real `dumpbin /dependents` check confirming
  `BeatShoreDesktop.exe` genuinely needs the VC++ x64 redistributable --
  `MSVCP140.dll`/`VCRUNTIME140.dll`/`VCRUNTIME140_1.dll` are dynamically
  linked, not assumed), but "the script is correct" and "it compiles and
  installs" are different claims, and only the first is verified.
- **No second, genuinely clean Windows machine available.** This exact
  environment has Visual Studio, CMake, JUCE, and npm installed -- which
  is precisely the set of things a real clean-machine test needs to *not*
  have. There is no way to honestly claim "tested on a clean machine" from
  inside the machine that built the thing being tested.

**Licensing, checked against the actual vendored files, not assumed from
general VST3/JUCE knowledge:**
- **VST3 SDK: resolved, not a blocker.** This project vendors SDK 3.8.1
  (`native/vst3sdk/CMakeLists.txt`), whose `LICENSE.txt` was read directly
  and is genuinely the MIT License (Steinberg relicensed VST3 to MIT
  starting at 3.8 -- the older GPLv3-or-paid-agreement requirement an
  earlier version of this document incorrectly warned about, based on
  outdated general knowledge rather than checking this project's actual
  vendored SDK version, does not apply here). What this genuinely requires:
  include the MIT copyright/license text in the installer (staged), and
  follow Steinberg's trademark rules if "VST" or the VST logo appear in
  public-facing material -- a naming/branding checklist item, not a
  distribution blocker.
- **JUCE licensing: resolved.** This project vendors JUCE 9.0.1
  (`native/JUCE/CMakeLists.txt`), dual-licensed AGPLv3 / commercial.
  JUCE's actual current terms (fetched directly, not assumed) put the
  free Starter tier's threshold at combined revenue + funding "up to
  $20,000" over the trailing 12 months. BeatShore is pre-revenue with no
  funding raised -- the Starter tier applies. Re-confirm if that changes,
  and give the full EULA (https://juce.com/legal/juce-9-licence/) a human
  read-through before shipping -- this was resolved from an AI summary of
  the terms, not a full manual legal read.

One real code change came out of writing the script, not just the script
itself: `main.cpp`'s Node engine launch used to hardcode `"node"`,
relying entirely on system PATH -- fine for this project's own dev
workflow, wrong for an installed app on a machine with no system Node.
`defaultNodeExe()` now prefers `<exe's own dir>\node\node.exe` (where the
installer stages a bundled copy) and only falls back to PATH if that
doesn't exist -- verified this doesn't regress the current dev workflow
(no bundled copy exists in this build tree, so it falls through to PATH
exactly as before; a fresh `tempo` round trip after the change still
produced the byte-identical `100.45` BPM result).

**Installer size: measured and reduced for real, not just estimated.**
`node_modules` was ~740MB in the dev tree, dominated by
`@tensorflow/tfjs-node`. A clean production copy was staged
(`npm ci --omit=dev` into a fresh directory, not a copy of the dev tree),
then investigated rather than trimmed on guesswork: `npm dedupe` /
`npm prune --omit=dev` barely moved the number (739MB → 738MB -- the bulk
isn't duplicate npm packages), but `tfjs-node/deps/` turned out to be a
genuine ~240MB *duplicate* of `tensorflow.dll` plus its `.lib` linker stub
(confirmed by inspecting `lib/napi-v8/`, which separately contains its own
complete, self-sufficient `tensorflow.dll` + `tfjs_binding.node` --
`deps/` is node-gyp's build-time staging area, never touched once the
native addon is already compiled), and every `*.map` source-map file
across `node_modules` (~190MB) is pure debug metadata, never loaded at
runtime. Removed both. **Real, measured result: 739MB → 299MB (~60%
reduction).**

This was not trusted on reasoning alone, per the explicit instruction to
verify against the exact staged directory: the full `BridgeClientTest` +
`MultiSessionTest` suites were run against the trimmed tree afterward,
including a real `transcribePolyphonic` round trip (91 notes,
`sha256:173a3d6c...`, byte-identical to every other run this project has
ever produced) and a full cancel-and-restart cycle (the desktop killing
and respawning `node.exe` against this exact staged `analyze.js`) -- both
passed cleanly.

**A second real bug found only by actually staging and testing, not by
inspection**: `analyze.js`'s unmodified `import ... from
'../../../beatshore-dsp.js'` (and `basic-pitch-model.js`'s equivalent
import of `vendor/basic-pitch-model/`) resolve relative to each file's own
location on disk -- which only lines up with where `beatshore-dsp.js`
actually lives at this project's *exact* dev-tree nesting depth. A naive
"just copy `engine/` into the installer" staging attempt failed
immediately (`Cannot find module '...\native\beatshore-dsp.js'`) once
actually run, not just reasoned about. Fixed by staging `analyze.js` at a
depth that mirrors the dev tree's own nesting inside the installer's
internal layout (an implementation detail the user never sees), with
`beatshore-dsp.js` and `vendor/` placed exactly three directories above it
-- not by editing `analyze.js`'s import, which would have broken the
"reuses `beatshore-dsp.js` unmodified" guarantee this project has
maintained throughout. The same fix applied to the model weights import.

What's left, in order: stage the remaining files the script expects (a
Release build's outputs, a chosen `node.exe`, the downloaded VC++
redistributable, assembled license texts -- the script's own trailing
comment block has the exact checklist, steps 4-6 of which are already done
and verified); install Inno Setup and compile it; test install, repair,
upgrade, and uninstall; then, and only then, is "tested on a clean
machine" an achievable next step -- and it needs a second machine this
environment doesn't have.

## 6. Superseded limitations

Claims made earlier in this document, in the order they appear above, that
later work made incorrect. Each is marked `SUPERSEDED` inline at its
original location; this table exists so a reviewer scanning quickly sees
the corrections in one place rather than needing to find each one:

| Original claim (where) | Current reality |
|---|---|
| "`CANCEL` is still an honest 'not yet supported' error" ("Traceability") | `CANCEL` genuinely works, including interrupting a request that's actually running, via a real Node-process kill-and-restart. See "Genuine cancellation and multiple plugin instances." Verified: ~100-150ms to resolve, with confirmed recovery. |
| "The desktop process still accepts exactly one plugin connection at a time... multiple simultaneous REAPER tracks... is unbuilt" ("Traceability") | Multiple simultaneous plugin instances are real and verified -- two real `BridgeClient` connections active at once, each correctly routed, no cross-contamination. See "Genuine cancellation and multiple plugin instances." |
| "Sending results back into the DAW as MIDI... is unbuilt" ("Traceability") | Real MIDI file export has existed since "Polyphonic transcription: now wired into the plugin, plus a real MIDI file" -- Standard MIDI Type 1, independently re-hashed and byte-inspected. |
| "only the `tempo` analysis kind has a UI trigger... `transcribePolyphonic`... not yet wired to a plugin UI control" ("Traceability") | A second UI trigger ("Transcribe Piano/Guitar") exists and is round-trip tested -- see "Polyphonic transcription: now wired into the plugin, plus a real MIDI file." |
| "today's desktop is single-threaded by design... There is no code path today where `CANCEL` can interrupt a request that's genuinely still running" ("Cancellation, timeouts, and stale-result rejection") | Replaced by the `PipeSessionOwner`/`JobQueue`/`NodeWorker` architecture -- see "Genuine cancellation and multiple plugin instances." The single-thread-per-pipe-handle constraint this paragraph was protecting is still honored; the architecture around it changed. |
| "the Steinberg VST3 SDK's license... requires either GPLv3 distribution or a separate Steinberg license agreement" (earlier "Clean-machine packaging" text) | Incorrect -- based on outdated general VST3 licensing knowledge, not this project's actual vendored SDK version. This project vendors SDK 3.8.1, genuinely MIT-licensed (confirmed by reading `native/vst3sdk/LICENSE.txt` directly). See the corrected "Clean-machine packaging" licensing subsection above. |
| Installer size estimated at "800MB-1GB+" (earlier "Clean-machine packaging" text) | Measured and reduced: 739MB → 299MB, verified against the real test suite. See "Clean-machine packaging" above. |
