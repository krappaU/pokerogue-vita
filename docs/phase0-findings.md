# Phase 0 findings

Phase 0 asked whether a QuickJS-based runtime on the PS Vita can run PokeRogue's JavaScript fast
enough. **Verdict: go, with native fast paths.** Measured October 2026 against PokeRogue v1.12.1 and
Phaser 3.90, on a PS Vita running firmware 3.65 with HENkaku and the CPU at 444 MHz.

## 1. What the game needs from a "browser"
Surveyed from the live site and the source:

- **JavaScript:** about 6 MB of ES2022 across 8 startup chunks (`#private` fields, `??=`, `?.`,
  `.at()`, ES modules), plus more chunks loaded on demand. No WebAssembly.
- **Rendering:** WebGL 1 only. `main.ts` forces `Phaser.WEBGL` and there is no canvas fallback.
  It has 3 custom GLSL shaders (`src/pipelines/glsl`), with `SpritePipeline` and `FieldSpritePipeline`
  used for Pokémon and field sprites. The canvas is 1920x1080 (a 320x180 layout scaled 6x).
- **Text:** Phaser `Text` and rex `BBCodeText` draw through Canvas 2D. Fonts are loaded with `FontFace`
  from .ttf files (`pokemon-emerald-pro.ttf` and others). `BitmapText` is also used in battle.
- **Audio:** Web Audio. Background music is mp3 (`audio/bgm/*.mp3`), sound effects are wav.
- **Storage and auth:** `localStorage`; the auth token is kept in `document.cookie` (`src/utils/cookies.ts`).
- **Input:** a keyboard config and several gamepad configs (`src/configs/inputs`, `gamepad: true`).
  Login uses DOM `<input>` elements via the rex InputText plugin.
- **Network:** `api.pokerogue.net` sits behind Cloudflare. A plain request gets **403**; sending a
  browser `User-Agent` (plus `Origin`/`Referer`) gets 200. Our fetch must send them.
- **Not available on Vita:** the built-in browser is NetFront (ES5, no WebGL), and no modern
  engine has been ported.

## 2. Benchmark design
`phase0/` contains one C program (`src/main.c`) that builds for Linux and Vita:

- **Test 1:** compile the live chunks as modules (compile only, with imports stubbed), write them
  as bytecode and read the bytecode back.
- **Test 2:** boot Phaser 3.90 inside a minimal fake DOM (`js/shim.js`) whose WebGL context is a
  no-op stub, then time frames of a synthetic battle screen (`js/bench.js`). This measures JS cost
  only. GPU, text rasterization and audio come on top.

## 3. Results

### Startup on the real Vita
| | real Vita | x86 QuickJS |
|---|---|---|
| Parse 6 MB of game JS | **14.8 s** | 0.89 s |
| Bytecode size | 22 MB (with debug info) | same |
| Load bytecode | **0.94 s** | 0.07 s |
| Heap used while compiling | 57 MB | – |
| Phaser alone (1.2 MB minified) | 3.4 s to parse | 0.2 s |

A bytecode cache keyed by chunk hash is mandatory. The first launch after a game update shows an
"updating" screen for about 15 s.

### Frame time (JS only, synthetic battle, 166 objects)
| build | real Vita | x86 QuickJS | x86 Node/V8 |
|---|---|---|---|
| Round 1: Thumb `-O2`, stock Phaser | 59.5 ms | 4.0 ms | 0.32 ms |
| Round 2: ARM `-O2 -mcpu=cortex-a9`, stock Phaser | 57.8 ms | 3.7 ms | – |
| Round 2: with native fast path | **27.4 ms** | 1.65 ms | – |
| Heavy scene (480 objects), with native fast path | 73.5 ms | 4.8 ms | – |

- The real Vita is about 15–17x slower than x86 QuickJS, and QuickJS is about 11x slower than V8.
- Vita frame times are very stable (p95 within 1 ms of the average).
- The ARM-mode compiler flags were worth only about 3%.
- **Where the time goes:** game logic (update) is about 3% of a frame. Phaser's render path is about
  95%, and `MultiPipeline.batchSprite` alone is about 60%.

