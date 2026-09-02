// Long-lived child process spawned by BeatShoreDesktop.exe. Speaks NDJSON on
// stdin/stdout. Reuses beatshore-dsp.js UNMODIFIED from the plugin window
// project (the same module the browser worker pool already uses and this
// session already verified against real audio) — the reconstruction logic
// itself isn't reimplemented here, only wrapped for a different transport.
//
// transcribePolyphonic (piano/guitar) uses @tensorflow/tfjs-node against
// the same vendored basic-pitch weights the browser prototype uses -- see
// basic-pitch-model.js and basic-pitch-decode.js. tfjs-node is imported
// lazily (dynamic import inside handle(), not at module load) so every
// other analysis kind's startup time and memory footprint is unaffected by
// a ~200MB native TensorFlow library most requests never touch.

import * as dsp from '../../../beatshore-dsp.js';
import { readFileSync } from 'node:fs';
import readline from 'node:readline';
import { toMono, resampleMono } from './resample.js';
import { writeMidiFile } from './midi-export.js';

function loadAudioBuffer(path) {
  const buf = readFileSync(path);
  if (buf.toString('ascii', 0, 4) !== 'BSM1') throw new Error('bad audio file magic (expected BSM1)');
  const sampleRate = buf.readUInt32LE(4);
  const channels = buf.readUInt32LE(8);
  const frames = buf.readUInt32LE(12);
  const dataOffset = 16;
  const expectedBytes = dataOffset + frames * channels * 4;
  if (buf.length < expectedBytes) throw new Error('audio file truncated: have ' + buf.length + ' bytes, need ' + expectedBytes);

  const channelData = [];
  for (let c = 0; c < channels; c++) channelData.push(new Float32Array(frames));
  for (let i = 0; i < frames; i++) {
    const base = dataOffset + i * channels * 4;
    for (let c = 0; c < channels; c++) channelData[c][i] = buf.readFloatLE(base + c * 4);
  }
  return {
    sampleRate, length: frames, numberOfChannels: channels,
    getChannelData: (c) => channelData[c]
  };
}

function send(obj) { process.stdout.write(JSON.stringify(obj) + '\n'); }

const SUPPORTED_KINDS = ['loudness', 'tempo', 'key', 'structure', 'chords', 'timbre', 'transcribeDrums', 'transcribeMono', 'transcribePolyphonic'];

// Names the actual function being called, not a marketing label -- shown
// back through ANALYSIS_RESULT.algorithm so a result can always be traced
// to the exact code path that produced it. No confidence score is included
// anywhere here: none of these functions compute one (estimateTempo's
// internal autocorrelation score isn't normalized/returned), and reporting
// a fabricated confidence would be worse than omitting the field.
const ALGORITHMS = {
  loudness: 'beatshore-dsp.loudness (RMS/peak)',
  tempo: 'beatshore-dsp.estimateTempo (spectral-flux autocorrelation)',
  key: 'beatshore-dsp.estimateKey (Krumhansl-Schmuckler)',
  structure: 'beatshore-dsp.analyzeStructure (self-similarity segmentation)',
  chords: 'beatshore-dsp.chordTrack (chroma template matching)',
  timbre: 'beatshore-dsp.timbreProfile (spectral centroid/rolloff/flatness)',
  transcribeDrums: 'beatshore-dsp.transcribeDrums (onset detection + band split)',
  transcribeMono: 'beatshore-dsp.transcribeMono (monophonic pitch track)',
  transcribePolyphonic: 'basic-pitch CNN (Spotify, Apache-2.0) via @tensorflow/tfjs-node',
};

