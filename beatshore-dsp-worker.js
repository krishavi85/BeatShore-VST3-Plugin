// Runs beatshore-dsp.js off the main thread. Loaded as a module worker by
// beatshore-dsp-client.js. Speaks a small request/response protocol over
// postMessage: { id, op, args } in, { id, ok, result|error } out.

import * as dsp from './beatshore-dsp.js';

// Reconstructs a minimal AudioBuffer-like object (the only interface
// beatshore-dsp.js actually uses: sampleRate, length, numberOfChannels,
// getChannelData) from the packed form sent by the client.
function unpackArgs(args) {
  return args.map(a => {
    if (a && a.__bsBuffer) {
      return {
        sampleRate: a.sampleRate,
        length: a.length,
        numberOfChannels: a.numberOfChannels,
        getChannelData(c) { return a.channelData[c]; }
      };
    }
    return a;
  });
}

self.onmessage = async (e) => {
  const { id, op, args } = e.data;
  try {
    const fn = dsp[op];
    if (typeof fn !== 'function') throw new Error('Unknown DSP op: ' + op);
    const result = await fn(...unpackArgs(args));
    self.postMessage({ id, ok: true, result });
  } catch (err) {
    self.postMessage({ id, ok: false, error: (err && err.message) || String(err) });
  }
};
