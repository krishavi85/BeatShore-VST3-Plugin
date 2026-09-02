// BeatShore DSP module — heuristic, in-browser signal analysis.
// Everything here is a real (but non-ML) DSP technique running on the main thread:
// autocorrelation pitch tracking, spectral-flux onset detection, Krumhansl-Schmuckler
// key finding, energy-based structure segmentation, triad-template chord matching.
// It replaces nothing that requires a trained model — polyphonic transcription,
// instrument/preset matching and effect estimation still need the native backend
// described in the audit. Long files are analyzed with an adaptive hop so runtime
// stays bounded, but this still runs on the main thread, not a worker.

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

// ---------------------------------------------------------------- helpers --

function toMono(buf) {
  const ch = buf.numberOfChannels, len = buf.length;
  const out = new Float32Array(len);
  for (let c = 0; c < ch; c++) {
    const d = buf.getChannelData(c);
    for (let i = 0; i < len; i++) out[i] += d[i] / ch;
  }
  return out;
}

function nextPow2(n) { let p = 1; while (p < n) p <<= 1; return p; }

// bounds the number of analysis frames for very long files by widening the hop
function adaptiveHop(dataLen, frameSize, baseHop, maxFrames) {
  const naive = Math.floor((dataLen - frameSize) / baseHop) + 1;
  if (naive <= maxFrames || naive <= 0) return baseHop;
  return Math.max(baseHop, Math.ceil((dataLen - frameSize) / maxFrames));
}

// in-place iterative radix-2 Cooley-Tukey FFT (n must be a power of two)
function fft(re, im) {
  const n = re.length;
  if (n <= 1) return;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let curWr = 1, curWi = 0;
      const half = len >> 1;
      for (let j = 0; j < half; j++) {
        const ur = re[i + j], ui = im[i + j];
        const vr = re[i + j + half] * curWr - im[i + j + half] * curWi;
        const vi = re[i + j + half] * curWi + im[i + j + half] * curWr;
        re[i + j] = ur + vr; im[i + j] = ui + vi;
        re[i + j + half] = ur - vr; im[i + j + half] = ui - vi;
        const nWr = curWr * wr - curWi * wi;
        const nWi = curWr * wi + curWi * wr;
        curWr = nWr; curWi = nWi;
      }
    }
  }
}

function magnitudeSpectrum(frame) {
  const n = nextPow2(frame.length);
  const re = new Float64Array(n), im = new Float64Array(n);
  const L = frame.length;
  for (let i = 0; i < L; i++) {
    const w = L > 1 ? 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (L - 1)) : 1;
    re[i] = frame[i] * w;
  }
  fft(re, im);
  const half = n >> 1;
  const mags = new Float64Array(half);
  for (let i = 0; i < half; i++) mags[i] = Math.hypot(re[i], im[i]);
  return mags;
}

// autocorrelation via Wiener-Khinchin (power spectrum -> inverse FFT), O(n log n)
function autocorrelateFFT(frame) {
  const n = nextPow2(frame.length * 2);
  const re = new Float64Array(n), im = new Float64Array(n);
  re.set(frame);
  fft(re, im);
  for (let i = 0; i < n; i++) { const p = re[i] * re[i] + im[i] * im[i]; re[i] = p; im[i] = 0; }
  for (let i = 0; i < n; i++) im[i] = -im[i];
  fft(re, im);
  const ac = new Float64Array(n);
  for (let i = 0; i < n; i++) ac[i] = re[i] / n;
  return ac;
}

function freqToMidi(f) { return 69 + 12 * Math.log2(f / 440); }

function chromaFromSpectrum(mags, sr, n) {
  const chroma = new Float64Array(12);
  for (let i = 1; i < mags.length; i++) {
    const freq = (i * sr) / n;
    if (freq < 40 || freq > 5000) continue;
    const midi = freqToMidi(freq);
    const pc = ((Math.round(midi) % 12) + 12) % 12;
    chroma[pc] += mags[i];
  }
  return chroma;
}

function maxOf(arr) { let m = -Infinity; for (let i = 0; i < arr.length; i++) if (arr[i] > m) m = arr[i]; return m; }
function minOf(arr) { let m = Infinity; for (let i = 0; i < arr.length; i++) if (arr[i] < m) m = arr[i]; return m; }

// ------------------------------------------------------------------- API --

