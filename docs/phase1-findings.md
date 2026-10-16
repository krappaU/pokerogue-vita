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

## On the real Vita (2026-10-15, runtime 0.17-0.20, PokeRogue live build of that day)
- Title/login screen: **~55 ms of JS per frame (16-17 fps)**, matching the estimate above.
- Startup: ~40 s of fetching and compiling the game code (~25 s compiling), then one ~79 s frame in
  which the game builds its UI (over 1,100 text objects).

### First optimizations (runtime 0.21), desktop measurements
| change | effect |
|---|---|
| Update list visits only sprites with a playing animation (1,166 sprites, none animating on the title) | update phase 1.32 -> 0.30 ms per frame |
| Text objects only measure on change; drawing and upload happen when rendered | fewer redraws/uploads, hidden texts never drawn |
| NineSlice vertex loop in native code | nine-slice was ~36% of the title screen's render time |
| CanvasPool create/remove constant-time | removed the top setup hotspot (pool scan) |
| Compiled-code cache | second start: game code loads 4-6x faster than compiling |
Title screen JS per frame on desktop: 3.7 ms -> 2.2 ms. Battle render (~6.7 ms on desktop) is now
the largest cost; the native display-list renderer (item 1 above) is the next step.

### Screen building (runtime 0.26), desktop profile of the setup frame (offline build e4e9b53)
On the Vita (0.25 log) the setup frame took 57 s and returning to the title 68 s. Profile:
i18next `translate` 35%, `indexOf` 11% self (Phaser's update list and display list checking
membership by scanning thousands of entries), canvas font parsing 8%. Changes: translation cache
(`js/i18n_cache.js`, ~46% hits on first setup, more on rebuilds), membership via Sets / the objects'
own fields (`js/fastpath.js` 7 and the update-list part of 3), font-string parse cache. Desktop
setup frame: ~5.5 s -> 4.9 s.

### Return to the title (runtime 0.27), offline build e4e9b53
On the Vita (0.26 log) "Save and Quit" was one 49.7 s frame, and starting the game spent the same in
its first title frame. Profile of that frame: event-emitter `removeListener` 25%, Phaser's
`SpliceOne` 15%, the JS `btoa`/`atob` 19% plus 5% in their regex replace (offline saves are base64
of the whole save, ~0.5 MB), `GetValue` 3%. Causes and changes:
- Every Sprite's AnimationState subscribes to the global AnimationManager's "remove" event (1,178
  listeners on the title). eventemitter3 rebuilds the listener array on each removal, so destroying
  the sprites is quadratic. Those subscriptions now live in a Set behind one listener
  (`js/fastpath.js` 9).
- `SpliceOne` is a JS loop shifting the array. Callers: `Utils.Array.Remove/RemoveAt` (containers,
  lists) and the WebGL renderer deleting textures from its 2,000+ entry wrapper list. Native splice /
  swap-with-last instead (`js/fastpath.js` 8).
- `btoa`/`atob` in C (a 6 MB round trip takes 33 ms on desktop).
Desktop, same save, Continue -> Save and Quit: 3.76 s -> 2.76 s for the rebuild frame. The desktop
profile weights these less than the Vita one did; expected Vita effect ~60% of that frame.

### Long frames and battle cost (runtime 0.28), 2026-10-15, offline build e4e9b53
Vita 0.27: return to the title 49.7 s -> 36.0 s; the first title frame at startup 49.8 s -> 42.0 s.
During such frames nothing was presented (a black screen); from 0.28 a "Loading" screen is drawn from
the JS interrupt callback once a frame has run for 2.5 s, with the game's GL state restored from
shadow copies in gl.c afterwards.
- Battles on the Vita: 50-60 ms of JS per frame, p95 110-140 ms; render is ~74% of the samples
  although a battle frame draws only ~220 objects (~41 quads, ~11 texts, ~55 pipeline switches;
  desktop probe). The game's sprite pipeline rebuilds 2x128-float uniform arrays with spread
  `splice` calls in onBind/onBatch for every Pokemon sprite, every frame (~10% of the frame).
  PokemonIconAnimHelper keeps 659 icons registered (starter select, party) and moves all of them on
  a 200 ms repeating tween (~5%).
- A full QuickJS cycle collection takes ~220 ms on desktop with the game's heap (~80 MB of JS
  objects), probably ~3 s on the Vita. It only runs after the JS heap grows by half; none occurred in
  desktop battles. The perf log line now reports frames that contained one.

