// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: time per frame in the update and render phases and in chosen functions
// (game-object renderers other than the natively handled Image/Sprite/Container/Text, masks, the
// game's pipeline callbacks, tween stepping), with draw calls skipped so the desktop's software GL
// doesn't hide the JS side. Nested timed functions count in both. `evalfile` it, wait, `eval __costs()`.
(() => {
  const game = __rt.renderer.game, gl = __rt.renderer.gl, proto = Object.getPrototypeOf(gl);
  const draw = proto.drawArrays, drawEl = proto.drawElements;
  proto.drawArrays = proto.drawElements = function () {};
  const t = {}, calls = {}, undo = [];
  let frames = 0;
  const wrap = (o, k, label) => {
    const f = o && o[k];
    if (typeof f !== "function" || f.__timed) return;
    const w = function () { calls[label] = (calls[label] || 0) + 1; const s = performance.now(); try { return f.apply(this, arguments); } finally { t[label] = (t[label] || 0) + performance.now() - s; } };
    w.__timed = true;
    o[k] = w;
    undo.push(() => { o[k] = f; });
  };
  const SM = Phaser.Scenes.SceneManager.prototype, GO = Phaser.GameObjects;
  wrap(SM, "update", "update (all)");
  wrap(SM, "render", "render (all)");
  for (const n of ["Graphics", "Rectangle", "Ellipse", "Arc", "Polygon", "Triangle", "Line", "BitmapText", "DynamicBitmapText", "TileSprite", "RenderTexture", "NineSlice", "Mesh", "Particles"])
    if (GO[n]) wrap(GO[n].prototype, "renderWebGL", "renderWebGL " + n);
  wrap(Phaser.Display.Masks.GeometryMask.prototype, "preRenderWebGL", "GeometryMask.preRenderWebGL");
  wrap(Phaser.Display.Masks.GeometryMask.prototype, "postRenderWebGL", "GeometryMask.postRenderWebGL");
  wrap(Phaser.Tweens.TweenManager.prototype, "step", "TweenManager.step");
  wrap(Phaser.Animations.AnimationState.prototype, "update", "AnimationState.update");
  const seen = new Set();
  for (const p of __rt.renderer.pipelines.pipelines.values()) {
    const P = Object.getPrototypeOf(p);
    if (seen.has(P)) continue;
    seen.add(P);
    for (const k of ["onBind", "onBatch", "batchQuad", "onPreRender"])
      if (Object.prototype.hasOwnProperty.call(P, k)) wrap(P, k, p.name + "." + k);
  }
  const onPost = () => frames++;
  game.events.on("postrender", onPost);
  globalThis.__costs = () => {
    game.events.off("postrender", onPost);
    for (const u of undo) u();
    proto.drawArrays = draw; proto.drawElements = drawEl;
    const f = frames || 1;
    return frames + " frames | " + Object.keys(t).sort((a, b) => t[b] - t[a]).map((k) => k + " " + (t[k] / f).toFixed(3) + " (" + (calls[k] / f).toFixed(1) + "x)").join(" | ") + " (ms/frame, calls/frame)";
  };
  return "armed";
})()