export function computePeaks(buf, n) {
  n = n || 700;
  const data = toMono(buf);
  const len = data.length;
  const bucket = Math.max(1, Math.floor(len / n));
  const peaks = [];
  for (let i = 0; i < n; i++) {
    const start = i * bucket, end = Math.min(len, start + bucket);
    let max = 0;
    for (let j = start; j < end; j++) { const v = Math.abs(data[j]); if (v > max) max = v; }
    peaks.push(Math.min(1, max));
  }
  return peaks;
}

// RMS loudness in dBFS — a real measurement, but not full ITU-R BS.1770 LUFS
// (no K-weighting or gating). Good enough for relative comparison between stems.
export function loudness(buf) {
  const data = toMono(buf);
  let sum = 0;
  for (let i = 0; i < data.length; i++) sum += data[i] * data[i];
  const rms = Math.sqrt(sum / Math.max(1, data.length));
  const db = 20 * Math.log10(Math.max(rms, 1e-9));
  return Math.round(db * 10) / 10;
}

export function estimateTempo(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 1024;
  const hop = adaptiveHop(data.length, frameSize, 512, 20000);
  const flux = [];
  let prevMags = null;
  for (let start = 0; start + frameSize <= data.length; start += hop) {
    const mags = magnitudeSpectrum(data.subarray(start, start + frameSize));
    if (prevMags) {
      let sum = 0;
      for (let i = 0; i < mags.length; i++) { const d = mags[i] - prevMags[i]; if (d > 0) sum += d; }
      flux.push(sum);
    } else flux.push(0);
    prevMags = mags;
  }
  if (flux.length < 8) return 120;
  let mean = 0; for (const v of flux) mean += v; mean /= flux.length;
  const f = flux.map(v => Math.max(0, v - mean));

  const frameRate = sr / hop;
  const minBPM = 60, maxBPM = 200;
  const minLag = Math.max(1, Math.floor((frameRate * 60) / maxBPM));
  const maxLag = Math.min(f.length - 1, Math.ceil((frameRate * 60) / minBPM));
  let bestLag = minLag, bestScore = -Infinity;
  for (let lag = minLag; lag <= maxLag; lag++) {
    let score = 0;
    for (let i = 0; i + lag < f.length; i++) score += f[i] * f[i + lag];
    if (score > bestScore) { bestScore = score; bestLag = lag; }
  }
  let bpm = (60 * frameRate) / bestLag;
  while (bpm < 70) bpm *= 2;
  while (bpm > 180) bpm /= 2;
  return Math.round(bpm * 100) / 100;
}

const KS_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88];
const KS_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];

function rotate(arr, n) { const out = new Array(12); for (let i = 0; i < 12; i++) out[i] = arr[(i - n + 12) % 12]; return out; }
function correlate(a, b) {
  let ma = 0, mb = 0;
  for (let i = 0; i < 12; i++) { ma += a[i]; mb += b[i]; }
  ma /= 12; mb /= 12;
  let num = 0, da = 0, db = 0;
  for (let i = 0; i < 12; i++) { const va = a[i] - ma, vb = b[i] - mb; num += va * vb; da += va * va; db += vb * vb; }
  const denom = Math.sqrt(da * db);
  return denom > 0 ? num / denom : 0;
}

// chroma accumulation + Krumhansl-Schmuckler key profile correlation
export function estimateKey(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 4096;
  const n = nextPow2(frameSize);
  const hop = adaptiveHop(data.length, frameSize, frameSize, 300);
  const totalChroma = new Float64Array(12);
  let frames = 0;
  for (let start = 0; start + frameSize <= data.length; start += hop) {
    const mags = magnitudeSpectrum(data.subarray(start, start + frameSize));
    const chroma = chromaFromSpectrum(mags, sr, n);
    for (let i = 0; i < 12; i++) totalChroma[i] += chroma[i];
    frames++;
  }
  if (frames === 0) return { key: 'C', mode: 'major' };
  const maxC = maxOf(totalChroma) || 1;
  const norm = Array.from(totalChroma, v => v / maxC);

  let best = { corr: -Infinity, key: 0, mode: 'major' };
  for (let root = 0; root < 12; root++) {
    const cMaj = correlate(norm, rotate(KS_MAJOR, root));
    const cMin = correlate(norm, rotate(KS_MINOR, root));
    if (cMaj > best.corr) best = { corr: cMaj, key: root, mode: 'major' };
    if (cMin > best.corr) best = { corr: cMin, key: root, mode: 'minor' };
  }
  return { key: NOTE_NAMES[best.key], mode: best.mode };
}

