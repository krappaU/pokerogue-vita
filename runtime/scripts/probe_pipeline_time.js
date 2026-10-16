// SPDX-License-Identifier: AGPL-3.0-only
// Render time per frame with the C pipeline machinery, Graphics, NineSlice, Rectangle and
// GeometryMask (js/fastpath.js parts 14, 15, 5, 16) vs the code they replace, alternating every
// 30 frames. Draw calls are skipped meanwhile (the screen goes blank): on the
// desktop's software GL they take ~95% of the render time and would hide the JS side.
// Run: scripts/drive.sh "evalfile scripts/probe_pipeline_time.js", wait 60 s, then "eval __ppt()".
(() => {
  const game = __rt.renderer.game, gl = __rt.renderer.gl;
  const swaps = [].concat(__rt.pipelineSwaps || [], __rt.graphicsSwap || [], __rt.nineSliceSwap || [], __rt.shapesSwap || []);
  if (!swaps.length) return "no native swaps active";
  const proto = Object.getPrototypeOf(gl), draw = proto.drawArrays, drawEl = proto.drawElements;
  proto.drawArrays = proto.drawElements = function () {};
  const set = (native) => { for (const [o, k, orig, nat] of swaps) o[k] = native ? nat : orig; };
  const res = { native: [], phaser: [] }; // "phaser": the replaced code
  let t0 = 0, frame = 0, mode = "native";
  const pre = () => { mode = Math.floor(frame / 30) % 2 ? "phaser" : "native"; set(mode === "native"); t0 = performance.now(); };
  const post = () => { if (frame % 30 > 2) res[mode].push(performance.now() - t0); frame++; };
  game.events.on("prerender", pre); game.events.on("postrender", post);
  globalThis.__ppt = () => {
    game.events.off("prerender", pre); game.events.off("postrender", post);
    set(true);
    proto.drawArrays = draw; proto.drawElements = drawEl;
    const s = (a) => { a = a.slice().sort((x, y) => x - y); return a.length + " frames, median " + a[a.length >> 1].toFixed(3) + " ms, mean " + (a.reduce((x, y) => x + y, 0) / a.length).toFixed(3); };
    return "native: " + s(res.native) + " | phaser: " + s(res.phaser);
  };
  return "armed";
})()
