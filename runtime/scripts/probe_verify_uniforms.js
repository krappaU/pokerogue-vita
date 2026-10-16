// SPDX-License-Identifier: AGPL-3.0-only
// Checks the native WebGLShader.setUniform1..4 (js/fastpath.js part 13) against Phaser's: with tweens,
// animations and the game clock frozen, frames alternate between the two and every GL uniform call
// (location, values) is recorded. Run: scripts/drive.sh "evalfile scripts/probe_verify_uniforms.js",
// wait a few seconds, then "eval __vfu()".
(() => {
  const game = __rt.renderer.game, gl = __rt.renderer.gl;
  const WS = Phaser.Renderer.WebGL.WebGLShader.prototype, orig = __rt.origSetUniform;
  if (!orig) return "native uniforms not active";
  const nat = [WS.setUniform1, WS.setUniform2, WS.setUniform3, WS.setUniform4];
  const scene = game.scene.getScenes(true)[0];
  scene.tweens.pauseAll(); scene.anims.pauseAll();
  const t0 = game.getTime(), origGetTime = game.getTime;
  game.getTime = () => t0;
  const proto = Object.getPrototypeOf(gl), names = Object.getOwnPropertyNames(proto).filter((k) => /^uniform/.test(k) && typeof proto[k] === "function");
  const saved = {};
  let cur = null;
  const ids = new Map();
  for (const k of names) {
    saved[k] = proto[k];
    proto[k] = function (loc, ...v) {
      if (cur) {
        if (!ids.has(loc)) ids.set(loc, ids.size);
        cur.push(k + "@" + ids.get(loc) + "=" + v.map((x) => (x && x.length !== undefined ? Array.from(x).join(",") : String(x))).join(";"));
      }
      return saved[k].call(this, loc, ...v);
    };
  }
  const frames = [];
  let n = 0;
  const set = (a) => { WS.setUniform1 = a[0]; WS.setUniform2 = a[1]; WS.setUniform3 = a[2]; WS.setUniform4 = a[3]; };
  const onPre = () => {
    if (n > 8) return;
    if (cur) frames.push(cur);
    if (n === 8) { cur = null; n++; return; }
    cur = [];
    set(n % 2 === 0 ? nat : orig);
    n++;
  };
  game.events.on("prerender", onPre);
  globalThis.__vfu = () => {
    game.events.off("prerender", onPre);
    for (const k of names) proto[k] = saved[k];
    set(nat);
    game.getTime = origGetTime;
    scene.tweens.resumeAll(); scene.anims.resumeAll();
    const cmp = (a, b) => {
      if (!a || !b) return "missing";
      if (a.join("|") === b.join("|")) return "identical (" + a.length + " calls)";
      let i = 0;
      while (i < a.length && a[i] === b[i]) i++;
      return "differ at call " + i + "/" + a.length + " vs " + b.length + ": " + a[i] + " <> " + b[i];
    };
    // frames 2+ (the first ones may still see the freeze settle)
    return ["2n-3o: " + cmp(frames[2], frames[3]), "3o-4n: " + cmp(frames[3], frames[4]), "4n-6n: " + cmp(frames[4], frames[6]), "5o-7o: " + cmp(frames[5], frames[7])].join(" | ");
  };
  return "armed";
})()