// energy-based novelty segmentation: bucket RMS into low/mid/high runs,
// merge short runs, label by position + energy level
export function analyzeStructure(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const winSize = Math.floor(sr * 1.0);
  const rms = [];
  for (let start = 0; start < data.length; start += winSize) {
    let sum = 0, count = 0;
    const end = Math.min(data.length, start + winSize);
    for (let i = start; i < end; i++) { sum += data[i] * data[i]; count++; }
    rms.push(Math.sqrt(sum / Math.max(1, count)));
  }
  if (rms.length === 0) return { sections: [] };
  const smooth = rms.map((_, i) => (rms[Math.max(0, i - 1)] + rms[i] + rms[Math.min(rms.length - 1, i + 1)]) / 3);
  const maxE = maxOf(smooth) || 1;
  const norm = smooth.map(v => v / maxE);
  const sorted = [...norm].sort((a, b) => a - b);
  const q1 = sorted[Math.floor(sorted.length * 0.33)];
  const q2 = sorted[Math.floor(sorted.length * 0.66)];
  const levelOf = v => (v <= q1 ? 'low' : v <= q2 ? 'mid' : 'high');
  const levels = norm.map(levelOf);

  const minRun = Math.max(1, 6);
  const runs = [];
  let curLevel = levels[0], curStart = 0;
  for (let i = 1; i <= levels.length; i++) {
    if (i === levels.length || levels[i] !== curLevel) {
      runs.push({ level: curLevel, start: curStart, end: i });
      if (i < levels.length) { curLevel = levels[i]; curStart = i; }
    }
  }
  const merged = [];
  for (const r of runs) {
    if (r.end - r.start < minRun && merged.length) merged[merged.length - 1].end = r.end;
    else merged.push(Object.assign({}, r));
  }
  if (merged.length > 1 && merged[0].end - merged[0].start < minRun) {
    merged[1].start = merged[0].start;
    merged.shift();
  }

  let chorusN = 0, verseN = 0;
  const sections = merged.map((r, i) => {
    const isFirst = i === 0, isLast = i === merged.length - 1;
    let label;
    if (isFirst && r.level === 'low') label = 'Intro';
    else if (isLast && r.level === 'low') label = 'Outro';
    else if (r.level === 'high') { chorusN++; label = chorusN === 1 ? 'Chorus' : 'Chorus ' + chorusN; }
    else if (r.level === 'mid') { verseN++; label = verseN === 1 ? 'Verse' : 'Verse ' + verseN; }
    else label = 'Bridge';
    return { time: r.start, dur: Math.max(1, r.end - r.start), label };
  });
  return { sections };
}

const TRIAD_TEMPLATES = (() => {
  const templates = [];
  for (let root = 0; root < 12; root++) {
    const maj = new Array(12).fill(0); maj[root] = 1; maj[(root + 4) % 12] = 1; maj[(root + 7) % 12] = 1;
    const min = new Array(12).fill(0); min[root] = 1; min[(root + 3) % 12] = 1; min[(root + 7) % 12] = 1;
    templates.push({ label: NOTE_NAMES[root], vec: maj });
    templates.push({ label: NOTE_NAMES[root] + 'm', vec: min });
  }
  return templates;
})();

// per-second chroma vector matched against major/minor triad templates
export function chordTrack(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 4096;
  const n = nextPow2(frameSize);
  const winSize = Math.max(frameSize, Math.floor(sr * 1.0));
  const out = [];
  let lastLabel = null;
  for (let start = 0; start + frameSize <= data.length; start += winSize) {
    const mags = magnitudeSpectrum(data.subarray(start, start + frameSize));
    const chroma = chromaFromSpectrum(mags, sr, n);
    const maxC = maxOf(chroma) || 1;
    for (let i = 0; i < 12; i++) chroma[i] /= maxC;
    let best = { score: -Infinity, label: 'N' };
    for (const t of TRIAD_TEMPLATES) {
      let score = 0;
      for (let i = 0; i < 12; i++) score += chroma[i] * t.vec[i];
      if (score > best.score) best = { score, label: t.label };
    }
    const time = start / sr;
    if (best.label !== lastLabel) { out.push({ time, label: best.label }); lastLabel = best.label; }
  }
  return out;
}

