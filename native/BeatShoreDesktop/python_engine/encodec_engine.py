#!/usr/bin/env python3
"""Real PythonEngine-facing wrapper around EnCodec (facebookresearch/encodec,
MIT) -- loads the actual pretrained model once at startup, then serves
ENCODE_DECODE requests over the same NDJSON line protocol echo_engine.py
established (READY on startup, one JSON object per line in and out).

Protocol:
  Startup:  {"type": "READY"}
  Request:  {"type": "ENCODE_DECODE", "requestId": "...",
             "inputWavPath": "...", "outputWavPath": "..."}
  Response: {"type": "ENCODE_DECODE_RESULT", "requestId": "...",
             "success": true, "sampleRate": 24000, "codebooksUsed": 8,
             "inputPeak": 0.5, "outputPeak": 0.53, ...}
            or {"type": "ENCODE_DECODE_RESULT", "requestId": "...",
             "success": false, "error": "..."}
  Shutdown: {"type": "SHUTDOWN"}

Audio is passed by WAV file path, not inline in the JSON -- a real audio
buffer inline as a JSON array would be enormous and slow to parse; a
shared file path is the same approach BeatShore's own Node pipeline
already uses for MIDI export. The caller is responsible for the paths
existing/being writable; this script doesn't manage a temp directory of
its own.
"""
import json
import os
import sys
import warnings

# Must happen before importing torch/encodec: ChildProcessEngine.h merges
# this process's stderr into the SAME stream as stdout (matching how
# NodeEngine has always handled Node's own stderr -- see that file's
# header comment), and this NDJSON protocol treats every line on that
# stream as a message. PyTorch prints real, benign deprecation warnings
# on import/first use (confirmed directly: a stray `FutureWarning` line
# arrived where the READY line was expected, corrupting the handshake in
# the very first real end-to-end test run).
warnings.filterwarnings("ignore")

# A stronger, more general version of the fix above -- see
# mt3_engine.py's own header comment for the full reasoning (found there
# first: mt3-infer's checkpoint-download progress bar bypasses
# `warnings` entirely on stdout, and a `transformers` advisory does the
# same on stderr). Applied here too for the same structural reason, not
# because this specific script has hit either failure yet: `send()` is
# the only thing allowed to write to the real stdout from this point on.
_real_stdout = sys.stdout
sys.stdout = open(os.devnull, "w")
sys.stderr = open(os.devnull, "w")

import numpy as np
import torch
from encodec import EncodecModel
import soundfile as sf


def send(message: dict) -> None:
    _real_stdout.write(json.dumps(message) + "\n")
    _real_stdout.flush()


def load_model() -> EncodecModel:
    model = EncodecModel.encodec_model_24khz()
    model.set_target_bandwidth(6.0)
    model.eval()
    return model


def handle_encode_decode(model: EncodecModel, msg: dict) -> dict:
    request_id = msg.get("requestId")
    try:
        audio, sr = sf.read(msg["inputWavPath"], dtype="float32", always_2d=True)
        # audio: (samples, channels) -> (channels, samples), resampled to
        # the model's own rate is the CALLER's responsibility for now (a
        # real integration would resample here; kept out of this proof-
        # of-concept to keep the failure surface small and legible).
        wav = torch.from_numpy(audio.T).unsqueeze(0)
        if wav.shape[1] != model.channels:
            if model.channels == 1:
                wav = wav.mean(dim=1, keepdim=True)
            else:
                wav = wav.repeat(1, model.channels, 1)

        with torch.no_grad():
            encoded_frames = model.encode(wav)
            decoded = model.decode(encoded_frames)
        decoded = decoded[:, :, : wav.shape[-1]]

        out = decoded.squeeze(0).transpose(0, 1).numpy()
        sf.write(msg["outputWavPath"], out, model.sample_rate)

        n_codebooks = int(encoded_frames[0][0].shape[1])
        return {
            "type": "ENCODE_DECODE_RESULT",
            "requestId": request_id,
            "success": True,
            "sampleRate": model.sample_rate,
            "codebooksUsed": n_codebooks,
            "inputPeak": float(wav.abs().max()),
            "outputPeak": float(decoded.abs().max()),
            "meanAbsError": float((decoded - wav).abs().mean()),
        }
    except Exception as e:  # noqa: BLE001 -- deliberately broad: any failure must be reported back over the protocol, not crash the engine silently
        return {"type": "ENCODE_DECODE_RESULT", "requestId": request_id, "success": False, "error": str(e)}


def main() -> int:
    model = load_model()
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
        if msg_type == "ENCODE_DECODE":
            send(handle_encode_decode(model, msg))
        elif msg_type == "SHUTDOWN":
            return 0
        else:
            send({"type": "ERROR", "message": f"unknown message type: {msg_type!r}"})

    return 0


if __name__ == "__main__":
    sys.exit(main())
