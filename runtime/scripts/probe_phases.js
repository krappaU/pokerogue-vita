// SPDX-License-Identifier: AGPL-3.0-only
// Diagnostic probe: average time per frame in the game's update and render phases (two timers,
// so it barely changes what it measures). `evalfile` it, wait, then `eval __phases()`.
(() => {
  const SM = Phaser.Scenes.SceneManager.prototype;
  const t = { update: 0, render: 0, frames: 0 };
  globalThis.__phases = (reset) => {
    const f = t.frames || 1, r = "update " + (t.update / f).toFixed(3) + " ms, render " + (t.render / f).toFixed(3) + " ms over " + t.frames + " frames";
    if (reset !== false) t.update = t.render = t.frames = 0;
    return r;
  };
  if (SM.__phased) return "reset";
  const u = SM.update, r = SM.render;
  SM.update = function () { const s = performance.now(); try { return u.apply(this, arguments); } finally { t.update += performance.now() - s; t.frames++; } };
  SM.render = function () { const s = performance.now(); try { return r.apply(this, arguments); } finally { t.render += performance.now() - s; } };
  SM.__phased = true;
  return "phase timers installed";
})();