function estimateAttackMs(data, sr) {
  const winSize = Math.max(1, Math.floor(sr * 0.005));
  const env = [];
  for (let i = 0; i < data.length; i += winSize) {
    let sum = 0;
    const end = Math.min(data.length, i + winSize);
    for (let j = i; j < end; j++) sum += data[j] * data[j];
    env.push(Math.sqrt(sum / winSize));
  }
  if (!env.length) return 0;
  let peakIdx = 0, peakVal = -1;
  for (let i = 0; i < env.length; i++) if (env[i] > peakVal) { peakVal = env[i]; peakIdx = i; }
  let startIdx = 0;
  for (let i = peakIdx; i >= 0; i--) { if (env[i] < peakVal * 0.1) { startIdx = i; break; } }
  const frames = Math.max(1, peakIdx - startIdx);
  return (frames * winSize / sr) * 1000;
}

// averaged spectrum -> centroid, rolloff85, flatness; plus onset-to-peak attack time
export function timbreProfile(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 2048;
  const hop = adaptiveHop(data.length, frameSize, 1024, 200);
  const n = nextPow2(frameSize);
  const avgMag = new Float64Array(n / 2);
  let frameCount = 0;
  for (let start = 0; start + frameSize <= data.length; start += hop) {
    const mags = magnitudeSpectrum(data.subarray(start, start + frameSize));
    for (let i = 0; i < avgMag.length; i++) avgMag[i] += mags[i];
    frameCount++;
  }
  if (frameCount === 0) return { centroid: 0, rolloff: 0, flatness: 0, attackMs: 0 };
  for (let i = 0; i < avgMag.length; i++) avgMag[i] /= frameCount;

  let sumMag = 0, sumFreqMag = 0;
  for (let i = 0; i < avgMag.length; i++) {
    const freq = (i * sr) / n;
    sumMag += avgMag[i];
    sumFreqMag += freq * avgMag[i];
  }
  const centroid = sumMag > 0 ? sumFreqMag / sumMag : 0;

  let cum = 0, rolloff = 0;
  const target = 0.85 * sumMag;
  for (let i = 0; i < avgMag.length; i++) {
    cum += avgMag[i];
    if (cum >= target) { rolloff = (i * sr) / n; break; }
  }

  let logSum = 0, count = 0;
  for (let i = 1; i < avgMag.length; i++) { logSum += Math.log(avgMag[i] + 1e-12); count++; }
  const geoMean = Math.exp(logSum / Math.max(1, count));
  const arithMean = sumMag / Math.max(1, count);
  const flatness = arithMean > 0 ? geoMean / arithMean : 0;

  return {
    centroid: Math.round(centroid),
    rolloff: Math.round(rolloff),
    flatness: Math.round(flatness * 1000) / 1000,
    attackMs: Math.round(estimateAttackMs(data, sr))
  };
}

// onset detection via spectral flux in three fixed bands (kick/snare/hat),
// adaptive per-band threshold, fixed GM note mapping
export function transcribeDrums(buf) {
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 1024;
  const hop = adaptiveHop(data.length, frameSize, 256, 20000);
  const n = nextPow2(frameSize);
  const bands = {
    kick: { lo: 30, hi: 150, midi: 36 },
    snare: { lo: 150, hi: 4000, midi: 38 },
    hat: { lo: 4000, hi: 16000, midi: 42 }
  };
  const bandEnergy = { kick: [], snare: [], hat: [] };
  const times = [];
  let prevMags = null;
  for (let start = 0; start + frameSize <= data.length; start += hop) {
    const mags = magnitudeSpectrum(data.subarray(start, start + frameSize));
    times.push(start / sr);
    for (const key in bands) {
      const { lo, hi } = bands[key];
      const i0 = Math.floor((lo * n) / sr), i1 = Math.min(mags.length, Math.ceil((hi * n) / sr));
      let flux = 0, e = 0;
      for (let i = i0; i < i1; i++) {
        e += mags[i];
        if (prevMags) { const d = mags[i] - prevMags[i]; if (d > 0) flux += d; }
      }
      bandEnergy[key].push(prevMags ? flux : e * 0.01);
    }
    prevMags = mags;
  }
  const notes = [];
  for (const key in bands) {
    const energy = bandEnergy[key];
    if (!energy.length) continue;
    let mean = 0; for (const v of energy) mean += v; mean /= energy.length;
    let variance = 0; for (const v of energy) variance += (v - mean) * (v - mean); variance /= energy.length;
    const threshold = mean + Math.sqrt(variance) * 1.5;
    const maxE = maxOf(energy) || 1;
    let lastOnset = -Infinity;
    const minGapSec = 0.06;
    for (let i = 1; i < energy.length - 1; i++) {
      if (energy[i] > threshold && energy[i] >= energy[i - 1] && energy[i] >= energy[i + 1] && times[i] - lastOnset > minGapSec) {
        const vel = Math.max(30, Math.min(127, Math.round((energy[i] / maxE) * 127)));
        notes.push({ time: times[i], dur: 0.1, midi: bands[key].midi, vel });
        lastOnset = times[i];
      }
    }
  }
  notes.sort((a, b) => a.time - b.time);
  return notes;
}

