# BeatShore Privacy Policy (Draft)

**Status: draft, not attorney-reviewed, not yet published anywhere.**
See `legal/LEGAL_REVIEW_PACKET.md` question 5 for why this exists as a
separate document from the EULA.

Last updated: 2026-08-29

## Summary

BeatShore Desktop and the BeatShore Bridge VST3 plugin (together, "the
Software") process your audio entirely on your own computer. We do not
collect, transmit, or have access to your audio, your MIDI exports, your
project data, or information about how you use the Software.

## What the Software does with your data

- **Audio you feed the plugin** is captured into local memory and
  temporary files on your own computer, analyzed locally by the
  Software's own bundled analysis engine (running as a local background
  process, `BeatShoreDesktop.exe`), and never sent over a network.
- **MIDI files** produced by transcription are written to a location on
  your own computer that you control (see the plugin's "Open Export
  Folder" button), and stay there until you move or delete them.
- **Temporary audio files** are created during analysis in your
  operating system's temp directory and are normally deleted once
  analysis completes. **Known limitation**: if the Software is
  terminated abnormally while an analysis is in progress (e.g., the
  background process crashes or is forcibly killed), the temporary file
  for that in-progress analysis is not automatically cleaned up on the
  next launch. It remains, unencrypted, in the OS temp directory until
  your operating system's own temp-file housekeeping removes it, or
  until you delete it yourself.
- **A self-test log** is written locally to help diagnose installation
  problems. It does not contain your audio or MIDI content.

## What the Software does NOT do

- It does not transmit audio, MIDI, project data, or usage statistics to
  us or to any third party.
- It has no telemetry, analytics, or crash-reporting functionality that
  sends data off your machine.
- It does not require or create a user account.
- It does not use cookies or any web-based tracking (it is a native
  desktop application, not a web service).

## Third-party components

The Software bundles open-source components (the VST3 SDK, JUCE, Node.js,
TensorFlow, the Basic Pitch model) that run entirely locally as part of
the Software. See `native/installer/assets/Licenses/` for their license
texts. None of them independently transmit your data anywhere as used in
this product.

## Children's privacy

The Software is not directed at children and we do not knowingly collect
any information from children, because the Software does not collect
information from anyone.

## Changes to this policy

If a future version of the Software changes what data it processes or
where it goes, we will update this policy and disclose the change in
that version's release notes before you're asked to accept updated
terms — consistent with the commitment already made in the EULA
(`native/installer/assets/LicenseFile.txt`, §5).

## Contact

Questions about this policy: krishanavinash@gmail.com

---
*This is a draft prepared for attorney review, not a published or legally
finalized policy. Do not rely on it as the Software's actual privacy
commitment until it has been reviewed and a live, hosted version exists
at a stable URL referenced from the installer.*
