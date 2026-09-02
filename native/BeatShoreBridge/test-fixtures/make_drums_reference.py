#!/usr/bin/env python3
"""Synthesizes a short, precisely-timed drum reference (kick/snare/hihat)
for testing BeatShore Bridge's real drum transcription against a KNOWN
ground truth. Two bars of 4/4 at 120 BPM (0.25s per 8th note): kick on
beats 1 & 3, snare on beats 2 & 4, closed hihat on every 8th note.

Percussive synthesis, not pitched tones -- each drum type gets its own
noise/tone shaping so the result actually resembles what a real kit
sounds like, matching how BeatShore's own real REAPER test (on an actual
song) mapped its detected hits to General MIDI drum note numbers
(kick=36/C2, snare=38/D2, closed hihat=42/F#2) -- this fixture's ground
truth uses the SAME GM numbers so the comparison is direct.
"""
import json
import math
import random
import struct
import wave

SAMPLE_RATE = 48000
random.seed(42)  # reproducible noise, not Math.random()-style nondeterminism

def env_exp_decay(n, decay_samples):
    return [math.exp(-3.0 * i / decay_samples) for i in range(n)]

def synth_kick(duration_s=0.18, velocity=1.0):
    n = int(duration_s * SAMPLE_RATE)
    env = env_exp_decay(n, int(0.06 * SAMPLE_RATE))
    samples = [0.0] * n
    for i in range(n):
        t = i / SAMPLE_RATE
        # Pitch sweep from ~150Hz down to ~50Hz -- the classic kick "thump"
        freq = 150.0 * math.exp(-18.0 * t) + 50.0
        phase = 2 * math.pi * freq * t
        click = 0.4 * math.exp(-i / (0.002 * SAMPLE_RATE)) if i < 200 else 0.0
        samples[i] = (math.sin(phase) * env[i] + click) * velocity
    return samples

def synth_snare(duration_s=0.16, velocity=1.0):
    n = int(duration_s * SAMPLE_RATE)
    env = env_exp_decay(n, int(0.05 * SAMPLE_RATE))
    body_freq = 190.0
    samples = [0.0] * n
    for i in range(n):
        t = i / SAMPLE_RATE
        body = 0.5 * math.sin(2 * math.pi * body_freq * t)
        noise = random.uniform(-1.0, 1.0)
        samples[i] = (body * 0.5 + noise * 0.5) * env[i] * velocity
    return samples

def synth_hihat(duration_s=0.06, velocity=1.0):
    n = int(duration_s * SAMPLE_RATE)
    env = env_exp_decay(n, int(0.015 * SAMPLE_RATE))
    # Filtered/inharmonic noise approximation: sum of high inharmonic
    # partials (a cheap, standard metallic-hihat synthesis trick) mixed
    # with broadband noise, both high-passed in spirit by their own
    # high frequencies -- no real filter needed for a short burst like this.
    partials = [3200, 4550, 6300, 8100, 9700]
    samples = [0.0] * n
    for i in range(n):
        t = i / SAMPLE_RATE
        tone = sum(math.sin(2 * math.pi * f * t) for f in partials) / len(partials)
        noise = random.uniform(-1.0, 1.0)
        samples[i] = (tone * 0.4 + noise * 0.6) * env[i] * velocity
    return samples

def mix_into(buffer, samples, start_sample):
    for i, s in enumerate(samples):
        idx = start_sample + i
        if idx < len(buffer):
            buffer[idx] += s

def main():
    eighth = 0.25  # seconds, 120 BPM
    bar = [0, 1, 2, 3, 4, 5, 6, 7]  # 8th-note grid positions within one bar
    events = []  # (type, gm_note, time_s, velocity)
    for bar_num in range(2):
        base = bar_num * 8 * eighth
        for pos in bar:
            t = base + pos * eighth
            events.append(("hihat", 42, t, 0.55))
            if pos == 0 or pos == 4:
                events.append(("kick", 36, t, 0.9))
            if pos == 2 or pos == 6:
                events.append(("snare", 38, t, 0.85))

    total_duration = max(e[2] for e in events) + 0.4
    total_samples = int(total_duration * SAMPLE_RATE)
    buffer = [0.0] * total_samples

    synth_fns = {"kick": synth_kick, "snare": synth_snare, "hihat": synth_hihat}
    for kind, gm_note, t, vel in events:
        samples = synth_fns[kind](velocity=vel)
        mix_into(buffer, samples, int(t * SAMPLE_RATE))

    peak = max(abs(s) for s in buffer) or 1.0
    headroom = 0.85 / peak
    pcm = [int(max(-32768, min(32767, s * headroom * 32767))) for s in buffer]

    out_path = "reference_drums.wav"
    with wave.open(out_path, "w") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        frames = bytearray()
        for s in pcm:
            frames += struct.pack("<hh", s, s)
        wf.writeframes(bytes(frames))

    ground_truth = {
        "sampleRate": SAMPLE_RATE,
        "totalDurationSeconds": round(total_duration, 3),
        "tempoBpm": 120,
        "description": "Two bars of 4/4 at 120 BPM: kick on beats 1 & 3, snare on beats 2 & 4, closed hihat on every 8th note. GM drum note numbers (kick=36, snare=38, closed hihat=42) match what BeatShore's own transcribeDrums output used on a real song.",
        "hits": [
            {"type": t, "gmNote": n, "timeSeconds": round(time, 3), "velocityHint": v}
            for t, n, time, v in sorted(events, key=lambda e: e[2])
        ],
    }
    with open("reference_drums.ground_truth.json", "w") as f:
        json.dump(ground_truth, f, indent=2)

    print(f"Wrote {out_path} ({total_duration:.2f}s, {SAMPLE_RATE}Hz stereo 16-bit, {len(events)} hits)")
    print("Wrote reference_drums.ground_truth.json")

if __name__ == "__main__":
    main()
