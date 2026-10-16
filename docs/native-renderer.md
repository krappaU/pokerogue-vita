# Native renderer (branch `native-renderer`)

Goal: cut the JavaScript time per battle frame on the Vita (60-80 ms in the 0.38-0.41 logs, so
10-15 fps) by doing more of Phaser's work in C, without touching the game's code and while keeping
auto-updates. Bug fixes continue on `main`; this branch merges `main` in regularly and goes back to
`main` piece by piece, each piece checked against Phaser's own output.

## Where the time goes (desktop profile, 2026-10-16, runtime 0.41, offline game e4e9b53)
Idle battle, 10,509 JS samples at one per 1,000 interpreter operations:
- **Rendering 47%.** Of that: PokeRogue's own sprite pipelines 55% (their `onBind`/`onBatch`
  per sprite: ~10 uniforms, colour arrays rebuilt with `splice(...spread)` and `map`), Phaser's JS
  40% (pipeline switching: `PipelineManager.set`, `rebind`, `setAttribPointers`,
  `createAttribLocation`; `setProgram`/`setBlendMode`; the `set1f`/`set3fv`... wrappers; renderers
  for Graphics, BitmapText and shapes; the top-level render walk), our JS 5%.
- **Updates 37%**, mostly PokeRogue's party-icon animation in this test (it moves the 578
  starter-select icons; it doesn't run in sessions that start from a saved run). Tweens and
  animations are the rest.

The game's pipeline code can't be skipped (it decides each sprite's shader values), but what it
calls into can be made cheaper, and everything around it can move to C.

## Steps
1. **Engine (QuickJS) fast paths. Done.** `patches/quickjs-0002-array-fast-paths.patch`:
   `f(a, ...arr)` without an iterator object; array species creation without a generic
   constructor call. Spread `splice` -31%, spread `push` -38%, `flat` -12% (standalone qjs,
   desktop). QuickJS's own tests unchanged.
2. **Pipeline machinery in C. Done** (`src/fastpath.c` "pipeline machinery", `js/fastpath.js`
   part 14): `PipelineManager.set`/`isCurrent`, `WebGLPipeline.bind`/`updateProjectionMatrix`
   and its 21 `set1f`... uniform wrappers, `WebGLShader.bind`/`setAttribPointers` and its 21
   `set1f`..., `WebGLRenderer.setProgram`/`setBlendMode` (48 functions). Each calls the others
   directly while an object still has the native version, so a uniform set from the game's
   `onBind` is one C call instead of three JS calls; anything a game overrides is called through
   JS. The rare paths stay Phaser's (`rebind` and `setAttribPointers(true)` after a GL reset).
   Installed only if Phaser's functions still look like 3.90's.
   - Checked (2026-10-16, offline game e4e9b53, Phaser 3.90.0): `scripts/probe_verify_pipeline.js`
     records every WebGL call (arguments, objects by first use, a hash of the vertex bytes per
     draw) with animations frozen, alternating native and Phaser frames: identical in the
     battle command menu (193 calls/frame), fight menu, during a move, the pause menu, the EXP
     message and the reward shop. `probe_verify_uniforms.js` and `probe_verify_batching.js`
     unchanged (identical).
   - Measured on desktop (`scripts/probe_pipeline_time.js`, draw calls skipped because the
     software GL dominates otherwise; render time per frame in the battle command menu,
     alternating every 30 frames, ~1,600 frames each): median 1.92 vs 2.02 ms and 1.83 vs
     1.87 ms, i.e. 2-5% of the render time. Less than the 16% of profile samples these
     functions had: a property read costs about the same in C as in the interpreter, so the
     saving is mostly the JS calls and bytecode around them.
   - Making desktop test runs reach a battle reliably: `scripts/run_saved_battle.sh` (starts from
     a saved run, presses through by the game's UI mode).
3. **Remaining game-object renderers:** Graphics (the HP/EXP bars, overlays), BitmapText,
   Rectangle and other shapes, TileSprite, RenderTexture; Text in the cases the current fast path
   skips.
4. **The render walk itself:** `WebGLRenderer.render` (cameras, display list, render lists,
   `preRender`/`postRender`), so the per-frame tree walk happens in C end to end.
5. **Updates:** Tween and animation stepping for the common cases (numeric properties, standard
   eases), the UpdateList.

Each step: profile, port, check with the probes (`scripts/probe_verify_render.js`,
`probe_verify_uniforms.js`, `probe_verify_batching.js`, plus new ones for each step), measure on
desktop, then a Vita build for the tester. Anything unusual (masks, post pipelines, custom
renderers) keeps Phaser's path, as the existing fast paths do.

## Not planned
Rewriting or patching the game's own code (its pipelines included): every game update would need
the same work again. A JIT for QuickJS stays an idea to check separately (executable memory is
available to Vita homebrew via the memory blocks emulators use).