### Native container renderer (runtime 0.29), desktop, offline build e4e9b53
Container.prototype.renderWebGL is now a C function (src/fastpath.c renderContainer): it walks the
container tree, does Phaser's per-child bookkeeping and batches plain Images/Sprites and Texts itself
(texture batching, flushes and the vertex writes), calling back into JS for anything else and for
pipelines with their own onBind/onBatch. Checked with scripts/probe_verify_render.js (native and JS
frames alternate; every flushed vertex batch compared): identical on the title, the save-slot
screen, starter select and a battle. Render time per frame (scripts/probe_render_time.js):
| screen | JS container renderer | native |
|---|---|---|
| battle (command menu) | 3.46 ms | 2.86 ms |
| starter select | 7.34 ms | 5.19 ms |
| save slots | 2.35 ms | 1.92 ms |
What remains in battle is mostly PokeRogue's own pipelines (FieldSpritePipeline/SpritePipeline
onBind/onBatch: per-object uniform arrays and a flush per object) and its icon tween.
QuickJS-ng 0.17 already pools small allocations in its own arenas, so a custom allocator for the
JS runtime (tried) changes nothing.

### Screen rebuilds, text creation (runtime 0.30), desktop, offline build e4e9b53
Vita 0.29 log: title at startup 42.3 s, entering starter select 21.9 s, return to the title 35.9 s.
Profile of the return to the title: the game rebuilds every UI screen, and 62% of it is the
starter-select and Pokedex screens (one container with texts and icons per species). Creating a
Text was 34% of the whole rebuild: TextStyle.setStyle 15% (GetValue/GetAdvancedValue), measuring
16% (the game changes each new text about four times: constructor, setPadding, setShadow,
setLineSpacing), a random UUID per text 3%. Changes:
- Text sizes are measured on first read instead of on every change (width/height/display origin
  are accessors); startup made 7,874 text changes but only 1,552 measurements.
- TextStyle.setStyle with pre-split paths and font metrics remembered per font (defaults read from
  a TextStyle built by Phaser; scripts/probe_textstyle.js: 1,632 real configs, all identical).
- Utils.String.UUID's replace call answered in C (4 us instead of 41 us).
- Container.add with arrays of new children skips Phaser's indexOf checks.
Creating 400 game-style texts: 330 ms -> 84-98 ms.
Starter-select icons showed 1-pixel stripes of neighbouring atlas frames on the Vita (not on
desktop): sprite edges fall exactly on pixel centres at half resolution. The projection matrix is
shifted by 1/16 pixel (gl.c uniform_matrix).

### Vita build flags (runtime 0.31)
Vita 0.30 log: title at startup 36.4 s (0.29: 42.3), return to the title 30.2 s (35.9), entering
starter select 21.3 s (21.9). Entering starter select is PokeRogue's filter code: resetting the
filter dropdowns toggles each option, and every toggle re-filters all species (array
map/filter/some, event-date checks with `new Date()` per species). Found while profiling it: the
Vita build overrode CMake's release flags without -DNDEBUG, so QuickJS's ~140 internal asserts
(atoms, shapes, GC marking) were compiled in; the desktop build always had NDEBUG. Fixed in 0.31,
along with a `Date` constructor without rest parameters.

