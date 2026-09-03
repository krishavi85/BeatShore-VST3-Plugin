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

1. [x] Load a fresh REAPER project. Insert a track, load "BeatShore
   Bridge" as an FX (confirm it appears in the FX browser under its real
   plugin name). **PASS** (2026-08-29, project "Without You (Studio)")
2. [x] Route real audio into that track — either a live input (guitar,
   keyboard, mic) or an existing audio item on the track that gets
   played back through it. Play/record at least 10 continuous seconds
   of actual pitched musical content (a chord progression or melody
   line — not silence, not noise). **PASS**
3. [x] Confirm the editor's **capture status label** shows "Capture:
   audio buffered, ready to analyze" after ~10s of audio has passed
   through. **PASS**
4. [x] Click **"Transcribe Piano/Guitar (last 10s captured)"**. **PASS**
5. [x] Confirm the status label shows a progress/running state, then
   resolves to a result (not stuck on "running" indefinitely — the
   protocol's own timeout is 60s for this request kind). **PASS** —
   resolved in 750ms
6. [x] Confirm the result detail area shows a note count > 0 for real
   musical content, and a MIDI filename with a byte size. **PASS** —
   232 notes found, `BeatShore_Untitled_transcribePolyphonic_
   354b42dae3d24e1e8a1f61842ab15bb....mid`, algorithm confirmed as
   "basic-pitch CNN (Spotify, Apache-2.0) via @tensorflow/tfjs-node",
   `source: live-captured`
7. [x] Click **"Open Export Folder"** — confirmed: Explorer opened
   showing the real accumulated history of exported files (14 `.mid`
   files across multiple sessions 8/25-8/29, matching timestamps).
   **PASS**
8. [x] Import that `.mid` file onto a new REAPER track — imported
   successfully, note data visible in the item. First attempt was
   silent in REAPER (**root-caused as expected REAPER behavior, not a
   BeatShore bug**: the track only had BeatShore Bridge, an audio
   effect, loaded -- no instrument to render MIDI into audio). Retried
   with ReaSynth added to a track ("ReaSynth" / "BeatShore
   Reconstruction" track), MIDI: All channels -- **sound plays
   correctly**. **PASS** (2026-08-29). Detailed musical judgment
   (register/rhythm/chord-stacking accuracy vs. what was actually
   played) not separately itemized by the user but the mechanism -- a
   real, valid, audibly-correct MIDI file reproducing the transcribed
   performance -- is now confirmed end to end.
9. [ ] Repeat with a *different* instrument/register for a second
   independent pass — not yet done (checklist's own pass criteria below
   asks for two).

**Pass criteria for this section**: at least two independent real
transcriptions produced plausible, importable MIDI with a sensible note
count and timing — not just "it didn't crash." **First pass PASSED
2026-08-29** (live-captured, 232 notes, 750ms) — the mechanism is now
genuinely confirmed working live; steps 7-9 (export-folder open, MIDI
re-import/inspection, second independent instrument) still worth doing
for full section pass criteria, but the core "does this actually work
live" question is answered: yes.

## Part B — Full REAPER lifecycle

1. **Save/reload** — [x] **PASS** (2026-08-29): saved, closed REAPER
   entirely, reopened, plugin reloaded cleanly and still works.

2. **Reconnect after desktop restart** — [x] **PASS** (2026-08-29):
   `BeatShoreDesktop.exe` terminated directly, bridge status label
   reacted to the disconnect, relaunched, status recovered to connected
   and a fresh analysis succeeded again without reloading the plugin.
   Exact label wording at each stage not captured verbatim in a
   screenshot, but the full disconnect → react → relaunch → reconnect →
   working-analysis cycle was confirmed.

3. **Multiple instances** — [x] **PASS** (2026-08-29): BeatShore Bridge
   loaded on two tracks, different audio fed into each, each instance's
   result correctly reflected its own audio — no cross-talk. Confirms
   from inside REAPER's real UI what `BridgeClientTest`'s "two
   simultaneous instances" test already verified at the protocol level.

4. **MIDI-learned trigger** — [x] **Real finding, not a bug**
   (2026-08-29): confirmed no automatable parameters are exposed —
   "Analyze Tempo" and "Transcribe" are plain JUCE `TextButton`s, not
   host-automatable/MIDI-learnable parameters, exactly as anticipated.
   MIDI-learned triggering of an analysis is **not currently possible**
   with this UI; would need the buttons (or a proxy) exposed as real
   `AudioProcessorParameter`s to ever support this. Tracked as a
   feature gap, not a defect in what exists today.

5. **Playback/passthrough under load** — [x] **PASS** (2026-08-29):
   BeatShore Bridge loaded alongside Ozone Pro's full chain (EQ×2,
   Dynamics, Maximizer) on the same track, playback running. Both
   Analyze Tempo (166.71 BPM, 125ms) and Transcribe (7 notes,
   `source: live-captured`, 109ms) triggered mid-playback and completed
   correctly, transport kept running, no reported glitching. A
   genuinely non-trivial concurrent plugin load, not an empty project.

**Pass criteria for this section**: each numbered scenario either passes
as described, or is recorded as a specific, reproducible finding (not a
vague "seemed off") — including the MIDI-learn item, which may
legitimately reveal a UI gap rather than a bug. **All five scenarios
completed 2026-08-29**: save/reload, reconnect-after-restart, multiple
instances, and playback-under-load all genuinely PASS; MIDI-learn is a
confirmed real finding (not supported by the current plain-button UI,
not a defect). Part B is done.

## Part C — Everything added since 2026-08-29 (sidebar reorg, Key/Chords/
Loudness/Drums/Bass/Lead, Humanize, Mix, Master, the state-persistence
fix, MT3, Cancel, MIDI preview)