// FFT-based normalized autocorrelation pitch tracking, grouped into note events
export function transcribeMono(buf, opts) {
  const fmin = (opts && opts.fmin) || 60, fmax = (opts && opts.fmax) || 1000;
  const data = toMono(buf);
  const sr = buf.sampleRate;
  const frameSize = 2048;
  const hop = adaptiveHop(data.length, frameSize, 512, 15000);
  const minLag = Math.max(2, Math.floor(sr / fmax));
  const maxLag = Math.min(frameSize - 1, Math.ceil(sr / fmin));
  const frames = [];
  for (let start = 0; start + frameSize <= data.length; start += hop) {
    const frame = data.subarray(start, start + frameSize);
    let energy = 0; for (let i = 0; i < frame.length; i++) energy += frame[i] * frame[i];
    const rms = Math.sqrt(energy / frame.length);
    let freq = 0;
    if (rms > 0.004) {
      const ac = autocorrelateFFT(frame);
      const ac0 = ac[0] || 1e-9;
      let bestLag = -1, bestVal = 0;
      for (let lag = minLag; lag <= maxLag && lag < ac.length; lag++) {
        const val = ac[lag] / ac0;
        if (val > bestVal) { bestVal = val; bestLag = lag; }
      }
      if (bestLag > 0 && bestVal > 0.3) freq = sr / bestLag;
    }
    frames.push({ time: start / sr, freq, energy: rms });
  }

  const notes = [];
  let cur = null;
  const semitoneTol = 0.6;
  const flush = () => { if (cur && cur.frameCount >= 2) notes.push(finalizeNote(cur)); cur = null; };
  for (let i = 0; i < frames.length; i++) {
    const f = frames[i];
    const midi = f.freq > 0 ? freqToMidi(f.freq) : null;
    if (midi != null && cur && Math.abs(midi - cur.avgMidi) <= semitoneTol) {
      cur.sumMidi += midi; cur.frameCount++;
      cur.avgMidi = cur.sumMidi / cur.frameCount;
      cur.end = f.time + hop / sr;
      cur.maxEnergy = Math.max(cur.maxEnergy, f.energy);
    } else {
      flush();
      cur = midi != null ? { start: f.time, end: f.time + hop / sr, avgMidi: midi, sumMidi: midi, frameCount: 1, maxEnergy: f.energy } : null;
    }
  }
  flush();
  return notes;

  function finalizeNote(n) {
    const midi = Math.round(n.avgMidi);
    const vel = Math.max(20, Math.min(127, Math.round(n.maxEnergy * 400)));
    return { time: n.start, dur: Math.max(0.05, n.end - n.start), midi, vel };
  }
}

// timing deviation / spread from a 16th-note grid, velocity range, 8th-note swing
export function humanizeStats(notes, tempo) {
  if (!notes || !notes.length) return { timingDevMs: 0, timingSpreadMs: 0, velMin: 0, velMax: 0, swingPct: 50 };
  const sixteenth = (60 / tempo) / 4;
  const devs = [];
  const oddDevs = [], evenDevs = [];
  let velMin = Infinity, velMax = -Infinity;
  for (const n of notes) {
    const gridPos = n.time / sixteenth;
    const nearest = Math.round(gridPos);
    const devSec = (gridPos - nearest) * sixteenth;
    devs.push(devSec);
    if (nearest % 2 === 0) evenDevs.push(devSec); else oddDevs.push(devSec);
    if (n.vel < velMin) velMin = n.vel;
    if (n.vel > velMax) velMax = n.vel;
  }
  let absMean = 0; for (const d of devs) absMean += Math.abs(d); absMean /= devs.length;
  let mean = 0; for (const d of devs) mean += d; mean /= devs.length;
  let variance = 0; for (const d of devs) variance += (d - mean) * (d - mean); variance /= devs.length;
  const spread = Math.sqrt(variance);
  const avgOdd = oddDevs.length ? oddDevs.reduce((a, b) => a + b, 0) / oddDevs.length : 0;
  const avgEven = evenDevs.length ? evenDevs.reduce((a, b) => a + b, 0) / evenDevs.length : 0;
  const swingPct = Math.max(0, Math.min(100, Math.round(50 + ((avgOdd - avgEven) / sixteenth) * 50)));
  return {
    timingDevMs: Math.round(absMean * 1000 * 10) / 10,
    timingSpreadMs: Math.round(spread * 1000 * 10) / 10,
    velMin: Math.round(velMin), velMax: Math.round(velMax),
    swingPct
  };
}

