// SPDX-License-Identifier: AGPL-3.0-only
// Usage: node act.js <shotName> [key|wait:ms ...]
// Keys: Space (action), Backspace (cancel), Enter (submit), ArrowUp/Down/Left/Right, Escape (menu)
const puppeteer = require("/opt/pptr/node_modules/puppeteer-core");
const [, , shot, ...steps] = process.argv;
(async () => {
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:9222", defaultViewport: { width: 1280, height: 720 } });
  const page = (await browser.pages()).find((p) => p.url().includes("127.0.0.1:8000"));
  await page.bringToFront();
  const cdp = await page.createCDPSession();
  await cdp.send("Emulation.setFocusEmulationEnabled", { enabled: true });
  await cdp.send("Page.setWebLifecycleState", { state: "active" });
  for (const s of steps) {
    if (s.startsWith("wait:")) await new Promise((r) => setTimeout(r, +s.slice(5)));
    else { await page.keyboard.down(s); await new Promise((r) => setTimeout(r, 80)); await page.keyboard.up(s); await new Promise((r) => setTimeout(r, 400)); }
  }
  await new Promise((r) => setTimeout(r, 1500));
  await page.screenshot({ path: "/shots/" + shot + ".png" });
  console.log(JSON.stringify(await page.evaluate(() => window.__pkrSummary(60))));
  browser.disconnect();
})();