Parts A and B above are real and still stand, but they predate every
feature below -- none of it has ever been touched by a human inside a
real REAPER session. This part exists because an environment without a
native GUI cannot click a button, listen to audio, or watch a meter
move; every step names the exact control text from the current
`Source/PluginEditor.cpp` so there's nothing to interpret or guess.
Use real piano, guitar, bass, drums, and a mixed (multi-instrument)
source across these steps, not one clip reused everywhere -- a real
transcription engine's behavior on a drum loop and on a chord pad is
genuinely different, and this is the first time either MT3 or the
mono/drum kinds get a live pass at all.

### C1 — Every sidebar page opens clean

Click through all ten sidebar entries in order: **Overview, Separate,
Repair, Transcribe, Reconstruct, Humanize, Sound Match, Mix, Master,
Export**.

- [ ] Each one renders with no clipped text, no overlapping controls, no
  control drawn outside its card panel, at the plugin's default window
  size.
- [ ] Separate/Repair/Sound Match/Export show the generic "NOT BUILT YET"
  panel.
- [ ] **Reconstruct specifically** shows a *different*, longer message
  mentioning DAC/EnCodec by name and that they're kept internal-only
  pending a defined feature -- confirms the Twenty-sixth section's
  product-decision text actually renders, not just exists in source.
- [ ] Resize the plugin window (drag a host-provided resize handle, if
  your REAPER build allows it) and re-check Transcribe and Mix (the two
  most control-dense pages) for clipping at a couple of different sizes.

### C2 — Basic Pitch transcription (regression, now via the new sidebar)

1. [ ] On the Transcribe page, feed real pitched audio (piano or guitar,
   ≥10s) through the track, confirm **capture status** reads "Capture:
   audio buffered, ready to analyze".
