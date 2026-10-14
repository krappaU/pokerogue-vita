# Phase 1 findings (in progress)

October 2026. Measured against PokeRogue source and live client at v1.12.x and Phaser 3.90, with the
desktop runtime in `runtime/`. Timings come from an x86 server (AMD EPYC, shared with other services)
using Mesa's software GL (llvmpipe), so they are noisy (±20%). Treat them as relative numbers.

## What works
- **Boot:** the live pokerogue.net client boots in the runtime: it fetches the page, loads the ES
  modules over HTTP, starts Phaser on native WebGL and loads the fonts, and shows the login screen
  with correct text.
- **Playing as a guest:** a production build of the current game source with login bypass plays
  through the title screen, starter select and into battles.
- **Startup on x86:** the entry module finishes evaluating about 2.8 s after launch with a warm
  HTTP cache. That is mostly compiling about 6 MB of JS.

## Things the game needed that weren't obvious
- **Shell index.html:** the game's newer build turns `index.html` into a small shell. It fetches
  `asset-manifest.json` and *adds* the entry `<script type="module">` from JS. The runtime runs
  scripts inserted at runtime, not just those in the HTML.
- **Fonts with `unicode-range`:** CJK and Devanagari fonts are registered under the same family names
  ("emerald", "pkmnems") with a `unicode-range`. Ignoring the range picked the wrong font and every
  glyph rendered as a box. Faces covering Latin are now preferred. Per-glyph fallback for mixed
  CJK text is still to do.
- **Video:** the loading scene preloads a video (`evo_bg`). Video elements "load" instantly as a 2x2
  black frame.
- **XML:** bitmap fonts (`item-count`) are XML, which Phaser parses with `DOMParser`.
- **Audio formats:** sounds come as mp3 (music), wav (effects) and **m4a/AAC** (battle animations).
  Phase 3 needs decoders for all three. The Vita has hardware AAC decoding.
- **URLs:** some sound files have spaces in their names. Browsers percent-encode them; curl does not.
- **Phaser global:** the minified game build still exposes `window.Phaser`, which is what lets the
  runtime attach native fast paths without modifying the game.

## Performance: the real game is heavier than the Phase 0 benchmark
Real late-game double battle (wave 120, ~40 items, weather), command menu idle:

| measure (x86, QuickJS) | value |
|---|---|
| JS per frame, median | **~12–15 ms** (Phase 0 synthetic battle: 3.7 ms) |
| of which game logic (update) | ~2.7 ms |
| of which rendering in JS | ~8.5 ms |
| of which WebGL calls | ~1.3 ms (≈300 calls) |
| Title/login screen, JS per frame | ~3.4 ms |

On the real Vita, the Phase 0 benchmark ran about 16.6x slower than on x86 QuickJS. Applied here,
that estimates **~200 ms per busy-battle frame (5 fps)** and **~55 ms on the title screen**, before
optimizations. This is an estimate; Phase 2 measures the real thing.

### Where the time goes (self-time profile, `runtime/scripts/probe_selfprof.js`)
- **Rendering:** the display-list walk (`WebGLRenderer.render` + per-object `renderWebGL`) and sprite
  batching (`batchSprite` with its matrix helpers) dominate.
- **Containers:** about **91 per frame**, holding 386 children, many of them nested containers.
  Each item icon in the item bar is its own container. Every container computes its own transform
  matrix with about 5 JS calls, plus pipeline pre/post-batch bookkeeping.
- **Animation updates:** `AnimationState.update` runs **879 times per frame**. Every sprite in the
  scene, including hidden UI icons, is updated even when it is not animating.
- **Text:** 27 `BitmapText` objects (drawn glyph by glyph in JS) and 29 `Text` objects per frame.
- **PokeRogue's custom Pokémon shaders** (`SpritePipeline`/`FieldSpritePipeline`) set about 20
  uniforms per Pokémon sprite: roughly 1.2 ms of JS for 5 sprites.

### The Phase 0 fast path on the real game
The fast path is enabled by default and can be toggled with `__rt.fastpath.set(bool)`. On the
real battle it saves only about **8%** (2.2x on the synthetic scene). Most sprites already take it;
the remaining cost is in containers, update lists, text and Phaser's per-object bookkeeping, which
the synthetic scene barely had.

## What this means for the plan
The 30 fps target for busy battles needs roughly another **5–7x** cut in JS time per frame on the Vita.
Candidates, by expected gain:
1. **Native display-list renderer:** walk containers and sprites in C, reading the game objects'
   fields directly, and build vertex batches in one pass, falling back to Phaser for anything unusual.
   This removes most per-object JS calls on the render side.
2. **Skip idle animation updates:** only update sprites whose animation is actually playing.
   The 879 calls per frame are mostly no-ops.
3. **30 fps game loop:** halves the work per second. The game's pixel-art animations run at about 12 fps anyway.
4. **Native BitmapText and uniform batching** for the custom Pokémon shaders.
5. **Skip unchanged frames:** when nothing moved, don't redraw (menus, waiting for input).

Measuring the real game on the real Vita (Phase 2) comes before investing in these, since the 16.6x
multiplier was derived from a different workload.
