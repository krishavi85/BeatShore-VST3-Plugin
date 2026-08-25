// Optional integration with the local BeatShore FastAPI backend
// (BeatShore_Reverse_Studio_Fluid_Dashboard_Fix/backend — Demucs separation +
// librosa/pYIN transcription). The plugin works fully offline without this
// file ever being called; every export here is opt-in from the UI.
//
// The backend has no auth today and is meant for localhost only — this client
// never talks to a non-loopback host, and every call goes through fetch with
// an explicit timeout so a stalled backend can't hang the plugin UI.

export const DEFAULT_BASE_URL = 'http://127.0.0.1:8765';

async function req(baseUrl, path, opts, timeoutMs) {
  const ctrl = new AbortController();
  const timer = timeoutMs ? setTimeout(() => ctrl.abort(), timeoutMs) : null;
  try {
    const res = await fetch((baseUrl || DEFAULT_BASE_URL) + path, Object.assign({ signal: ctrl.signal }, opts));
    if (!res.ok) {
      let detail = '';
      try { detail = JSON.stringify(await res.json()); } catch (e) {}
      throw new Error('backend ' + res.status + ' on ' + path + (detail ? ': ' + detail : ''));
    }
    return res;
  } catch (err) {
    if (err && err.name === 'AbortError') throw new Error('backend request to ' + path + ' timed out');
    throw err;
  } finally {
    if (timer) clearTimeout(timer);
  }
}

export async function checkHealth(baseUrl, timeoutMs) {
  try {
    const res = await req(baseUrl, '/api/health', {}, timeoutMs || 4000);
    const j = await res.json();
    return { online: true, version: j.version, processing: j.processing };
  } catch (err) {
    return { online: false, error: err.message };
  }
}

export async function createProject(baseUrl, file) {
  const fd = new FormData();
  fd.append('file', file, file.name);
  const res = await req(baseUrl, '/api/projects', { method: 'POST', body: fd }, 60000);
  return res.json(); // { id, name, source_file, status, ... }
}

// Demucs runs synchronously on the server today (no job queue yet), so this
// can legitimately take minutes on a long file — there's no progress to
// report mid-flight, only success or failure at the end.
export async function separate(baseUrl, pid, quality) {
  const q = encodeURIComponent(quality || 'balanced');
  const res = await req(baseUrl, '/api/projects/' + pid + '/separate?quality=' + q, { method: 'POST' }, 30 * 60 * 1000);
  return res.json(); // { stems: [{ name, file, size, engine }], engine }
}

export async function transcribe(baseUrl, pid) {
  const res = await req(baseUrl, '/api/projects/' + pid + '/transcribe', { method: 'POST' }, 5 * 60 * 1000);
  return res.json(); // { tracks: [{ stem, file, notes: <count>, method } | { stem, error }] }
}

export async function fetchStemAudio(baseUrl, pid, kind) {
  const res = await req(baseUrl, '/api/projects/' + pid + '/audio/' + encodeURIComponent(kind), {}, 60000);
  return res.blob();
}

// Normalizes the backend's { track, name, pitch, start, end, velocity, drum }
// note shape into the { time, dur, midi, vel } shape beatshore-dsp.js and the
// plugin UI already use everywhere else.
export async function fetchNotes(baseUrl, pid, midiFile) {
  const bare = String(midiFile).split('/').pop();
  const res = await req(baseUrl, '/api/projects/' + pid + '/midi/' + encodeURIComponent(bare) + '/notes', {}, 30000);
  const raw = await res.json();
  return raw.map(n => ({ time: n.start, dur: Math.max(0.02, n.end - n.start), midi: n.pitch, vel: n.velocity }));
}
