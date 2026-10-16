// SPDX-License-Identifier: AGPL-3.0-only
// Checks the C WebGLPipeline.flush / pushBatch and MultiPipeline.batchQuad (js/fastpath.js part 10)
// against Phaser's: with tweens, animations and the game clock frozen, frames alternate between the
// two and every buffer upload (hash of the bytes), texture bind and draw call is recorded.
// Run: scripts/drive.sh "evalfile scripts/probe_verify_batching.js", wait a few seconds, "eval __vfb()".
(() => {
  const game = __rt.renderer.game, gl = __rt.renderer.gl, o = __rt.origPipeline, nat = __rt.nativePipeline;
  if (!o) return "native batching not active";
  const WP = Phaser.Renderer.WebGL.WebGLPipeline.prototype, MP = Phaser.Renderer.WebGL.Pipelines.MultiPipeline.prototype;
  const scene = game.scene.getScenes(true)[0];
  scene.tweens.pauseAll(); scene.anims.pauseAll();
  const t0 = game.getTime(), origGetTime = game.getTime;
  game.getTime = () => t0;
  const proto = Object.getPrototypeOf(gl), saved = {};
  let cur = null;
  const ids = new Map(), id = (x) => { if (!ids.has(x)) ids.set(x, ids.size); return ids.get(x); };
  const hash = (v) => {
    const b = new Uint8Array(v.buffer || v, v.byteOffset || 0, v.byteLength);
    let h = 2166136261;
    for (let i = 0; i < b.length; i++) h = Math.imul(h ^ b[i], 16777619);
    return b.length + "#" + (h >>> 0);
  };
  // The C flush uploads the vertex bytes without gl.bufferSubData, so uploads aren't logged; each
  // draw call logs a hash of the flushing pipeline's vertex bytes instead.
  for (const k of ["drawArrays", "bindTexture", "activeTexture", "bindBuffer"]) {
    saved[k] = proto[k];
    proto[k] = function (...a) {
      if (cur) {
        let extra = "";
        if (k === "drawArrays") {
          const p = __rt.renderer.pipelines.current;
          extra = p ? " " + p.name + ":" + hash(p.bytes.subarray(0, p.vertexCount * p.currentShader.vertexSize)) : "";
        }
        cur.push(k + "(" + a.map((x) => (x && typeof x === "object" ? "o" + id(x) : String(x))).join(",") + ")" + extra);
      }
      return saved[k].apply(this, a);
    };
  }
  const set = (x) => { WP.flush = x.flush; WP.pushBatch = x.pushBatch; MP.batchQuad = x.batchQuad; };
  const frames = [];
  let n = 0;
  const onPre = () => {
    if (n > 8) return;
    if (cur) frames.push(cur);
    if (n === 8) { cur = null; n++; return; }
    cur = [];
    set(n % 2 === 0 ? nat : o);
    n++;
  };
  game.events.on("prerender", onPre);
  globalThis.__vfb = () => {
    game.events.off("prerender", onPre);
    for (const k of Object.keys(saved)) proto[k] = saved[k];
    set(nat);
    game.getTime = origGetTime;
    scene.tweens.resumeAll(); scene.anims.resumeAll();
    const cmp = (a, b) => {
      if (!a || !b) return "missing";
      if (a.join("|") === b.join("|")) return "identical (" + a.length + " calls)";
      let i = 0;
      while (i < a.length && a[i] === b[i]) i++;
      return "differ at " + i + "/" + a.length + " vs " + b.length + ": " + a[i] + " <> " + b[i];
    };
    return ["2n-3o: " + cmp(frames[2], frames[3]), "3o-4n: " + cmp(frames[3], frames[4]), "4n-6n: " + cmp(frames[4], frames[6]), "5o-7o: " + cmp(frames[5], frames[7])].join(" | ");
  };
  return "armed";
})()
