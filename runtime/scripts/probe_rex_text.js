// SPDX-License-Identifier: AGPL-3.0-only
// js/fastpath.js 4b in the running game: builds ten option labels the way PokeRogue's party screen
// builds its option window (rex BBCodeText with the game's own style, taken from a text on screen:
// constructor, setShadow, setLineSpacing, setText twice, then displayWidth), with the lazy drawing
// off and on, and reports the time of the build plus the frame that draws them, and whether the
// rendered pixels and sizes are the same. Run it on a screen that shows a BBCodeText (a menu):
//   scripts/drive.sh "evalfile scripts/probe_rex_text.js"   -> "[probe-rex] ..." lines in the log
(async () => {
  const rt = globalThis.__rt, game = rt.renderer.game, sc = game.scene.getScene("battle"), gl = game.renderer.gl;
  const log = (s) => console.log("[probe-rex] " + s);
  const find = (list) => {
    for (const o of list) {
      if (o.type === "rexBBCodeText" && o.style) return o;
      if (o.list) { const f = find(o.list); if (f) return f; }
    }
    return null;
  };
  const model = find(sc.children.list);
  if (!model) return log("no BBCodeText on screen to take the game's style from");
  const style = model.style.toJSON();
  // drawn at the size the game shows them (its UI container is scaled up)
  let k = 1;
  for (let p = model.parentContainer; p; p = p.parentContainer) k *= p.scaleX;
  const words = ["Leftovers", "Shell Bell", "Lum Berry", "King's Rock", "Quick Claw", "Focus Band", "Soothe Bell",
    "Lucky Egg", "All (12)", "Cancel"];
  const frame = () => new Promise((res) => {
    let t0 = 0;
    const pre = () => { t0 = performance.now(); };
    game.events.once("prestep", pre);
    game.events.once("postrender", () => {
      const px = new Uint8Array(gl.drawingBufferWidth * gl.drawingBufferHeight * 4);
      gl.readPixels(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight, gl.RGBA, gl.UNSIGNED_BYTE, px);
      res({ ms: performance.now() - t0, px });
    });
  });
  // an opaque backdrop over the probe's area: the screen behind animates (the party's icons)
  const back = sc.add.rectangle(0, 0, 640, 30 + 60 * words.length + 60, 0x203040).setOrigin(0).setDepth(1e9 - 1);
  const round = async (lazy) => {
    rt.lazyRexText = lazy;
    const before = await frame();
    const t0 = performance.now();
    const ts = words.map((s, i) => {
      const t = sc.add.rexBBCodeText(40, 40 + i * 60, s, JSON.parse(JSON.stringify(style))).setScale(model.scaleX * k, model.scaleY * k)
        .setShadow(model.style.shadowOffsetX, model.style.shadowOffsetY, model.style.shadowColor);
      t.setLineSpacing(model.lineSpacing || 0);
      if (i % 3 === 0) t.setText(s + ` (${i + 1})`);
      t.setText(`[shadow]${t.text}[/shadow]`);
      t.setDepth(1e9);
      return t;
    });
    const sizes = ts.map((t) => t.displayWidth.toFixed(3) + "x" + t.displayHeight.toFixed(3)).join(" ");
    const area = { x0: 0, y0: 0, x1: 640, y1: 30 + 60 * words.length + 60 };
    const build = performance.now() - t0;
    const f = await frame();
    ts.forEach((t) => t.destroy());
    return { build, render: f.ms, px: f.px, sizes, area, before };
  };
  const results = [];
  for (const lazy of [false, true, false, true]) results.push(await round(lazy));
  rt.lazyRexText = true;
  back.destroy();
  const [off, on, off2, on2] = results;
  // Pixels compared where the labels are (the screen behind may animate between frames); the
  // camera shows the world 1:1 in the drawing buffer, whose rows go bottom up.
  const W = gl.drawingBufferWidth, H = gl.drawingBufferHeight, area = off.area;
  const differ = (a, b) => {
    let n = 0;
    for (let y = Math.max(0, area.y0); y < Math.min(H, area.y1); y++)
      for (let x = Math.max(0, area.x0); x < Math.min(W, area.x1); x++) {
        const i = ((H - 1 - y) * W + x) * 4;
        if (a.px[i] !== b.px[i] || a.px[i + 1] !== b.px[i + 1] || a.px[i + 2] !== b.px[i + 2]) n++;
      }
    return n;
  };
  const diff = differ(off, on), diff2 = differ(off2, on2), shown = differ(off.before, off);
  const f = (r) => r.build.toFixed(1) + " ms build + " + r.render.toFixed(1) + " ms frame";
  log("rex's own drawing: " + f(off) + ", again " + f(off2));
  log("drawn when rendered: " + f(on) + ", again (kept textures) " + f(on2));
  log("sizes " + (off.sizes === on.sizes ? "the same" : "DIFFER: " + off.sizes + " vs " + on.sizes) + "; pixels where the labels are (" +
    Math.round(area.x1 - area.x0) + "x" + Math.round(area.y1 - area.y0) + "): " + diff + " differ (" + diff2 + " the second time); the labels change " + shown + " of them");
  log("stats " + JSON.stringify(rt.rexTextStats()));
})();