### The native fast path (`phase0/src/fastpath.c`, `phase0/js/fastpath.js`)
- `batchSprite` math (applyITRS, multiplyWithOffset, multiply, setQuad, tint packing) and the
  42 vertex writes run in one C call instead of about 15 JS calls.
- `Container.renderWebGL` hands plain Image/Sprite children straight to the native batcher, without
  the temporary setAlpha/setScrollFactor round trip (6 JS calls per child).
- Rare cases fall back to Phaser's own code: cropped sprites, custom pivots, masks, post pipelines,
  and pipelines that override batchSprite/batchQuad (PokeRogue's SpritePipeline does).
- A built-in check renders identical scene state both ways and compares the vertex buffers:
  **0 differences**, on x86 and on the Vita.
- The speedup was about 2.1–2.3x everywhere (x86, Vita3K and real Vita).

### Calibration against the real game (`phase0/measure/`)
The real client ran in headless Chrome, with per-frame counting hooks:

| screen | objects drawn/frame | quads | containers | flushes | WebGL calls |
|---|---|---|---|---|---|
| title | ~40 | ~15 | ~10–19 | ~10 | – |
| starter select (new save, menu open) | ~200 | ~120 | ~64 | ~21 | – |
| early battle (wave 1, single) | ~50–80 | ~20–55 | ~25–40 | ~15 | – |
| late battle (wave 120 double, ~40 items, weather) | ~225 | ~135 | ~88 | ~19 | ~290 |
| synthetic battle (bench.js) | 166 | ~160 | 6 | 1 | ~71 |

- **Busy late-game screens:** fewer plain sprites than the synthetic scene, but about 15x more
  containers (each item icon is its own container) and about 19 pipeline flushes. The flushes come
  from SpritePipeline updating uniforms for each Pokémon sprite.
- **Estimate for a busy battle on the Vita with today's fast path:** about 35–45 ms (22–28 fps).
  Calmer screens should reach 30 fps.
- **Not measured:** the real game's frame cost in QuickJS. Phase 1 measures it directly.

## 4. Things learned the hard way
- **Emulator speed means nothing.** Vita3K recompiles ARM code to x86 and runs it as fast as the
  host allows. It ran our JS about 2–3x *faster* than the real Vita. Use it only to check that the
  app runs without crashing. Ratios between variants transfer reasonably; absolute numbers do not.
- **`%zu` is not supported** by the Vita's newlib printf, and the garbled varargs shift every later
  column. Cast to `unsigned long` and use `%lu`.
- VitaSDK builds need `-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake`. Including
  `vita.cmake` alone silently uses the host compiler.
- `_newlib_heap_size_user = 192 MB` and an 8 MB main thread stack work. QuickJS needs a big stack
  to parse minified 2–4 MB chunks.
- QuickJS-ng resolves static imports even with `JS_EVAL_FLAG_COMPILE_ONLY`. Install a module
  loader (the benchmark returns empty stub modules).
- Phaser feature detection needs `window.WebGLRenderingContext` and `CanvasRenderingContext2D`,
  and its CanvasPool reuses one canvas for both `2d` and `webgl` contexts. Cache contexts per type.
- Headless Chrome with swiftshader manages only about 2 fps on the PokeRogue canvas. That is fine
  for counting objects, but useless for timing.
- `JS_GetTypedArrayBuffer` + `JS_GetArrayBuffer` is cheap enough to call per sprite from C.

## 5. Next steps (Phase 1)
1. Desktop runtime (Linux, SDL2 + GLES2) that boots the live bundle in QuickJS with real WebGL
   bindings, Canvas 2D text, fetch, localStorage and cookies.
2. Measure the real game's frame cost in QuickJS on x86 and multiply by about 16.6 for the Vita.
3. Native-path candidates, in order: the container walk and transform matrices, batching of
   SpritePipeline uniforms, BitmapText glyph batching, and a 30 fps cap.
