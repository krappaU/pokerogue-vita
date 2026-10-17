# Native renderer

Goal: cut the JavaScript time per battle frame on the Vita (60-80 ms in the 0.38-0.41 logs, so
10-15 fps) by doing more of Phaser's work in C, without touching the game's code and while keeping
auto-updates. Developed on `main` since 0.49 (2026-10-17; steps 1-3d were on the `native-renderer`
and `claude/project-thread-v4icpr` branches before), each piece checked against Phaser's own output.

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
3. **Remaining game-object renderers. Done for what PokeRogue draws** (`src/fastpath.c`
   "Graphics", "NineSlice", "Rectangle, GeometryMask"; `js/fastpath.js` parts 5, 15, 16):
   - `Graphics.renderWebGL` for command buffers of fill/line styles and `fillRect` (all of the
     game's Graphics in battles and menus are mask rectangles like that); paths, arcs and
     transforms keep Phaser's renderer. Installed only if `Graphics` still writes 3.90's
     command numbers.
   - `NineSlice.renderWebGL` (window frames) wholly in C; before, only its vertex loop was.
   - `Rectangle.renderWebGL` without stroke or rounded corners, and `GeometryMask`'s
     `preRenderWebGL`/`applyStencil`/`postRenderWebGL`.
   - Not done: BitmapText, TileSprite, RenderTexture, the other shapes. None of them showed up in
     the battle or menu screens measured (`scripts/probe_costs.js`, which times each renderer
     and the game's pipeline callbacks with draw calls skipped).
   - Measured costs (desktop, draw calls skipped, 2026-10-16), before -> after, ms per frame:
     pause menu over a battle: NineSlice 0.155 -> 0.108, Rectangle 0.050 -> 0.024, Graphics
     0.052 -> 0.021. GeometryMask stays ~0.12: its time is the flush and the stencil clear, i.e.
     GL work. Render time per frame, steps 2+3 against the code they replace
     (`scripts/probe_pipeline_time.js`, ~1,600 frames each): pause menu 2.24 -> 2.04 ms, battle
     command menu 2.20 -> 2.03 ms (-8 to -9%).
   - Checked with `scripts/probe_verify_pipeline.js` (all 54 replaced functions, and each group
     alone): identical WebGL calls in the battle command menu, during a move, the pause menu,
     the settings screen. Of what's left of the render time, about half is the game's own
     pipeline callbacks (`onBind`/`onBatch`/`batchQuad` of its sprite pipelines).
3b. **More engine (QuickJS) fast paths. Done** (`patches/quickjs-0004-more-array-fast-paths.patch`).
   Measured per call in a battle (`scripts/probe_costs.js`, desktop, draws skipped, 2026-10-17):
   PokeRogue's `SpritePipeline` runs only twice per frame (once per Pokemon) but costs ~0.43 ms each
   time on desktop (`onBind` 0.2 ms, `batchQuad` + `onBatch` 0.14 ms): on the Vita that is most of
   a battle frame. Nearly all of it is building the 128-value colour arrays: 64
   `splice(len, 0, ...four)` per array (~1 us each on desktop), `flat()`, `map`. The patch makes
   species creation, appending splices, small spread/apply calls and `flat()` of numbers cheaper
   (standalone qjs, best of 9: spread splice 0.61-0.69 -> 0.58-0.60 us, push(...four) 0.37 -> 0.27
   us, flat of 128 numbers 5.4 -> 1.9 us; single runs on this server vary by 30%). In the game:
   `SpritePipeline.onBind` 0.415 -> 0.323 ms per frame, `onBatch` 0.246 -> 0.217 (40 s averages,
   so also only roughly). The interpreter's own work per operation dominates the rest.
3c. **Profiling on the Vita.** `config.txt` `sample=1` samples the JS profile from the start and
   `profile_rate=1000` samples ten times as often (QuickJS's interrupt rate is now set at run time,
   `patches/quickjs-0005`); a build with CMake `PKR_PROFILE_BUILD=ON` does both by default. Every
   30 s the log gets `[prof] groups:` (render: PokeRogue's sprite pipelines / text / native code /
   Phaser JS, update: tweens / sprite animations / other, frame events, runtime JS outside the
   frame) besides the top functions. Desktop, battle command menu, 2026-10-17: pipelines 35%,
   Phaser JS (render) 19%, tweens 16%, native 8%, sprite animations 5-8%, other updates 5%,
   frame events 4%, our JS 3-4%. Samples count JS operations (calls and loop iterations), not
   time: TweenManager.step is ~3% of the time on desktop but 16% of the samples.
3d. **Time-based profiles and three runtime-side savings** (2026-10-17). `sample_ms=1` (on in
   profiling builds) takes at most one sample per ~1 ms (random gaps; the timer restarts at each
   main-loop step so C time between JS runs isn't charged to the next JS), so samples follow time
   rather than operation counts. Desktop battle, draws skipped: PokeRogue's sprite pipelines 44%,
   native render code 16%, Phaser render JS 17%, tweens 8%, sprite animations 3%, frame events 5%.
   That profile showed, and these remove:
   - eventemitter3's `emit` (12% of samples): in C, skipping listeners that are WebGLPipeline's
     empty `onPreRender`/`onRender`/`onPostRender` (26 of the 30 renderer-event calls per frame).
     `scripts/probe_verify_emit.js` checks it against the JS one (once, removal while emitting,
     contexts, 0-7 arguments, a throwing listener); WebGL calls identical.
   - Text size accessors (3%): the C renderer reads a drawn text's stored size and origin instead
     of calling the lazy-measuring getters four times per text per frame.
   - `querySelectorAll("input")` every 30 frames (177 us on desktop, a ~5 ms spike on the Vita):
     a bare tag name is matched directly (53 us); the text-field loop checks the cheap things first.
   Left in the profile: the game's `isOutside()` (5%, a 10-case switch over an enum object, called
   from every field sprite's `onBind`) and the style Proxy behind Phaser's DOM elements (2%).
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