2. [ ] Click **"Transcribe Piano/Guitar (basic-pitch, last 10s
   captured)"**. Confirm a note count > 0, `Algorithm: basic-pitch CNN
   (Spotify, Apache-2.0) via @tensorflow/tfjs-node`, and a written MIDI
   filename+size.

### C3 — Key / Chords / Loudness / Drums / Bass / Lead (never live-tested before)

1. [ ] With real musical audio playing, click **Key**, then **Chords**,
   then **Loudness** (Quick Analysis row) -- each should replace the same
   result label with a plausible value (e.g. `{"key":"C","mode":"major"}`
   for Key) within a few seconds, not hang.
2. [ ] Feed a real drum loop through the track, click **"Transcribe
   Drums"** -- confirm a note count > 0 and a written MIDI file.
3. [ ] Feed a real bass line, click **"Transcribe Bass"** -- confirm a
   note count > 0.
4. [ ] Feed a real melodic line (different register/instrument than
   step 3), click **"Transcribe Lead"** -- confirm a note count > 0.

### C4 — MT3 transcription: real progress, real MIDI

1. [ ] Feed real polyphonic/mixed audio (≥10s), click **"Transcribe
   (MT3 -- polyphonic, neural, last 10s captured)"**.
2. [ ] Watch the status label and the thin progress bar under the
   Transcribe card while it runs. Confirm the percentage genuinely
   advances (expect roughly two visible jumps -- ~10% then ~85% -- not a
   smooth animation and not stuck at 0% until it suddenly finishes; see
   STATUS.md's "Twenty-sixth" section for why it's exactly two real
   checkpoints, not more).
3. [ ] Confirm it resolves to a real result: note count > 0,
   `Algorithm: mt3-infer/mr_mt3`, a written MIDI filename+size.
4. [ ] Compare against the SAME audio run through basic-pitch (C2) --
   they don't need to match note-for-note (different models), but both
   should produce a plausible, non-empty transcription of the same real
   input.

### C4a — Core-only install: the "Model Pack not installed" message

This step is ONLY meaningful on a real clean-machine install where the
optional "MT3 Model Pack" component was deliberately left unchecked in
the installer wizard -- not testable on this dev machine, which always
has the dev-tree checkpoint present. Fold into the clean-machine test
(item 2 of the overall next-steps list) rather than running standalone
against a dev build.

1. [ ] With "MT3 Model Pack" unchecked at install time, click
   **"Transcribe (MT3 -- polyphonic, neural...)"**.
2. [ ] Confirm the status label shows a clear, specific,
   actionable message -- "The MT3 Model Pack is not installed. Re-run
   the BeatShore installer and select the optional \"MT3 Model Pack\"
   component to enable MT3 transcription." -- not a generic "engine
   failed to start", a hang, or a crash. (See `main.cpp`'s
   `mt3ModelPackInstalled()`/`MT3_MODEL_PACK_MISSING` -- this is checked
   BEFORE the desktop even attempts to spawn `python.exe`, so it should
   appear immediately, not after any real wait.)
3. [ ] Confirm every OTHER transcription kind (basic-pitch, drums, bass,
   lead, key/chords/loudness) still works normally -- a missing MT3
   Model Pack should never affect anything else.

### C5 — Cancel: mid-analysis, and the worker actually recovers

1. [ ] Trigger **"Transcribe (MT3 -- polyphonic, neural...)"** again.
   While the status label still shows "Analyzing... N%" (before it
   resolves), click **Cancel**.
2. [ ] Confirm the status label shows a cancellation outcome (not a
   generic crash/hang), and every trigger button re-enables within a
   couple of seconds.
3. [ ] **The real pass/fail condition**: immediately trigger MT3 again
   (fresh audio) and confirm THIS request completes normally with a real
   result. MT3 inference can't be aborted mid-computation -- a cancel
   kills and restarts the Python engine process (see `runPythonRequest`
   in `main.cpp`) -- so the thing actually being tested here is that the
   restarted engine comes back healthy, not just that the cancel click
   itself did something.
4. [ ] Repeat once against **basic-pitch** (C2's button) instead of MT3,
   for the same reason Node's own cancel/restart path deserves the same
   live check MT3's just got.

### C6 — MIDI preview: sent to a real instrument, not just a label

1. [ ] After C4 or C2 produces a result, add a real instrument plugin
   (e.g. ReaSynth, or any synth you have) directly after BeatShore
   Bridge in the *same track's* FX chain.
2. [ ] Click **"Preview MIDI"** (button text should change to "Stop
   Preview"). Confirm you actually **hear** the transcribed notes coming
   out of that downstream instrument -- this is real MIDI leaving
   BeatShore Bridge's own MIDI output during `processBlock()`, not a
   simulated/logged event.
3. [ ] Let it play past the end of the transcription once, and confirm
   it **loops cleanly** -- no stuck/held notes carrying over into the
   next lap (listen for a note that never releases).
4. [ ] Click **"Stop Preview"** mid-playback (ideally while a note is
   audibly sounding) and confirm every note goes silent immediately, not
   after a delay.
5. [ ] Trigger a NEW transcription while Preview is still enabled from
   the previous result -- confirm playback switches to the new result's
   notes without requiring you to click Preview again (see
   `loadMidiPreviewFile()`'s own comment: loading a new result while
   preview is on should just start playing the new one).

### C7 — MIDI recording: capture the preview onto a real REAPER MIDI item

1. [ ] Route BeatShore Bridge's MIDI output to a separate MIDI/
   instrument track (REAPER: track routing -- add a MIDI send from the
   BeatShore track to the target track's MIDI input; or, simpler, if
   your REAPER version supports "record output (REAPER, VSTi) as MIDI
   item" on the same track, use that instead), record-arm that target
   track.
2. [ ] Start recording, click **"Preview MIDI"**, let it play a full
   pass, stop recording.
3. [ ] Confirm a real MIDI item was captured on the target track, and
   that REAPER's own piano roll opens it and shows real note data
   matching what you heard in C6.
4. [ ] Do a real edit in REAPER's piano roll (move a note, change its
   velocity) and confirm it holds after clicking elsewhere -- this is
   the actual "editing" claim from STATUS.md's "Twenty-sixth" section:
   BeatShore Bridge itself has no editor, REAPER's does.

### C8 — Export

1. [ ] Click **"Open Export Folder"** -- confirm Explorer opens to the
   real folder containing this session's `.mid` files (both basic-pitch
   and MT3 filenames should be present after C2/C4).
2. [ ] Drag the most recent MT3-produced `.mid` file onto a fresh REAPER
   track and confirm it imports and plays correctly (same check Part A
   step 8 already did for basic-pitch -- this is MT3's own file format
   getting the same real check for the first time).

### C9 — Humanize actually alters the next transcription

1. [ ] On the Humanize page, set **Timing**, **Velocity**, **Dynamics**,
   and **Articulation** all to a clearly nonzero value (e.g. 60-80%).
2. [ ] Go to Transcribe, run the SAME kind against the SAME captured
   audio you used in C2 or C3 (re-play/re-capture identical source
   material if possible).
3. [ ] Compare the two MIDI files (the earlier non-humanized one from
   C2/C3, and this new one) in REAPER's piano roll or any MIDI viewer --
   confirm real, audible/visible differences in note timing, velocity,
   note-length (articulation), and that turning **Preserve Groove** on
   vs. off visibly changes how much the timing drifts.
4. [ ] Set all four sliders back to 0 and confirm a subsequent
   transcription looks like the original, non-humanized result again.

### C10 — Mix makes an audible difference

1. [ ] On the Mix page, tick **Mix Enabled**.
2. [ ] With audio playing through the track, sweep **Comp Thresh**
   downward and confirm audible gain-reduction/compression by ear.
3. [ ] Sweep **Low Shelf**/**Mid Peak**/**High Shelf** and confirm each
   audibly changes tone in the expected direction (boosting High Shelf
   should audibly brighten, etc.).
4. [ ] Pull **Limiter** down and confirm the loudest peaks visibly get
   caught (REAPER's own track meter, or Master page's true-peak
   readout) without obvious distortion.
5. [ ] Untick **Mix Enabled** and confirm the audio audibly reverts to
   unprocessed -- the "bit-for-bit passthrough when disabled" claim in
   `PluginProcessor.cpp`'s own comment, checked by ear here rather than
   just read in source.

### C11 — Offline render includes Mix

This is the one STATUS.md flags as the real regression check for the
`getStateInformation()` bug (STATUS.md's "Twenty-second" section) --
worth being exact about.

1. [ ] With Mix enabled and clearly audible (per C10), render the
   project to a new audio file (File > Render).
2. [ ] Play back the RENDERED file (not the live REAPER session) in
   REAPER or any player and confirm the SAME Mix processing is present
   in the render -- not the unprocessed original.

### C12 — Master meters respond and reset

1. [ ] On the Master page, play audio through the track and confirm
   **Momentary**, **Short-Term**, and **Integrated** LUFS readouts and
   **True Peak** all move in response to real signal (not stuck at
   `-inf`).
2. [ ] Click **Reset Meter**, confirm Integrated LUFS and the true-peak
   hold both go back to `-inf`/a fresh starting state, then resume
   accumulating once playback continues.

### C13 — Save/reload survives with REAL non-default state (re-run, not just cited from 2026-08-29)

Part B's original save/reload pass (2026-08-29) predates Mix, Master,
and Humanize entirely -- it only proved the plugin window itself
survived a reload, not that any of this session's new parameter state
does. This is the actual regression test for the
`getStateInformation()`/`setStateInformation()` fix (STATUS.md's
"Twenty-second" section).

1. [ ] Set Mix Enabled on with clearly non-default EQ/Comp/Limiter
   values, and Humanize sliders to non-default values (per C9/C10).
2. [ ] Save the project, close REAPER entirely, reopen it.
3. [ ] Confirm the Mix page's toggle and every knob show the SAME values
   you set, not defaults -- and confirm this by ear too (C10's audible
   effect should still be present on reopen, not just the knob position).
4. [ ] Confirm the Humanize sliders show the same values you set.

### C14 — Reconnect doesn't freeze REAPER

1. [ ] With REAPER running and BeatShore Bridge connected, kill
   `BeatShoreDesktop.exe` directly (Task Manager or `taskkill`).
2. [ ] Confirm REAPER's UI stays fully responsive throughout (drag the
   timeline, open a menu) -- the bridge status label going to
   "Not connected... Retrying..." should not stall anything else.
3. [ ] Relaunch `BeatShoreDesktop.exe`, confirm the status label recovers
   to Connected on its own (no plugin reload needed), and that a fresh
   MT3 or basic-pitch request afterward succeeds.

### C15 — Multiple instances stay independent

1. [ ] Load BeatShore Bridge on two different tracks with two different
   real audio sources.
2. [ ] Trigger MT3 on one and basic-pitch on the other at roughly the
   same time. Confirm each instance's own status/result reflects only
   its own track's audio -- no cross-talk, no shared "in flight" state
   blocking the other instance.

**Pass criteria for Part C**: every checkbox above checked, with any
failure recorded as a specific, reproducible finding (exact step, exact
REAPER version, exact status-label text, desktop log if still running) —
not a vague "seemed off". C11 and C13 are the two that specifically
confirm the state-persistence fix under real REAPER render/reload, not
just live monitoring — treat those two as blocking for any release, not
optional.

## After running this

Update `RELEASE_STATUS.md`'s "Known issues" and the release acceptance
checklist with the real results — check off what passed, and turn
anything that failed into a real, findable STATUS.md entry the way every
other finding in this project has been recorded (what broke, how it was
found, whether/how it's fixed).