function clamp01(x) { const v = Number(x); return Number.isFinite(v) ? Math.max(0, Math.min(1, v)) : 0; }

// Real, honest about what it is: a parameterized randomization applied to
// an already-transcribed note list, NOT a learned humanization model --
// the inverse of humanizeStats() above (which *measures* how
// human/quantized a performance already is; this one *adds* controlled
// variation). Each knob is 0..1 (the UI sends a 0-100% slider / 100):
//   timing: per-note timing jitter, scaled to a fraction of a 16th note
//     (using the same tempo-relative grid humanizeStats() itself uses,
//     so the two functions agree on what "one grid unit" means).
//   velocity: per-note velocity jitter, multiplicative, bounded to the
//     real MIDI 1-127 range.
//   dynamics: a slow, whole-phrase velocity contour (a gentle arc,
//     louder mid-phrase) -- deliberately distinct from velocity's
//     per-note noise, so the two controls do visibly different things
//     rather than both just adding more randomness to the same knob.
//   articulation: per-note duration jitter -- shortens/lengthens notes
//     for a staccato/legato feel.
//   preserveGroove: halves the timing jitter's magnitude so small
//     per-note offsets stay closer to each note's own original position
//     rather than letting them compound into audible drift across a long
//     phrase -- keeps the transcribed performance's overall groove/tempo
//     feel intact while still adding real micro-timing variation.
// seed: optional -- same notes + same seed + same amounts always produces
// the same output (a real xorshift32 PRNG, not Math.random()), so a
// result is reproducible on request; omit for a fresh random result each
// call. Returns a NEW array; never mutates the input notes.
export function applyHumanization(notes, opts) {
  if (!notes || !notes.length) return notes || [];
  const timing = clamp01(opts && opts.timing);
  const velocity = clamp01(opts && opts.velocity);
  const dynamics = clamp01(opts && opts.dynamics);
  const articulation = clamp01(opts && opts.articulation);
  const preserveGroove = !!(opts && opts.preserveGroove);
  const tempo = (opts && opts.tempo) || 120;
  const sixteenth = (60 / tempo) / 4;

  let seed = (opts && Number.isFinite(opts.seed)) ? (opts.seed >>> 0) : ((Date.now() ^ Math.floor(Math.random() * 0xffffffff)) >>> 0);
  const rand = () => { // xorshift32 -- fast, deterministic given a seed, no external dependency
    seed ^= seed << 13; seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5; seed >>>= 0;
    return seed / 0xffffffff;
  };
  const signedRand = () => rand() * 2 - 1; // -1..1

  const timingScale = sixteenth * (preserveGroove ? 0.25 : 0.5); // max per-note shift at timing=1.0
  const n = notes.length;

  return notes.map((note, i) => {
    const timingJitter = signedRand() * timing * timingScale;
    const velocityJitter = signedRand() * velocity * 0.35; // up to +-35% at velocity=1.0
    const contour = Math.sin((i / Math.max(1, n - 1)) * Math.PI) * 0.5 + 0.5; // 0..1, peaks mid-phrase
    const dynamicsShape = (contour - 0.5) * dynamics * 0.4;
    const articulationJitter = 1 + signedRand() * articulation * 0.3; // duration scale 0.7x..1.3x at articulation=1.0

    return {
      ...note,
      time: Math.max(0, note.time + timingJitter),
      vel: Math.round(Math.max(1, Math.min(127, note.vel * (1 + velocityJitter + dynamicsShape)))),
      dur: Math.max(0.02, (note.dur || 0.1) * articulationJitter),
    };
  });
}

// -------------------------------------------------------------- exporters --

