// SPDX-License-Identifier: AGPL-3.0-only
// A silent Web Audio API. Enough for Phaser's WebAudioSoundManager to load and "play" sounds
// (sources end after their duration) until the native mixer arrives in Phase 3.
(function (g) {
  "use strict";
  const rt = g.__rt;

  class AudioParam {
    constructor(v = 1) { this.value = v; this.defaultValue = v; this.minValue = -3.4e38; this.maxValue = 3.4e38; }
    setValueAtTime(v) { this.value = v; return this; }
    linearRampToValueAtTime(v) { this.value = v; return this; }
    exponentialRampToValueAtTime(v) { this.value = v; return this; }
    setTargetAtTime(v) { this.value = v; return this; }
    setValueCurveAtTime() { return this; }
    cancelScheduledValues() { return this; }
    cancelAndHoldAtTime() { return this; }
  }
  class AudioNode extends EventTarget {
    constructor(ctx) { super(); this.context = ctx; this.numberOfInputs = 1; this.numberOfOutputs = 1; this.channelCount = 2; }
    connect(dest) { return dest; }
    disconnect() {}
  }
  class GainNode extends AudioNode { constructor(c) { super(c); this.gain = new AudioParam(1); } }
  class StereoPannerNode extends AudioNode { constructor(c) { super(c); this.pan = new AudioParam(0); } }
  class AudioBuffer {
    constructor({ length = 1, sampleRate = 44100, numberOfChannels = 2 } = {}) {
      this.length = length; this.sampleRate = sampleRate; this.numberOfChannels = numberOfChannels;
      this.duration = length / sampleRate;
    }
    getChannelData() { return new Float32Array(Math.min(this.length, 1)); }
    copyFromChannel() {} copyToChannel() {}
  }
  class AudioBufferSourceNode extends AudioNode {
    constructor(c) {
      super(c);
      this.buffer = null; this.loop = false; this.loopStart = 0; this.loopEnd = 0;
      this.playbackRate = new AudioParam(1); this.detune = new AudioParam(0);
      this.onended = null; this._timer = 0;
    }
    start(when = 0, offset = 0, duration) {
      if (this.loop || !this.buffer) return;
      const len = (duration ?? this.buffer.duration - offset) / (this.playbackRate.value || 1);
      this._timer = g.setTimeout(() => this.dispatchEvent(new Event("ended")), Math.max(0, len * 1000));
    }
    stop() { if (this._timer) { g.clearTimeout(this._timer); this._timer = 0; g.setTimeout(() => this.dispatchEvent(new Event("ended")), 0); } }
  }
  class AudioDestinationNode extends AudioNode { constructor(c) { super(c); this.maxChannelCount = 2; } }
  class AudioListener { constructor() { this.positionX = new AudioParam(0); this.positionY = new AudioParam(0); this.positionZ = new AudioParam(0); } setPosition() {} setOrientation() {} }

  class AudioContext extends EventTarget {
    constructor() {
      super();
      this.sampleRate = 44100;
      this.state = "running";
      this.destination = new AudioDestinationNode(this);
      this.listener = new AudioListener();
      this.baseLatency = 0.01;
      this._t0 = g.performance.now();
      this.onstatechange = null;
    }
    get currentTime() { return (g.performance.now() - this._t0) / 1000; }
    createGain() { return new GainNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createBuffer(ch, len, rate) { return new AudioBuffer({ numberOfChannels: ch, length: len, sampleRate: rate }); }
    createDynamicsCompressor() { return new AudioNode(this); }
    createAnalyser() { return Object.assign(new AudioNode(this), { fftSize: 2048, getByteFrequencyData() {}, getFloatFrequencyData() {} }); }
    createBiquadFilter() { return Object.assign(new AudioNode(this), { frequency: new AudioParam(350), Q: new AudioParam(1), gain: new AudioParam(0), type: "lowpass" }); }
    createOscillator() { return Object.assign(new AudioNode(this), { frequency: new AudioParam(440), start() {}, stop() {} }); }
    // Estimate the duration from the file size (mp3 at ~128 kbit/s, wav at 16-bit stereo 44.1 kHz).
    decodeAudioData(ab, ok, fail) {
      const bytes = ab ? ab.byteLength : 0;
      const u8 = new Uint8Array(ab || new ArrayBuffer(0), 0, Math.min(4, bytes));
      const isWav = u8[0] === 0x52 && u8[1] === 0x49 && u8[2] === 0x46 && u8[3] === 0x46;
      const seconds = isWav ? bytes / (44100 * 4) : (bytes * 8) / 128000;
      const buf = new AudioBuffer({ length: Math.max(1, Math.round(seconds * 44100)), sampleRate: 44100, numberOfChannels: 2 });
      return new Promise((res) => g.setTimeout(() => { if (ok) ok(buf); res(buf); }, 0));
    }
    resume() { this.state = "running"; return Promise.resolve(); }
    suspend() { this.state = "suspended"; return Promise.resolve(); }
    close() { this.state = "closed"; return Promise.resolve(); }
  }
  Object.assign(g, { AudioContext, webkitAudioContext: AudioContext, AudioBuffer, AudioNode, GainNode, AudioParam,
    AudioBufferSourceNode, StereoPannerNode });
  rt.audioStub = true;
})(globalThis);
