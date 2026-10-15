// SPDX-License-Identifier: AGPL-3.0-only
// Checks js/fastpath.js's TextStyle.setStyle against Phaser's: records the style configs the game
// passes for a while, then builds each style both ways and compares every field.
// Run: scripts/drive.sh "evalfile scripts/probe_textstyle.js", use the game, then "eval __tsCheck()".
(() => {
  const TS = Phaser.GameObjects.TextStyle, TSP = TS.prototype, fast = TSP.setStyle, orig = __rt.origSetStyle;
  if (!orig) return "fast setStyle not installed";
  const seen = [];
  TSP.setStyle = function (e, t, n) {
    if (seen.length < 3000) seen.push([e, !!n]);
    return fast.apply(this, arguments);
  };
  globalThis.__tsCheck = () => {
    TSP.setStyle = fast;
    const parent = { updateText() { return this; } };
    let bad = 0, first = "";
    for (const [e, n] of seen) {
      const build = (fn) => {
        TSP.setStyle = fn;
        const s = new TS(parent, {});
        fn.call(s, e, false, n);
        TSP.setStyle = fast;
        const o = s.toJSON();
        o._font = s._font;
        return JSON.stringify(o, (k, v) => (typeof v === "function" ? "fn:" + v.name : v));
      };
      const a = build(fast), b = build(orig);
      if (a !== b) { bad++; if (!first) first = a + " VS " + b; }
    }
    return seen.length + " configs, " + bad + " differ" + (first ? ": " + first.slice(0, 600) : "");
  };
  return "recording";
})()
