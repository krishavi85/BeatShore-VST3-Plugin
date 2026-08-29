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

## After running this

Update `RELEASE_STATUS.md`'s "Known issues" and the release acceptance
checklist with the real results — check off what passed, and turn
anything that failed into a real, findable STATUS.md entry the way every
other finding in this project has been recorded (what broke, how it was
found, whether/how it's fixed).
