#!/usr/bin/env python3
"""Real PythonEngine-facing wrapper around mt3-infer's MR-MT3 model (MIT
code + MIT weights, both confirmed directly against the upstream repo and
HuggingFace model card -- see STATUS.md). Same protocol family as
dac_engine.py/encodec_engine.py: NDJSON lines, READY on startup.

IMPORTANT: mt3_infer.transcribe()'s `model` argument defaults to
"mt3_pytorch" (kunato's port, no declared upstream license -- confirmed
directly, see STATUS.md) if not specified. This script ALWAYS passes
model="mr_mt3" explicitly; never remove that argument to "simplify" a
call, since the default silently picks the unlicensed model.

Protocol:
  Startup:  {"type": "READY"}
  Request:  {"type": "TRANSCRIBE", "requestId": "...",
             "inputAudioPath": "...", "outputMidiPath": "..."}
             inputAudioPath is BeatShoreDesktop's own BSM1 raw-audio temp
             file format (see main.cpp / native/protocol/
             SharedAudioBuffer.h): 4-byte "BSM1" magic, then uint32
             sampleRate, uint32 channels, uint32 frames, then
             frames*channels interleaved float32 samples -- the same file
             every existing analysis kind (Node-routed) already reads,
             so this engine reads the identical format rather than
             requiring a separate WAV conversion step.
  Response: {"type": "TRANSCRIBE_RESULT", "requestId": "...",
             "success": true, "noteCount": 2, "midiPath": "..."}
            or {"type": "TRANSCRIBE_RESULT", "requestId": "...",
             "success": false, "error": "..."}
  Shutdown: {"type": "SHUTDOWN"}
"""
import json
import os
import struct
import sys
import warnings

warnings.filterwarnings("ignore")

# A stronger, more general version of the same fix encodec_engine.py/
# dac_engine.py apply for warnings specifically: mt3-infer's own
# checkpoint-download/load path prints a real progress bar directly to
# stdout, and `transformers` (a real dependency of mt3-infer) prints its
# own advisory warnings straight to stderr -- neither goes through the
# `warnings` module, so filterwarnings() alone caught neither (confirmed
# directly, one at a time: the progress bar corrupted the READY line
# first; fixing stdout alone then revealed the SAME corruption from
# stderr on the very next run, a `transformers` "trust_remote_code"
# advisory landing where the result line was expected). Rather than keep
# patching one noise source at a time as new libraries find new streams
# to write to, this redirects BOTH sys.stdout and sys.stderr away from
# the real pipe for the ENTIRE script -- `send()` is the only thing that
# ever writes to the real stdout again, via the saved reference below.
# Any future print()/progress-bar/banner/warning from any dependency, on
# either stream, known or not yet encountered, is now structurally
# incapable of corrupting the protocol stream.
_real_stdout = sys.stdout
sys.stdout = open(os.devnull, "w")
sys.stderr = open(os.devnull, "w")

import numpy as np
from mt3_infer import transcribe

MR_MT3_TARGET_SR = 16000  # the sample rate MR-MT3 was trained/verified against (see smoke_test_mt3.py)


def send(message: dict) -> None:
    _real_stdout.write(json.dumps(message) + "\n")
    _real_stdout.flush()


def read_bsm1(path: str):
    """Reads BeatShoreDesktop's own BSM1 raw-audio temp file format
    directly -- see this module's own header comment for the exact
    layout. Returns (mono_float32_array, sample_rate)."""
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"BSM1":
            raise ValueError(f"not a BSM1 file (magic={magic!r})")
        sample_rate, channels, frames = struct.unpack("<III", f.read(12))
        raw = np.frombuffer(f.read(frames * channels * 4), dtype="<f4")
    audio = raw.reshape(frames, channels)
    mono = audio.mean(axis=1) if channels > 1 else audio[:, 0]
    return mono.astype(np.float32), sample_rate


def resample_linear(audio: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:
    """Simple linear-interpolation resampler -- not broadcast-quality,
    but adequate for feeding a fixed-rate transcription model and keeps
    this engine dependency-free (no extra resampling library) beyond
    what mt3-infer itself already pulls in."""
    if src_sr == dst_sr:
        return audio
    duration = len(audio) / src_sr
    dst_len = int(round(duration * dst_sr))
    src_x = np.linspace(0.0, duration, num=len(audio), endpoint=False)
    dst_x = np.linspace(0.0, duration, num=dst_len, endpoint=False)
    return np.interp(dst_x, src_x, audio).astype(np.float32)


def handle_transcribe(msg: dict) -> dict:
    request_id = msg.get("requestId")
    try:
        audio, src_sr = read_bsm1(msg["inputAudioPath"])
        if src_sr != MR_MT3_TARGET_SR:
            audio = resample_linear(audio, src_sr, MR_MT3_TARGET_SR)

        midi = transcribe(audio, model="mr_mt3", sr=MR_MT3_TARGET_SR)
        midi.save(msg["outputMidiPath"])

        note_on_count = sum(
            1
            for track in midi.tracks
            for m in track
            if m.type == "note_on" and getattr(m, "velocity", 1) > 0
        )

        return {
            "type": "TRANSCRIBE_RESULT",
            "requestId": request_id,
            "success": True,
            "noteCount": note_on_count,
            "midiPath": msg["outputMidiPath"],
        }
    except Exception as e:  # noqa: BLE001 -- see dac_engine.py's own comment on why this stays broad
        return {"type": "TRANSCRIBE_RESULT", "requestId": request_id, "success": False, "error": str(e)}


def main() -> int:
    # MR-MT3's weights load lazily on first transcribe() call inside
    # mt3-infer's own cache -- warm it here so READY genuinely means
    # "ready to transcribe fast", not "about to stall on the first real
    # request" the way the DAC/EnCodec engines' explicit load_model()-at-
    # startup already behaves.
    dummy = np.zeros(MR_MT3_TARGET_SR, dtype=np.float32)
    transcribe(dummy, model="mr_mt3", sr=MR_MT3_TARGET_SR)

    send({"type": "READY"})

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            send({"type": "ERROR", "message": f"invalid JSON: {line!r}"})
            continue

        msg_type = msg.get("type")
        if msg_type == "TRANSCRIBE":
            send(handle_transcribe(msg))
        elif msg_type == "SHUTDOWN":
            return 0
        else:
            send({"type": "ERROR", "message": f"unknown message type: {msg_type!r}"})

    return 0


if __name__ == "__main__":
    sys.exit(main())
