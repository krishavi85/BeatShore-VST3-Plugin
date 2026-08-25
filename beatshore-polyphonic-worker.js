// Real polyphonic pitch/note detection — runs Spotify's "basic-pitch"
// pretrained CNN (https://github.com/spotify/basic-pitch, Apache License
// 2.0) locally via TensorFlow.js, entirely offline (weights vendored in
// vendor/basic-pitch-model/, no network call). This is what piano/guitar
// transcription actually needs and neither the JS heuristic DSP nor the
// FastAPI backend's pYIN transcription provide — pYIN and the autocorrelation
// pitch tracker in beatshore-dsp.js are both fundamentally monophonic.
//
// Runs on the 'cpu' backend deliberately, not WebGL: WebGL needs a canvas,
// which a plain Worker doesn't have, and this is what makes it possible to
// keep the heavy inference off the main thread at all. That's a real
// tradeoff — slower than GPU inference — made explicitly, not hidden.
//
// Classic (non-module) worker so tfjs's UMD build and the decode helpers can
// load via importScripts() without a bundler.
importScripts('vendor/tfjs.min.js', 'beatshore-polyphonic-decode.js');

const OUTPUT_TO_TENSOR_NAME = { frames: 'Identity_1', onsets: 'Identity_2', contours: 'Identity' };
const AUDIO_SAMPLE_RATE = 22050;
const FFT_HOP = 256;
const ANNOTATIONS_FPS = Math.floor(AUDIO_SAMPLE_RATE / FFT_HOP);
const AUDIO_WINDOW_LENGTH_SECONDS = 2;
const AUDIO_N_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_WINDOW_LENGTH_SECONDS - FFT_HOP;
const N_OVERLAPPING_FRAMES = 30;
const N_OVERLAP_OVER_2 = Math.floor(N_OVERLAPPING_FRAMES / 2);
const OVERLAP_LENGTH_FRAMES = N_OVERLAPPING_FRAMES * FFT_HOP;
const HOP_SIZE = AUDIO_N_SAMPLES - OVERLAP_LENGTH_FRAMES;

let modelPromise = null;
function getModel() {
  if (!modelPromise) {
    tf.setBackend('cpu');
    modelPromise = tf.loadGraphModel('vendor/basic-pitch-model/model.json');
  }
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
    const singleBatch = tf.slice(reshapedInput, [i, 0, 0], [1, -1, -1]); // reshapedInput is rank 3: [batch, samples, 1]
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

self.onmessage = async (e) => {
  const { id, audio } = e.data; // audio: Float32Array at 22050Hz mono
  try {
    const model = await getModel();
    const { frames, onsets } = await evaluateModel(model, audio, pct => self.postMessage({ id, progress: pct }));
    const rawNotes = BeatShorePolyDecode.outputToNotesPoly(frames, onsets);
    const timedNotes = BeatShorePolyDecode.noteFramesToTime(rawNotes);
    const notes = timedNotes
      .filter(n => n.durationSeconds > 0)
      .map(n => ({
        time: n.startTimeSeconds,
        dur: n.durationSeconds,
        midi: n.pitchMidi,
        vel: Math.max(1, Math.min(127, Math.round(n.amplitude * 127)))
      }))
      .sort((a, b) => a.time - b.time);
    self.postMessage({ id, ok: true, notes });
  } catch (err) {
    self.postMessage({ id, ok: false, error: (err && err.message) || String(err) });
  }
};
