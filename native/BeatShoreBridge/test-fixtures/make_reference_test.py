#!/usr/bin/env python3
"""Synthesizes a short, precisely-timed reference melody for testing
BeatShore Bridge's real polyphonic transcription (Basic Pitch) inside
REAPER against a KNOWN ground truth, instead of judging accuracy purely
by ear. Writes a 48kHz/16-bit stereo WAV plus a ground_truth.json listing
the exact MIDI pitch/start/duration of every note actually synthesized,
so the resulting exported MIDI file can be compared against a real
answer key.

Each note is additive-synthesized (fundamental + 4 harmonics, ADSR
envelope) rather than a bare sine wave -- Basic Pitch was trained on real
instrument recordings, and a pure sine tone is a poor, unrepresentative
stand-in for the harmonic content it actually expects to see.
"""
import json
import math
import struct
import wave

SAMPLE_RATE = 48000

def midi_to_freq(note: int) -> float:
    return 440.0 * (2.0 ** ((note - 69) / 12.0))

def synth_note(freq: float, duration_s: float, velocity: float) -> list:
    n = int(duration_s * SAMPLE_RATE)
    attack = int(0.008 * SAMPLE_RATE)
    decay = int(0.08 * SAMPLE_RATE)
    release = int(0.15 * SAMPLE_RATE)
    sustain_level = 0.6
    harmonics = [(1, 1.0), (2, 0.5), (3, 0.3), (4, 0.15), (5, 0.08)]
    samples = [0.0] * n
    for i in range(n):
        t = i / SAMPLE_RATE
        # ADSR envelope
        if i < attack:
            env = i / attack
        elif i < attack + decay:
            env = 1.0 - (1.0 - sustain_level) * ((i - attack) / decay)
        elif i < n - release:
            env = sustain_level
        else:
            rel_pos = (i - (n - release)) / release
            env = sustain_level * (1.0 - rel_pos)
        s = 0.0
        for harmonic_num, amp in harmonics:
            s += amp * math.sin(2 * math.pi * freq * harmonic_num * t)
        samples[i] = s * env * velocity
    return samples

def mix_into(buffer: list, notes: list, start_sample: int):
    for i, s in enumerate(notes):
        idx = start_sample + i
        if idx < len(buffer):
            buffer[idx] += s

def main():
    # (midi_note, start_time_s, duration_s, velocity) -- a C-major arpeggio
    # up and down, ending in a held C-major triad (tests polyphony too).
    # C4=60, E4=64, G4=67, C5=72.
    events = [
        (60, 0.00, 0.45, 0.85),
        (64, 0.60, 0.45, 0.85),
        (67, 1.20, 0.45, 0.85),
        (72, 1.80, 0.45, 0.85),
        (67, 2.40, 0.45, 0.85),
        (64, 3.00, 0.45, 0.85),
        (60, 3.60, 1.20, 0.80),  # chord root
        (64, 3.60, 1.20, 0.80),  # chord third
        (67, 3.60, 1.20, 0.80),  # chord fifth
    ]

    total_duration = max(e[1] + e[2] for e in events) + 0.3  # trailing silence
    total_samples = int(total_duration * SAMPLE_RATE)
    buffer = [0.0] * total_samples

    for note, start, dur, vel in events:
        freq = midi_to_freq(note)
        samples = synth_note(freq, dur, vel)
        mix_into(buffer, samples, int(start * SAMPLE_RATE))

    peak = max(abs(s) for s in buffer) or 1.0
    headroom = 0.85 / peak
    pcm = [int(max(-32768, min(32767, s * headroom * 32767))) for s in buffer]

    out_path = "reference_test_melody.wav"
    with wave.open(out_path, "w") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for s in pcm:
            frames += struct.pack("<hh", s, s)  # duplicate mono -> stereo
        wf.writeframes(bytes(frames))

    ground_truth = {
        "sampleRate": SAMPLE_RATE,
        "totalDurationSeconds": round(total_duration, 3),
        "description": "C-major arpeggio (C4-E4-G4-C5-G4-E4) followed by a held C-major triad (C4+E4+G4)",
        "notes": [
            {"midiNote": n, "noteName": {60: "C4", 64: "E4", 67: "G4", 72: "C5"}[n],
             "startSeconds": s, "durationSeconds": d, "velocityHint": v}
            for n, s, d, v in events
        ],
    }
    with open("ground_truth.json", "w") as f:
        json.dump(ground_truth, f, indent=2)

    print(f"Wrote {out_path} ({total_duration:.2f}s, {SAMPLE_RATE}Hz stereo 16-bit)")
    print("Wrote ground_truth.json")

if __name__ == "__main__":
    main()
