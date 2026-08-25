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
  the actual staged tree before ever compiling. Its `-CleanEngine` path
  (full `npm ci` from scratch, the verified `tfjs-node` trim reapplied)
  has now actually been run and passed — the release doesn't secretly
  depend on stale `node_modules`.
- Real EULA content (not a placeholder) and a persistent, detailed
  self-test failure log with an incomplete-install marker.
- A real, brand-derived icon (not the generic Windows default), embedded
  in `BeatShoreDesktop.exe` (taskbar, tray, Explorer) and the installer,
  verified by extracting it back out of both compiled binaries.
- Under real version control (`git`) for the first time, tagged
  (`v0.2.0-rc1`, `v0.2.0-rc2`). A GitHub Actions release workflow exists
  (`.github/workflows/release.yml`) but is unverified — no live GitHub
  Actions runner has run it yet, and it has two labeled TODOs (fetching
  the vendored JUCE/VST3 SDK sources; making `build-release.ps1`'s
  hardcoded local tool paths CI-portable).
- Two additional failure modes verified empirically against the real
  staged desktop: a corrupted Basic Pitch model file and a genuinely
  unwritable MIDI export directory both produce an isolated, correctly-
  attributed self-test failure (exit 1) — not a crash, not a false pass
  on the unrelated checks.

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
- **The 512MB audio-memory budget and 24-job global queue depth have not
  been tested under genuine sustained concurrent load** — both require
  real, simultaneously-queued jobs from multiple real `BridgeClient`
  sessions (not just malformed/fake requests, which the existing
  hardening tests already cover), not yet run.
- **The CI workflow is unverified** — written, never run on a real
  GitHub Actions runner; has two labeled TODOs (see above).

## Exact artifact version and hashes

Current build, produced by `native/installer/build-release.ps1 -CleanEngine`,
full detail (including per-file staging hashes and the machine-readable
report) in `RELEASE_MANIFEST.md`:

| Field | Value |
|---|---|
| Version | 0.2.0 |
| Build ID | 20260824.1 |
| Source commit | `8e2fbf7` (tag `v0.2.0-rc2`) |
| Installer filename | `BeatShoreSetup-0.2.0.exe` |
| Installer SHA-256 | `d10064b24588ce65bbddb536015238dcc21d66817559729622f00f2602afc335` |
| Installer size | 98,838,285 bytes (~94.3MB) |
| `BeatShoreDesktop.exe` SHA-256 | `6da67873ef53af7efc05efe480926ab2a0cddead4fcb1f0f72a9476f9a8a7691` |
| `BeatShore Bridge.vst3` SHA-256 | `28ca81e6efc1804044cd9d5c1572768c56052d3488f6f1098c5cae665ae153f7` |
| Code-signed | No |

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
5. Only one Node worker — see "Deferred decisions" below.
6. Memory budget and queue-depth limits untested under genuine sustained
   concurrent load (see "Current limitations").
7. CI workflow unverified (no real GitHub Actions run yet).

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
- [x] Real icon (tray, exe, installer, Add/Remove Programs) — done, see
      "Current verified capabilities" above
- [ ] Live polyphonic transcription verified in REAPER
      (`audioSource:"live-captured"`, MIDI import onto a new track,
      timing/chords/velocities checked)
- [ ] REAPER lifecycle verified (save/reload, remove/reinsert, desktop
      restart mid-session, multiple instances, MIDI-learned trigger,
      playback/passthrough under load)
- [ ] Failure/recovery behavior tested on an installed build (desktop
      unavailable, Node crash, full disk, oversized/malformed messages,
      long-running cancellation, DAW closes mid-analysis, memory budget
      and queue depth under sustained concurrent load — missing model
      files and unwritable MIDI destination already tested, see
      "Current verified capabilities")
- [ ] Placeholder publisher/support/product URLs and copyright replaced
      with real values
- [ ] Other Windows DAWs tested (Cubase, Ableton Live, FL Studio, Studio
      One)
- [ ] CI workflow verified on a real GitHub Actions runner (currently
      written but untested; two labeled TODOs remain)

Until the starred items are done, this is a **controlled Windows beta
candidate**, not a public release.
