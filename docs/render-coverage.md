# Native rendering coverage

What the runtime draws in C today and what still runs through Phaser's own JavaScript. As of
2026-10-16: runtime 0.41 (`main`), Phaser 3.90, game v1.12.x. Read from the code
(`runtime/js/fastpath.js`, `runtime/src/fastpath.c`) and [phase1-findings.md](phase1-findings.md);
the full native renderer is planned on branch `native-renderer` (its `docs/native-renderer.md`).

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

Each is checked frame by frame against Phaser with `probe_verify_render.js`,
`probe_verify_uniforms.js` and `probe_verify_batching.js`. localStorage switches turn them off
(`PKR_FASTPATH`, `PKR_NATIVE_RENDER`, `PKR_NATIVE_BATCH`, `PKR_NATIVE_UNIFORMS` set to `0`).

## Still on Phaser's JS path
| Feature | `native-renderer` step | Notes |
|---|---|---|
| Pipeline switching: `PipelineManager.set`/`rebind`, `WebGLPipeline.bind`, `WebGLShader.bind`/`setAttribPointers`, `setProgram`, `setBlendMode`, the `set1f`/`set3fv`... wrappers | 2 | Each switch re-creates attribute locations and re-sets every pointer; ~55 switches per battle frame |
| The game's FieldSpritePipeline/SpritePipeline onBind/onBatch (~10 uniforms, colour arrays rebuilt with `splice(...spread)`, a flush per sprite) | not planned (game code) | The largest single cost; steps 1 and 2 make what it calls cheaper |
| Graphics (HP/EXP bars, overlays) | 3 | |
| BitmapText (e.g. `item-count`) | 3 | |
| Rectangle and other shapes | 3 | |
| TileSprite, RenderTexture | 3 | |
| Text that is flipped, cropped, or on a post or non-plain pipeline | 3 | |
| Top-level render walk (`WebGLRenderer.render`: cameras, scene display list, pre/postRender) | 4 | Only containers are walked in C |
| Tween and animation stepping | 5 | The update list already visits only animating sprites |
| Masks, post pipelines, cropped sprites, custom pivots | none | Always Phaser's path, by design |

Step 1 (QuickJS fast paths for spread arguments and array species creation) is done on
`native-renderer` but not merged.

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
- Merge `main` into `native-renderer` (it predates 0.39-0.41) and bring step 1 back to `main`.
- Step 2, then Graphics and BitmapText from step 3: battles use both.
