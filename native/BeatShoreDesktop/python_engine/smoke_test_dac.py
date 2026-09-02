#!/usr/bin/env python3
"""Real smoke test for DAC (Descript Audio Codec): loads the actual
pretrained 44.1kHz model, encodes a synthetic audio buffer, decodes it
back, and reports real, measured numbers. Same standard as
smoke_test_encodec.py.
"""
import sys
import numpy as np
import torch
import dac


def main():
    print("Downloading/loading DAC 44kHz pretrained model...")
    model_path = dac.utils.download(model_type="44khz")
    model = dac.DAC.load(model_path)
    model.eval()
    print(f"Model loaded. Sample rate: {model.sample_rate}")

    duration_s = 1.0
    sr = model.sample_rate
    t = np.linspace(0, duration_s, int(sr * duration_s), endpoint=False)
    freq = 440.0
    audio = 0.5 * np.sin(2 * np.pi * freq * t).astype(np.float32)
    wav = torch.from_numpy(audio).unsqueeze(0).unsqueeze(0)  # (batch, channels, samples)

    print(f"Input audio: shape={tuple(wav.shape)}, peak={wav.abs().max().item():.4f}, rms={wav.pow(2).mean().sqrt().item():.4f}")

    with torch.no_grad():
        x = model.preprocess(wav, sr)
        z, codes, latents, _, _ = model.encode(x)
        decoded = model.decode(z)

    decoded = decoded[:, :, :wav.shape[-1]]
    print(f"Decoded audio: shape={tuple(decoded.shape)}, peak={decoded.abs().max().item():.4f}, rms={decoded.pow(2).mean().sqrt().item():.4f}")
    print(f"Latent code shape: {tuple(codes.shape)}")

    diff = (decoded - wav).abs()
    mean_abs_error = diff.mean().item()
    max_abs_error = diff.max().item()
    print(f"Reconstruction error: mean_abs={mean_abs_error:.4f}, max_abs={max_abs_error:.4f}")

    ok = mean_abs_error < 0.3 and codes.numel() > 0 and decoded.abs().max().item() > 0.01
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
