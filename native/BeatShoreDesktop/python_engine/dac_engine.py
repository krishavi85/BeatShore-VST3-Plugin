#!/usr/bin/env python3
"""Real PythonEngine-facing wrapper around DAC (descriptinc/descript-audio-
codec, MIT) -- same protocol shape as encodec_engine.py (see that file's
own header comment for the full protocol), loading the actual pretrained
44.1kHz model once at startup and serving ENCODE_DECODE requests over
NDJSON lines.
"""
import json
import os
import sys
import warnings

# See encodec_engine.py's own header comment for why this must run before
# importing torch/dac -- the exact same stderr-corrupts-the-NDJSON-stream
# bug, same fix, plus the stronger general stdout-redirect fix (see
# mt3_engine.py's own header comment for where that one was actually
# needed) applied here too for consistency.
warnings.filterwarnings("ignore")
_real_stdout = sys.stdout
sys.stdout = open(os.devnull, "w")
sys.stderr = open(os.devnull, "w")

import torch
import dac
import soundfile as sf


def send(message: dict) -> None:
    _real_stdout.write(json.dumps(message) + "\n")
    _real_stdout.flush()


def load_model() -> "dac.DAC":
    model_path = dac.utils.download(model_type="44khz")
    model = dac.DAC.load(model_path)
    model.eval()
    return model


def handle_encode_decode(model, msg: dict) -> dict:
    request_id = msg.get("requestId")
    try:
        audio, sr = sf.read(msg["inputWavPath"], dtype="float32", always_2d=True)
        wav = torch.from_numpy(audio.T).unsqueeze(0)  # (batch, channels, samples)
        if wav.shape[1] > 1:
            wav = wav.mean(dim=1, keepdim=True)  # DAC's 44kHz model is mono

        with torch.no_grad():
            x = model.preprocess(wav, sr)
            z, codes, _latents, _, _ = model.encode(x)
            decoded = model.decode(z)
        decoded = decoded[:, :, : wav.shape[-1]]

        out = decoded.squeeze(0).transpose(0, 1).numpy()
        sf.write(msg["outputWavPath"], out, model.sample_rate)

        return {
            "type": "ENCODE_DECODE_RESULT",
            "requestId": request_id,
            "success": True,
            "sampleRate": model.sample_rate,
            "codebooksUsed": int(codes.shape[1]),
            "inputPeak": float(wav.abs().max()),
            "outputPeak": float(decoded.abs().max()),
            "meanAbsError": float((decoded - wav).abs().mean()),
        }
    except Exception as e:  # noqa: BLE001 -- see encodec_engine.py's own comment on why this stays broad
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
