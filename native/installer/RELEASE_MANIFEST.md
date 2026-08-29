# BeatShore release manifest

**Regenerated again 2026-08-29.** This table had gone genuinely stale:
it still described commit `bc62426` (2026-08-26) while `main` had moved
to `817b4bf` (the fresh-checkout CI fix, confirmed passing on a real
GitHub-hosted runner as `v0.2.0-rc7`) and then to real new work this
same day -- the orphaned-temp-file cleanup fix (STATUS.md's
"Fifteenth"). **Do not distribute `BeatShoreSetup-0.2.0.exe` with the
hash `5b260b55ed8e...` (the previous table's installer hash) -- it was
built from `bc62426`, three real commits and one real code change
behind current `main`.** The hashes below are from a fresh rebuild at
commit `fad817c` (full: `fad817c6b1cdce33c76ddeb8fef33175a1d10391`),
build ID `20260829.1` (`ProductVersion 0.2.0.5`), verified end to end:
Validator 47/47 against the rebuilt `BeatShore Bridge.vst3`, staged
self-test (real Basic Pitch inference) PASS, full regression suite
(`tempo`, `transcribePolyphonic`, `MultiSessionTest`) PASS, Inno Setup
6.7.3 compile clean (0 warnings, 97.7s). `BeatShoreDesktop.exe` itself
changed (303,616 bytes, up from 300,032 -- the real new
`sweepOrphanedTempFiles()` code); `BeatShore Bridge.vst3` is
byte-identical to the previous build, since nothing in its own source
changed. Two staged engine files came back with different hashes than
the last recorded values despite no source changes: `package.json`
(611 bytes now vs. a stale "348 bytes" the previous table recorded --
confirmed by direct comparison that the working tree and `git show
HEAD:...` are byte-identical at 611 bytes, so the *previous* manifest
entry was simply outdated documentation, not a real discrepancy) and
`tfjs_binding.node` (the native tfjs-node addon, whose exact bytes
aren't guaranteed identical across separate `npm ci` runs the way
source-controlled JS files are -- functionally verified working via the
real Basic Pitch inference in this build's own self-test and regression
suite, not just hashed and assumed). See "How these were verified"
below for the full account, and STATUS.md's "Fifteenth" section for the
fix this build actually ships.

**Superseded, do not use**: the `bc62426`-era hashes throughout the rest
of this file below this notice, including `5b260b55ed8e...`
(installer), `6da67873ef53af...` (the pre-fix `BeatShoreDesktop.exe`),
and `ff52a75129088...` (the old staging-manifest hash). `BeatShore
Bridge.vst3`'s hash (`28ca81e6efc180...`) is the one value from that
table that's still current, since the VST3's own source hasn't changed.

## Current build (commit `fad817c`, build `20260829.1`)

| File | SHA-256 | Size |
|---|---|---|
| `BeatShoreDesktop.exe` | `85bc818790ac8a7ec7ce55db67c703cace040af3cf6c560419cce279dddf6357` | 303,616 bytes |
| `BeatShore Bridge.vst3` (unchanged from the `bc62426` build) | `28ca81e6efc1804044cd9d5c1572768c56052d3488f6f1098c5cae665ae153f7` | 3,966,464 bytes |
| `BeatShoreSetup-0.2.0.exe` (installer, `ProductVersion 0.2.0.5`) | `0994890df1a74a1aea45f694de5521050506a1f03942ab0b99297bd40927ac6c` | 98,844,794 bytes |
| Staging-manifest hash (see the `bc62426`-era note below for the exact algorithm) | `f132dc251e2b9f474677b91cf2cf34cefc4a8d6de45673183409790ecc90f072` | staged file count: 8,481 |

Not code-signed (no certificate available in this environment).
**CI-confirmed**: tagged and pushed as `v0.2.0-rc8` (commit `a462104`,
one commit ahead of `fad817c` above with only this manifest/doc
regeneration itself, no further binary changes) — run `33272954916`
completed with `conclusion: success` on a real GitHub-hosted
`windows-latest` runner, "Run release build" finishing in 13m27s.

---

**Regenerated again 2026-08-25** via `build-release.ps1 -CleanEngine` --
the first real run of the clean-engine path (full `npm ci` from scratch,
the verified `tfjs-node` trim reapplied, complete regression suite,
confirms the release doesn't secretly depend on stale `node_modules`),
plus a real, brand-derived icon now embedded in both
`BeatShoreDesktop.exe` and the installer (previously the generic Windows
default). **The hashes in this file now describe this build; the
`c9a44cc1...`/`0d4e8b28...`/`90ff09b0...` installer hashes and the
`f667b19a...` desktop-exe hash from the paragraph below are all
superseded.** This project is also, as of this same update, a real git
repository for the first time -- see "Source commit" in the table below.
See STATUS.md's "Eighth: source control, CI, and the `-CleanEngine`
verification" for the full account.

Generated 2026-08-24 for a `0.2.0` staging build, **regenerated twice the
same day**: once after the Start-at-login path-resolution fix, the first
round of named-pipe security hardening, graceful tray shutdown, and a
real (non-placeholder) EULA (STATUS.md's "Fifth..." section); and again,
from a fully restaged tree with a real build ID assigned, after a second
hardening pass tightened resource limits further (duration-relative
frame limits, a system-wide memory budget, a stricter pipe line cap,
connection throttling, log redaction, requestId no longer touching the
filesystem, a SID-based pipe ACL, and a dedicated `BROKER_SHUTTING_DOWN`
protocol message) -- see STATUS.md's "Sixth: resource-limit
tightening..." section for the full account of what changed and how it
was verified. **The hashes in this file describe the current,
second-round build (build ID `20260824.1`) -- do not distribute the
earlier build's hash under this same filename; they are different
binaries.** This records the exact artifacts staged for, and compiled
by, `BeatShoreSetup.iss` in this session -- **not** a signed, published
release. The installer this manifest describes **has** been compiled
(Inno Setup 6.7.3, zero warnings), self-tested and regression-tested
against the actual staged tree (not just the dev build directory -- see
"How these were verified" below), but **not** installed on a clean
machine (this environment's own dev tools make that impossible from
here -- see STATUS.md) and **not** code-signed. Regenerate this file for
every real release, including a fresh build ID; do not reuse these
hashes for a different build, and regenerate them again after signing --
signing alters the binary and therefore its hash.

## Tool versions

| Component | Version | Notes |
|---|---|---|
| Node.js | v24.19.0 | Active LTS ("Krypton") as of this writing. Node v25.x was used in an earlier pass and correctly identified as already end-of-life (Node's own release schedule: v25 EOL 2026-03-31) -- do not regress to it. |
| npm | bundled with Node v24.19.0 | Used for `npm ci --omit=dev` against a completely clean `native/BeatShoreDesktop/engine/` checkout. |
| `@tensorflow/tfjs-node` | 4.22.0 | Per `native/BeatShoreDesktop/engine/package.json`. Native binding rebuilt fresh against Node 24's N-API version as part of `npm ci` + the existing `fix-tfjs-node-binding.js` postinstall repair (still required -- tfjs-node 4.22.0's own napi-v10 prebuilt is incomplete; the postinstall script copies `tensorflow.dll` into the working `napi-v8` binding directory). |
| VST3 SDK | 3.8.1 | `native/vst3sdk/CMakeLists.txt`. Genuinely MIT-licensed (confirmed by reading `LICENSE.txt` directly). |
| JUCE | 9.0.1 | `native/JUCE/CMakeLists.txt`. Dual AGPLv3/commercial -- **resolved**: free Starter tier applies (BeatShore is pre-revenue, no funding raised, under JUCE's $20,000 combined revenue+funding threshold). See STATUS.md. |

## Staged artifact hashes (SHA-256)

| File | SHA-256 | Size |
|---|---|---|
| `BeatShoreDesktop.exe` (Start-at-login fix, resource-limit tightening, requestId no longer touching the filesystem, SID-based pipe ACL, connection throttling, log redaction, graceful shutdown with a dedicated `BROKER_SHUTTING_DOWN` message, **now with a real embedded icon**) | `6da67873ef53af7efc05efe480926ab2a0cddead4fcb1f0f72a9476f9a8a7691` | 300,032 bytes |
| `BeatShore Bridge.vst3` (binary inside bundle: `Contents/x86_64-win/BeatShore Bridge.vst3`, rebuilt against the updated `BridgeClient.h`/`PluginEditor.cpp` — reacts to the new `BROKER_SHUTTING_DOWN` message) | `28ca81e6efc1804044cd9d5c1572768c56052d3488f6f1098c5cae665ae153f7` | 3,966,464 bytes |
| `node/node.exe` (v24.19.0) | `3602f2bb1a10f2cbab4c36886218a33c1ab3db87290e73b033c46c77147d0237` | 92,825,416 bytes |
| `engine/native/BeatShoreDesktop/engine/analyze.js` | `ecdfe755d6622ce53e8cf9d99a04f9a7735da984e3954178361117d3e278f5c6` | -- |
| `engine/beatshore-dsp.js` | `fc85359db2658ad8ae1d759bf334fa45b588402dc8918253ac9d8b3deb5da978` | -- |
| `engine/package.json` (fixes Node module-type ambiguity for `beatshore-dsp.js`) | `3ad5858ae00186f2d586da16eb275349d4dc396d92a8fbc871e7b68004139cfc` | 348 bytes |
| `tfjs_binding.node` (native addon, `lib/napi-v8/`) | `4a022a9d5ba8da2dbad0dc68452734a7d251bc0f8a542a19ed5232d3ef15c066` | -- |
| `tensorflow.dll` (`lib/napi-v8/`) | `2a75bddf21f04f1e01dc6433609c405987f432473d4ebcc4c17bc96d63c393dc` | -- |
| `vendor/basic-pitch-model/model.json` | `1ed1aaee3409ec1dc098c8b01f430c0911f6fe9412e7af8086750f9e8f302f68` | -- |
| `vendor/basic-pitch-model/group1-shard1of1.bin` (model weights) | `b142a95737a52e1e412d5f92e73d8bb80dfe8d04941acc0702f11f4524fb377c` | -- |
| `LicenseFile.txt` (real EULA -- licensor Singh's Innovation & Advisory, governing law Suriname; no longer placeholder text) | `80ecead1825afcf73f48353b904e0173ebd5b4a8d6117f2a254db85f45ae2c8d` | 5,041 bytes |

## License file hashes (for provenance -- confirms which exact license text was staged)

| File | SHA-256 |
|---|---|
| `native/vst3sdk/LICENSE.txt` (MIT) | `b99e03d84202686308ba0c6556fcdff70885897cab8a87c02e75e4964a29aa76` |
| `native/JUCE/LICENSE.md` | `dc86c8d10fad61e4f16b026a2bab258187c4f1b20997998ec56ae912939b6b5a` |
| `vendor/basic-pitch-model/LICENSE-basic-pitch-apache-2.0.txt` | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` |

## Bundled redistributable and the compiled installer

| File | SHA-256 | Size | Notes |
|---|---|---|---|
| `vc_redist.x64.exe` | (see below) | 25,635,768 bytes | Downloaded directly from `https://aka.ms/vs/17/release/vc_redist.x64.exe`, version 14.44.35211.0. Authenticode signature verified via `Get-AuthenticodeSignature` -- `Status: Valid`, signed by Microsoft Corporation, thumbprint `8F985BE8FD256085C90A95D3C74580511A1DB975`. A stale cached copy should never be reused across releases without re-downloading and re-verifying. |

(`vc_redist.x64.exe`'s own SHA-256 wasn't separately recorded before it
was consumed by the compile above -- re-download and hash it fresh for
the next real release rather than trusting this note.)

### The compiled installer artifact

| Field | Value |
|---|---|
| Filename | `BeatShoreSetup-0.2.0.exe` |
| Version | `0.2.0` (`AppVersion`/`MyAppVersion` -- the release this build belongs to) |
| Build ID | `20260825.1` (`MyBuildId` in `BeatShoreSetup.iss`) — `VersionInfoTextVersion="build 20260825.1"`. **A real bug in this same field was found and fixed this build**: `VersionInfoVersion`'s 4th numeric component was hardcoded to a literal `.1` regardless of `MyBuildId`'s actual value, despite this file's own then-comment claiming "the build-id sequence number becomes the 4th component" — every prior compile showed an identical `ProductVersionRaw` (`0.2.0.1`) no matter how many times `MyBuildId` was bumped, silently failing to be the discriminator the comment claimed. Fixed with a new, separate `MyBuildNumber` define (`"4"` for this build — Windows version-resource components are 16-bit, so `MyBuildId`'s own `YYYYMMDD`-based text can't be used directly). Confirmed working, not just compiling without error: `(Get-Item ...).VersionInfo.ProductVersionRaw` now reads `0.2.0.4`. |
| Size | 98,836,284 bytes (~94.3MB) |
| SHA-256 | `5b260b55ed8e26ae8dce97be6a2f3e370740e67f77e31f19150ee39bc591f1e6` |
| Compilation time | 2026-08-26T04:27:05Z (Inno Setup 6.7.3, zero warnings) |
| Source commit | `bc62426` -- the "stage everything, not just binaries" fix (see STATUS.md's "Twelfth" section): `stage\engine\` is now actually populated by the script itself (previously never staged by any automated step at all), the EULA/license notices come from a real git-tracked `native/installer/assets/` directory, and the bundled Node.js runtime / VC++ redistributable are fetched automatically when absent. Supersedes `b78d31e`/`v0.2.0-rc4`. Verify with `git log --oneline --decorate` or `git show bc62426 --stat`. |
| Staging-manifest hash | `ff52a7512908844213e0813dbdadf092ba5da82f00b4384af29958cf1d3855db` -- SHA-256 over every staged file's own SHA-256, sorted by path relative to `stage\`, joined `"<sha256>  <relpath>"` one per line (forward slashes, UTF-8, `\n`-joined) and hashed once more into this single value -- the exact algorithm is `build-release.ps1`'s own staging-manifest step, so this number is reproducible by that specific, documented procedure, not "however sha256sum happens to format its output" (see the prior build's note in this file's history for why a naive `sha256sum`/`sort` pipeline produces a *different* aggregate value over the *same* underlying files). The full per-file listing is saved alongside this manifest at `native/installer/staging-file-hashes.txt`. Staged file count: 8,481 (up from 8,479 -- this build's `stage\engine\` was populated by the script's own new staging step for the first time, rather than an old manually-assembled copy, so a small file-count difference here is expected, not a regression). |

**Not code-signed** -- no signing certificate available in this
environment; hashes above are of an **unsigned** binary. Per the standing
project note: hashes must be regenerated after signing, since signing
alters the binary and therefore its hash.

**Produced by `native/installer/build-release.ps1`**, this project's new
automated release script (see STATUS.md's "Automated release build"
section) -- not a manual build-copy-compile sequence. The script rebuilds
both binaries from source, unconditionally overwrites the staged copies
(never trusting a previous restage), validates the staged directory
layout, self-tests and regression-tests against the staged tree
specifically, and only then compiles the installer -- refusing to
proceed (non-zero exit, nothing compiled) if any step fails. Its full
structured output for this exact run is saved at
`native/installer/release-report.json`.

**Note on reproducibility**: this exact installer was compiled twice from
byte-identical inputs during this session (once manually, once via the
script above) and produced two *different* SHA-256 hashes each time
(`0d4e8b2807f929a3f705ce103699ff431964ec44f4049db194354d263ea33ed6` for
the manual compile, superseded by the value in the table above) — Inno
Setup embeds build-time metadata in its compiled output that isn't
purely a function of the `.iss` script and staged files, so "recompile
from the same source" is not expected to reproduce the same installer
hash bit-for-bit. The two staged binaries inside it
(`BeatShoreDesktop.exe`, the VST3) ARE expected to be, and were, byte-
identical across both compiles — see their own hashes below, unchanged
from the prior table. Treat the installer's own SHA-256 as identifying
*this specific compiled file*, not as a checksum that a future rebuild
from unchanged source should reproduce.

**A real staging bug was caught and fixed one round before this table,
not a smooth compile-and-hash pass from the start**: the attempt before
the manual compile referenced just above (hash
`90ff09b0d19fa47d4bddf22eae7966e7baa89f108782f69fb5a929f5b4464335` --
**do not use that hash either, it is stale**) staged a
`BeatShoreDesktop.exe`/`BeatShore Bridge.vst3` pair that predated this
session's resource-limit/ACL/throttling/redaction/shutdown-message
hardening -- caught by comparing file timestamps (`stat`) between the dev
build directories and `stage\`, not by any build or test failure, since
the stale binaries were still fully functional, just not the ones the
source actually describes. This exact class of mistake is what motivated
`build-release.ps1`'s unconditional-restage design (see its own top-of-
file comment) — the automated script makes it structurally impossible to
repeat, rather than relying on a human remembering to check timestamps
after the fact.

## Not yet included in this manifest

- **Code-signing signatures** for `BeatShoreDesktop.exe`, the VST3
  binary, and the installer -- no signing certificate available in this
  environment; these three hashes above are of **unsigned** binaries.

## How these were verified (not just hashed)

Every artifact above was produced by, and tested as part of, the same
staged tree: `npm ci --omit=dev` on a clean checkout using Node 24.19.0,
trimmed of `tfjs-node`'s build-time-only artifacts and source maps
(739MB -> ~302MB, see `BeatShoreSetup.iss`'s "Verified staging" note),
then exercised end-to-end via:
- `BeatShoreDesktop.exe --self-test <analyze.js path>` -- exit 0, all
  four checks (Node launches, tempo round trip, polyphonic transcription
  via a real Basic Pitch inference, export directory writable) PASS.
- The full `BridgeClientTest` suite (real `tempo` -> 100.45 BPM; real
  `transcribePolyphonic` -> 91 notes, `sha256:173a3d6c...`, matching
  every other run this project has ever produced on any Node version).
- The full `MultiSessionTest` suite (two simultaneous plugin instances;
  cancelling a queued job; cancelling a genuinely running job, which
  kills and restarts this exact staged `node.exe` and `analyze.js`, with
  a verified-successful follow-up request afterward).
- The system tray app (added after the above, see STATUS.md's "Clean-machine
  packaging" section): real `WM_COMMAND` messages sent to the actual tray
  window verified "Start at login" correctly creates/removes the
  `HKCU\...\Run` registry value and "Quit" cleanly terminates the process;
  the full test suite above was re-run against this tray-enabled build
  afterward and still passed.
- `BeatShoreSetup.iss` itself compiled clean (Inno Setup 6.7.3, zero
  warnings) after fixing two real bugs the compiler caught that neither
  manual review nor this project's own testing had found (a missing
  doubled brace in the AppId GUID; a Pascal-comment-syntax bug in the
  trailing documentation block) -- see STATUS.md for the full account.
- **This round's rebuild** (Start-at-login fix, security hardening,
  graceful shutdown): `BeatShoreDesktop.exe` and `BeatShore Bridge.vst3`
  were both rebuilt and re-verified before restaging --
  `SchedulerTest` all green, Steinberg Validator still 47/47 on the
  rebuilt VST3, and two dedicated empirical tests of the new graceful
  shutdown (a real `WM_COMMAND(ID_TRAY_QUIT)` send confirming clean exit
  and genuine mutex release -- a second instance starts immediately
  afterward -- and a real connected pipe session confirming it receives
  `BROKER_SHUTTING_DOWN` before its pipe closes). The Start-at-login fix
  itself was verified by launching `BeatShoreDesktop.exe` with no
  arguments from `C:\Windows\System32` against a byte-for-byte replica of
  the installed layout -- see STATUS.md's "Fifth..." section for the full
  account. `BeatShoreSetup.iss` was then recompiled from this freshly
  restaged tree: Inno Setup 6.7.3, zero warnings.
- **Self-test failure-handling rewrite**: `RunSelfTest()`'s new
  cmd.exe-redirection technique (captures `BeatShoreDesktop.exe
  --self-test`'s actual stdout/stderr into a persistent `selftest-log.txt`
  Exec() alone can't retrieve) was reproduced and verified directly
  outside of Inno Setup, for both outcomes: a real passing self-test
  produced a log with all three `PASS:` lines, and a deliberately broken
  script path produced a log containing `FATAL:`/`FAIL:` lines --
  confirming the exact string-matching `SelfTestFailureSummary()` (Pascal
  Script) performs against this file correctly extracts the failing check
  in both cases. The `.iss` script itself compiled clean after adding
  this. **Not verified**: the actual `Abort()`-forced nonzero Setup exit
  code, the incomplete-install marker file, and the uninstall-offer
  dialog all require watching a real (UAC-elevated) failing install
  happen, which this environment cannot do -- see STATUS.md.
- **This round's rebuild** (resource-limit tightening, requestId no
  longer touching the filesystem, SID-based pipe ACL, connection
  throttling, log redaction, the dedicated `BROKER_SHUTTING_DOWN`
  message, a real build ID): `BeatShoreDesktop.exe` and
  `BeatShore Bridge.vst3` were rebuilt (the VST3 against the updated
  `BridgeClient.h`/`PluginEditor.cpp`) and both re-verified before
  restaging -- `SchedulerTest` and Steinberg Validator (47/47) both
  clean. A raw NDJSON protocol test (bypassing `BridgeClient.h`,
  confirming the desktop's own rejections rather than the plugin's good
  behavior) confirmed `UNSUPPORTED_KIND`, the new `RATE_LIMITED` (engaged
  at exactly the configured threshold), the invalid-message disconnect,
  and the tightened 1MB line cap all behave correctly against the
  rebuilt desktop.
- **Verified against the actual staged tree, not just the dev build
  directory** -- the distinction that matters for "is this what actually
  ships," and the distinction whose absence is exactly what let the
  stale-staging bug described above slip past a first pass:
  `BeatShoreDesktop.exe --self-test` run from inside
  `native/installer/stage\` itself, using the staged `node\node.exe` and
  the staged `engine\` tree at their real installed-layout paths, exit 0,
  all three checks PASS. The full `BridgeClientTest` (`tempo` and
  `transcribePolyphonic`, the latter producing a real MIDI file with a
  matching SHA-256), `MultiSessionTest` (two simultaneous instances;
  cancelling a queued job; cancelling a genuinely running job with a
  verified Node restart and successful follow-up request), and the raw
  NDJSON hardening test (`UNSUPPORTED_KIND`, `RATE_LIMITED`, the
  invalid-message disconnect, the 1MB line cap) were all run against a
  `BeatShoreDesktop.exe` launched directly from the staging directory --
  **after** the stale-binary bug was caught and both binaries genuinely
  rebuilt and restaged, not before (the first pass at this same
  verification, run against the stale pair, is not reported here since
  it wasn't actually testing what the manifest describes). `BeatShoreSetup.iss`
  was then recompiled from this corrected, freshly-restaged, freshly-tested
  tree: Inno Setup 6.7.3, zero warnings, build ID `20260824.1` confirmed
  embedded in the compiled exe's own version resource
  (`FileVersionRaw`/`ProductVersionRaw` = `0.2.0.1`) before this
  manifest's final hashes were computed.
- **`-CleanEngine`, run for real for the first time, plus a real icon**:
  every prior build recorded in this file took the fast path and never
  exercised `build-release.ps1 -CleanEngine`. This build did: a
  completely fresh `npm ci --omit=dev` (124 packages, 23s) against the
  pinned Node 24 toolchain, the verified `tfjs-node` trim reapplied, the
  postinstall binding repair re-run, then the full staged self-test and
  regression suite -- all passed, and every individual engine artifact
  hash (`analyze.js`, `beatshore-dsp.js`, `tfjs_binding.node`,
  `tensorflow.dll`, the Basic Pitch model files) came back **byte-
  identical** to the non-clean build, confirmed by re-hashing each one
  directly rather than assumed. `BeatShoreDesktop.exe` also now embeds a
  real, brand-derived icon (see STATUS.md's "Eighth..." section) --
  confirmed present by extracting it back out of the compiled exe, not
  just trusting the resource compiler. Two additional failure-mode tests
  were run against this exact staged tree: a corrupted Basic Pitch
  `model.json` (self-test correctly isolates the failure to just that
  check, exits 1) and a genuinely unwritable MIDI export directory (a
  temporary, narrowly-scoped `icacls` deny rule, removed and verified
  restored immediately after -- self-test again isolates the failure
  correctly, exits 1). Neither test found a bug; both are real,
  executed verification, not assumed from the code.
- **`v0.2.0-rc4` (`b78d31e`)**: real publisher/copyright filled in, the
  `VersionInfoVersion` build-discriminator bug fixed (see the table
  above), rebuilt via `build-release.ps1` (not `-CleanEngine` this time
  -- the engine tree was already freshly clean-staged one build earlier
  in this same session and nothing engine-related changed since).
  Validator 47/47, full regression suite (`self-test`, `tempo`,
  `transcribePolyphonic`, `MultiSessionTest`) all passed, zero installer
  warnings. `BeatShoreDesktop.exe`/the VST3/the staging-manifest hash are
  all byte-identical to the prior build (only the `.iss` script itself
  changed, and it isn't part of the staged tree being hashed) -- the
  installer's own hash and `ProductVersionRaw` (now genuinely `0.2.0.4`,
  not the previously-stuck `0.2.0.1`) are the only things that changed.

Regenerate this manifest -- and re-run the same verification -- for every
build before it ships. Do not hand-edit hashes; recompute them
(`sha256sum <file>` or PowerShell's `Get-FileHash`) from the actual
staged files each time.