export function encodeWAV(buf) {
  const numChannels = buf.numberOfChannels;
  const sampleRate = buf.sampleRate;
  const numFrames = buf.length;
  const blockAlign = numChannels * 2;
  const dataSize = numFrames * blockAlign;
  const out = new ArrayBuffer(44 + dataSize);
  const view = new DataView(out);
  const writeStr = (offset, str) => { for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i)); };
  writeStr(0, 'RIFF'); view.setUint32(4, 36 + dataSize, true); writeStr(8, 'WAVE');
  writeStr(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, numChannels, true); view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * blockAlign, true); view.setUint16(32, blockAlign, true); view.setUint16(34, 16, true);
  writeStr(36, 'data'); view.setUint32(40, dataSize, true);
  const channels = []; for (let c = 0; c < numChannels; c++) channels.push(buf.getChannelData(c));
  let offset = 44;
  for (let i = 0; i < numFrames; i++) {
    for (let c = 0; c < numChannels; c++) {
      let s = Math.max(-1, Math.min(1, channels[c][i]));
      s = s < 0 ? s * 0x8000 : s * 0x7FFF;
      view.setInt16(offset, s, true);
      offset += 2;
    }
  }
  return new Blob([out], { type: 'audio/wav' });
}

const PPQ = 480;
function secToTicks(sec, tempo) { return Math.round(sec * (tempo / 60) * PPQ); }
function pushAll(dst, src) { for (let i = 0; i < src.length; i++) dst.push(src[i]); }
function writeVarLen(value, out) {
  const buffer = [value & 0x7f];
  value >>= 7;
  while (value > 0) { buffer.unshift((value & 0x7f) | 0x80); value >>= 7; }
  pushAll(out, buffer);
}
function textMeta(type, str) {
  const bytes = [0xFF, type];
  writeVarLen(str.length, bytes);
  for (let i = 0; i < str.length; i++) bytes.push(str.charCodeAt(i) & 0xFF);
  return bytes;
}
function buildTrackBytes(events) {
  events.sort((a, b) => a.tick - b.tick || ((a.bytes[0] & 0xF0) === 0x80 ? -1 : 1) - ((b.bytes[0] & 0xF0) === 0x80 ? -1 : 1));
  const out = [];
  let lastTick = 0;
  for (const ev of events) { writeVarLen(ev.tick - lastTick, out); pushAll(out, ev.bytes); lastTick = ev.tick; }
  writeVarLen(0, out); out.push(0xFF, 0x2F, 0x00);
  return out;
}
function chunk(id, bytes) {
  const out = [];
  for (let i = 0; i < 4; i++) out.push(id.charCodeAt(i));
  const len = bytes.length;
  out.push((len >>> 24) & 0xFF, (len >>> 16) & 0xFF, (len >>> 8) & 0xFF, len & 0xFF);
  pushAll(out, bytes);
  return out;
}

// Standard Format-1 SMF: a conductor track (tempo + markers) plus one track per stem.
export function encodeMIDITracks(tempo, tracks, markers) {
  const trackChunks = [];

  const conductor = [];
  const usPerBeat = Math.round(60000000 / tempo);
  conductor.push({ tick: 0, bytes: [0xFF, 0x51, 0x03, (usPerBeat >> 16) & 0xFF, (usPerBeat >> 8) & 0xFF, usPerBeat & 0xFF] });
  // Time signature isn't detected (see the UI's own "NOT DETECTED" label) —
  // this is an assumed 4/4 default so the file has a complete, valid time-
  // signature event, not a claim that 4/4 was measured.
  conductor.push({ tick: 0, bytes: [0xFF, 0x58, 0x04, 4, 2, 24, 8] });
  conductor.push({ tick: 0, bytes: textMeta(0x03, 'BeatShore Reconstruction') });
  (markers || []).forEach(m => conductor.push({ tick: secToTicks(m.time, tempo), bytes: textMeta(0x06, m.label) }));
  trackChunks.push(chunk('MTrk', buildTrackBytes(conductor)));

  (tracks || []).forEach((t, idx) => {
    const channel = t.drums ? 9 : Math.min(15, idx + 1);
    const events = [];
    events.push({ tick: 0, bytes: textMeta(0x03, t.name || ('Track ' + (idx + 1))) });
    events.push({ tick: 0, bytes: [0xC0 | channel, 0] });
    (t.notes || []).forEach(n => {
      const startTick = secToTicks(n.time, tempo);
      const endTick = Math.max(startTick + 1, secToTicks(n.time + n.dur, tempo));
      const vel = Math.max(1, Math.min(127, Math.round(n.vel)));
      events.push({ tick: startTick, bytes: [0x90 | channel, n.midi & 0x7F, vel] });
      events.push({ tick: endTick, bytes: [0x80 | channel, n.midi & 0x7F, 0] });
    });
    trackChunks.push(chunk('MTrk', buildTrackBytes(events)));
  });

  const ntrk = trackChunks.length;
  const header = chunk('MThd', [0, 1, (ntrk >> 8) & 0xFF, ntrk & 0xFF, (PPQ >> 8) & 0xFF, PPQ & 0xFF]);
  const all = [];
  pushAll(all, header);
  trackChunks.forEach(c => pushAll(all, c));
  return new Blob([new Uint8Array(all)], { type: 'audio/midi' });
}

