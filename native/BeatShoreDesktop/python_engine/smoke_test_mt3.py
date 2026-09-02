#!/usr/bin/env python3
"""Real smoke test for mt3-infer's MR-MT3 model (MIT code + MIT weights,
verified directly against the HuggingFace model card): loads the actual
pretrained model, transcribes a synthetic audio clip, and reports real
measured output -- note count, pitch range -- not just "didn't throw".
"""
import sys
import warnings
warnings.filterwarnings("ignore")

import numpy as np
import soundfile as sf
from mt3_infer import transcribe


def main():
    sr = 16000
    duration_s = 4.0
    t = np.linspace(0, duration_s, int(sr * duration_s), endpoint=False)
    # A simple two-note melody (C4, E4) with real ADSR-ish shaping, same
    # spirit as this project's own reference_test_melody.wav fixture --
    # not a bare sine the whole clip through, actual note events.
    audio = np.zeros_like(t)
    notes = [(261.63, 0.0, 2.0), (329.63, 2.0, 2.0)]
    for freq, start, dur in notes:
        mask = (t >= start) & (t < start + dur)
        local_t = t[mask] - start
        envelope = np.clip(local_t / 0.02, 0, 1) * np.clip((dur - local_t) / 0.3, 0, 1)
        audio[mask] += 0.5 * envelope * np.sin(2 * np.pi * freq * local_t)
    audio = audio.astype(np.float32)

    wav_path = "mt3_smoke_input.wav"
    sf.write(wav_path, audio, sr)
    print(f"Wrote {wav_path}: {duration_s}s, {sr}Hz (for reference; transcribe() takes the array directly)")

    # IMPORTANT: model defaults to "mt3_pytorch" (kunato's port), which has
    # NO declared license upstream -- confirmed directly via mt3-infer's
    # own LICENSE file and by finding no LICENSE in kunato/mt3-pytorch
    # itself. Always pass model="mr_mt3" explicitly (MIT code + MIT
    # weights, both confirmed directly) rather than relying on the default.
    print("Transcribing with MR-MT3 (MIT-licensed, weights confirmed MIT on HuggingFace) -- first run downloads weights...")
    midi = transcribe(audio, model="mr_mt3", sr=sr)
    print(f"Result type: {type(midi)}")

    out_midi_path = "mt3_smoke_output.mid"
    midi.save(out_midi_path)
    print(f"Saved {out_midi_path}")

    note_events = []
    for track in midi.tracks:
        for msg in track:
            if msg.type in ("note_on", "note_off"):
                note_events.append(msg)
    note_on_count = sum(1 for m in note_events if m.type == "note_on" and getattr(m, "velocity", 1) > 0)

    print(f"MIDI tracks: {len(midi.tracks)}")
    print(f"Total note_on/note_off events: {len(note_events)}, real note-on count: {note_on_count}")
    for m in note_events[:20]:
        print(" ", m)

    ok = note_on_count > 0
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
