# BeatShore — Legal Review Packet

Prepared 2026-08-29 for handoff to a licensed attorney. This packet exists
so counsel's time goes into judgment calls, not into hunting through a
repository. **Nothing in this document is legal advice** — it's a
factual inventory of what exists today, assembled by the engineering
work on this project, plus a list of specific open questions.

Product: BeatShore Bridge (VST3 plugin) + BeatShore Desktop (companion
background app), Windows only. Publisher: Singh's Innovation & Advisory.
Contact: krishanavinash@gmail.com. Repository:
[github.com/krishavi85/BeatShore-VST3-Plugin](https://github.com/krishavi85/BeatShore-VST3-Plugin).

---

## 1. Documents that need review

| Document | Location | Status |
|---|---|---|
| End User License Agreement | `native/installer/assets/LicenseFile.txt` | Drafted, real content, **not attorney-reviewed** — shown to every user during install, must be accepted to proceed |
| Privacy Policy | `legal/PRIVACY_POLICY_DRAFT.md` (this packet) | Drafted for this review — no live version has ever existed; the installer currently has no privacy-policy URL wired in |
| Third-party license notices | `native/installer/assets/Licenses/` | Assembled from actual vendored files, shown as reference material (not an acceptance click-through) |

## 2. Third-party components bundled with the shipped product

Everything below is compiled/bundled into the installer and runs on the
end user's machine. None of it is modified from upstream; nothing is
redistributed as source.

| Component | Version | License | Where |
|---|---|---|---|
| Steinberg VST3 SDK | 3.8.1 | MIT | `Licenses/VST3-SDK-LICENSE.txt` |
| JUCE Framework | 9.0.1 | **Dual: AGPLv3 or commercial "JUCE 9" license** — this project uses the free commercial tier ("JUCE Starter") | `Licenses/JUCE-LICENSE.md` |
| Node.js runtime | bundled, pinned 24.19.0 | MIT | `Licenses/Node.js-LICENSE.txt` |
| TensorFlow C library (via `@tensorflow/tfjs-node`) | ^4.22.0 | Apache-2.0 | `Licenses/TensorFlow-C-LICENSE.txt` + `TensorFlow-C-THIRD-PARTY-NOTICES.txt` |
| Basic Pitch model | — | Apache-2.0 (Spotify) | `Licenses/basic-pitch-LICENSE-Apache-2.0.txt` |
| `@tensorflow/tfjs` / `@tensorflow/tfjs-node` JS layer | ^4.22.0 | Apache-2.0 | same boilerplate as Basic Pitch's notice |

## 3. Specific open questions for counsel

These are the judgment calls the engineering side of this project
cannot make:

1. **JUCE Starter-tier eligibility.** The commercial JUCE 9 license has
   tiers gated by the licensee's revenue/funding (Starter is free below
   a threshold defined in JUCE's own EULA at
   [juce.com/legal/juce-9-licence](https://juce.com/legal/juce-9-licence/)).
   Counsel should confirm Singh's Innovation & Advisory's actual
   eligibility for that tier given the business's real financial
   situation, and what happens contractually if that situation changes
   after release (upgrade obligation, timing, etc.).
2. **AGPLv3 avoidance.** This project relies on the *commercial* JUCE
   license specifically to avoid AGPLv3's source-disclosure
   requirements. Confirm the commercial license was actually accepted
   correctly (it's a click-through on JUCE's own site tied to a JUCE
   account, not something this repository can show proof of) and that
   nothing in this project's distribution model accidentally re-triggers
   AGPLv3 obligations.
3. **"VST" trademark usage.** Steinberg's VST3 SDK license (MIT) covers
   the *code*; Steinberg separately maintains trademark guidelines for
   using the "VST" name/logo on products built with it. Confirm the
   product's naming ("BeatShore Bridge VST3 plugin"), marketing copy,
   and any logo usage comply with those guidelines — this hasn't been
   checked against Steinberg's current trademark terms.
4. **Governing law choice.** The EULA currently names Suriname
   (§9) as governing law — confirm this is actually the intended and
   appropriate choice given where the business is organized and where
   users are expected to be located, and that it's enforceable as
   written.
5. **Sufficiency of the EULA's data-handling section as a privacy
   policy.** EULA §5 states the software doesn't transmit audio, MIDI,
   project data, or usage information anywhere, and has no telemetry —
   confirm whether that section, plus the standalone draft in this
   packet, is legally sufficient on its own, or whether a separately
   *hosted* (live URL) privacy policy is required for the intended
   distribution channels (e.g., some plugin marketplaces or payment
   processors require a reachable URL, not just installer text).
6. **Liability/warranty language (EULA §§6-7) and limitation-of-liability
   enforceability** in whatever jurisdiction(s) actually matter for this
   product's real user base.

## 4. Factual notes relevant to the privacy review

- All audio processing, transcription, and MIDI export happen locally
  on the end user's machine. Nothing is sent to Licensor or any third
  party over a network by this software.
- The software writes: MIDI export files (user-visible, user-controlled
  location), a self-test failure log, and temporary raw-audio files
  during analysis (normally deleted on completion).
- **A known, real gap**: temp audio files from a session that's killed
  mid-request (e.g., the desktop process crashes or is force-terminated
  while analyzing) are not currently cleaned up automatically on next
  startup. They contain the same audio the user fed the plugin, stored
  unencrypted in the OS temp directory, until the OS's own temp-file
  housekeeping eventually reclaims them. This is disclosed in the
  privacy policy draft; flag if counsel thinks it needs stronger
  treatment before release.
- No user accounts, no registration, no analytics SDKs, no crash
  reporters that phone home.

---
*This packet was assembled from the actual files in this repository as
of commit `ad26ca0` — it is a snapshot, not a living document. Re-verify
against the repository directly before relying on any specific claim
here for a final decision.*