### Screens built on first use (runtime 0.32)
Vita 0.31 log: title at startup 35.7 s, return to the title 29.3 s, battles 48-58 ms per frame.
Handler setup times on desktop during a return to the title: StarterSelectUiHandler 674 ms,
TestDialogueUiHandler (a developer tool) 422 ms, PokedexUiHandler 166 ms, settings screens ~270 ms
in total, PokedexPageUiHandler 43 ms, EggSummaryUiHandler 31 ms. js/lazy_ui.js defers the setup of
the starter select, Pokedex, Pokedex page, dialogue tester and egg summary handlers until a method
is first called on them, then moves the objects they create back to their original drawing order.
Only `instanceof` checks and UI.freeUIData's destroy() touch these handlers from outside (both
handled). Desktop: first title frame 4.4 s -> 2.7 s, return to the title 2.6 s -> 1.4 s; starter
select, its Pokedex page and the menu's Pokedex build on first use and look as before. Opening the
Pokedex itself takes ~10 s of JS on desktop even when already built (PokeRogue's own work); not
changed here.

### Filter resets (runtime 0.33)
Vita 0.32 log: title at startup 16.4 s (0.31: 35.7), return to the title 8.0 s (29.3), first
New Game (starter select built and shown) 27.9 s; JS heap 78 -> 55 MB, canvases 1,680 -> 714.
Showing the starter select or the Pokedex resets the filter bar, and every dropdown reset or option
toggle calls the screen's change callback (a full re-filter and re-sort of all species): about 20
passes, and show() then runs one more. js/lazy_ui.js now holds those callbacks back during
resetFilters() and runs the last one once. Checked: starter select reset+update 1.1-1.2 s ->
0.14-0.23 s on desktop, identical filtered list, cursor, dropdown settings and visible icons (only
the stale positions of hidden icons differ, and updateScroll positions every icon before showing
it); Pokedex 8.3-8.8 s -> 3.6-4.0 s with an identical state (scripts/probe_filter_coalesce.js).

### Threads and cores (runtime 0.35)
The Vita gives apps three of its four Cortex-A9 cores. The game's JavaScript (game logic, Phaser,
building draw batches) is one QuickJS context and runs on one thread; it can't be split. Off the
main thread: audio mixing (core 1) and decoding (core 2) since 0.34, and vitaGL's display queue
(the vblank wait happens there, with triple buffering, not on the main thread). 0.35 logs the
non-JS parts of a frame in the `[perf]` line (draw = HUD/overlays, swap = vglSwapBuffers,
after = per-frame texture housekeeping) to see what else could move.
Battle-frame changes in 0.35: the scene update list keeps its sprite/non-sprite split and its
membership sets up to date incrementally (it was rebuilt from the whole list, 1,000+ objects, in
every frame where something was added or removed: 5-7% of battle frames on the Vita), and the
texture-eviction scan of 16,384 slots runs at most every 30 frames when nothing can be evicted.
0.36 moves I/O and image decoding off the main thread: HTTP transfers, HTTP-cache reads and
offline-zip reads run on a network thread (core 2, curl multi), and the main thread only picks up
finished responses (cache/zip hits within its 8 ms per-frame budget as before); PNG/JPEG decoding
for image elements runs on its own thread (core 1) and the image's `load` fires on a later frame.
Audio's per-frame hook now skips re-folding node chains when no gain, pan, rate or connection
changed and no ramp is running. Checked on desktop (offline game to a battle turn, live site to its
login screen) and in Vita3K (no crash); device timings pending.

### Saves, stale builds and the shop (runtime 0.37, 2026-10-15)
- **Save encryption:** the game keeps AES-encrypted copies of its saves in localStorage
  (crypto-js, `AES.encrypt(json, key).toString()`); on the Vita each save spent 7-16 s in it
  (27-log: `stringify`, `parse`, `_doCryptBlock` under `saveAll`), and loading a save at login
  most of its time too. js/savecrypt.js finds crypto-js as it sets itself up and gives it native
  `AES.encrypt`/`decrypt` (OpenSSL, src/savecrypt.c) for that call pattern, after checking both
  directions against crypto-js. Desktop, 321 KB of JSON (scripts/probe_savecrypt.js): encrypt
  728 -> 4.5 ms, decrypt 689 -> 2 ms.
- **"Session out of date: existing version is greater":** the runtime sent `Origin` with every
  request. Cloudflare keeps a separate cached copy of pokerogue.net for requests with that header,
  and it held a July build (`index-B1LWFm2v.js`) while browsers got the current one
  (`index-CTFFh6u_.js`); the server then refused the older client's saves. Same-origin GETs no
  longer carry `Origin`, as in browsers.
- **Shop items missing:** the reward items appear from a 1250 ms counter tween (divided by the game
  speed) that reveals at most one item per update, so at ~12 fps with fast animations the last
  ones never showed. Tweens with an onUpdate callback now advance in steps of ~17 ms on long frames.
- **Boxes in the shop's price line:** U+00A0 between name and price; text drawing and measuring now
  treat no-break/typographic spaces as spaces and zero-width characters as nothing, as browsers do
  when the font lacks them.

