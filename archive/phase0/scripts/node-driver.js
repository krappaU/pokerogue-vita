// SPDX-License-Identifier: AGPL-3.0-only
// Runs the same Phaser benchmark under Node (V8 with JIT) as a reference point.
// Usage: node scripts/node-driver.js   (from phase0/)
const fs = require("fs");
const vm = require("vm");
const { performance } = require("perf_hooks");
const now = () => performance.now();
globalThis.__log = (s) => console.log("  [js] " + s);
globalThis.__now = now;
delete globalThis.navigator; // Node 21+ has its own; use the shim's
for (const f of ["js/shim.js", "data/phaser.min.js", "js/bench.js"]) {
  const t0 = now();
  vm.runInThisContext(fs.readFileSync(f, "utf8"), { filename: f });
  console.log(`  loaded ${f} in ${(now() - t0).toFixed(0)} ms`);
}
if (__bench.error) { console.log(__bench.error); process.exit(1); }
let sim = 1000;
const tick = () => { sim += 1000 / 60; __tick(sim); };
(async () => {
  for (let i = 0; i < 600 && !__bench.ready; i++) { tick(); await null; }
  for (const name of ["idle", "battle", "heavy"]) {
    const objs = (__bench.start(name), __bench.objectCount());
    for (let i = 0; i < 60; i++) { tick(); await null; }
    const ft = [];
    for (let i = 0; i < 300; i++) { const t0 = now(); tick(); await null; ft.push(now() - t0); }
    ft.sort((a, b) => a - b);
    const avg = ft.reduce((a, b) => a + b) / ft.length;
    console.log(`  ${name.padEnd(7)} ${objs} objs  avg ${avg.toFixed(2)} ms  p50 ${ft[150].toFixed(2)}  p95 ${ft[285].toFixed(2)}  max ${ft[299].toFixed(2)}`);
  }
})();
