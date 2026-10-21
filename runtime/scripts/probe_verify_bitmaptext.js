// SPDX-License-Identifier: AGPL-3.0-only
// Checks js/fastpath.js part 18 (BitmapText.renderWebGL with its character loop in C) against
// Phaser's renderer: with tweens, animations and the game clock frozen, frames alternate between the
// two (0 and 2 native, 1 and 3 Phaser's) and every flushed vertex batch is compared. The native
// frames have the renderer itself installed, so containers draw their bitmap texts in C
// (src/fastpath.c batch_bitmap_text) and the rest go through its JS with the character loop in C.
// Run: scripts/drive.sh "evalfile scripts/probe_verify_bitmaptext.js", wait, then "eval __vbt()".
(() => {
  const game = __rt.renderer.game, swap = __rt.bitmapTextSwap;
  if (!swap) return "native bitmap text not active";
  const [BTP, , orig, fast] = swap[0];
  for (const s of game.scene.getScenes(true)) { s.tweens.pauseAll(); s.anims.pauseAll(); }
  const t0 = game.getTime();
  game.getTime = () => t0;
  const WP = Phaser.Renderer.WebGL.WebGLPipeline.prototype, origFlush = WP.flush;
  const frames = [];
  let cur = null, n = 0, chars = 0;
  const count = (list) => {
    for (const o of list) {
      if (o instanceof Phaser.GameObjects.BitmapText && o.visible) chars += o.text.length;
      if (o.list) count(o.list);
    }
  };
  for (const s of game.scene.getScenes(true)) count(s.children.list);
  WP.flush = function () {
    if (cur && this.vertexCount > 0) cur.push(this.name + ":" + this.vertexCount + ":" + Array.from(new Uint32Array(this.vertexData, 0, this.vertexCount * 7)).join(","));
    return origFlush.apply(this, arguments);
  };
  const onPre = () => {
    if (n >= 5) return;
    if (cur) frames.push(cur);
    cur = n < 4 ? [] : null;
    BTP.renderWebGL = n % 2 === 0 ? fast : orig;
    n++;
  };
  game.events.on("prerender", onPre);
  globalThis.__vbt = () => {
    game.events.off("prerender", onPre);
    WP.flush = origFlush;
    BTP.renderWebGL = fast;
    if (!chars) return "0n-1js: no bitmap text drawn";
    const cmp = (a, b) => {
      if (!a || !b) return "missing";
      if (a.length !== b.length) return "flushes " + a.length + " vs " + b.length;
      for (let i = 0; i < a.length; i++) {
        if (a[i] === b[i]) continue;
        const x = a[i].split(":"), y = b[i].split(":"), xs = x[2].split(","), ys = y[2].split(",");
        const f = (u) => new Float32Array(new Uint32Array([+u]).buffer)[0];
        for (let j = 0; j < Math.max(xs.length, ys.length); j++)
          if (xs[j] !== ys[j]) return "flush " + i + " (" + x[0] + ", " + x[1] + " vs " + y[1] + " vertices): vertex " + Math.floor(j / 7) +
            " field " + (j % 7) + ": " + xs[j] + "/" + f(xs[j]) + " vs " + ys[j] + "/" + f(ys[j]);
        return "flush " + i + " differs";
      }
      return "identical";
    };
    return ["0n-1js: " + cmp(frames[0], frames[1]), "1js-2n: " + cmp(frames[1], frames[2]), "0n-2n: " + cmp(frames[0], frames[2]),
      "1js-3js: " + cmp(frames[1], frames[3])].join(" | ");
  };
  return "armed";
})()
