// SPDX-License-Identifier: AGPL-3.0-only
// Adds a timing breakdown to the running game page, samples for a few seconds, prints it.
const puppeteer = require("/opt/pptr/node_modules/puppeteer-core");
const secs = +(process.argv[2] || 4);
(async () => {
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:9222", defaultViewport: { width: 1280, height: 720 } });
  const page = (await browser.pages()).find((p) => p.url().includes(process.env.PAGE || "127.0.0.1:8000/"));
  await page.bringToFront();
  const cdp = await page.createCDPSession();
  await cdp.send("Emulation.setFocusEmulationEnabled", { enabled: true });
  await cdp.send("Page.setWebLifecycleState", { state: "active" });
  await page.evaluate(() => {
    if (window.__probe) { window.__probe.reset(); return; }
    const game = window.__game, P = window.Phaser;
    const acc = {};
    const add = (k, ms) => { const a = (acc[k] ||= { ms: 0, n: 0 }); a.ms += ms; a.n++; };
    const timeIt = (obj, name, label) => {
      const o = obj[name];
      obj[name] = function () { const t = performance.now(); try { return o.apply(this, arguments); } finally { add(label, performance.now() - t); } };
    };
    timeIt(game.loop, "callback", "frame");
    timeIt(game.scene, "update", "update (game logic, tweens, anims)");
    timeIt(game.scene, "render", "render (display list walk + batching)");
    timeIt(P.Renderer.WebGL.Pipelines.MultiPipeline.prototype, "flush", "  pipeline flush");
    timeIt(P.GameObjects.Text.prototype, "updateText", "  Text.updateText (canvas text redraw)");
    // Count and time every WebGL call
    let glCalls = 0, glMs = 0;
    const GLP = WebGLRenderingContext.prototype;
    for (const k of Object.getOwnPropertyNames(GLP)) {
      const d = Object.getOwnPropertyDescriptor(GLP, k);
      if (!d || typeof d.value !== "function") continue;
      const o = d.value;
      GLP[k] = function () { glCalls++; const t = performance.now(); try { return o.apply(this, arguments); } finally { glMs += performance.now() - t; } };
    }
    window.__probe = {
      reset() { for (const k in acc) delete acc[k]; glCalls = 0; glMs = 0; },
      report() {
        const n = (acc.frame && acc.frame.n) || 1;
        const out = {};
        for (const [k, v] of Object.entries(acc)) out[k] = { msPerFrame: +(v.ms / n).toFixed(3), callsPerFrame: +(v.n / n).toFixed(1) };
        out["  WebGL calls (inside render)"] = { msPerFrame: +(glMs / n).toFixed(3), callsPerFrame: +(glCalls / n).toFixed(1) };
        out.frames = n;
        return out;
      },
    };
  });
  await new Promise((r) => setTimeout(r, secs * 1000));
  const rep = await page.evaluate(() => window.__probe.report());
  for (const [k, v] of Object.entries(rep)) console.log(k.padEnd(42), typeof v === "object" ? `${String(v.msPerFrame).padStart(7)} ms/frame  ${String(v.callsPerFrame).padStart(6)} calls/frame` : v);
  browser.disconnect();
})();
