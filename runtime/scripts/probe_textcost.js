// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: total time spent in the functions that create Phaser text objects. Send
// `evalfile scripts/probe_textcost.js` as soon as "[phaser] patched" appears, then `eval __tc()`.
(() => {
  const P = Phaser, T = {};
  globalThis.__tc = () => JSON.stringify(Object.fromEntries(Object.entries(T).map(([k, v]) => [k, v.n + " calls, " + Math.round(v.ms) + " ms"])));
  function wrap(obj, name, label) {
    const f = obj[name];
    if (typeof f !== "function") return;
    obj[name] = function () {
      const t = performance.now();
      try { return f.apply(this, arguments); } finally {
        const s = T[label] || (T[label] = { n: 0, ms: 0 });
        s.n++; s.ms += performance.now() - t;
      }
    };
    Object.assign(obj[name], f);
    obj[name].prototype = f.prototype;
  }
  const CP = P.Display.Canvas.CanvasPool;
  wrap(CP, "create", "CanvasPool.create");
  wrap(CP, "first", "CanvasPool.first");
  const TP = P.GameObjects.Text.prototype;
  wrap(TP, "updateText", "Text.updateText");
  wrap(TP, "setText", "Text.setText");
  wrap(TP, "setStyle", "Text.setStyle");
  wrap(P.GameObjects.TextStyle.prototype, "update", "TextStyle.update");
  wrap(P.GameObjects.TextStyle.prototype, "setStyle", "TextStyle.setStyle");
  wrap(P.GameObjects.TextStyle.prototype, "getTextMetrics", "TextStyle.getTextMetrics");
  wrap(TP, "getWrappedText", "Text.getWrappedText");
  wrap(TP, "runWordWrap", "Text.runWordWrap");
  const C2 = globalThis.CanvasRenderingContext2D && CanvasRenderingContext2D.prototype;
  if (C2) for (const k of ["measureText", "fillText", "strokeText", "getImageData", "clearRect", "fillRect"]) wrap(C2, k, "ctx." + k);
  const HC = globalThis.HTMLCanvasElement && HTMLCanvasElement.prototype;
  if (HC) wrap(HC, "getContext", "canvas.getContext");
  wrap(P.Renderer.WebGL.WebGLRenderer.prototype, "canvasToTexture", "renderer.canvasToTexture");
  return "text cost probe installed";
})();
