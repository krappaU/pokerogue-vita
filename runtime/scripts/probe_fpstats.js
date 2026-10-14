// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: run with `scripts/drive.sh "evalfile scripts/probe_fpstats.js"` while the runtime is running.
(() => {
  const P = Phaser, MP = P.Renderer.WebGL.Pipelines.MultiPipeline.prototype;
  const stats = { calls: 0, reasons: {}, pipelines: {} };
  const reason = (r) => { stats.reasons[r] = (stats.reasons[r] || 0) + 1; };
  const cur = MP.batchSprite;
  const origQuad = MP.batchQuad;
  MP.batchSprite = function (go, camera, parent) {
    stats.calls++;
    const name = this.constructor && this.constructor.name || "?";
    stats.pipelines[name] = (stats.pipelines[name] || 0) + 1;
    if (go.isCropped) reason("cropped");
    else if (go.frame.customPivot) reason("customPivot");
    else if (this.batchQuad !== origQuad) reason("batchQuad overridden (" + name + ")");
    else if (this.currentShader.vertexComponentCount !== 7) reason("vertexComponentCount=" + this.currentShader.vertexComponentCount + " (" + name + ")");
    else reason("fast-eligible");
    return cur.apply(this, arguments);
  };
  const CR = P.GameObjects.Container.prototype.renderWebGL;
  let containers = 0, children = 0, kids = {};
  P.GameObjects.Container.prototype.renderWebGL = function (r, c) {
    containers++;
    for (const ch of c.list) { children++; kids[ch.type] = (kids[ch.type] || 0) + 1; }
    return CR.apply(this, arguments);
  };
  let frames = 0;
  __rt.frameHooks.push(() => frames++);
  setTimeout(() => {
    const per = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, +(v / frames).toFixed(1)]));
    __native.log("[fpstats] frames " + frames + " batchSprite/frame " + (stats.calls / frames).toFixed(1));
    __native.log("[fpstats] reasons " + JSON.stringify(per(stats.reasons)));
    __native.log("[fpstats] pipelines " + JSON.stringify(per(stats.pipelines)));
    __native.log("[fpstats] containers/frame " + (containers / frames).toFixed(1) + " children/frame " + (children / frames).toFixed(1) + " by type " + JSON.stringify(per(kids)));
  }, 5000);
  return "ok";
})()
