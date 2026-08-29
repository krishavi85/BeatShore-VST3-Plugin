# REAPER live test checklist

For the two items that need a human at a real REAPER session — see
`RELEASE_STATUS.md`'s acceptance checklist. This is a step-by-step
script, not open-ended exploration: each step names an exact UI control
(taken from `Source/PluginEditor.cpp` — "Analyze Tempo (last 10s
captured)", "Transcribe Piano/Guitar (last 10s captured)", "Open Export
Folder") and what result confirms it passed. Note there is currently no
in-plugin Cancel button — cancellation is only exercised via the
automated `BridgeClientTest`/`MultiSessionTest` harnesses, not tested
live here.

Fill in ✅/❌ and notes per step as you go. Anything that fails: note the
exact REAPER version, plugin bridge status label text, and (if the
desktop process is still running) grab its log before closing anything.

## Part A — Live polyphonic transcription

Only `tempo` has been observed live so far; this is the first time
`transcribePolyphonic` gets a real, human-watched, real-audio run inside
REAPER.

1. [ ] Load a fresh REAPER project. Insert a track, load "BeatShore
   Bridge" as an FX (confirm it appears in the FX browser under its real
   plugin name).
2. [ ] Route real audio into that track — either a live input (guitar,
   keyboard, mic) or an existing audio item on the track that gets
   played back through it. Play/record at least 10 continuous seconds
   of actual pitched musical content (a chord progression or melody
   line — not silence, not noise).
3. [ ] Confirm the editor's **capture status label** shows "Capture:
   audio buffered, ready to analyze" after ~10s of audio has passed
   through.
4. [ ] Click **"Transcribe Piano/Guitar (last 10s captured)"**.
5. [ ] Confirm the status label shows a progress/running state, then
   resolves to a result (not stuck on "running" indefinitely — the
   protocol's own timeout is 60s for this request kind).
6. [ ] Confirm the result detail area shows a note count > 0 for real
   musical content, and a MIDI filename with a byte size.
7. [ ] Click **"Open Export Folder"** (should now be enabled) — confirm
   Windows Explorer opens to the actual folder containing the new
   `.mid` file.
8. [ ] Import that `.mid` file onto a new REAPER track. Confirm:
   - [ ] The notes roughly correspond to what you actually played
     (right register, right approximate rhythm — this is an ML model,
     not exact transcription, so judge musically not byte-for-byte).
   - [ ] Timing lines up plausibly with the source audio when both
     tracks play together.
   - [ ] Chords (if you played any) show as stacked simultaneous notes,
     not smeared/arpeggiated artifacts.
9. [ ] Repeat once more with a *different* instrument/register (e.g. if
   step 2 was guitar, do a second pass with piano or vice versa) to
   confirm it's not a one-off fluke.

**Pass criteria for this section**: at least two independent real
transcriptions produced plausible, importable MIDI with a sensible note
count and timing — not just "it didn't crash."

## Part B — Full REAPER lifecycle

1. **Save/reload**
   - [ ] With BeatShore Bridge loaded and at least one successful
     analysis result showing, save the REAPER project (`.rpp`).
   - [ ] Close REAPER entirely.
   - [ ] Reopen the saved project. Confirm the plugin reloads without
     REAPER reporting a missing/failed plugin, and the editor opens
     showing its normal UI (result labels may reset — that's expected,
     just confirm no crash/error dialog).
   - [ ] Run a fresh tempo analysis (see below) to confirm it still
     functions post-reload, not just that the UI redraws.

2. **Reconnect after desktop restart**
   - [ ] With the plugin loaded and working, find and terminate
     `BeatShoreDesktop.exe` directly (Task Manager, or however you'd
     naturally do it) — simulating the desktop process dying
     unexpectedly.
   - [ ] Confirm the plugin's **bridge status label** reflects the
     disconnect (not just silently stuck on the last-known "connected"
     text).
   - [ ] Relaunch `BeatShoreDesktop.exe` (or restart REAPER, whichever
     is the intended real-world recovery path for BeatShore users —
     note which one you tested).
   - [ ] Confirm the bridge status label recovers to a connected state
     and a fresh analysis (tempo or transcription) succeeds again,
     without reloading the plugin itself.

3. **Multiple instances**
   - [ ] Load BeatShore Bridge on two different tracks in the same
     project (or two different projects open at once, if that's a
     realistic REAPER usage pattern you want covered).
   - [ ] Feed different audio into each, run an analysis on both around
     the same time.
   - [ ] Confirm each instance's result reflects *its own* audio, not
     the other instance's (no cross-talk/cross-session corruption —
     this is already verified at the protocol level via
     `BridgeClientTest`'s "two simultaneous instances" test, so this
     step is confirming it holds true from inside REAPER's real UI,
     not re-discovering it from scratch).

4. **MIDI-learned trigger**
   - [ ] Right-click the "Analyze Tempo" or "Transcribe" button (or use
     REAPER's standard "Learn" workflow for parameters this plugin
     exposes, if any are exposed as automatable parameters rather than
     plain JUCE buttons — note in your results which mechanism actually
     applies, since this may reveal the buttons aren't MIDI-learnable
     as currently built).
   - [ ] If MIDI-learn is possible: bind it to a MIDI controller/note
     and confirm triggering it via MIDI produces the same result as
     clicking the button directly.
   - [ ] If MIDI-learn is *not* possible with the current UI (plain
     JUCE `TextButton`s are typically not host-automatable), record
     that as a real finding, not a test failure — it tells us whether
     "MIDI-learned trigger" needs a UI change before it can ever be
     verified, separate from whether the underlying analysis works.

5. **Playback/passthrough under load**
   - [ ] Start playback of a project with other tracks/plugins running
     alongside BeatShore Bridge (a realistic session, not an empty
     project).
   - [ ] While playback continues, trigger an analysis (tempo or
     transcription).
   - [ ] Confirm: audio playback through REAPER does not glitch, stutter,
     or dropout while the analysis runs in the background; the
     transport keeps running normally; the analysis itself still
     completes and returns a correct result.
   - [ ] Note CPU load in REAPER's performance meter before and during
     the analysis, for a rough sense of real-world impact.

**Pass criteria for this section**: each numbered scenario either passes
as described, or is recorded as a specific, reproducible finding (not a
vague "seemed off") — including the MIDI-learn item, which may
legitimately reveal a UI gap rather than a bug.

## After running this

Update `RELEASE_STATUS.md`'s "Known issues" and the release acceptance
checklist with the real results — check off what passed, and turn
anything that failed into a real, findable STATUS.md entry the way every
other finding in this project has been recorded (what broke, how it was
found, whether/how it's fixed).
