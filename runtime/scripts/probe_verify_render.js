// SPDX-License-Identifier: AGPL-3.0-only
// Checks the native container renderer against the JS one: alternates them frame by frame and compares
// every flushed vertex batch. Run: scripts/drive.sh "evalfile scripts/probe_verify_render.js", wait, then "eval __vfy()".
// Frames 0/2 are native, 1/3 JS; on a still screen all pairs must be identical (animations differ in every pair).
(() => {
  const game = __rt.renderer.game;
  const C = Phaser.GameObjects.Container.prototype;
  const nat = __native.renderContainer;
  if (C.renderWebGL !== nat) return "native not active";
  // JS version: Phaser's own container renderer, with the fastpath.js sprite batcher behind it.
  const jsRender = __rt.jsContainerRender;
  if (!jsRender) return "no js renderer exported";
  const WP = Phaser.Renderer.WebGL.WebGLPipeline.prototype, origFlush = WP.flush;
  let frames = [], cur = null, n = 0;
  WP.flush = function () {
    if (cur && this.vertexCount > 0) cur.push(this.name + ":" + this.vertexCount + ":" + Array.from(new Uint32Array(this.vertexData, 0, this.vertexCount * 7)).join(","));
    return origFlush.apply(this, arguments);
  };
  const onPre = () => {
    if (n >= 6) return;
    if (cur) frames.push(cur);
    cur = [];
    C.renderWebGL = n % 2 === 0 ? nat : jsRender;
    n++;
  };
  game.events.on("prerender", onPre);
  globalThis.__vfy = () => {
    game.events.off("prerender", onPre);
    WP.flush = origFlush;
    C.renderWebGL = nat;
    if (cur) frames.push(cur);
    const cmp = (a, b) => {
      if (!a || !b) return "missing";
      if (a.length !== b.length) return "flushes " + a.length + " vs " + b.length;
      const out = [];
      for (let i = 0; i < a.length; i++) {
        if (a[i] === b[i]) continue;
        const x = a[i].split(":")[2].split(","), y = b[i].split(":")[2].split(",");
        const verts = new Set();
        let first = "";
        for (let j = 0; j < Math.max(x.length, y.length); j++) {
          if (x[j] === y[j]) continue;
          verts.add(Math.floor(j / 7));
          if (!first) {
            const f = (u) => new Float32Array(new Uint32Array([+u]).buffer)[0];
            first = " (vertex " + Math.floor(j / 7) + " field " + (j % 7) + ": " + x[j] + "/" + f(x[j]) + " vs " + y[j] + "/" + f(y[j]) + ")";
          }
        }
        out.push("f" + i + ":" + [...verts].join("/") + first);
      }
      return out.length ? out.join(" ") : "identical";
    };
    return ["0n-1js: " + cmp(frames[0], frames[1]), "1js-2n: " + cmp(frames[1], frames[2]), "0n-2n: " + cmp(frames[0], frames[2]), "1js-3js: " + cmp(frames[1], frames[3])].join(" | ");
  };
  return "armed";
})()
