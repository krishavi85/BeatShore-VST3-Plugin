// Polyphonic note decoding -- adapted from Spotify's "basic-pitch" project
// (https://github.com/spotify/basic-pitch, Apache License 2.0, see
// vendor/basic-pitch-model/LICENSE-basic-pitch-apache-2.0.txt). Pure
// post-processing math (turning the model's raw frame/onset tensors into
// discrete notes) -- no ML code of its own.
//
// This is a DELIBERATE, DOCUMENTED DUPLICATE of ../../../beatshore-polyphonic-decode.js,
// not an independent reimplementation and not a silent fork. The browser
// version has to stay a classic (non-module) script so it can load via
// importScripts() next to tfjs's UMD build in a Worker with no bundler;
// this desktop engine is an ES module (see package.json's "type") and
// can't import a classic script's global-scope assignment. Splitting the
// difference (converting the browser file to a real ES module) would break
// the already-verified browser worker. If this math is ever fixed, both
// copies need the same fix -- check ../../../beatshore-polyphonic-decode.js too.
const MIDI_OFFSET = 21;
const AUDIO_SAMPLE_RATE = 22050;
const AUDIO_WINDOW_LENGTH = 2;
const FFT_HOP = 256;
const ANNOTATIONS_FPS = Math.floor(AUDIO_SAMPLE_RATE / FFT_HOP);
const ANNOT_N_FRAMES = ANNOTATIONS_FPS * AUDIO_WINDOW_LENGTH;
const AUDIO_N_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_WINDOW_LENGTH - FFT_HOP;
const WINDOW_OFFSET = (FFT_HOP / AUDIO_SAMPLE_RATE) * (ANNOT_N_FRAMES - AUDIO_N_SAMPLES / FFT_HOP) + 0.0018;
const MAX_FREQ_IDX = 87;

const hzToMidi = hz => 12 * (Math.log2(hz) - Math.log2(440.0)) + 69;
const modelFrameToTime = frame => (frame * FFT_HOP) / AUDIO_SAMPLE_RATE - WINDOW_OFFSET * Math.floor(frame / ANNOT_N_FRAMES);

function argMax(arr) {
  return arr.length === 0 ? null : arr.reduce((maxIndex, currentValue, index) => (arr[maxIndex] > currentValue ? maxIndex : index), -1);
}
function whereGreaterThanAxis1(arr2d, threshold) {
  const outputX = [], outputY = [];
  for (let i = 0; i < arr2d.length; i++) for (let j = 0; j < arr2d[i].length; j++) if (arr2d[i][j] > threshold) { outputX.push(i); outputY.push(j); }
  return [outputX, outputY];
}
function meanStdDev(array) {
  const [sum, sumSquared, count] = array.reduce((prev, row) => {
    const [rowSum, rowSumsSquared, rowCount] = row.reduce((p, value) => [p[0] + value, p[1] + value * value, p[2] + 1], [0, 0, 0]);
    return [prev[0] + rowSum, prev[1] + rowSumsSquared, prev[2] + rowCount];
  }, [0, 0, 0]);
  const mean = sum / count;
  const std = Math.sqrt((1 / (count - 1)) * (sumSquared - (sum * sum) / count));
  return [mean, std];
}
function globalMax(array) { return array.reduce((prev, row) => Math.max(prev, ...row), 0); }
function min3dForAxis0(array) {
  const minArray = array[0].map(v => v.slice());
  for (let x = 1; x < array.length; ++x) for (let y = 0; y < array[0].length; ++y) for (let z = 0; z < array[0][0].length; ++z) minArray[y][z] = Math.min(minArray[y][z], array[x][y][z]);
  return minArray;
}
function max3dForAxis0(array) {
  const maxArray = array[0].map(v => v.slice());
  for (let x = 1; x < array.length; ++x) for (let y = 0; y < array[0].length; ++y) for (let z = 0; z < array[0][0].length; ++z) maxArray[y][z] = Math.max(maxArray[y][z], array[x][y][z]);
  return maxArray;
}
function argRelMax(array, order) {
  order = order || 1;
  const result = [];
  for (let col = 0; col < array[0].length; ++col) {
    for (let row = 0; row < array.length; ++row) {
      let isRelMax = true;
      for (let cmp = Math.max(0, row - order); isRelMax && cmp <= Math.min(array.length - 1, row + order); ++cmp) {
        if (cmp !== row) isRelMax = isRelMax && array[row][col] > array[cmp][col];
      }
      if (isRelMax) result.push([row, col]);
    }
  }
  return result;
}
function isNotNull(t) { return t !== null; }
function constrainFrequency(onsets, frames, maxFreq, minFreq) {
  if (maxFreq) { const i = hzToMidi(maxFreq) - MIDI_OFFSET; onsets.forEach(r => r.fill(0, i)); frames.forEach(r => r.fill(0, i)); }
  if (minFreq) { const i = hzToMidi(minFreq) - MIDI_OFFSET; onsets.forEach(r => r.fill(0, 0, i)); frames.forEach(r => r.fill(0, 0, i)); }
}
function getInferredOnsets(onsets, frames, nDiff) {
  nDiff = nDiff || 2;
  const diffs = Array.from(Array(nDiff).keys()).map(n => n + 1).map(n => {
    const framesAppended = Array(n).fill(Array(frames[0].length).fill(0)).concat(frames);
    const nPlus = framesAppended.slice(n), minusN = framesAppended.slice(0, -n);
    return nPlus.map((row, r) => row.map((v, c) => v - minusN[r][c]));
  });
  let frameDiff = min3dForAxis0(diffs);
  frameDiff = frameDiff.map(row => row.map(v => Math.max(v, 0)));
  frameDiff = frameDiff.map((row, r) => (r < nDiff ? row.fill(0) : row));
  const onsetMax = globalMax(onsets), frameDiffMax = globalMax(frameDiff);
  frameDiff = frameDiff.map(row => row.map(v => (onsetMax * v) / frameDiffMax));
  return max3dForAxis0([onsets, frameDiff]);
}

