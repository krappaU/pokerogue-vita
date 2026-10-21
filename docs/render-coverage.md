# Native rendering coverage

What the runtime draws in C today and what still runs through Phaser's own JavaScript. As of
2026-10-17: runtime 0.49 (`main`), Phaser 3.90, game v1.12.x. Read from the code
(`runtime/js/fastpath.js`, `runtime/src/fastpath.c`) and [phase1-findings.md](phase1-findings.md);
the steps of the native renderer and their measurements are in [native-renderer.md](native-renderer.md).

There is no separate renderer yet. Phaser still drives every frame from JS; the runtime replaces
hot spots with C versions and sends anything unusual back to Phaser's own code. So every screen
draws correctly, and what differs between screens is how much of the drawing takes a native path.

## Native on main
| Feature | Where | Since |
|---|---|---|
| Container tree walk (willRender, blend mode, type bookkeeping, render list, nested containers without a mask) | `renderContainer` (fastpath.c) | 0.29 |
| Plain Image/Sprite batching: texture batching, flushes, vertex writes, Phaser's exact rounding | `batch_sprite` | 0.29, 0.38 |
| Sprites on pipelines with their own batchQuad (Pokémon sprites): matrix and tint math only; the game's onBind/onBatch stay in JS | `generic_batch_sprite` | 0.38 |
| Text: drawn and uploaded only when rendered; plain pipeline, no flip, crop or post pipeline | `fast_text`, lazy `updateText` | 0.21, 0.29 |
| NineSlice (window frames): vertex loop | `batchVerts` | 0.21 |
| `WebGLShader.setUniform1..4` | `js_set_uniform` | 0.38 |
| `WebGLPipeline.flush`/`pushBatch`, `MultiPipeline.batchQuad` | fastpath.c | 0.38 |
| setAlpha/setScrollFactor round trip of generic container children | `set_alpha4`, `set_scroll_factor` | 0.38 |
| Pipeline switching: `PipelineManager.set`/`isCurrent`, `WebGLPipeline.bind`, `WebGLShader.bind`/`setAttribPointers`, `setProgram`, `setBlendMode`, the 42 `set1f`... wrappers | "pipeline machinery" | 0.42 (step 2) |
| Graphics with fill/line styles and `fillRect` (mask rectangles); Rectangle without stroke or rounded corners; GeometryMask; NineSlice wholly | "Graphics", "NineSlice", "Rectangle, GeometryMask" | 0.42 (step 3) |
| QuickJS: spread arguments, array species creation, appending `splice`, small `apply`, `flat()` of numbers | `patches/quickjs-0002`, `-0004` | 0.42, 0.44 (steps 1, 3b) |
| eventemitter3 `emit` (skips WebGLPipeline's empty render listeners); text size read from the drawn text | `js_ee_emit`, `fast_text` | 0.47 (step 3d) |
| BitmapText (item stack counts): the character loop everywhere; in containers the whole renderer (no per-character colours or post pipeline) | `batchChars`, `batch_bitmap_text` | 0.81 |

Each is checked frame by frame against Phaser with `probe_verify_render.js`,
`probe_verify_uniforms.js`, `probe_verify_batching.js`, `probe_verify_pipeline.js` (every WebGL call)
`probe_verify_emit.js` and `probe_verify_bitmaptext.js`, all run in CI on `tests/phaser`. localStorage switches turn them off
(`PKR_FASTPATH`, `PKR_NATIVE_RENDER`, `PKR_NATIVE_BATCH`, `PKR_NATIVE_UNIFORMS` set to `0`).

## Still on Phaser's JS path
| Feature | `native-renderer` step | Notes |
|---|---|---|
| Pipeline `rebind` and `setAttribPointers(true)` (after a GL reset) | none | Rare |
| The game's FieldSpritePipeline/SpritePipeline onBind/onBatch (~10 uniforms, a flush per sprite) | game code | The largest single cost (44% of time-based samples in a desktop battle, 0.47); steps 1-3b make what it calls cheaper; since 0.79-0.80 src/gamefix.c skips the colour array loops when they'd give zeros and keeps shiny colours |
| Graphics paths, arcs and transforms; other shapes; Rectangle with stroke or rounded corners | 3 (not needed so far) | Not seen in the battle and menu screens measured |
| TileSprite, RenderTexture, DynamicBitmapText; BitmapText with per-character colours | 3 (not needed so far) | Not seen in the battle and menu screens measured |
| Text that is flipped, cropped, or on a post or non-plain pipeline | 3 | |
| Top-level render walk (`WebGLRenderer.render`: cameras, scene display list, pre/postRender) | 4 | Only containers are walked in C |
| Tween and animation stepping | 5 | The update list already visits only animating sprites |
| Masks, post pipelines, cropped sprites, custom pivots | none | Always Phaser's path, by design |


## Screens
| Screen | Compared against Phaser | Render time per frame, desktop (JS → native) |
|---|---|---|
| Title | yes (0.29) | |
| Save slots | yes (0.29) | 2.35 → 1.92 ms |
| Starter select | yes (0.29) | 7.34 → 5.19 ms |
| Battle (command menu) | yes (0.29, 0.38) | 3.46 → 2.86 ms; JS per frame 3.6 → 2.9 ms (0.38) |
| Shop / rewards, party, summary, Pokédex, egg hatch, menus, settings, mystery encounters, login | no | Draw through the same paths; not yet probed or timed |
| Evolution and other video | n/a | Stand-in frame (no video decoding) |

On the Vita, battles run at 10-20 fps (0.38-0.41 logs).

## Next checks
- Run `probe_verify_render.js` and `probe_render_time.js` on the shop, party and Pokédex.
- Read the 0.48 profiling build's `[prof] groups:` lines from the Vita: they decide between step 4
  (render walk) and step 5 (tweens and animations).
- Step 4: `WebGLRenderer.render` (cameras, display list, pre/postRender) in C.
