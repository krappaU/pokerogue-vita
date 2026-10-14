// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: run with `scripts/drive.sh "evalfile scripts/probe_selfprof.js"` while the runtime is running.
(() => {
  const P = globalThis.Phaser;
  // Usage: evalfile this file, then `eval __sp.reset()`, wait, then `eval __sp.report(40)`.
  if (globalThis.__sp) { globalThis.__sp.reset(); return "reset"; }
  const stats = new Map();
  const stack = [];
  let frames = 0;
  function wrap(obj, name, label) {
    const o = obj && obj[name];
    if (typeof o !== "function" || o.__sp) return;
    const w = function () {
      const t0 = performance.now();
      stack.push(0);
      try { return o.apply(this, arguments); } finally {
        const child = stack.pop();
        const total = performance.now() - t0;
        let s = stats.get(label);
        if (!s) stats.set(label, (s = { self: 0, total: 0, n: 0 }));
        s.self += total - child; s.total += total; s.n++;
        if (stack.length) stack[stack.length - 1] += total;
      }
    };
    w.__sp = true;
    obj[name] = w;
  }
  const G = P.GameObjects;
  for (const [n, C] of Object.entries(G)) {
    if (typeof C !== "function" || !C.prototype) continue;
    if (Object.prototype.hasOwnProperty.call(C.prototype, "renderWebGL")) wrap(C.prototype, "renderWebGL", "render:" + n);
    if (Object.prototype.hasOwnProperty.call(C.prototype, "preUpdate")) wrap(C.prototype, "preUpdate", "preUpdate:" + n);
  }
  const pipes = new Set();
  const PM = P.Renderer.WebGL.PipelineManager.prototype;
  for (const m of ["set", "preBatch", "postBatch", "rebind", "isCurrent"]) wrap(PM, m, "PipelineManager." + m);
  const collect = (C) => { for (let p = C.prototype; p && p !== Object.prototype; p = Object.getPrototypeOf(p)) pipes.add(p); };
  for (const C of Object.values(P.Renderer.WebGL.Pipelines)) if (typeof C === "function") collect(C);
  // Game-defined pipelines (SpritePipeline etc.) are subclasses: find them via the pipeline manager instances.
  const RP = P.Renderer.WebGL.WebGLRenderer.prototype;
  const rr = RP.render;
  let game = null;
  RP.render = function () { game = this.game; RP.render = rr; return rr.apply(this, arguments); };
  const later = () => {
    if (!game) return setTimeout(later, 50);
    for (const p of game.renderer.pipelines.pipelines.values()) for (let q = Object.getPrototypeOf(p); q && q !== Object.prototype; q = Object.getPrototypeOf(q)) pipes.add(q);
    for (const proto of pipes) {
      const cname = (proto.constructor && proto.constructor.name) || "Pipeline";
      for (const m of ["batchSprite", "batchQuad", "batchTri", "batchTexture", "batchTextureFrame", "flush", "onBind", "onBatch", "onPreRender", "onRender", "bind", "rebind", "setGameObject", "setTexture2D", "shouldFlush", "pushBatch", "set1f", "set2f", "set3f", "set4f", "set1i", "set1fv", "set2fv", "set3fv", "set4fv", "setMatrix4fv", "batchFillRect", "batchVert"]) {
        if (Object.prototype.hasOwnProperty.call(proto, m)) wrap(proto, m, cname + "." + m);
      }
    }
    const TM = P.GameObjects.Components.TransformMatrix.prototype;
    for (const m of ["applyITRS", "multiply", "multiplyWithOffset", "setQuad", "loadIdentity", "translate", "rotate", "scale", "copyFrom", "getX", "getY"]) wrap(TM, m, "TransformMatrix." + m);
    wrap(P.GameObjects.GameObject.prototype, "willRender", "GameObject.willRender");
    wrap(P.Cameras.Scene2D.Camera.prototype, "addToRenderList", "Camera.addToRenderList");
    const R = P.Renderer.WebGL.WebGLRenderer.prototype;
    for (const m of ["render", "setBlendMode", "preRender", "postRender", "setTextureZero", "isNewNormalMap", "setScissor", "pushScissor", "popScissor"]) wrap(R, m, "WebGLRenderer." + m);
    const TW = P.Tweens;
    if (TW.Tween) wrap(TW.Tween.prototype, "update", "Tween.update");
    if (TW.TweenManager) wrap(TW.TweenManager.prototype, "update", "TweenManager.update");
    wrap(P.Time.Clock.prototype, "update", "Clock.update");
    wrap(P.Animations.AnimationState.prototype, "update", "AnimationState.update");
    wrap(P.Scenes.SceneManager.prototype, "update", "SceneManager.update");
    wrap(P.Scenes.SceneManager.prototype, "render", "SceneManager.render");
    wrap(game.loop, "callback", "FRAME");
    const GLP = WebGLRenderingContext.prototype;
    for (const k of Object.getOwnPropertyNames(GLP)) {
      const d = Object.getOwnPropertyDescriptor(GLP, k);
      if (d && typeof d.value === "function" && !/^[A-Z]/.test(k) && k !== "constructor") wrap(GLP, k, "gl." + k);
    }
    globalThis.__sp = {
      reset() { stats.clear(); },
      report(top = 35) {
        const f = (stats.get("FRAME") || { n: 1 }).n;
        const rows = [...stats.entries()].sort((a, b) => b[1].self - a[1].self).slice(0, top);
        __native.log("[selfprof] frames=" + f + " (ms per frame: self / total, calls per frame)");
        for (const [k, s] of rows) __native.log("[selfprof] " + k.padEnd(48) + (s.self / f).toFixed(3).padStart(8) + " / " + (s.total / f).toFixed(3).padStart(8) + "  " + (s.n / f).toFixed(1).padStart(7));
        return "ok";
      },
    };
  };
  later();
  return "installed";
})()
