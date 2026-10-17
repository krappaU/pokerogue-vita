// SPDX-License-Identifier: AGPL-3.0-only
// Checks the C EventEmitter.emit (js/fastpath.js part 17) against eventemitter3's on scripted cases:
// single and several listeners, once, removal and addition while emitting, contexts, 0-7
// arguments, return values, a throwing listener. Run: scripts/drive.sh "evalfile scripts/probe_verify_emit.js".
(() => {
  const sw = __rt.emitSwap;
  if (!sw) return "native emit not active";
  const [o, k, js, nat] = sw[0];
  const run = (impl) => {
    o[k] = impl;
    const log = [];
    try {
      const E = Phaser.Events.EventEmitter, e = new E(), ctx = { name: "ctx" };
      const rec = (tag) => function (...a) { log.push(tag + ":" + (this && this.name) + ":" + a.length + ":" + a.join(",")); };
      log.push("none:" + e.emit("nothing", 1));
      e.on("a", rec("a1"), ctx);
      for (let n = 0; n <= 7; n++) log.push("ret:" + e.emit("a", ...Array.from({ length: n }, (_, i) => i)));
      e.once("b", rec("b1")); e.on("b", rec("b2"), ctx); e.once("b", rec("b3"));
      e.emit("b", "x"); e.emit("b", "y", "z");
      const self = function () { log.push("self"); e.off("c", self); e.on("c", rec("late")); };
      e.on("c", self); e.on("c", rec("c2")); e.emit("c", 1, 2, 3, 4, 5); e.emit("c");
      e.on("d", () => { throw new Error("boom"); }); e.on("d", rec("d2"));
      try { e.emit("d", 1); } catch (err) { log.push("threw:" + err.message); }
      e.on("f", Phaser.Renderer.WebGL.WebGLPipeline.prototype.onPreRender, ctx); e.on("f", rec("f2"));
      log.push("f:" + e.emit("f", 9));
      log.push("count:" + e.listenerCount("b") + "," + e.listenerCount("c"));
    } catch (err) { log.push("error:" + err); }
    o[k] = nat;
    return log.join(" ");
  };
  const a = run(js), b = run(nat);
  return a === b ? "identical: " + a.slice(0, 300) : "DIFFER\njs:  " + a + "\nnat: " + b;
})()
