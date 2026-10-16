# Measuring the real game

These tools run the real PokeRogue client in headless Chrome and count, per frame, what Phaser draws.
They first calibrated the Phase 0 synthetic benchmark (archive/phase0) against the real game; Chrome's
output is also the reference picture for the runtime's renderer.

1. Clone pagefaultgames/pokerogue and `git clone --depth 1 https://github.com/pagefaultgames/pokerogue-locales locales` inside it.
   The assets submodule is not needed: `serve.js` fetches missing files from pokerogue.net and caches them.
2. Optional: `git apply late-game-overrides.patch` (wave 120, level 100, double battles, ~40 items).
3. `pnpm install`, then `VITE_BYPASS_TUTORIAL=1 pnpm exec vite build --mode development`. Dev mode skips login.
4. `./start_browser.sh <pokerogue>/dist` builds the Chrome image and starts Chrome plus the asset proxy.
5. `docker exec pkr-browse node /tools/run.js` loads the page with `inject.js` and tries to reach a battle (its timing is fragile, so finish by hand if needed).
   `docker exec pkr-browse node /tools/act.js <shot> <keys...>` drives the menus and prints per-frame
   counts. Keys: Space = action, Backspace = cancel, Enter = start, arrow keys = move.
   `probe.js` prints a per-frame timing breakdown; `cpuprof.js` prints the top functions from Chrome's CPU profiler.

Chrome uses software GL (swiftshader) here, so it only manages about 2 fps. That is fine for counting
objects, but its timings say little about the Vita.

## Results (2026-10-14, game v1.12.1)
| screen | objects rendered / frame | quads / frame | containers | pipeline flushes | WebGL calls |
|---|---|---|---|---|---|
| title | ~40 | ~15 | ~10–19 | ~10 | – |
| starter select (fresh save, menu open) | ~200 | ~120 | ~64 | ~21 | – |
| early battle (wave 1, single) | ~50–80 | ~20–55 | ~25–40 | ~15 | – |
| late battle (wave 120 double, ~40 items, weather) | ~225 | ~135 | ~88 | ~19 | ~290 |
| synthetic "battle" scene (bench.js) | 166 | ~160 | 6 | 1 | ~71 |
