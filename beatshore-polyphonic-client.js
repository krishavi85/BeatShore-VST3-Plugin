// Main-thread facade for real polyphonic transcription (basic-pitch running
// locally via TensorFlow.js — see beatshore-polyphonic-worker.js for why).
// A single worker, not a pool: this is a rare, heavy, explicitly-triggered
// operation (once per piano/guitar stem), not a per-call dispatch pattern
// like the heuristic DSP pool.

let worker = null;
let nextId = 1;
const pending = new Map();

function getWorker() {
  if (worker) return worker;
  worker = new Worker(new URL('beatshore-polyphonic-worker.js', import.meta.url)); // classic worker — importScripts() needs it
  worker.onmessage = (e) => {
    const { id, ok, notes, error, progress } = e.data;
    const p = pending.get(id);
    if (!p) return;
    if (progress != null && ok == null) { if (p.onProgress) p.onProgress(progress); return; }
    pending.delete(id);
    if (ok) p.resolve(notes); else p.reject(new Error(error));
  };
  worker.onerror = (err) => {
    const failure = new Error((err && err.message) || 'Polyphonic transcription worker error');
    for (const p of pending.values()) p.reject(failure);
    pending.clear();
    try { worker.terminate(); } catch (e) {}
    worker = null;
  };
  return worker;
}

// basic-pitch requires exactly 22050Hz mono. Resampling needs Web Audio's
// OfflineAudioContext, which isn't available inside a Worker, so it happens
// here on the main thread before handing plain sample data off to the worker.
async function resampleTo22050Mono(buf) {
  const targetRate = 22050;
  const targetLength = Math.ceil(buf.duration * targetRate);
  const offlineCtx = new OfflineAudioContext(1, targetLength, targetRate);
  const src = offlineCtx.createBufferSource();
  src.buffer = buf;
  src.connect(offlineCtx.destination);
  src.start(0);
  const rendered = await offlineCtx.startRendering();
  return rendered.getChannelData(0).slice(); // detach from the rendered buffer before transfer
}

export async function transcribePolyphonic(buf, onProgress) {
  const audio = await resampleTo22050Mono(buf);
  const w = getWorker();
  const id = nextId++;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject, onProgress });
    w.postMessage({ id, audio }, [audio.buffer]);
  });
}

export function terminate() {
  if (!worker) return;
  const failure = new Error('Polyphonic transcription worker terminated');
  for (const p of pending.values()) p.reject(failure);
  pending.clear();
  try { worker.terminate(); } catch (e) {}
  worker = null;
}
