// SPDX-License-Identifier: AGPL-3.0-only
// Injected before the game loads. Wraps Phaser to count, per frame, how many objects of
// each type render and how many quads get batched, and how long game.step takes.
(() => {
  const hist = [];
  let counters = {};
  const count = (k) => { counters[k] = (counters[k] || 0) + 1; };
  window.__pkrSummary = (n = 60) => {
    const h = hist.slice(-n);
    if (!h.length) return { frames: 0 };
    const sum = {};
    for (const f of h) for (const [k, v] of Object.entries(f.c)) sum[k] = (sum[k] || 0) + v;
    const avg = {};
    for (const k of Object.keys(sum).sort()) avg[k] = +(sum[k] / h.length).toFixed(1);
    const dts = h.map((f) => f.dt).sort((a, b) => a - b);
    return { frames: h.length, stepMsAvg: +(dts.reduce((a, b) => a + b) / dts.length).toFixed(2),
      stepMsP50: +dts[dts.length >> 1].toFixed(2), perFrame: avg };
  };
  const iv = setInterval(() => {
    const P = window.Phaser;
    if (!P || !P.Game || !P.Renderer) return;
    clearInterval(iv);
    const step = P.Game.prototype.step;
    P.Game.prototype.step = function (time, delta) {
      window.__game = this;
      counters = {};
      const t0 = performance.now();
      const r = step.call(this, time, delta);
      hist.push({ dt: performance.now() - t0, c: counters });
      if (hist.length > 600) hist.shift();
      return r;
    };
    for (const [name, cls] of Object.entries(P.GameObjects)) {
      if (typeof cls !== "function" || !cls.prototype || typeof cls.prototype.renderWebGL !== "function") continue;
      const orig = cls.prototype.renderWebGL;
      cls.prototype.renderWebGL = function () { count("render:" + name); return orig.apply(this, arguments); };
    }
    const MP = P.Renderer.WebGL.Pipelines.MultiPipeline.prototype;
    for (const m of ["batchSprite", "batchTexture", "batchTextureFrame", "batchQuad", "batchTri", "batchFillRect", "flush"]) {
      const orig = MP[m];
      if (orig) MP[m] = function () { count("pipe:" + m); return orig.apply(this, arguments); };
    }
    const WP = P.Renderer.WebGL.WebGLPipeline.prototype;
    for (const m of ["flush"]) { const o = WP[m]; if (o && o !== MP[m]) WP[m] = function () { count("pipe(base):" + m); return o.apply(this, arguments); }; }
    window.__pkrHooked = true;
  }, 0);
})();