function rppEscape(s) { return String(s).replace(/"/g, "'"); }

// Minimal-but-valid plain-text REAPER project: tempo, markers, one audio track
// per stem, each holding its WAV as a single PCM item at position 0. Real
// project depth (routing, FX, per-track state, automation) is out of scope
// for a heuristic export — this is a basic project, not a full one.
export function buildRPP(tempo, markers, stemFiles) {
  const lines = [];
  lines.push('<REAPER_PROJECT 0.1 "6.0/win64" 0');
  lines.push('  TEMPO ' + tempo.toFixed(4) + ' 4 4');
  (markers || []).forEach((m, i) => lines.push('  MARKER ' + (i + 1) + ' ' + m.time.toFixed(6) + ' "' + rppEscape(m.label) + '" 0 0 1'));
  (stemFiles || []).forEach(s => {
    lines.push('  <TRACK');
    lines.push('    NAME "' + rppEscape(s.name) + '"');
    lines.push('    <ITEM');
    lines.push('      POSITION 0');
    lines.push('      LENGTH ' + (s.duration || 0).toFixed(6));
    lines.push('      NAME "' + rppEscape(s.name) + '"');
    lines.push('      <SOURCE WAVE');
    lines.push('        FILE "' + s.path + '"');
    lines.push('      >');
    lines.push('    >');
    lines.push('  >');
  });
  lines.push('>');
  return lines.join('\n');
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    table[n] = c >>> 0;
  }
  return table;
})();
function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) crc = CRC_TABLE[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

// PKZIP archive, STORE method (no compression) — local headers + central directory + EOCD
export async function zipStore(files) {
  const encoder = new TextEncoder();
  const localParts = [], centralParts = [];
  let offset = 0;
  const now = new Date();
  const dosTime = ((now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1)) & 0xFFFF;
  const dosDate = (((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate()) & 0xFFFF;

  for (const f of files) {
    const nameBytes = encoder.encode(f.name);
    const dataBuf = new Uint8Array(await f.blob.arrayBuffer());
    const crc = crc32(dataBuf);
    const size = dataBuf.length;

    const local = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true); lv.setUint16(6, 0, true); lv.setUint16(8, 0, true);
    lv.setUint16(10, dosTime, true); lv.setUint16(12, dosDate, true); lv.setUint32(14, crc, true);
    lv.setUint32(18, size, true); lv.setUint32(22, size, true);
    lv.setUint16(26, nameBytes.length, true); lv.setUint16(28, 0, true);
    local.set(nameBytes, 30);
    localParts.push(local, dataBuf);

    const central = new Uint8Array(46 + nameBytes.length);
    const cv = new DataView(central.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 20, true); cv.setUint16(6, 20, true);
    cv.setUint16(8, 0, true); cv.setUint16(10, 0, true); cv.setUint16(12, dosTime, true); cv.setUint16(14, dosDate, true);
    cv.setUint32(16, crc, true); cv.setUint32(20, size, true); cv.setUint32(24, size, true);
    cv.setUint16(28, nameBytes.length, true); cv.setUint16(30, 0, true); cv.setUint16(32, 0, true);
    cv.setUint16(34, 0, true); cv.setUint16(36, 0, true); cv.setUint32(38, 0, true); cv.setUint32(42, offset, true);
    central.set(nameBytes, 46);
    centralParts.push(central);

    offset += local.length + dataBuf.length;
  }

  const centralStart = offset;
  let centralSize = 0;
  for (const c of centralParts) centralSize += c.length;

  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(4, 0, true); ev.setUint16(6, 0, true);
  ev.setUint16(8, files.length, true); ev.setUint16(10, files.length, true);
  ev.setUint32(12, centralSize, true); ev.setUint32(16, centralStart, true); ev.setUint16(20, 0, true);

  return new Blob([...localParts, ...centralParts, end], { type: 'application/zip' });
}

export function download(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
