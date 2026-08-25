// Generates a small, real BSM1-format audio fixture (a 2-second, 44.1kHz
// mono sine wave at C4) for build-release.ps1's staged regression tests --
// BridgeClientTest/MultiSessionTest need a real .bsmraw file as input, and
// this keeps the release script from depending on a hand-crafted fixture
// living outside version control. Deterministic output (same bytes every
// run), not randomized -- lets a future run's fixture hash be compared
// against a prior one if that's ever useful.
//
// Usage: node gen-test-fixture.js <output-path.bsmraw>
// ESM, not CommonJS require() -- this project's root package.json has
// "type":"module", and Node resolves a script's module type from the
// nearest ANCESTOR package.json, not this file's own directory (there is
// none closer). A require()-based version fails immediately here for
// exactly that reason -- confirmed the hard way, not assumed.
import fs from 'node:fs';

const outPath = process.argv[2];
if (!outPath) {
    console.error('usage: node gen-test-fixture.js <output-path.bsmraw>');
    process.exit(1);
}

const sampleRate = 44100;
const channels = 1;
const durationSec = 2;
const frames = sampleRate * durationSec;

const header = Buffer.alloc(16);
header.write('BSM1', 0, 'ascii');
header.writeUInt32LE(sampleRate, 4);
header.writeUInt32LE(channels, 8);
header.writeUInt32LE(frames, 12);

const samples = Buffer.alloc(frames * channels * 4);
for (let i = 0; i < frames; i++) {
    const t = i / sampleRate;
    const v = 0.3 * Math.sin(2 * Math.PI * 261.63 * t); // C4
    samples.writeFloatLE(v, i * 4);
}

fs.writeFileSync(outPath, Buffer.concat([header, samples]));
console.log('wrote ' + outPath + ' (' + (header.length + samples.length) + ' bytes)');
