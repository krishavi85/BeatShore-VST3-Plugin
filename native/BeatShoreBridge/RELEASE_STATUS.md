# BeatShore — release status (short form)

This is the short, current-state-only summary. For the full engineering
narrative — what was built, what broke, how each fix was found and
verified — see `STATUS.md` in this same directory. That file is a
chronological log, not a status report; this one is the status report.
Last updated 2026-08-25.

## Current verified capabilities

- Native VST3 plugin, hosted in REAPER, passes the Steinberg Validator
  47/47.
- Full protocol round trip (plugin ↔ desktop broker ↔ Node/tfjs-node
  engine) verified for `tempo` and `transcribePolyphonic`, including a
  real MIDI file export with an independently-verified SHA-256.
- Live-captured REAPER round trip verified for `tempo` (real audio →
  capture → analysis → result in the plugin's own editor). Polyphonic
  transcription is verified through the full protocol via test harnesses
  but **not yet observed live inside REAPER** — see "Known issues" below.
- Genuine cancellation (a `CANCEL` for a request that's actually running
  kills and restarts the desktop's Node engine, ~100–150ms, with verified
  recovery), multiple simultaneous plugin sessions, and a bounded
  graceful shutdown (dedicated `BROKER_SHUTTING_DOWN` protocol message,
  not a bare process kill).
- Named-pipe IPC hardened: current-user SID-based ACL, `requestId` never
  touches the filesystem, duration-relative audio limits with a checked-
  multiplication overflow guard, a system-wide in-flight-audio memory
  budget, connection throttling, and log redaction by default.
- Installer compiles clean (Inno Setup 6.7.3, zero warnings) via an
  automated, reproducible build script
  (`native/installer/build-release.ps1`) that rebuilds from source,
  unconditionally restages, and runs the full regression suite against
  the actual staged tree before ever compiling.
- Real EULA content (not a placeholder) and a persistent, detailed
  self-test failure log with an incomplete-install marker.

## Current limitations

- **Not installed on a clean machine.** No UAC-capable, dependency-free
  Windows VM is available in the environment this was built in. This is
  the single largest remaining release blocker.
- **Not code-signed.** No certificate available. `BeatShoreDesktop.exe`,
  the VST3, and the installer are all unsigned.
- **JUCE 9 terms and the BeatShore EULA have not had a human/legal
  review.** The EULA has real, filled-in content (not a placeholder) but
  says explicitly it hasn't been attorney-reviewed.
- **Placeholder publisher/support/product URLs and copyright line** in
  the installer script — real business decisions, not filled in here.
- **No custom icon** — the tray, executable, and installer all use the
  generic Windows default icon.
- **Only Analyze Tempo has been observed live inside REAPER.**
  `transcribePolyphonic`'s live-captured round trip, REAPER project
  save/reload, and reconnect-after-desktop-restart are all unobserved in
  a real hosted session (exercised only via automated test harnesses).
- **Only one Node worker** (`kMaxConcurrentNodeJobs = 1`) — sessions
  correctly share and queue behind it, but there's no genuine parallel
  inference throughput. See "Deferred decisions" below for why this
  isn't being changed yet.
- **Untested on other DAWs** (Cubase, Ableton Live, FL Studio, Studio
  One) and **no macOS/Logic Pro support** (would need a separate AU
  build and macOS desktop broker — a platform milestone, not a packaging
  task).
- **Named-pipe hardening is scoped to same-user, local threats.** No
  cross-user or network-attacker defenses (not this project's threat
  model), no persistent per-identity ban list, and the elevated-desktop/
  non-elevated-DAW mandatory-integrity-control scenario is unverified
  (standing guidance: don't run `BeatShoreDesktop.exe` elevated).

## Exact artifact version and hashes

Current build, produced by `native/installer/build-release.ps1`, full
detail (including per-file staging hashes and the machine-readable
report) in `RELEASE_MANIFEST.md`:

| Field | Value |
|---|---|
| Version | 0.2.0 |
| Build ID | 20260824.1 |
| Installer filename | `BeatShoreSetup-0.2.0.exe` |
| Installer SHA-256 | `c9a44cc1c0ae4235ac5056ab1ca5e020e9250e66b2cc79bd059fc4c71e6e702b` |
| Installer size | 98,587,684 bytes (~94.0MB) |
| `BeatShoreDesktop.exe` SHA-256 | `f667b19a9a189c3cdc6458a48ac2d2fd1be221760efdc19f68099dd439873c3f` |
| `BeatShore Bridge.vst3` SHA-256 | `28ca81e6efc1804044cd9d5c1572768c56052d3488f6f1098c5cae665ae153f7` |
| Code-signed | No |
| Source commit | N/A — not a git repository |

**Do not treat this table as long-lived** — regenerate it (and
`RELEASE_MANIFEST.md`) via `build-release.ps1` for every real build; the
hashes above describe one specific compile, not "BeatShore 0.2.0" in
general (Inno Setup's own output isn't byte-reproducible across separate
compiles of identical source — see `RELEASE_MANIFEST.md`).

## Supported hosts

| DAW | Status |
|---|---|
| Steinberg Validator | 47/47 |
| REAPER | Hosted IPC verified for Analyze Tempo; polyphonic transcription verified via test harness only, not yet observed live |
| Cubase | Untested |
| Ableton Live | Untested |
| FL Studio | Untested |
| Studio One | Untested |
| Logic Pro | Not supported (Windows-only build; would need a separate AU/macOS milestone) |

## Known issues

1. Live polyphonic transcription unobserved in REAPER (file-based path
   proven; live-captured path is not).
2. Clean-machine install (interactive, silent, repair, upgrade,
   uninstall, cancelled-install and failed-self-test behavior) unverified
   — needs a real VM.
3. Elevated-desktop / non-elevated-DAW pipe-connection scenario
   unverified — don't run `BeatShoreDesktop.exe` elevated.
4. No code signing — expect SmartScreen warnings on a real install.
5. Generic icon everywhere a real BeatShore icon should be.
6. Only one Node worker — see "Deferred decisions" below.

## Deferred decisions

- **Parallel ML worker pool**: not built. The broker already supports
  concurrent client *sessions*; only inference itself is serialized
  through one Node/tfjs-node worker. Acceptable for an initial release.
  Revisit only after measuring, on real usage: queue wait times, tfjs
  memory usage under concurrent load, CPU contention with the host DAW,
  and cancellation behavior with several simultaneous requests — not
  before, since building a worker pool without those numbers would be
  guessing at a cost/benefit tradeoff (each additional resident
  `tfjs-node` process costs ~200MB+ of native TensorFlow library) rather
  than deciding it.

## Release acceptance checklist

Gate the next public release on all of the following. None of the
starred items can be completed from an automated/CLI-only environment —
they need real hardware, a certificate, or a human.

- [ ] ★ Clean-machine install verified (interactive + silent, VC++
      runtime, self-test, VST3 discovery, tempo + polyphonic
      transcription, MIDI export/import, start-at-login, repair,
      same-version reinstall, upgrade, uninstall, cancelled-install and
      failed-self-test behavior)
- [ ] ★ Code-signed (`BeatShoreDesktop.exe`, VST3, installer;
      timestamped; SmartScreen behavior checked; manifest hashes
      regenerated after signing)
- [ ] ★ Human legal review (JUCE 9 Starter terms, BeatShore EULA,
      third-party notices, a privacy policy)
- [ ] ★ Real icon (tray, exe, installer, Add/Remove Programs)
- [ ] Live polyphonic transcription verified in REAPER
      (`audioSource:"live-captured"`, MIDI import onto a new track,
      timing/chords/velocities checked)
- [ ] REAPER lifecycle verified (save/reload, remove/reinsert, desktop
      restart mid-session, multiple instances, MIDI-learned trigger,
      playback/passthrough under load)
- [ ] Failure/recovery behavior tested on an installed build (desktop
      unavailable, Node crash, missing model files, unwritable MIDI
      destination, full disk, oversized/malformed messages, long-running
      cancellation, DAW closes mid-analysis)
- [ ] Placeholder publisher/support/product URLs and copyright replaced
      with real values
- [ ] Other Windows DAWs tested (Cubase, Ableton Live, FL Studio, Studio
      One)

Until the starred items are done, this is a **controlled Windows beta
candidate**, not a public release.
