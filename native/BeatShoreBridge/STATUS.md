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

### Second round: real blockers found by external review, fixed for real

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
