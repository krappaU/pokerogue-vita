// SPDX-License-Identifier: AGPL-3.0-only
// Fresh save -> new Classic run with two starters -> wait in battle -> measure.
const puppeteer = require("/opt/pptr/node_modules/puppeteer-core");
const fs = require("fs");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const browser = await puppeteer.connect({ browserURL: "http://127.0.0.1:9222", defaultViewport: { width: 1280, height: 720 } });
  for (const p of await browser.pages()) if (p.url().includes("127.0.0.1:8000")) await p.close();
  const page = await browser.newPage();
  await page.evaluateOnNewDocument(fs.readFileSync("/tools/inject.js", "utf8"));
  await page.goto("http://127.0.0.1:8000/", { waitUntil: "load", timeout: 120000 });
  await page.evaluate(() => localStorage.clear());
  await page.reload({ waitUntil: "load" });
  const press = async (k, after = 600) => { await page.keyboard.down(k); await sleep(80); await page.keyboard.up(k); await sleep(after); };
  const shot = async (name) => { await page.screenshot({ path: "/shots/" + name + ".png" }); };
  const measure = async (label, ms = 4000) => {
    await sleep(ms);
    const s = await page.evaluate(() => window.__pkrSummary(120));
    console.log(label.padEnd(22), JSON.stringify(s));
    await shot(label);
  };
  await sleep(10000);
  await press("Space", 1500); await press("Space", 2500);   // intro text, gender
  await press("Space", 3000);                              // Boy -> title
  await measure("title");
  await press("Space", 2500); await press("Space", 6000);  // New Game, Classic
  await measure("starter-select");
  await press("Space", 800); await press("Space", 800);    // add Bulbasaur
  await press("ArrowRight", 400); await press("Space", 800); await press("Space", 800); // add Charmander
  await press("Enter", 1500); await press("Space", 3000);  // start, yes
  await press("Space", 20000);                             // save slot, then wait for battle intro
  await measure("battle-idle", 6000);
  browser.disconnect();
})();
