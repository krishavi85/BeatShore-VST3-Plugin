// Playback engine abstraction (implementation plan, section 3: "Replace
// AudioContext with Runtime Adapters"). The UI/Component talks to this
// interface — decode, play a set of {buffer, gain} sources, stop, report
// elapsed time — and never touches AudioContext directly. WebAudioEngine is
// the only implementation that exists today, because it's the only runtime
// that exists today: there is no BeatShore desktop process and no VST3 host
// process for DesktopAudioEngine/VST3AudioEngine to run inside of yet (see
// native/BeatShoreBridge for the VST3 shell's current state). When those
// runtimes exist, they implement this same interface — routing/gain
// decisions (routeGain in the Component) stay engine-agnostic either way.
//
// Interface every engine implementation must provide:
//   isSupported()                          -> boolean
//   ensureContext()                        -> opaque context handle (engine-specific)
//   decodeAudioData(arrayBuffer)           -> Promise<AudioBuffer-like>
//   createOfflineContext(ch, len, rate)    -> OfflineAudioContext-like, for resampling
//   play(sources, offsetSeconds)           -> void; sources: [{ buffer, gain }]
//   stop()                                 -> void
//   getElapsed()                           -> seconds since play() was called, minus offset

export class WebAudioEngine {
  constructor() {
    this._ctx = null;
    this._master = null;
    this._limiter = null;
    this._srcs = [];
    this._startedAt = 0;
  }

  isSupported() {
    return typeof (window.AudioContext || window.webkitAudioContext) !== 'undefined';
  }

  ensureContext() {
    if (!this._ctx) this._ctx = new (window.AudioContext || window.webkitAudioContext)();
    return this._ctx;
  }

  decodeAudioData(arrayBuffer) {
    return this.ensureContext().decodeAudioData(arrayBuffer);
  }

  createOfflineContext(numberOfChannels, length, sampleRate) {
    return new OfflineAudioContext(numberOfChannels, length, sampleRate);
  }

  _ensureMasterChain() {
    if (this._master) return;
    const ctx = this._ctx;
    this._master = ctx.createGain();
    this._master.gain.value = 1;
    // Peak protection, not loudness matching — a safety net on top of the
    // per-source gain staging the caller already computed.
    this._limiter = ctx.createDynamicsCompressor();
    this._limiter.threshold.value = -1;
    this._limiter.knee.value = 0;
    this._limiter.ratio.value = 20;
    this._limiter.attack.value = 0.003;
    this._limiter.release.value = 0.1;
    this._master.connect(this._limiter);
    this._limiter.connect(ctx.destination);
  }

  // sources: [{ buffer: AudioBuffer, gain: number }] — routing/mute/solo/mode
  // decisions are the caller's job; this just plays whatever it's handed.
  play(sources, offsetSeconds) {
    const ctx = this.ensureContext();
    if (ctx.state === 'suspended') ctx.resume();
    this._ensureMasterChain();
    const off = offsetSeconds || 0;
    this._startedAt = ctx.currentTime - off;
    this.stop();
    (sources || []).forEach(({ buffer, gain }) => {
      if (!buffer || !(gain > 0)) return;
      const src = ctx.createBufferSource();
      src.buffer = buffer;
      const g = ctx.createGain();
      g.gain.value = gain;
      src.connect(g);
      g.connect(this._master);
      try { src.start(0, Math.min(off, buffer.duration)); } catch (e) { src.start(0); }
      this._srcs.push(src);
    });
  }

  stop() {
    this._srcs.forEach(s => { try { s.stop(); } catch (e) {} });
    this._srcs = [];
  }

  getElapsed() {
    return this._ctx ? this._ctx.currentTime - this._startedAt : 0;
  }
}