async function handle(req) {
  const { requestId, kind, audioFile, role, hostTrackName,
          humanizeTiming, humanizeVelocity, humanizeDynamics, humanizeArticulation, preserveGroove } = req;
  if (SUPPORTED_KINDS.indexOf(kind) < 0) {
    send({ type: 'ERROR', requestId, errorCode: 'UNSUPPORTED_KIND', message: "unsupported analysis kind '" + kind + "' (desktop engine v1 supports: " + SUPPORTED_KINDS.join(', ') + ')' });
    return;
  }

  let buf;
  try {
    buf = loadAudioBuffer(audioFile);
  } catch (err) {
    send({ type: 'ERROR', requestId, errorCode: 'AUDIO_LOAD_FAILED', message: (err && err.message) || String(err) });
    return;
  }
  send({ type: 'ANALYSIS_PROGRESS', requestId, progress: 0.1 });

  const startedAtMs = Date.now();
  try {
    if (kind === 'transcribeDrums' || kind === 'transcribeMono' || kind === 'transcribePolyphonic') {
      let notes;
      if (kind === 'transcribeDrums') {
        notes = dsp.transcribeDrums(buf);
      } else if (kind === 'transcribeMono') {
        notes = dsp.transcribeMono(buf, role === 'bass' ? { fmin: 35, fmax: 400 } : { fmin: 80, fmax: 1200 });
      } else {
        const channelData = [];
        for (let c = 0; c < buf.numberOfChannels; c++) channelData.push(buf.getChannelData(c));
        const mono = toMono(channelData);
        const resampled = resampleMono(mono, buf.sampleRate, 22050);
        // Lazy: only requests that actually need it pay tfjs-node's load cost.
        const { transcribePolyphonic } = await import('./basic-pitch-model.js');
        notes = await transcribePolyphonic(resampled, (pct) => send({ type: 'ANALYSIS_PROGRESS', requestId, progress: 0.1 + pct * 0.75 }));
      }

      const tempo = req.tempo || 120;

      // Real, parameterized note-level randomization (dsp.applyHumanization,
      // see its own comment for exactly what each knob does) -- only run
      // when at least one amount is actually nonzero, so a request that
      // doesn't ask for it costs nothing extra and behaves exactly as
      // before this feature existed. Applied here, before humanizeStats()
      // below and before the MIDI file is written, so both the reported
      // stats and the exported file reflect the humanized result, not the
      // raw transcription.
      const humanizeAmounts = {
        timing: humanizeTiming || 0, velocity: humanizeVelocity || 0,
        dynamics: humanizeDynamics || 0, articulation: humanizeArticulation || 0,
      };
      const humanizeRequested = Object.values(humanizeAmounts).some((v) => v > 0);
      if (humanizeRequested && notes.length) {
        notes = dsp.applyHumanization(notes, { ...humanizeAmounts, preserveGroove: !!preserveGroove, tempo });
      }

      const computeMs = Date.now() - startedAtMs;
      const hum = notes.length ? dsp.humanizeStats(notes, tempo) : null;

      const result = {
        type: 'MIDI_RESULT', requestId, algorithm: ALGORITHMS[kind], computeMs, noteCount: notes.length, notes,
        humanize: hum, humanizeApplied: humanizeRequested,
      };
      if (notes.length) {
        send({ type: 'ANALYSIS_PROGRESS', requestId, progress: 0.95 });
        try {
          // trackName here is the MIDI file's internal track-name meta
          // event (dsp.encodeMIDITracks) -- a different concept from
          // hostTrackName, which is the DAW's own name for the plugin's
          // track, used below only for the exported *filename*.
          const trackName = kind === 'transcribeDrums' ? 'Drums' : (role || 'Transcription');
          const { path, sha256, sizeBytes, generatedAt } = await writeMidiFile(tempo, [{ name: trackName, notes, drums: kind === 'transcribeDrums' }], kind, requestId, hostTrackName);
          result.midiPath = path;
          result.sha256 = sha256;
          result.midiSizeBytes = sizeBytes;
          result.midiGeneratedAt = generatedAt;
        } catch (err) {
          // A failed MIDI write shouldn't discard a successful transcription
          // -- the plugin still gets real notes back, just no file. Report
          // the write failure honestly rather than silently omitting it.
          result.midiWriteError = (err && err.message) || String(err);
        }
      }
      send(result);
      return;
    }

    let result;
    if (kind === 'loudness') result = dsp.loudness(buf);
    else if (kind === 'tempo') result = dsp.estimateTempo(buf);
    else if (kind === 'key') result = dsp.estimateKey(buf);
    else if (kind === 'structure') result = dsp.analyzeStructure(buf);
    else if (kind === 'chords') result = dsp.chordTrack(buf);
    else if (kind === 'timbre') result = dsp.timbreProfile(buf);

    const computeMs = Date.now() - startedAtMs;
    send({ type: 'ANALYSIS_RESULT', requestId, kind, result, algorithm: ALGORITHMS[kind], computeMs });
  } catch (err) {
    send({ type: 'ERROR', requestId, errorCode: 'ANALYSIS_FAILED', message: (err && err.message) || String(err) });
  }
}

// Last-resort safety nets. handle() is declared async but has no `await`s,
// so any throw inside it becomes an unhandled promise rejection rather than
// a synchronous exception at the `handle(req)` call site below -- and on
// Node 15+, an unhandled rejection crashes the process by default. Every
// risky operation inside handle() is already individually try/caught, but
// this exists so a gap in that coverage degrades to a logged ERROR instead
// of the whole engine silently dying (which is exactly what BeatShoreDesktop
// observed once and had no diagnostic for: a NODE_WRITE_FAILED on the next
// request, with nothing explaining why Node was gone).
process.on('unhandledRejection', (err) => {
  send({ type: 'ERROR', errorCode: 'INTERNAL_ERROR', message: 'unhandled rejection: ' + ((err && err.stack) || String(err)) });
});
process.on('uncaughtException', (err) => {
  send({ type: 'ERROR', errorCode: 'INTERNAL_ERROR', message: 'uncaught exception: ' + ((err && err.stack) || String(err)) });
});

const rl = readline.createInterface({ input: process.stdin, terminal: false });
rl.on('line', (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  let req;
  try { req = JSON.parse(trimmed); } catch (e) { send({ type: 'ERROR', errorCode: 'BAD_REQUEST_JSON', message: 'bad json line: ' + e.message }); return; }
  handle(req).catch((err) => {
    send({ type: 'ERROR', requestId: req && req.requestId, errorCode: 'INTERNAL_ERROR', message: 'handle() rejected: ' + ((err && err.stack) || String(err)) });
  });
});

send({ type: 'READY', supportedKinds: SUPPORTED_KINDS });
