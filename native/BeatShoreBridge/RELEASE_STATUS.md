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
- **Real publisher and copyright** (Singh's Innovation & Advisory,
  matching the EULA's own licensor) in the installer, replacing
  placeholders — product website, support email/URL, and a
  privacy-policy URL are still placeholders (real, live addresses
  needed, not invented here). Fixing this surfaced and fixed a real,
  separate bug: the installer's own `ProductVersionRaw` had been stuck
  at `0.2.0.1` on every build regardless of the actual build ID, despite
  the source's own comment claiming otherwise — confirmed via
  `(Get-Item ...).VersionInfo.ProductVersionRaw` actually changing across
  two builds after the fix (`0.2.0.1` → `0.2.0.4`), not just compiling
  without error.
- Under real version control (`git`), pushed to
  [github.com/krishavi85/BeatShore-VST3-Plugin](https://github.com/krishavi85/BeatShore-VST3-Plugin)
  for the first time, tagged (`v0.2.0-rc1` through `v0.2.0-rc4`). Actions
  are now enabled and CI genuinely runs — but both real runs so far
  failed fast (~80-110s); root cause found and fixed (see "Current
  limitations" below) but not yet confirmed against a real fresh CI run.
- A real held-open-connection load test
  (`native/BridgeClientTest/Source/load_boundary_test.cpp`,
  `LoadBoundaryTest.exe`) genuinely triggered **two of the three**
  resource-limit rejections for the first time: `kMaxConcurrentSessions`
  (25 near-simultaneous connections → 17 connected / 8 rejected) and the
  512MB audio-memory budget (8 sessions × one 92MB request each →
  5 accepted / 3 rejected with `SERVER_BUSY`) — both matching the
  desktop's own log exactly. `kMaxGlobalQueueDepth=24` was **not**
  reached despite real, escalating effort (three audio durations, a
  barrier-synchronized burst) — see "Current limitations" below.
- Four failure modes verified empirically against the **installed**
  build (`native/installer/stage`'s own `BeatShoreDesktop.exe`, not the
  dev tree): a corrupted Basic Pitch model file, a genuinely missing
  model file, and an unwritable MIDI export directory all produce an
  isolated, correctly-attributed self-test failure (exit 1) — not a
  crash, not a false pass on the unrelated checks. A real Node crash
  mid-inference (`taskkill` on `node.exe` during a genuine
  `transcribePolyphonic` request) produces a structured client-side
  error, the desktop process itself stays alive, and a follow-up request
  correctly reports the worker unavailable rather than hanging (matching
  documented, deliberate behavior — no auto-respawn on an *unexpected*
  Node exit, only after a genuine cancel).
- Real concurrent load tested against the installed build: 40
  simultaneous real `BridgeClient` sessions (genuine shared-memory
  audio), spawned within 0.31s of each other, **40/40 succeeded**, zero
  failures, zero leaked temp files. This did **not** reach the literal
  16-concurrent-session or 24-job-queue-depth *rejection* boundaries —
  see "Current limitations" below for why and what would be needed to.

## Current limitations

- **Not installed on a clean machine.** No UAC-capable, dependency-free
  Windows VM is available in the environment this was built in. This is
  the single largest remaining release blocker.
- **Not code-signed.** No certificate available. `BeatShoreDesktop.exe`,
  the VST3, and the installer are all unsigned.
- **JUCE 9 terms and the BeatShore EULA have not had a human/legal
  review.** The EULA has real, filled-in content (not a placeholder) but
  says explicitly it hasn't been attorney-reviewed.
- **Placeholder product website, support email/URL, and privacy-policy
  URL** in the installer script — publisher name and copyright are now
  real (see above); these three still need genuinely live addresses,
  not invented here.
- **Only Analyze Tempo has been observed live inside REAPER.**
  `transcribePolyphonic`'s live-captured round trip, REAPER project
  save/reload, and reconnect-after-desktop-restart are all unobserved in
  a real hosted session (exercised only via automated test harnesses).
- **Only one Node worker** (`kMaxConcurrentNodeJobs = 1`) — sessions
  correctly share and queue behind it, but there's no genuine parallel
  inference throughput. See "Deferred decisions" below for why this
  isn't being changed yet.
- **Untested on other Windows DAWs** (Cubase, Ableton Live, FL Studio,
  Studio One). macOS/Logic Pro is intentionally out of scope for this
  product — a separate platform milestone (its own AU build, macOS
  desktop broker, Apple signing/notarization), not a gap in the Windows
  release this document tracks. Not listed as a limitation below for
  that reason.
- **Named-pipe hardening is scoped to same-user, local threats.** No
  cross-user or network-attacker defenses (not this project's threat
  model), no persistent per-identity ban list, and the elevated-desktop/
  non-elevated-DAW mandatory-integrity-control scenario is unverified
  (standing guidance: don't run `BeatShoreDesktop.exe` elevated).
- **`kMaxConcurrentSessions=16` and the 512MB audio-memory budget are now
  both genuinely verified. `kMaxGlobalQueueDepth=24` still is not.**
  The memory budget: 8 real concurrent sessions each submitting a
  single request at the actual per-request maximum size (192kHz stereo,
  60s, ~92MB) produced `accepted=5 SERVER_BUSY=3` — 5 × 92MB ≈ 460MB fit
  under the 512MB ceiling, the 6th correctly tipped it over, matching
  the desktop's own log exactly (3 real `SERVER_BUSY` rejections). Queue
  depth: despite real, escalating attempts with the same purpose-built
  held-open-connection test client (`LoadBoundaryTest.exe`) — three
  audio durations (0.2s/10s/60s) and both a naive and a
  barrier-synchronized simultaneous burst, up to 32 real concurrent
  requests (the maximum `kMaxConcurrentSessions × kMaxActiveJobsPerSession`
  allows) — every attempt was accepted, zero `QUEUE_FULL`. Working
  hypothesis, not confirmed: `kMaxConcurrentSessions` combined with each
  session's own strictly sequential per-connection message processing
  may genuinely bound real-world queue depth below 24 regardless of
  load pattern. The rejection logic itself reads correctly and is
  structurally identical to the two now-verified checks; confirming it
  fires under real conditions would need instrumenting the desktop's
  own live queue depth, not attempted since that means modifying
  production code purely to observe a test.
- **A genuine, minor gap found along the way, not previously
  confirmed**: temp audio files from sessions killed mid-request
  (`taskkill`, matching how this project's own testing has repeatedly
  terminated the desktop) are not cleaned up — found ~95 orphaned
  `bsr_*.bsmraw` files in the OS temp directory dating back to earlier
  test runs, none from normal completion. Low severity (the OS's own
  temp-directory housekeeping eventually reclaims this), but real: there
  is no "clean up orphaned temp files on startup" sweep today.
- ~~CI now runs but every run so far has failed~~ **Resolved and
  confirmed**: `v0.2.0-rc7` (commit `817b4bf`) passed cleanly on a real
  GitHub-hosted `windows-latest` runner (run `33168764375`) — full
  build, Validator, staged self-test with real Basic Pitch inference,
  full regression suite, clean Inno Setup compile, installer artifact
  uploaded. Two real bugs (a hardcoded Node path in `-CleanEngine`, and
  `stage\` never being created before the EULA copy) were found via an
  actual from-scratch local repro and fixed; see STATUS.md's
  "Fourteenth" section for the full account.

## Exact artifact version and hashes

Current build, produced by `native/installer/build-release.ps1`, full
detail (including per-file staging hashes and the machine-readable
report) in `RELEASE_MANIFEST.md`:

| Field | Value |
|---|---|
| Version | 0.2.0 |
| Build ID | 20260825.1 (ProductVersion 0.2.0.4) |
| Source commit | `bc62426` (untagged -- the "stage everything, not just binaries" fix on `main`, after `v0.2.0-rc4`) |
| Installer filename | `BeatShoreSetup-0.2.0.exe` |
| Installer SHA-256 | `5b260b55ed8e26ae8dce97be6a2f3e370740e67f77e31f19150ee39bc591f1e6` |
| Installer size | 98,836,284 bytes (~94.3MB) |
| `BeatShoreDesktop.exe` SHA-256 | `6da67873ef53af7efc05efe480926ab2a0cddead4fcb1f0f72a9476f9a8a7691` |
| `BeatShore Bridge.vst3` SHA-256 | `28ca81e6efc1804044cd9d5c1572768c56052d3488f6f1098c5cae665ae153f7` |
| Code-signed | No |

**Do not treat this table as long-lived** — regenerate it (and
`RELEASE_MANIFEST.md`) via `build-release.ps1` for every real build; the
hashes above describe one specific compile, not "BeatShore 0.2.0" in
general (Inno Setup's own output isn't byte-reproducible across separate
compiles of identical source — see `RELEASE_MANIFEST.md`).

## Supported hosts

Windows VST3 only — this is the current product's actual scope, not a
limitation of it. macOS/Logic Pro is a separate platform milestone, not
tracked in this table.

| DAW | Status |
|---|---|
| Steinberg Validator | 47/47 |
| REAPER | Hosted IPC verified for Analyze Tempo; polyphonic transcription verified via test harness only, not yet observed live |
| Cubase | Untested |
| Ableton Live | Untested |
| FL Studio | Untested |
| Studio One | Untested |

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
6. Job-queue-depth *rejection* (`kMaxGlobalQueueDepth=24`) still not
   reached under real load despite real, escalating attempts — see
   "Current limitations" (`kMaxConcurrentSessions` and the 512MB memory
   budget **are** now both verified).
7. ~~CI has never passed~~ Fixed and confirmed: `v0.2.0-rc7` (commit
   `817b4bf`) is the first genuinely passing run on a real GitHub-hosted
   `windows-latest` runner — full build, Validator 51/51, staged
   self-test (real Basic Pitch inference), full regression suite, and a
   clean Inno Setup compile, all green. Two real bugs found via an
   actual from-scratch local repro and fixed: `-CleanEngine`'s `npm ci`
   hardcoded a path nothing ever populated, and `stage\` itself was
   never created before the EULA file was copied into it. See
   STATUS.md's "Fourteenth" section for the full writeup.
8. Temp files from a session killed mid-request aren't cleaned up on
   the next startup (low severity, but real — see "Current
   limitations").

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
      third-party notices, a privacy policy) — review packet assembled
      at `legal/LEGAL_REVIEW_PACKET.md`, privacy policy drafted at
      `legal/PRIVACY_POLICY_DRAFT.md`, both awaiting actual counsel
- [x] Real icon (tray, exe, installer, Add/Remove Programs) — done, see
      "Current verified capabilities" above
- [ ] Live polyphonic transcription verified in REAPER
      (`audioSource:"live-captured"`, MIDI import onto a new track,
      timing/chords/velocities checked)
- [ ] REAPER lifecycle verified (save/reload, remove/reinsert, desktop
      restart mid-session, multiple instances, MIDI-learned trigger,
      playback/passthrough under load) — step-by-step script ready at
      `REAPER_TEST_CHECKLIST.md`, not yet run
- [ ] Remaining failure/recovery behavior on an installed build: full
      disk (not safely simulable here), long-running cancellation during
      an actual DAW session, DAW closes mid-analysis, orphaned-temp-file
      cleanup on startup after a killed session, and the
      `kMaxGlobalQueueDepth` *rejection* boundary under real sustained
      load (needs a test client that can hold genuine queue depth —
      see "Current limitations"; the session-cap and 512MB memory-budget
      boundaries **are** now verified). Desktop-unavailable, Node-crash,
      oversized/malformed messages, missing/corrupted model files, and
      unwritable MIDI destination are already tested — see "Current
      verified capabilities"
- [x] Publisher name and copyright replaced with real values — done
- [ ] Product website, support email/URL, and privacy-policy URL
      replaced with real, live addresses
- [ ] Other Windows DAWs tested (Cubase, Ableton Live, FL Studio, Studio
      One)
- [x] CI workflow verified on a real GitHub Actions runner — `v0.2.0-rc7`
      (commit `817b4bf`) passed cleanly on `windows-latest`, run
      `33168764375`

Until the starred items are done, this is a **controlled Windows beta
candidate**, not a public release.
