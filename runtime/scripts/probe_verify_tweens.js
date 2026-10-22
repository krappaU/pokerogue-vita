// SPDX-License-Identifier: AGPL-3.0-only
// Checks js/fastpath.js part 19 (Tween.update / TweenData.update in C) against Phaser's code: the same
// tweens on two sets of targets, in two TweenManagers the scene never steps, are updated with the same
// deltas, one set by the native update and the other by Phaser's, and the targets, the tweens' state
// and every callback (with its arguments) are compared after each step.
// Run: scripts/drive.sh "evalfile scripts/probe_verify_tweens.js", then "eval __vtw()".
(() => {
  const swap = __rt.tweenSwap;
  if (!swap) return "native tweens not active";
  const [TP, , orig, nat] = swap[0];
  const game = __rt.renderer.game, scene = game.scene.getScenes(true)[0];
  const configs = [
    { x: 100, duration: 300, ease: "Sine.easeInOut" },
    { x: -40, y: 70, duration: 250, ease: "Sine.easeIn", yoyo: true },
    { alpha: 0, duration: 180, ease: "Sine.easeOut", repeat: 2, repeatDelay: 40 },
    { x: 33.3, duration: 500, ease: "Cubic.easeOut", delay: 70, hold: 60, yoyo: true },
    { y: 12, duration: 220, ease: "Cubic.easeIn" },
    { x: 7, y: -7, duration: 410, ease: "Cubic.easeInOut", repeat: 1, yoyo: true },
    { x: 250, duration: 333, ease: "Linear" },
    { x: 60, duration: 290, ease: "Back.easeOut" },
    { y: 90, duration: 350, ease: "Bounce.Out", timeScale: 1.5 },
    { x: 15, duration: 0, ease: "Sine.easeIn" },
    { scaleX: 2, scaleY: 0.5, duration: 260, ease: "Sine.easeInOut", loop: 1 },
  ];
  const deltas = [16.7, 33.3, 33.4, 41, 5, 100, 16.7, 0, 66.6, 33.3, 250, 33.3, 33.3, 12.5, 400, 33.3, 33.3, 700, 33.3];
  const mk = () => {
    const tm = new Phaser.Tweens.TweenManager(scene), log = [], targets = [];
    const rec = (name) => (t, ...a) => log.push(name + ":" + a.map((v) => (v && typeof v === "object" ? "obj" : String(v))).join(","));
    configs.forEach((c, i) => {
      const o = { x: i, y: -i, alpha: 1, scaleX: 1, scaleY: 1 }, o2 = { x: 2 * i, y: i, alpha: 0.5, scaleX: 1, scaleY: 1 };
      targets.push(o, o2);
      const t = tm.add(Object.assign({}, c, { targets: i % 3 === 1 ? [o, o2] : o, onStart: rec("start" + i),
        onUpdate: i % 2 ? rec("update" + i) : undefined, onYoyo: rec("yoyo" + i), onRepeat: rec("repeat" + i), onComplete: rec("complete" + i) }));
      if (i % 4 === 3) t.on("update", (tw, key, target, cur, prev) => log.push("ev" + i + ":" + key + "," + cur + "," + prev));
    });
    const counter = tm.addCounter({ from: 0, to: 1, duration: 400, ease: "Sine.easeIn", onUpdate: (t) => log.push("counter:" + t.getValue()) });
    return { tm, log, targets, counter };
  };
  const a = mk(), b = mk();
  const state = (s) => s.tm.tweens.map((t) => [t.state, t.elapsed, t.progress, t.totalElapsed, t.totalProgress,
    t.data.map((d) => [d.state, d.elapsed, d.progress, d.current, d.previous].join("/")).join(";")].join("|")).join(" ") +
    " # " + s.targets.map((o) => [o.x, o.y, o.alpha, o.scaleX, o.scaleY].join(",")).join(" ");
  const step = (s, fn, dt) => {
    TP.update = fn;
    try { for (const t of s.tm.tweens.slice()) t.update(dt); } finally { TP.update = nat; }
  };
  let diff = "";
  deltas.forEach((dt, k) => {
    step(a, nat, dt);
    step(b, orig, dt);
    if (!diff) {
      const x = state(a), y = state(b);
      if (x !== y) diff = "step " + k + ": state " + x.slice(0, 200) + " vs " + y.slice(0, 200);
      else if (a.log.join(";") !== b.log.join(";")) diff = "step " + k + ": callbacks differ: " + a.log.slice(-3).join(";") + " vs " + b.log.slice(-3).join(";");
    }
  });
  const result = (diff ? "0n-1js: " + diff : "0n-1js: identical") + " | callbacks: " + (a.log.length === b.log.length && a.log.length > 50 ? "identical" : a.log.length + " vs " + b.log.length) +
    " | values: " + (state(a) === state(b) ? "identical" : "differ") + " | tweens: " + (a.tm.tweens.length ? "identical" : "none");
  globalThis.__vtw = () => result;
  return "armed";
})()
