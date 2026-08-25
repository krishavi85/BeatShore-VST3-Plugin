// Writes a real Standard MIDI Type 1 file for a transcription result, using
// beatshore-dsp.js's encodeMIDITracks() UNMODIFIED (the same encoder the
// browser prototype's download button uses -- format-1 header, tempo +
// time-signature meta events, one track per note stream). This file exists
// so "return a real MIDI file" is a genuine deliverable, not just notes in
// a JSON array: something a user can actually drag into a DAW.
import * as dsp from '../../../beatshore-dsp.js';
import { mkdirSync, writeFileSync, renameSync, unlinkSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { homedir } from 'node:os';

const EXPORT_DIR = join(homedir(), 'Documents', 'BeatShore', 'Exports');

// Windows forbids \ / : * ? " < > | in filenames (plus trailing dots/spaces
// and reserved device names, not worth guarding here since a DAW track name
// hitting those is vanishingly unlikely) -- replace anything outside a safe
// set, collapse whitespace, and cap the length so a long track name can't
// push the full path anywhere near MAX_PATH. Empty/whitespace-only/missing
// input falls back to 'Untitled' rather than producing a filename with a
// blank identity segment.
function sanitizeForFilename(name, fallback) {
  const cleaned = String(name || '')
    .replace(/[\\/:*?"<>|]/g, '_')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 40);
  return cleaned.length ? cleaned : fallback;
}

// tracks: [{ name, notes, drums }] -- same shape encodeMIDITracks expects.
// hostTrackName: the DAW's own name for the track the plugin is on, if the
// host reports one (see BridgeClient.h's requestAnalysis()) -- purely for a
// human-readable filename; NOT what prevents collisions (requestId's UUID
// already guarantees that on its own, so this being empty/generic is fine).
// Returns { path, sha256, sizeBytes, generatedAt, requestId }. Throws on
// failure (caller already wraps handle() in a try/catch that reports
// ERROR/ANALYSIS_FAILED).
export async function writeMidiFile(tempo, tracks, kind, requestId, hostTrackName) {
  mkdirSync(EXPORT_DIR, { recursive: true });

  const blob = dsp.encodeMIDITracks(tempo, tracks, []);
  const bytes = Buffer.from(await blob.arrayBuffer());
  const sha256 = createHash('sha256').update(bytes).digest('hex');

  const trackPart = sanitizeForFilename(hostTrackName, 'Untitled');
  const kindPart = sanitizeForFilename(kind, 'Analysis');
  // requestId is a UUID and appears in full, never truncated -- it is the
  // only component here that actually guarantees no two exports ever
  // collide, regardless of how many requests share the same track name.
  const finalPath = join(EXPORT_DIR, `BeatShore_${trackPart}_${kindPart}_${requestId}.mid`);
  const tempPath = finalPath + '.tmp';
  // Write-then-rename on the same directory/volume: a reader (or a DAW's
  // own directory watcher) can never observe a partially-written file at
  // the final name, and a crash mid-write leaves only an orphaned .tmp,
  // never a corrupt .mid.
  writeFileSync(tempPath, bytes);
  try {
    renameSync(tempPath, finalPath);
  } catch (err) {
    try { unlinkSync(tempPath); } catch { /* best effort cleanup */ }
    throw err;
  }

  return { path: finalPath, sha256, sizeBytes: bytes.length, generatedAt: new Date().toISOString(), requestId };
}
