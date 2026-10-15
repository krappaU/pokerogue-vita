// SPDX-License-Identifier: AGPL-3.0-only
// Render time per frame, native vs JS container renderer (alternating every 60 frames).
// Run: scripts/drive.sh "evalfile scripts/probe_render_time.js", wait 30 s, then "eval __rt_done()".
(() => {
  const game = __rt.renderer.game, C = Phaser.GameObjects.Container.prototype;
  const modes = [["native", __native.renderContainer], ["js", __rt.jsContainerRender]];
  const res = { native: [], js: [] };
  let t0 = 0, frame = 0, mode = 0;
  const pre = () => { if (frame % 60 === 0) mode = (frame / 60) % 2; C.renderWebGL = modes[mode][1]; t0 = performance.now(); };
  const post = () => { res[modes[mode][0]].push(performance.now() - t0); frame++; };
  game.events.on("prerender", pre); game.events.on("postrender", post);
  globalThis.__rt_done = () => {
    game.events.off("prerender", pre); game.events.off("postrender", post); C.renderWebGL = __native.renderContainer;
    const s = (a) => { a = a.slice().sort((x, y) => x - y); return a.length + " frames, median " + a[a.length >> 1].toFixed(3) + " ms, mean " + (a.reduce((x, y) => x + y, 0) / a.length).toFixed(3); };
    return "native: " + s(res.native) + " | js: " + s(res.js);
  };
  return "armed";
})()