// frames/onsets: 2D arrays [time][pitchBin] straight from the model.
// Returns [{ startFrame, durationFrames, pitchMidi, amplitude }, ...]
export function outputToNotesPoly(frames, onsets, onsetThresh, frameThresh, minNoteLen, inferOnsets, maxFreq, minFreq, melodiaTrick, energyTolerance) {
  onsetThresh = onsetThresh == null ? 0.5 : onsetThresh;
  frameThresh = frameThresh == null ? 0.3 : frameThresh;
  minNoteLen = minNoteLen == null ? 5 : minNoteLen;
  inferOnsets = inferOnsets == null ? true : inferOnsets;
  melodiaTrick = melodiaTrick == null ? true : melodiaTrick;
  energyTolerance = energyTolerance == null ? 11 : energyTolerance;

  let inferredFrameThresh = frameThresh;
  if (inferredFrameThresh === null) { const [mean, std] = meanStdDev(frames); inferredFrameThresh = mean + std; }
  const nFrames = frames.length;
  constrainFrequency(onsets, frames, maxFreq, minFreq);
  let inferredOnsets = inferOnsets ? getInferredOnsets(onsets, frames) : onsets;
  const peakThresholdMatrix = inferredOnsets.map(o => o.map(() => 0));
  argRelMax(inferredOnsets).forEach(([row, col]) => { peakThresholdMatrix[row][col] = inferredOnsets[row][col]; });
  const [noteStarts, freqIdxs] = whereGreaterThanAxis1(peakThresholdMatrix, onsetThresh);
  noteStarts.reverse(); freqIdxs.reverse();
  const remainingEnergy = frames.map(frame => frame.slice());

  const noteEvents = noteStarts.map((noteStartIdx, idx) => {
    const freqIdx = freqIdxs[idx];
    if (noteStartIdx >= nFrames - 1) return null;
    let i = noteStartIdx + 1, k = 0;
    while (i < nFrames - 1 && k < energyTolerance) { if (remainingEnergy[i][freqIdx] < inferredFrameThresh) k += 1; else k = 0; i += 1; }
    i -= k;
    if (i - noteStartIdx <= minNoteLen) return null;
    for (let j = noteStartIdx; j < i; ++j) {
      remainingEnergy[j][freqIdx] = 0;
      if (freqIdx < MAX_FREQ_IDX) remainingEnergy[j][freqIdx + 1] = 0;
      if (freqIdx > 0) remainingEnergy[j][freqIdx - 1] = 0;
    }
    const amplitude = frames.slice(noteStartIdx, i).reduce((prev, row) => prev + row[freqIdx], 0) / (i - noteStartIdx);
    return { startFrame: noteStartIdx, durationFrames: i - noteStartIdx, pitchMidi: freqIdx + MIDI_OFFSET, amplitude };
  }).filter(isNotNull);

  if (melodiaTrick) {
    while (globalMax(remainingEnergy) > inferredFrameThresh) {
      const [iMid, freqIdx] = remainingEnergy.reduce((prevCoord, currRow, rowIdx) => {
        const colMaxIdx = argMax(currRow);
        return currRow[colMaxIdx] > remainingEnergy[prevCoord[0]][prevCoord[1]] ? [rowIdx, colMaxIdx] : prevCoord;
      }, [0, 0]);
      remainingEnergy[iMid][freqIdx] = 0;
      let i = iMid + 1, k = 0;
      while (i < nFrames - 1 && k < energyTolerance) {
        if (remainingEnergy[i][freqIdx] < inferredFrameThresh) k += 1; else k = 0;
        remainingEnergy[i][freqIdx] = 0;
        if (freqIdx < MAX_FREQ_IDX) remainingEnergy[i][freqIdx + 1] = 0;
        if (freqIdx > 0) remainingEnergy[i][freqIdx - 1] = 0;
        i += 1;
      }
      const iEnd = i - 1 - k;
      i = iMid - 1; k = 0;
      while (i > 0 && k < energyTolerance) {
        if (remainingEnergy[i][freqIdx] < inferredFrameThresh) k += 1; else k = 0;
        remainingEnergy[i][freqIdx] = 0;
        if (freqIdx < MAX_FREQ_IDX) remainingEnergy[i][freqIdx + 1] = 0;
        if (freqIdx > 0) remainingEnergy[i][freqIdx - 1] = 0;
        i -= 1;
      }
      const iStart = i + 1 + k;
      if (iStart < 0 || iEnd >= nFrames) continue;
      const amplitude = frames.slice(iStart, iEnd).reduce((sum, row) => sum + row[freqIdx], 0) / (iEnd - iStart);
      if (iEnd - iStart <= minNoteLen) continue;
      noteEvents.push({ startFrame: iStart, durationFrames: iEnd - iStart, pitchMidi: freqIdx + MIDI_OFFSET, amplitude });
    }
  }
  return noteEvents;
}

export const noteFramesToTime = notes => notes.map(note => ({
  pitchMidi: note.pitchMidi,
  amplitude: note.amplitude,
  startTimeSeconds: modelFrameToTime(note.startFrame),
  durationSeconds: modelFrameToTime(note.startFrame + note.durationFrames) - modelFrameToTime(note.startFrame)
}));

export { AUDIO_SAMPLE_RATE };
