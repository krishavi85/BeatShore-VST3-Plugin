// basic-pitch requires exactly 22050Hz mono audio. The browser prototype
// gets this for free from Web Audio's OfflineAudioContext
// (beatshore-polyphonic-client.js) -- that API doesn't exist in Node, so
// this is a real (if modest) resampler: a windowed-sinc FIR lowpass for
// anti-aliasing when downsampling, followed by linear-interpolation
// resampling to the exact target rate. Not bit-identical to what
// OfflineAudioContext or librosa would produce, but a legitimate DSP
// implementation, not a naive decimation that would alias.

function lowpassFIR(input, sampleRate, cutoffHz, numTaps = 63) {
  const taps = new Float32Array(numTaps);
  const fc = cutoffHz / sampleRate;
  const M = numTaps - 1;
  let sum = 0;
  for (let n = 0; n < numTaps; n++) {
    const x = n - M / 2;
    const sinc = x === 0 ? 2 * fc : Math.sin(2 * Math.PI * fc * x) / (Math.PI * x);
    const window = 0.54 - 0.46 * Math.cos((2 * Math.PI * n) / M); // Hamming
    taps[n] = sinc * window;
    sum += taps[n];
  }
  for (let n = 0; n < numTaps; n++) taps[n] /= sum; // normalize DC gain to 1

  const output = new Float32Array(input.length);
  const half = Math.floor(numTaps / 2);
  for (let i = 0; i < input.length; i++) {
    let acc = 0;
    for (let k = 0; k < numTaps; k++) {
      const idx = i + k - half;
      if (idx >= 0 && idx < input.length) acc += input[idx] * taps[k];
    }
    output[i] = acc;
  }
  return output;
}

export function toMono(channelData) {
  if (channelData.length === 1) return channelData[0];
  const n = channelData[0].length;
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let sum = 0;
    for (const ch of channelData) sum += ch[i];
    out[i] = sum / channelData.length;
  }
  return out;
}

export function resampleMono(input, srcRate, dstRate) {
  if (srcRate === dstRate) return input;
  const src = dstRate < srcRate ? lowpassFIR(input, srcRate, dstRate * 0.45) : input;
  const ratio = srcRate / dstRate;
  const outLength = Math.max(1, Math.floor(src.length / ratio));
  const out = new Float32Array(outLength);
  for (let i = 0; i < outLength; i++) {
    const srcPos = i * ratio;
    const i0 = Math.floor(srcPos);
    const frac = srcPos - i0;
    const s0 = i0 < src.length ? src[i0] : 0;
    const s1 = i0 + 1 < src.length ? src[i0 + 1] : s0;
    out[i] = s0 + (s1 - s0) * frac;
  }
  return out;
}
