// SPDX-License-Identifier: AGPL-3.0-only
// CPU-profile the game page for N seconds; print top functions by self time.
const puppeteer = require("/opt/pptr/node_modules/puppeteer-core");
const secs = +(process.argv[2] || 6);
(async () => {
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:9222" });
  const pages = await browser.pages();
  const page = pages.find((p) => p.url() === (process.env.URL || "http://127.0.0.1:8000/"));
  await page.bringToFront();
  const cdp = await page.createCDPSession();
  await cdp.send("Profiler.enable");
  await cdp.send("Profiler.setSamplingInterval", { interval: 100 });
  await cdp.send("Profiler.start");
  await new Promise((r) => setTimeout(r, secs * 1000));
  const { profile } = await cdp.send("Profiler.stop");
  const frames = await page.evaluate(() => window.__pkrSummary ? window.__pkrSummary(600).frames : 0);
  const dt = profile.timeDeltas; const self = new Map(); let total = 0;
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  for (let i = 0; i < profile.samples.length; i++) {
    const n = byId.get(profile.samples[i]); const d = dt[i] || 0;
    const cf = n.callFrame; const key = (cf.functionName || "(anon)") + " " + cf.url.split("/").pop() + ":" + (cf.lineNumber + 1);
    if (["(idle)", "(program)", "(garbage collector)"].includes(cf.functionName)) { self.set(cf.functionName, (self.get(cf.functionName) || 0) + d); continue; }
    self.set(key, (self.get(key) || 0) + d); total += d;
  }
  const rows = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 40);
  console.log("JS busy (ms):", (total / 1000).toFixed(0), "over", secs, "s");
  for (const [k, v] of rows) console.log((v / 1000).toFixed(1).padStart(8), "ms ", (100 * v / total).toFixed(1).padStart(5) + "%", k);
  browser.disconnect();
})();
