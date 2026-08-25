// Main-thread facade for beatshore-dsp.js. Same function names and argument
// order as the raw module, but every call (except download, which needs the
// DOM) is dispatched into a pool of beatshore-dsp-worker.js instances and
// returns a Promise, so the FFT/autocorrelation/transcription work never
// blocks the UI thread — and independent calls (e.g. one per stem) can run
// on separate cores instead of queueing behind each other.

const POOL_SIZE = Math.max(1, Math.min((typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4, 8));

let pool = null; // Array<{ worker, index, inFlight }>
let nextId = 1;
const pending = new Map(); // id -> { resolve, reject, workerIndex }

function spawnWorkerEntry(index) {
  const w = new Worker(new URL('beatshore-dsp-worker.js', import.meta.url), { type: 'module' });
  const entry = { worker: w, index, inFlight: 0 };
  w.onmessage = (e) => {
    const { id, ok, result, error } = e.data;
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    entry.inFlight = Math.max(0, entry.inFlight - 1);
    if (ok) p.resolve(result); else p.reject(new Error(error));
  };
  w.onerror = (err) => {
    // Only the calls in flight on THIS worker fail — the rest of the pool keeps going.
    const failure = new Error((err && err.message) || 'BeatShore DSP worker error');
    for (const [id, p] of pending) {
      if (p.workerIndex === index) { pending.delete(id); p.reject(failure); }
    }
    try { entry.worker.terminate(); } catch (e) {}
    pool[index] = spawnWorkerEntry(index); // respawn so the pool stays at full size
  };
  return entry;
}

function getPool() {
  if (pool) return pool;
  pool = [];
  for (let i = 0; i < POOL_SIZE; i++) pool.push(spawnWorkerEntry(i));
  return pool;
}

// least-busy dispatch — keeps concurrent stem/file analyses spread across cores
function pickWorker() {
  const p = getPool();
  let best = p[0];
  for (const entry of p) if (entry.inFlight < best.inFlight) best = entry;
  return best;
}

export function poolSize() { return POOL_SIZE; }

// Terminates every worker in the pool and fails any calls still in flight.
// Not required for correctness (the tab closing does this for free) but keeps
// long-lived sessions tidy if the host app wants to call it on unmount.
export function terminate() {
  if (!pool) return;
  const failure = new Error('BeatShore DSP pool terminated');
  for (const p of pending.values()) p.reject(failure);
  pending.clear();
  for (const entry of pool) { try { entry.worker.terminate(); } catch (e) {} }
  pool = null;
}

function isAudioBufferLike(a) {
  return !!a && typeof a.getChannelData === 'function' && typeof a.numberOfChannels === 'number';
}

// Copies each channel out of the AudioBuffer (the original must stay intact —
// it's still needed for playback) and marks the copies as transferable so the
// hand-off to the worker is a zero-copy ownership move, not a structured clone.
function packArgs(args) {
  const transfer = [];
  const packed = args.map(a => {
    if (!isAudioBufferLike(a)) return a;
    const numberOfChannels = a.numberOfChannels;
    const channelData = [];
    for (let c = 0; c < numberOfChannels; c++) {
      const copy = new Float32Array(a.getChannelData(c));
      channelData.push(copy);
      transfer.push(copy.buffer);
    }
    return { __bsBuffer: true, sampleRate: a.sampleRate, length: a.length, numberOfChannels, channelData };
  });
  return { packed, transfer };
}

function call(op, args) {
  const entry = pickWorker();
  const { packed, transfer } = packArgs(args);
  const id = nextId++;
  entry.inFlight++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject, workerIndex: entry.index });
    entry.worker.postMessage({ id, op, args: packed }, transfer);
  });
}

export const loudness = (buf) => call('loudness', [buf]);
export const computePeaks = (buf, n) => call('computePeaks', [buf, n]);
export const estimateTempo = (buf) => call('estimateTempo', [buf]);
export const estimateKey = (buf) => call('estimateKey', [buf]);
export const analyzeStructure = (buf) => call('analyzeStructure', [buf]);
export const chordTrack = (buf) => call('chordTrack', [buf]);
export const timbreProfile = (buf) => call('timbreProfile', [buf]);
export const transcribeDrums = (buf) => call('transcribeDrums', [buf]);
export const transcribeMono = (buf, opts) => call('transcribeMono', [buf, opts]);
export const humanizeStats = (notes, tempo) => call('humanizeStats', [notes, tempo]);
export const encodeWAV = (buf) => call('encodeWAV', [buf]);
export const encodeMIDITracks = (tempo, tracks, markers) => call('encodeMIDITracks', [tempo, tracks, markers]);
export const buildRPP = (tempo, markers, wavRelPath, duration) => call('buildRPP', [tempo, markers, wavRelPath, duration]);
export const zipStore = (files) => call('zipStore', [files]);

// Must run on the main thread — it touches document/DOM, which a worker can't reach.
export function download(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
