#!/usr/bin/env python3
"""Minimal Standard MIDI File (SMF) parser -- just enough to pull real
note-on/note-off events with timing (in seconds, using the file's own
tempo + division) and velocity out of BeatShore's exported .mid files, so
their content can be checked for real rather than eyeballing a garbled
raw-byte dump. No external deps (mido etc. not assumed available)."""
import struct
import sys

def read_varlen(data, pos):
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not (byte & 0x80):
            break
    return value, pos

def parse_midi(path):
    with open(path, "rb") as f:
        data = f.read()

    pos = 0
    assert data[0:4] == b"MThd"
    header_len = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, division = struct.unpack(">HHH", data[8:8 + header_len])
    pos = 8 + header_len

    ticks_per_quarter = division if division & 0x8000 == 0 else None
    tempo_us_per_quarter = 500000  # default 120 BPM until a tempo meta event says otherwise

    tracks = []
    for _ in range(ntracks):
        assert data[pos:pos + 4] == b"MTrk"
        track_len = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        track_start = pos + 8
        track_end = track_start + track_len
        events = []
        p = track_start
        running_status = None
        abs_ticks = 0
        track_name = None
        while p < track_end:
            delta, p = read_varlen(data, p)
            abs_ticks += delta
            status = data[p]
            if status < 0x80:
                # running status
                status = running_status
            else:
                p += 1
                running_status = status if status < 0xF0 else running_status

            if status == 0xFF:  # meta event
                meta_type = data[p]
                p += 1
                length, p = read_varlen(data, p)
                payload = data[p:p + length]
                p += length
                if meta_type == 0x51 and length == 3:  # set tempo
                    tempo_us_per_quarter = (payload[0] << 16) | (payload[1] << 8) | payload[2]
                    events.append(("tempo", abs_ticks, tempo_us_per_quarter))
                elif meta_type == 0x03:  # track name
                    track_name = payload.decode("latin-1", errors="replace")
                elif meta_type == 0x2F:
                    pass  # end of track
            elif status in (0xF0, 0xF7):  # sysex
                length, p = read_varlen(data, p)
                p += length
            else:
                event_type = status & 0xF0
                if event_type in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    d1, d2 = data[p], data[p + 1]
                    p += 2
                    if event_type == 0x90 and d2 > 0:
                        events.append(("note_on", abs_ticks, d1, d2))
                    elif event_type == 0x80 or (event_type == 0x90 and d2 == 0):
                        events.append(("note_off", abs_ticks, d1, d2))
                elif event_type in (0xC0, 0xD0):
                    p += 1
                else:
                    p += 1
        tracks.append((track_name, events))
        pos = track_end

    return ticks_per_quarter, tracks

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"

def ticks_to_seconds(ticks, ticks_per_quarter, tempo_us_per_quarter):
    return ticks * (tempo_us_per_quarter / 1_000_000.0) / ticks_per_quarter

def summarize(path):
    ticks_per_quarter, tracks = parse_midi(path)
    print(f"\n=== {path} ===")
    print(f"ticks_per_quarter={ticks_per_quarter}, tracks={len(tracks)}")
    tempo = 500000
    for name, events in tracks:
        notes = []
        open_notes = {}
        t = 500000
        for ev in events:
            if ev[0] == "tempo":
                t = ev[2]
            elif ev[0] == "note_on":
                _, ticks, pitch, vel = ev
                open_notes.setdefault(pitch, []).append((ticks, vel))
            elif ev[0] == "note_off":
                _, ticks, pitch, vel = ev
                if open_notes.get(pitch):
                    start_ticks, start_vel = open_notes[pitch].pop(0)
                    notes.append((start_ticks, ticks, pitch, start_vel))
        if not notes:
            continue
        notes.sort()
        print(f"  track '{name}': {len(notes)} notes")
        pitches = [n[2] for n in notes]
        print(f"    pitch range: {note_name(min(pitches))} ({min(pitches)}) .. {note_name(max(pitches))} ({max(pitches)})")
        for start_t, end_t, pitch, vel in notes[:40]:
            start_s = ticks_to_seconds(start_t, ticks_per_quarter, tempo)
            end_s = ticks_to_seconds(end_t, ticks_per_quarter, tempo)
            print(f"    t={start_s:6.3f}s  dur={end_s - start_s:5.3f}s  {note_name(pitch):>4} ({pitch:3d})  vel={vel}")
        if len(notes) > 40:
            print(f"    ... and {len(notes) - 40} more")

if __name__ == "__main__":
    for path in sys.argv[1:]:
        try:
            summarize(path)
        except Exception as e:
            print(f"\n=== {path} ===\nFAILED TO PARSE: {e}")
