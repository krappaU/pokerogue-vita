// SPDX-License-Identifier: AGPL-3.0-only
// Checks the C versions of Phaser code against Phaser's (or, for NineSlice, the JS fast path's):
// the pipeline machinery (js/fastpath.js part 14: pipeline switching, setProgram, setBlendMode,
// attribute pointers, the set1f... uniform wrappers), the Graphics renderer (part 15), the
// NineSlice renderer (part 5), Rectangle and GeometryMask (part 16). globalThis.__vfpWhich =
// "pipeline", "graphics", "nineslice" or "shapes" checks one of them. With tweens, animations and
// the game clock frozen, frames alternate between the two and every WebGL call (arguments, objects
// numbered by first use) is recorded, each draw call with a hash of the drawing pipeline's vertex
// bytes. Run: scripts/drive.sh "evalfile scripts/probe_verify_pipeline.js", wait a few seconds,
// then "eval __vfp()".
(() => {
  const game = __rt.renderer.game, gl = __rt.renderer.gl;
  // which natives to alternate: "pipeline", "graphics", "nineslice", "shapes" or (default) all
  const which = globalThis.__vfpWhich || "all";
  const pick = (k) => which === "all" || which === k;
  const swaps = [].concat(pick("pipeline") && __rt.pipelineSwaps || [], pick("graphics") && __rt.graphicsSwap || [],
    pick("nineslice") && __rt.nineSliceSwap || [], pick("shapes") && __rt.shapesSwap || []);
  if (!swaps.length) return "no native swaps active";
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
  const arg = (x) => (x && typeof x === "object" ? (x.length !== undefined ? "[" + Array.from(x).join(",") + "]" : "o" + id(x)) : String(x));
  for (const k of Object.getOwnPropertyNames(proto)) {
    const d = Object.getOwnPropertyDescriptor(proto, k);
    if (!d || typeof d.value !== "function" || k === "constructor") continue;
    saved[k] = proto[k];
    proto[k] = function (...a) {
      if (cur) {
        let extra = "";
        if (k === "drawArrays") {
          const p = __rt.renderer.pipelines.current;
          extra = p ? " " + p.name + ":" + hash(p.bytes.subarray(0, p.vertexCount * p.currentShader.vertexSize)) : "";
        }
        cur.push(k + "(" + a.map(arg).join(",") + ")" + extra);
      }
      return saved[k].apply(this, a);
    };
  }
  const set = (native) => { for (const [o, k, orig, nat] of swaps) o[k] = native ? nat : orig; };
  const frames = [];
  let n = 0;
  const onPre = () => {
    if (n > 8) return;
    if (cur) frames.push(cur);
    if (n === 8) { cur = null; n++; return; }
    cur = [];
    set(n % 2 === 0);
    n++;
  };
  game.events.on("prerender", onPre);
  globalThis.__vfp = () => {
    game.events.off("prerender", onPre);
    for (const k of Object.keys(saved)) proto[k] = saved[k];
    set(true);
    game.getTime = origGetTime;
    scene.tweens.resumeAll(); scene.anims.resumeAll();
    const cmp = (a, b) => {
      if (!a || !b) return "missing";
      if (a.join("|") === b.join("|")) return "identical (" + a.length + " calls)";
      let i = 0;
      while (i < a.length && a[i] === b[i]) i++;
      return "differ at " + i + "/" + a.length + " vs " + b.length + ": " + a[i] + " <> " + b[i];
    };
    return [swaps.length + " functions", "2n-3o: " + cmp(frames[2], frames[3]), "3o-4n: " + cmp(frames[3], frames[4]),
      "4n-6n: " + cmp(frames[4], frames[6]), "5o-7o: " + cmp(frames[5], frames[7])].join(" | ");
  };
  return "armed";
})()