### Battle rendering profile and native pipeline machinery (runtime 0.38, 2026-10-16)
Desktop profile of an idle battle (offline build, 19,382 JS samples taken every 1,000 interpreter
operations in a profiling build): rendering 62% of the JS time, updates 27% (16% of it PokeRogue's
party-icon bob, which moves the 578 starter-select icons every 200 ms / game speed). Of rendering,
54% was Phaser's own JS and 37% PokeRogue's: its FieldSpritePipeline and SpritePipeline set ~10
uniforms per sprite in onBind (the sprite pipeline also rebuilds 128-value colour arrays with
`splice(...spread)`), and flush after every sprite in onBatch, so most objects cost a draw call.
Moved to C (each checked frame by frame against Phaser with animations and the clock frozen:
scripts/probe_verify_render.js, probe_verify_uniforms.js, probe_verify_batching.js all identical):
- the setAlpha/setScrollFactor round trip of every generic container child (Alpha, AlphaSingle,
  ScrollFactor and Container's versions recognised and written directly);
- MultiPipeline.batchSprite for sprites on pipelines with their own batchQuad (Pokémon sprites);
- WebGLShader.setUniform1..4, WebGLPipeline.flush and pushBatch, MultiPipeline.batchQuad.
The sprite matrix math now rounds exactly like Phaser's (doubles, stored as float32): the float
version differed by one ulp for sprites with fractional scales.
Median JS per battle frame on desktop: 3.6 -> 2.9 ms. What remains is mostly the game's pipeline
code. Desktop's occasional ~75 ms frames are llvmpipe stalling inside drawArrays; the [perf] line
now reports time inside GL draw calls (and textures re-uploaded after eviction) to see whether the
Vita's 0.5-0.8 s battle hitches are GPU waits too.

### 29-log (0.38 on the Vita, online session, 2026-10-16)
- Time inside GL draw calls stayed under 1 ms per frame (max 1 ms): the GPU isn't behind the
  0.2-0.8 s battle hitches. They come with "N textures re-uploaded" (20-30 at once): textures used
  every few waves (shop, per-wave UI) are evicted after 600 idle frames (over a minute at 9 fps) and
  decoded from PNG again on the next use. 0.39 doubles a texture's idle time each time it comes back
  (up to 16x).
- Slow rival-battle load: batches of downloads failed after ~16 s with 0 bytes, first seen with 0.36
  (the network thread). It shared CPU core 2 with the audio decoder at the same priority, and the
  Vita doesn't time-slice equal priorities, so a busy decoder kept it from running. 0.39 runs it on
  core 1 above the decoders (image decoding there runs below it).
- Missing "Lv" glyph: the game's text now uses U+F000, drawn only by the newer pokemon-emerald-pro
  file the game registers under "pkmnems" for symbol/CJK ranges; the page's own @font-face file,
  served from Cloudflare's cache since July, lacks it (browsers presumably show a box too). 0.39 has
  per-character font fallback: a character the font lacks comes from a registered face that has it
  (same family first), which also fixes mixed CJK text. A face registered again for the same family,
  style and unicode-range replaces the earlier one (browser precedence).
- The [perf] JS time now includes the promise jobs before the frame (fetched files are handled there).

### Input repeat after slow frames, links (runtime 0.40, 2026-10-16)
- Some list entries (e.g. a move with a long description in the "forget a move" list) couldn't be
  selected: the cursor jumped past them. PokeRogue repeats a held button with a 250 ms setInterval
  that stops when the game sees the button released (in Phaser's frame, from the Gamepad API). The
  runtime ran due timers before the frame, so after a frame longer than 250 ms the repeat fired
  before the release was seen. Timers now run after the game's frame. Checked with a scripted pad
  (one-frame press, 400 ms frame): cursor moved 2 entries before, 1 after; holding still repeats.
- window.open (the menu's Wiki/Discord/GitHub/Reddit/donate links) shows the link as a QR code with
  the address (src/linkqr.c, Nayuki's qrcodegen); Discord/Google sign-in gets an explanation that it
  can't be done on the Vita.

### Save import/export, page reload, memory pressure (runtime 0.41, 2026-10-16)
- Export (`<a download href=blob:>`.click()) writes the file to `<data>/exports/` and shows where;
  import (`<input type=file>`.click()) opens a file picker over the game on that folder and hands
  the file to the game as `input.files` + "change". Exports from either mode can be imported in the
  other (online <-> offline). Checked on desktop with the offline game: export, import, confirm.
- `location.reload()` (the game reloads after an import) restarts the app (execv on Linux,
  sceAppMgrLoadExec on the Vita) and goes straight back to the same mode via `<data>/relaunch.txt`.
- 31-log: GPU textures reached 122 MB (budget 64) with the GPU pools full, so vitaGL spilled into
  the main heap (206 MB in use): 0.39's longer idle time for re-uploaded textures kept them from
  being evicted. Under memory pressure (textures over 1.5x budget, or under 16 MB free in the
  pools) textures idle for 90 frames are evicted regardless.
- The [perf] line lists other main-loop steps taking 2+ ms per frame ("also"): 31-log had
  loading windows at ~3 fps with only ~29 ms of JS per frame.
