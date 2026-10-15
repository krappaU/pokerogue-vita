// SPDX-License-Identifier: AGPL-3.0-only
// The Web Audio API, as much of it as Phaser's WebAudioSoundManager uses, on top of the native
// mixer in src/audio.c.
//
// Nodes form chains (source -> gain/panner nodes -> destination). When a source starts, and once
// per frame while it plays, the chain is folded into a left/right gain and a playback rate for the
// native voice; only changes are sent. decodeAudioData hands the bytes to the native decoder
// thread and resolves when it is done (results are collected once per frame). A file that can't
// be decoded becomes a silent buffer of about the right length, so the game carries on.
(function (g) {
  "use strict";
  const rt = g.__rt, N = g.__native;
  const native = typeof N.audioStart === "function";

  // Anything that can change a playing voice's gains or rate bumps `gen`; the frame hook only
  // re-folds the node chains when it changed, or while a ramp is running.
  let gen = 0, ramps = 0;
  class AudioParam {
    constructor(v = 1) { this._v = v; this._ramp = null; this.defaultValue = v; this.minValue = -3.4e38; this.maxValue = 3.4e38; }
    get value() {
      const r = this._ramp;
      if (r) {
        const now = N.audioTime();
        if (now >= r.t1 || r.t1 <= r.t0) { this._v = r.to; this._setRamp(null); }
        else this._v = r.from + (r.to - r.from) * Math.max(0, (now - r.t0) / (r.t1 - r.t0));
      }
      return this._v;
    }
    set value(v) { this._setRamp(null); this._set(v); }
    _set(v) { v = +v; if (v !== this._v) { this._v = v; gen++; } }
    _setRamp(r) { if (!!r !== !!this._ramp) ramps += r ? 1 : -1; this._ramp = r; gen++; }
    // Scheduled changes are applied at once (Phaser schedules them for "now").
    setValueAtTime(v) { if (this._ramp) this._setRamp(null); this._set(v); return this; }
    linearRampToValueAtTime(v, t) { const from = this.value; this._setRamp({ from, to: +v, t0: N.audioTime(), t1: +t || 0 }); return this; }
    exponentialRampToValueAtTime(v, t) { return this.linearRampToValueAtTime(v, t); }
    setTargetAtTime(v) { return this.setValueAtTime(v); }
    setValueCurveAtTime(curve) { if (curve && curve.length) this.setValueAtTime(curve[curve.length - 1]); return this; }
    cancelScheduledValues() { const v = this.value; if (this._ramp) this._setRamp(null); this._set(v); return this; }
    cancelAndHoldAtTime() { return this.cancelScheduledValues(); }
  }

  class AudioNode extends EventTarget {
    constructor(ctx) { super(); this.context = ctx; this._out = null; this.numberOfInputs = 1; this.numberOfOutputs = 1; this.channelCount = 2; }
    connect(dest) { this._out = dest; gen++; return dest; }
    disconnect() { this._out = null; gen++; }
  }
  class GainNode extends AudioNode { constructor(c) { super(c); this.gain = new AudioParam(1); } }
  class StereoPannerNode extends AudioNode { constructor(c) { super(c); this.pan = new AudioParam(0); } }
  class PannerNode extends AudioNode {
    constructor(c) {
      super(c);
      for (const k of ["positionX", "positionY", "positionZ", "orientationX", "orientationY", "orientationZ"]) this[k] = new AudioParam(0);
      this.panningModel = "equalpower"; this.distanceModel = "inverse"; this.refDistance = 1; this.maxDistance = 10000;
      this.rolloffFactor = 1; this.coneInnerAngle = 360; this.coneOuterAngle = 0; this.coneOuterGain = 0;
    }
    setPosition() {} setOrientation() {}
  }
  class AudioDestinationNode extends AudioNode { constructor(c) { super(c); this.maxChannelCount = 2; } }
  class AudioListener {
    constructor() { for (const k of ["positionX", "positionY", "positionZ", "forwardX", "forwardY", "forwardZ", "upX", "upY", "upZ"]) this[k] = new AudioParam(0); }
    setPosition() {} setOrientation() {}
  }

  class AudioBuffer {
    constructor({ length = 1, sampleRate = 48000, numberOfChannels = 2 } = {}) {
      this.length = length; this.sampleRate = sampleRate; this.numberOfChannels = numberOfChannels;
      this.duration = length / sampleRate;
      this._data = null; // native AudioData (src/audio.c), null for a silent buffer
    }
    // The samples live in native memory; nothing in the game reads them back.
    getChannelData() { return new Float32Array(Math.min(this.length, 65536)); }
    copyFromChannel() {} copyToChannel() {}
  }

  const active = new Map(); // native voice id -> AudioBufferSourceNode
  const pendingDecodes = new Map(); // job id -> callback

  // Folds the chain from a source to the destination into [left gain, right gain].
  function chainGains(node) {
    let gain = 1, pan = 0, reached = false;
    let n = node._out;
    for (let i = 0; n && i < 32; i++) {
      if (n instanceof AudioDestinationNode) { reached = true; break; }
      if (n instanceof GainNode) gain *= n.gain.value;
      else if (n instanceof StereoPannerNode) pan = n.pan.value;
      n = n._out;
    }
    if (!reached) gain = 0;
    pan = Math.max(-1, Math.min(1, pan || 0));
    return [gain * (pan > 0 ? 1 - pan : 1), gain * (pan < 0 ? 1 + pan : 1)];
  }
  const rateOf = (s) => (s.playbackRate.value || 0) * Math.pow(2, (s.detune.value || 0) / 1200);

  class AudioBufferSourceNode extends AudioNode {
    constructor(c) {
      super(c);
      this.buffer = null; this._loop = false; this._loopStart = 0; this._loopEnd = 0;
      this.playbackRate = new AudioParam(1); this.detune = new AudioParam(0);
      this.onended = null; this._id = 0; this._timer = 0; this._started = false; this._key = "";
    }
    start(when = 0, offset = 0, duration) {
      if (this._started) throw new DOMException("start() called twice", "InvalidStateError");
      this._started = true;
      const b = this.buffer;
      if (native && b && b._data) {
        const [gl, gr] = chainGains(this), rate = rateOf(this);
        this._id = N.audioStart(b._data, +when || 0, +offset || 0, duration === undefined ? -1 : +duration,
          this.loop, this.loopStart, this.loopEnd, rate, gl, gr);
        if (this._id) {
          this._key = gl + "|" + gr + "|" + rate + "|" + this.loop + "|" + this.loopStart + "|" + this.loopEnd;
          active.set(this._id, this);
          return;
        }
      }
      // Silent buffer: end after its duration, like the real thing.
      if (this.loop || !b) return;
      const len = (duration ?? b.duration - offset) / (this.playbackRate.value || 1);
      const delay = Math.max(0, (+when || 0) - this.context.currentTime) + Math.max(0, len);
      this._timer = g.setTimeout(() => { this._timer = 0; this._fireEnded(); }, delay * 1000);
    }
    stop(when = 0) {
      if (this._id) N.audioStop(this._id, +when || 0);
      else if (this._timer) { g.clearTimeout(this._timer); this._timer = 0; g.setTimeout(() => this._fireEnded(), 0); }
    }
    _fireEnded() { this.dispatchEvent(new Event("ended")); }
    get loop() { return this._loop; }
    set loop(v) { this._loop = !!v; gen++; }
    get loopStart() { return this._loopStart; }
    set loopStart(v) { this._loopStart = +v || 0; gen++; }
    get loopEnd() { return this._loopEnd; }
    set loopEnd(v) { this._loopEnd = +v || 0; gen++; }
  }

  class AudioContext extends EventTarget {
    constructor() {
      super();
      this.sampleRate = 48000;
      this.state = "running";
      this.destination = new AudioDestinationNode(this);
      this.listener = new AudioListener();
      this.baseLatency = 0.02;
      this.outputLatency = 0.04;
      this.onstatechange = null;
    }
    get currentTime() { return N.audioTime(); }
    createGain() { return new GainNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    createPanner() { return new PannerNode(this); }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createBuffer(ch, len, rate) { return new AudioBuffer({ numberOfChannels: ch, length: len, sampleRate: rate }); }
    createDynamicsCompressor() { return new AudioNode(this); }
    createAnalyser() { return Object.assign(new AudioNode(this), { fftSize: 2048, getByteFrequencyData() {}, getFloatFrequencyData() {} }); }
    createBiquadFilter() { return Object.assign(new AudioNode(this), { frequency: new AudioParam(350), Q: new AudioParam(1), gain: new AudioParam(0), type: "lowpass" }); }
    createOscillator() { return Object.assign(new AudioNode(this), { frequency: new AudioParam(440), start() {}, stop() {} }); }
    decodeAudioData(ab, ok, fail) {
      return new Promise((resolve) => {
        const bytes = ab ? ab.byteLength : 0;
        const finish = (buf) => { if (ok) { try { ok(buf); } catch (e) { N.log("[audio] decode callback: " + e); } } resolve(buf); };
        const silent = () => {
          // A rough length from the size (mp3 ~128 kbit/s, else 16-bit stereo 44.1 kHz).
          const u8 = new Uint8Array(ab || new ArrayBuffer(0), 0, Math.min(4, bytes));
          const isMp3 = u8[0] === 0x49 || u8[0] === 0xff;
          const seconds = isMp3 ? (bytes * 8) / 128000 : bytes / (44100 * 4);
          return new AudioBuffer({ length: Math.max(1, Math.round(seconds * 48000)), sampleRate: 48000, numberOfChannels: 2 });
        };
        const id = native && bytes ? N.audioDecode(ab) : 0;
        if (!id) { g.setTimeout(() => finish(silent()), 0); return; }
        pendingDecodes.set(id, (data, ch, rate, frames) => {
          if (!data) {
            N.log("[audio] could not decode a " + bytes + "-byte file; it stays silent");
            finish(silent());
            return;
          }
          const buf = new AudioBuffer({ length: frames, sampleRate: rate, numberOfChannels: ch });
          buf._data = data;
          finish(buf);
        });
      });
    }
    resume() { this._setState("running"); return Promise.resolve(); }
    suspend() { this._setState("suspended"); return Promise.resolve(); }
    close() { this._setState("closed"); return Promise.resolve(); }
    _setState(s) { if (this.state !== s) { this.state = s; this.dispatchEvent(new Event("statechange")); } }
  }

  if (native) {
    let seenGen = -1;
    rt.frameHooks.push(() => {
      if (pendingDecodes.size) {
        for (const [id, data, ch, rate, frames] of N.audioPoll()) {
          const cb = pendingDecodes.get(id);
          pendingDecodes.delete(id);
          if (cb) cb(data, ch, rate, frames);
        }
      }
      if (!active.size) return;
      for (const id of N.audioEnded()) {
        const node = active.get(id);
        if (!node) continue;
        active.delete(id);
        node._id = 0;
        node._fireEnded();
      }
      if (gen === seenGen && ramps === 0) return;
      seenGen = gen;
      for (const [id, s] of active) {
        const [gl, gr] = chainGains(s), rate = rateOf(s);
        const key = gl + "|" + gr + "|" + rate + "|" + s.loop + "|" + s.loopStart + "|" + s.loopEnd;
        if (key !== s._key) {
          s._key = key;
          N.audioUpdate(id, rate, gl, gr, s.loop, s.loopStart, s.loopEnd);
        }
      }
    });
  }

  Object.assign(g, { AudioContext, webkitAudioContext: AudioContext, AudioBuffer, AudioNode, GainNode, AudioParam,
    AudioBufferSourceNode, StereoPannerNode, PannerNode });
  rt.audioStub = !native;
})(globalThis);
