// Runs Spotify's "basic-pitch" pretrained CNN
// (https://github.com/spotify/basic-pitch, Apache License 2.0) via
// @tensorflow/tfjs-node against the SAME vendored model weights the browser
// prototype uses (vendor/basic-pitch-model/), reusing the tfjs API calls
// from beatshore-polyphonic-worker.js nearly verbatim -- tfjs's public API
// (tf.tensor, tf.signal.frame, model.execute, ...) is identical between the
// browser build and tfjs-node, only the backend and model-loading I/O
// differ. See basic-pitch-decode.js for why the post-processing math is a
// documented duplicate rather than a shared import.
//
// Deliberately does NOT call tf.setBackend(): tfjs-node registers its own
// native 'tensorflow' backend (real compiled TensorFlow, not the pure-JS
// fallback the browser's 'cpu' backend is) as the default the moment
// @tensorflow/tfjs-node is imported. Forcing 'cpu' here (as the browser
// worker does, for its own good reason -- no WebGL/canvas in a Worker)
// would throw away the whole point of using tfjs-node.
// tfjs-node 4.22.0 (published for older Node releases) calls the
// long-deprecated node:util.isNullOrUndefined, which Node v25 has fully
// removed (present but throwing "not a function" is what a real run showed
// here, confirming this isn't a hypothetical). It's a one-line function
// (`v === null || v === undefined`) that Node itself shipped and documented
// for years before deprecating it -- restoring it is a narrow, well-
// understood polyfill for a specific removed API, not a behavior change.
// Must run before any tfjs-node kernel executes (module import order is
// fine since this patches the shared node:util module object at runtime,
// not at parse time), so it's the first thing in this file.
import util from 'node:util';
if (typeof util.isNullOrUndefined !== 'function') {
  util.isNullOrUndefined = (v) => v === null || v === undefined;
}

import * as tf from '@tensorflow/tfjs-node';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { outputToNotesPoly, noteFramesToTime, AUDIO_SAMPLE_RATE } from './basic-pitch-decode.js';

const OUTPUT_TO_TENSOR_NAME = { frames: 'Identity_1', onsets: 'Identity_2', contours: 'Identity' };
const FFT_HOP = 256;
const ANNOTATIONS_FPS = Math.floor(AUDIO_SAMPLE_RATE / FFT_HOP);
const AUDIO_WINDOW_LENGTH_SECONDS = 2;
const AUDIO_N_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_WINDOW_LENGTH_SECONDS - FFT_HOP;
const N_OVERLAPPING_FRAMES = 30;
const N_OVERLAP_OVER_2 = Math.floor(N_OVERLAPPING_FRAMES / 2);
const OVERLAP_LENGTH_FRAMES = N_OVERLAPPING_FRAMES * FFT_HOP;
const HOP_SIZE = AUDIO_N_SAMPLES - OVERLAP_LENGTH_FRAMES;

const engineDir = dirname(fileURLToPath(import.meta.url));
// ../../../vendor/basic-pitch-model relative to this file, matching the
// same vendored weights native/BeatShoreDesktop/engine/analyze.js already
// reuses beatshore-dsp.js from.
const modelPath = join(engineDir, '..', '..', '..', 'vendor', 'basic-pitch-model', 'model.json');

let modelPromise = null;
function getModel() {
  if (!modelPromise) modelPromise = tf.loadGraphModel('file://' + modelPath.replace(/\\/g, '/'));
  return modelPromise;
}

function prepareData(singleChannelAudioData) {
  const wavSamples = tf.concat1d([tf.zeros([Math.floor(OVERLAP_LENGTH_FRAMES / 2)], 'float32'), tf.tensor(singleChannelAudioData)]);
  const framed = tf.expandDims(tf.signal.frame(wavSamples, AUDIO_N_SAMPLES, HOP_SIZE, true, 0), -1);
  wavSamples.dispose();
  return [framed, singleChannelAudioData.length];
}

function unwrapOutput(result) {
  const raw = result.slice([0, N_OVERLAP_OVER_2, 0], [-1, result.shape[1] - 2 * N_OVERLAP_OVER_2, -1]);
  const shape = raw.shape;
  const reshaped = raw.reshape([shape[0] * shape[1], shape[2]]);
  raw.dispose();
  return reshaped;
}

async function evaluateModel(model, singleChannelAudioData, onProgress) {
  const [reshapedInput, audioOriginalLength] = prepareData(singleChannelAudioData);
  const nOutputFramesOriginal = Math.floor(audioOriginalLength * (ANNOTATIONS_FPS / AUDIO_SAMPLE_RATE));
  let calculatedFrames = 0;
  const allFrames = [], allOnsets = [];
  const nBatches = reshapedInput.shape[0];
  for (let i = 0; i < nBatches; ++i) {
    onProgress(i / nBatches);
    const singleBatch = tf.slice(reshapedInput, [i, 0, 0], [1, -1, -1]);
    const results = model.execute(singleBatch, [OUTPUT_TO_TENSOR_NAME.frames, OUTPUT_TO_TENSOR_NAME.onsets, OUTPUT_TO_TENSOR_NAME.contours]);
    singleBatch.dispose();
    let unwrappedFrames = unwrapOutput(results[0]);
    let unwrappedOnsets = unwrapOutput(results[1]);
    results[0].dispose(); results[1].dispose(); results[2].dispose();

    const calculatedFramesTmp = unwrappedFrames.shape[0];
    if (calculatedFrames < nOutputFramesOriginal) {
      if (calculatedFramesTmp + calculatedFrames >= nOutputFramesOriginal) {
        const framesToOutput = nOutputFramesOriginal - calculatedFrames;
        const f = unwrappedFrames.slice([0, 0], [framesToOutput, -1]);
        const o = unwrappedOnsets.slice([0, 0], [framesToOutput, -1]);
        unwrappedFrames.dispose(); unwrappedOnsets.dispose();
        unwrappedFrames = f; unwrappedOnsets = o;
      }
      calculatedFrames += calculatedFramesTmp;
      const framesArr = await unwrappedFrames.array();
      const onsetsArr = await unwrappedOnsets.array();
      for (const row of framesArr) allFrames.push(row);
      for (const row of onsetsArr) allOnsets.push(row);
    }
    unwrappedFrames.dispose(); unwrappedOnsets.dispose();
  }
  reshapedInput.dispose();
  onProgress(1);
  return { frames: allFrames, onsets: allOnsets };
}

// audio22050Mono: Float32Array, already resampled to mono 22050Hz (see
// resample.js -- this module doesn't resample, that's the caller's job).
export async function transcribePolyphonic(audio22050Mono, onProgress) {
  const model = await getModel();
  const { frames, onsets } = await evaluateModel(model, audio22050Mono, onProgress || (() => {}));
  const rawNotes = outputToNotesPoly(frames, onsets);
  const timedNotes = noteFramesToTime(rawNotes);
  return timedNotes
    .filter(n => n.durationSeconds > 0)
    .map(n => ({
      time: n.startTimeSeconds,
      dur: n.durationSeconds,
      midi: n.pitchMidi,
      vel: Math.max(1, Math.min(127, Math.round(n.amplitude * 127)))
    }))
    .sort((a, b) => a.time - b.time);
}
