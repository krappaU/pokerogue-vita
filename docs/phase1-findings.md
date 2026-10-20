# Findings log (Phase 1 onward)

Measurements, device logs and the reasons behind each change, oldest first. Version numbers refer
to `runtime/VERSION`; device logs are numbered as the tester sent them.

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

### Wave-60 hang from a lost download (0.42 dev 2fa1176, Vita log of 2026-10-17)
- Entering a wave-60 trainer fight: ~30 asset requests were pending at once (RAM pool 4-8 MB free,
  video pool 0.1-0.5 of 88 MB, GPU textures ~99 MB), the frame rate fell to 2-4 fps with the
  network step taking 200-410 ms per frame, and five requests failed after 19-21 s with 0 bytes
  (`korrina.json`, `dig-charging.json`, three battle SFX). The first failure reached the game as
  `TypeError: Failed to fetch` (unhandled rejection in net.js), its loader never finished, and the
  game sat on the loading screen at a steady 26 ms of JS per frame with 0 requests pending.
- Not the renderer: the first failure is a download timeout, before any new draw path runs.
- Change (runtime, not yet run on a Vita): `request()` in net.js repeats a GET/HEAD that fails
  at the transport level up to twice (after 0.5 s, then 1 s) and logs `[net] retry n/2: url`. POSTs
  are never repeated. Checked with a stubbed native client: success, one and two failures
  recovered, three failures reject, a POST that fails is not repeated.
- Still open: why ~30 parallel requests starve under low RAM (the 8-connection cap is in
  net.c); if retries don't help, limit concurrent requests while the pools are nearly full.
- Also in this log: `[error] Exported save data is invalid JSON!` several times around a save
  export that still wrote `data_Krappa.prsv` (198 KB); not looked into yet.

### Startup: where the first ~100 s go, and what runs in parallel now (2026-10-17, branch claude/startup-load-speedup)
Read from the two 0.42 logs (online, warm caches; the log has no timestamps, so the timeline is
rebuilt from the `[module]`, `[perf]` and `[net]` lines, ±2 s). Desktop numbers are from this
branch, live site, desktop runtime; **nothing here is timed on the Vita yet**.
- **Evaluated after 27-31 s**, then ~70 s more to the login screen. 8-9 s of the 27 s are the
  module graph, ~10 s are the game waiting for **8.7 MB it downloads on every start** (the
  `/fonts/*.ttf` files, 7.2 MB, and `manifest.json`: not in the disk cache, which only took
  `/assets/` and `?t=` URLs; the server sends the fonts with `max-age=315360000`), with the CPU idle
  (JS 1 ms/frame at 55 fps in those windows). The rest are single-threaded JS frames (13 s, 9.5 s,
  15.8 s: game setup) that QuickJS can't split, and ~25 s of asset loading in which the main thread
  is ~80% idle (cause not found yet; the `[perf]` line now has what's needed, see below).
- **Module graph, 8.4 s with everything cached:** it read 5.4 MB of source (only to hash it) and
  21 MB of compiled code from the memory card, one chunk after the other, at ~4 MB/s (fetch of
  the 2.6 MB chunk 0.7 s = 3.7 MB/s). Deserializing is cheap (desktop: 36 ms for all of it). The
  compiled files were 3.7x larger than needed because QuickJS stores each function's source text
  inside it, once per nesting level (21 MB vs 5.6 MB for 12 chunks without; with `JS_WRITE_OBJ_STRIP_SOURCE`
  file names and line numbers in stack traces stay). `/assets/name-HASH.js` chunks are now keyed
  by URL (the hash is the bundler's content hash; the HTTP cache already assumed that) so the
  source isn't read at all, a thread reads last run's chunks ahead (`bytecode/startup.txt`) while
  the main thread fetches the page, and the loading overlay is only drawn for big chunks (each
  present cost ~110 ms: the tiny modules' "fetched 111 ms" were that, not the disk).
- **Fonts and 30-day-old unversioned cache entries:** `/fonts/` is cached like `/assets/`; an entry
  without `?t=` older than 30 days is fetched again.
- **First start after a game update (new chunk names, ~30 s of compiling on the main thread):**
  `precompile.c` fetches and compiles the 23 chunks (6.1 MB) on three worker threads, each with its
  own QuickJS runtime, following the imports it finds in each chunk (static and `import()`), and
  hands the compiled bytes to the loader in memory. Desktop, cold cache: 23 chunks in 2.7 s (the
  biggest chunk, `loading-scene` 2.6 MB, is the limit: ~2.3x over one thread; Vita estimate 30 s -> ~13 s).
  A compile uses ~27 MB at most per thread.
- **Not changed:** the game's own JS frames, fonts' parse time, the 8 ms/frame limit on handing
  cached responses to JS.
- **To read in the next Vita log:** `[module]` lines now give fetch / read / load per chunk with
  their imports' time taken out; `[perf]` ends with `disk cache: N reads, MB, ms`, `images
  decoded: N, ms` and `t=` (seconds since start), and `net:` says how many MB came over the network.
  The idle ~25 s of asset loading is either slow cache reads (the `disk cache` ms will show it),
  Phaser's loader queue, or I/O latency on the card; that decides what is worth doing next.

### File opens on the memory card (0.50 logs, 2026-10-19; runtime 0.52)
Three 0.50 dev logs (online, live site of that day: a 70 s start to the title screen and two
~6.5 min sessions with a battle) agree:
- **Disk cache reads cost 20-40 ms each, 70-120 ms while the game loads**, whatever their size:
  `109 reads, 6.3 MB, 9875 ms`, `75 reads, 1.4 MB, 9198 ms`, and even 0.1 MB in ~100 reads took
  ~4 s. They ran one after the other on the network thread, which ran no transfers meanwhile, so
  asset loading went at the speed of single file opens (the "main thread ~80% idle" while loading,
  see "Startup" above). Likely cause: every entry was in one folder, `<data>/http/` (~800 for one
  session of the game, thousands after a few game releases), and exFAT finds a name by scanning
  its folder.
- **The control channel cost 3 ms per frame, 30-45 ms while loading**: it opened
  `ux0:data/pokerogue/control.txt` every frame, waiting behind the cache's opens.
- One log line was garbled by two threads logging at once (the watchdog and the main thread).

Changes (0.52):
- The cache moved to `<data>/http-cache/<2 hex>/<14 hex>` (256 folders, ~3 entries each per game
  version). The old `http/` folder isn't migrated (each move would be one more slow open in that
  folder): a minute after start, the cache thread deletes it 8 files every 250 ms while it has
  nothing else to do, and its entries are downloaded again as the game asks for them (~25 MB, once).
  0.52 on the console (log9, 2026-10-19): with no other file work going on, the control channel's
  twice-a-second open waited ~50-100 ms behind those deletes (7-20 ms per frame), and the folder
  wasn't gone after 5 minutes. 0.54 deletes one file at a time and waits four times as long as
  that delete took (at least 100 ms), so it takes at most ~20% of the card's time, and logs its
  progress every 500 files.
- Cache reads and writes run on their own thread (`pkr_cache`, core 1, below the network thread);
  a miss goes on to the network thread, a download is written there before it's handed to JS.
- The control channel (dev builds; player builds have none since 0.51) is read twice a second, and
  right away again after a `key`/`wait` that left commands behind it.
- `host_log` takes a lock.

Checked on desktop with the live site: from an empty cache, 808 entries in 245 folders; next start
808 of 811 requests from the cache (the rest: page, manifest, API); a stand-in old `http/` folder of
300 files was gone ~70 s after start; queued control commands ran in order without added delays.
**Not timed on the Vita yet**: the next log's `disk cache: N reads, MB, ms` shows whether the
folder size was the cause. If opens stay slow, the next step is fewer, bigger files (a pack file).

### Installing test builds needs a second app (0.50 on the console, 2026-10-19; runtime 0.53)
The dev build found and downloaded 0.52 from the `dev-build` release, then failed on its first
file: `couldn't write ux0:app/PKRV00001/eboot.bin.new`. A running app can't write its own app folder
(the in-place install had never been tried on the console). VitaShell updates itself with a second
app, VSUPDATER, that installs while VitaShell isn't running. 0.53 does the same with
`runtime/updater/` (PokeRogue Updater, `PKRVUPD01`, installed once with VitaShell): the game unpacks
the build to `<data>/update/files/` and starts it; it copies the files (eboot.bin last, `.new` copies
first so a failure keeps the old program) and starts the game again. The copy code was checked on
the desktop (`cc -DUPDATER_TEST`); **not yet run on the console**, in particular whether one homebrew
app may write another's `ux0:app` folder (FTP and VitaShell do). VitaSDK's `vita_create_vpk` is a
macro that appends to `VITA_MKSFOEX_FLAGS`/`VITA_PACK_VPK_FLAGS` in the caller's scope: a second
call in the same CMakeLists must reset them, or the second .vpk gets the first one's files and
`TITLE_ID`.

### The fast paths were off on the Vita since 0.49 (log16, 0.54, 2026-10-19; runtime 0.55)
The first battle profile from the console (`sample=1`, `sample_ms=1`, `profile_rate=1000`; two
~35 s windows in a battle, 3500-4500 samples each, consistent): PokeRogue's sprite pipelines 41-42%,
Phaser's render JS 22-23%, tweens 10-11%, frame events 5-13%, sprite animations 4-5%, our native
render code 3-4%. Phaser's own `setUniform1` (5% self), `setBlendMode` and `PipelineManager.set`
(27% on the stack) were in it as JS, though C versions of them exist since 0.38 and 0.42.

Cause: the startup work of 0.42-0.49 stores compiled code without function source
(`JS_WRITE_OBJ_STRIP_SOURCE`), and such a function prints as `function f() { [native code] }`.
`js/fastpath.js` installs uniforms (part 13), pipeline switching (14), Graphics (15),
Rectangle/GeometryMask (16), emit (17) and the UUID wrapper (12) only after checking Phaser's code
with `String(fn)`: from the compiled-code cache, i.e. on every start but the first, all of them
were skipped without a word. Merging that work with the native renderer into 0.49 made this
happen on the console; the tests always started from an empty cache.

Fix (0.55): Phaser's chunk (recognised by `createAttribLocation`) is compiled with its source
(7.0 MB instead of 1.4 MB on the memory card: ~1.4 s more to read, partly ahead of time; every
other chunk stays stripped). The compiled-code format is now `.qbc3` (the old files are deleted
at the first start, and the game is compiled once more). `js/fastpath.js` logs
`[fastpath] native: ... | left to Phaser's JS: ...`, and `tests/run_page.sh` runs each page a second
time from the compiled code it wrote, failing if a fast path was left out (checked: with the old
flags the second run fails). On desktop with the live site: all six installed on both starts. A
way to drop the 5.6 MB later: check Phaser's source once when compiling and remember the result.

### Battles after the fast paths came back, and what the swap waits for (log19, 0.55, 2026-10-20; runtime 0.56)
0.55 on the console, two waves (`[fastpath] native: uniforms, pipeline (48 functions), graphics,
shapes, emit, uuid`): JS per battle frame 27-45 ms (p50 27-41) instead of 51-67 (p50 48-57) in
0.54, 15-19 fps instead of 11-14. Profile groups in battle: PokeRogue's sprite pipelines 20-43%,
Phaser render JS 13-25% (BitmapText renderer, GetCalcMatrix, copyFrom, willRender), tweens 16-24%,
sprite animations 5-10%. But the swap took 17-38 ms per frame (11-17 before), and JS + swap stayed
at ~55-65 ms whatever the JS time: the frame looks bound by the GPU. GL draw calls themselves cost
0.2 ms per frame. GPU memory was full (textures 98-104 MB, pools 0.1-4.8 MB video and ~2 MB RAM
free), with 14-38 textures re-uploaded per window.

One suspect: vitaGL reports 16 texture units, so Phaser's multi-texture shader picks each pixel's
texture through 16 `if`s. On the desktop (software GL, where pixel work is what costs), the Phaser
test scene's draw calls took 15-19 ms per frame with 16 units and 4-5 ms with 4, for the same 9-10
calls per frame; the probes are unchanged.

0.56 (dev builds): the `[perf]` line has `GPU N ms/frame` (every 30th frame, glFinish before the
swap is timed: on this tile-based GPU the pixel work starts when the scene ends, so the wait is
about the frame's GPU time) and `N calls/frame`; the log says how many texture units Phaser uses;
`maxtextures=N` in config.txt tells Phaser of at most N units. To decide: a battle with the default
and one with `maxtextures=4` (more draw calls, a cheaper pixel shader).

### The game installs its own updater (2026-10-20; runtime 0.58)
The copying updater worked on the console (0.53 onwards: `updater: ok 15 files`), but it had to be
installed by hand once and left `sce_sys/` alone, so LiveArea changes (0.57) still needed VitaShell.
0.58 does what VitaShell does with VSUPDATER: the dev build carries the updater (`updater.vpk`),
installs it with the system's installer (ScePromoterUtil: PAF module with a 1.5 MB heap, then
`scePromoterUtilityPromotePkgWithRif(dir, 1)`), starts it and quits; the updater installs the game
the same way and starts it again; the game uninstalls the updater at every start
(`scePromoterUtilityDeletePkg`). The installer wants `sce_sys/package/head.bin`, which VitaShell
makes from a template with the content id and three SHA-1 based checksums; `scripts/make_head_bin.py`
does it at build time for both apps (byte for byte VitaShell's output for `PKRV00001` and
`PKRVUPD01`, checked against its C code built on the host). Builds without one (before 0.58) get
the installed game's, which is the same for every build. A full install replaces the app folder as a
whole, so a crash in the middle can leave a broken bubble (VitaShell's updater has the same risk);
dev builds only. **Not yet run on the console**: the first 0.58 is installed by 0.57's copying
updater, the first real use is the update after that. Vita3K can't test the installer.

### Texture units, the GPU timer and video memory (log23/log24, 0.56, 2026-10-20; runtime 0.59)
Two runs of 0.56, the same title screen and a few battles each. Title screen, the one scene both
runs share: default (16 texture units) 38.7-41.7 fps, swap 14.0-14.3 ms, GPU reading 50-54 ms;
`maxtextures=4` 43.7-44.4 fps, swap 11.7-12.9 ms, GPU reading 46-50 ms; JS 9.3-10.4 ms in both.
Battles differ too much to compare (default: 6.6-13.5 fps, swap 22-68 ms; 4 units: 7.3-14.3 fps,
swap 14-31 ms, 15-40 draw calls instead of 9-17 at 0.3-0.5 ms per frame in all). So 4 units save
~2.5 ms of GPU time per frame; the rest of the GPU time isn't the shader's `if` chain.

The GPU reading was about twice the frame time (52 ms on a title screen at 24 ms per frame): the
timed glFinish also waited for the frame before. Still, every scene is bound by the GPU: the title
screen has 5 draw calls and 9 ms of JS, and waits ~14 ms per frame in the swap. Battles take
50-60 ms of JS per frame too, so both sides need cuts.

GPU memory in those runs: textures peaked at ~120 MB during loading and stayed at 80-92 MB
(~1250 textures, all RGBA8). vitaGL's pools: 88 MB of CDRAM with 2-15 MB free, 64 MB of RAM with
~2 MB free (0.1 MB right after loading), 16 MB phycont with 10-14 MB free. Of the 112 MB of CDRAM
an app gets, libgxm takes 16 MB for the parameter buffer (vitaGL's default size), and 8 MB were
left free because the system keyboard once failed without them; it failed anyway. vitaGL also
had a 4 MB pool from the system dialogs' memory budget, the likely reason the keyboard never
opened.

0.59: the built-in keyboard is the only one on the Vita; CDRAM pool 95 MB (1 MB stays free); the
dialogs' whole budget (8.8 MB) goes to vitaGL; Phaser gets 4 texture units by default; the GPU
timer waits for the frame before first; debug builds log how many MB of textures are in each pool
and the biggest ones outside CDRAM; `plainshaders=1` swaps every textured fragment shader for a
plain texture read, to measure what the shaders cost. **To check on the console:** the title
screen and a battle with and without `plainshaders=1`, and the new `[mem]` line.

### 0.59 crashed at start: the installer ran out of memory (2026-10-20; runtime 0.60)
0.56 installed 0.59 with the hand-installed copying updater (`updater: ok 16 files`); 0.59 then
uninstalled that updater at its first start, after graphics init, and the console hung (a partial
crash dump only); later starts crashed. The full dump (vita-parse-core, see "Vita gotchas" in
development.md): `PC 0x0`, a prefetch abort in `scePromoterUtilityExit`. Loading PAF, which the
system's installer needs, had failed with `0x80024302` (`SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE`),
and the cleanup called into the promoter module that never loaded (VitaShell returns at the first
error instead). 0.59 had just given vitaGL the whole system dialogs' budget and more CDRAM.

0.60: the cleanup only unloads what loaded; the game calls the installer only early in a start,
before vitaGL takes memory (installing the updater now restarts the game once first); vitaGL gets
4 MB of the dialogs' budget again (no dialog is used, but PAF likely needs the rest); every
installer call is bracketed by a marker file, so a crash or hang in it turns the installer off for
that build instead of repeating at every start. To leave 0.59: install 0.60 with VitaShell (0.59's
own update path still crashes), after deleting `ux0:app/PKRVUPD01`.

### The GPU time is the shaders (log33/log35, 0.60, 2026-10-20; PokeRogue live build of that day)
0.60's GPU timer (one frame, after the frame before was waited for) on the console. Default
shaders (log33): battle windows 46-81 ms of GPU time per frame, swap 14-35 ms, 5.6-19.9 fps, with
JS at 33-82 ms per frame; the first title/menu window 24 ms. Textures: 52-78 MB in CDRAM, up to
17 MB in the RAM pool and 16 MB in phycont at times, none in the heap. The reading has a floor of
11-15 ms on near-empty screens (probably the wait for a free display buffer).

`plainshaders=1` (log35, every textured fragment shader replaced by a plain texture read; colours
and text wrong): battle 2.2-3.3 ms of GPU time per frame, swap 0.3 ms, 12.6-28.8 fps, now bound by
JS (34-54 ms per frame); a menu screen 8 ms of GPU at 60 fps. The textures sat in the same pools
(17-39 MB outside CDRAM), so where a texture lives barely matters: ~95% of the battle's GPU time
is fragment shading.

The candidates are PokeRogue's own shaders (`src/pipelines/glsl` in the game): `SpritePipeline`
(Pokémon and trainers) runs two loops of up to 32 colour comparisons per pixel over uniform arrays
(variant and fusion palettes; they break at the first entry with alpha 0, usually the first),
then tera, day/night and shadow branches, all in highp; `FieldSpritePipeline` (the arena and most
other sprites) picks the texture with an `if` per unit, then day/night and terrain branches. How
vitaGL's GLSL-to-Cg path compiles those loops and branches for the SGX is the open question. Next:
GPU time per shader program, then shader rewrites that keep the output identical.

### GPU time per shader (log40/log42, 0.61, 2026-10-20; PokeRogue live build of that day)
`gpuprofile=1` (one frame every 10 s waits for the GPU after each draw; ~0.8 ms of that is the
wait's own scene overhead per draw). With Phaser told of 4 texture units (log40), in battle:
PokeRogue's field shader 37-58 ms in 3-7 draws, of which the full-screen arena backdrop alone,
one quad, ~19.5 ms; PokeRogue's sprite shader ~10-12 ms per Pokémon (11-28 ms in 1-6 draws);
Phaser's multi-texture shader ~1-3 ms per draw, up to 10 ms for a full-screen one (5-57 ms per
frame). With 16 units (log42) the field quad stayed at ~19 ms, but Phaser's biggest draws took
~19 ms instead of ~10: the texture if-chain costs Phaser's shader, not the field's. With 1 unit
the sprite shader can't link (it reads `uMainSampler[1]` for the tera pattern) and the game stops.
The sprite shader reads its texture unconditionally, so its cost is elsewhere: its 32-step colour
loops and blocks under conditions that mix a per-pixel test with a setting (tera, day/night), which
the compiler may run for every pixel. The field shader has such a block too (terrain, with two
RGB/HSL conversions).

0.62: `js/webgl.js` rewrites those shaders for the same pixels (texture pick without branches,
colour loops skipped when their palette is empty, setting tested first), checked byte for byte by
`tests/shaders` on the desktop (23 checks; a deliberately broken rewrite fails 11). The profiler no
longer waits for the GPU every frame on screens with nothing drawn. **To measure on the console:**
`gpuprofile=1`, and one run with `shaderfix=0` for the old numbers.

### The stats graph showed a texture (photo, 0.61; runtime 0.63)
On the egg-hatch summary the stats hexagon (two `Phaser.GameObjects.Polygon` fills in PokeRogue's
`stats-container.ts`) showed a blurred sprite inside, with 4 and with 16 texture units. Phaser fills
polygons with `batchTri(null, ..., tintEffect 2)`: solid colour, no texture of their own, on the
current unit. The shaders test `outTintEffect == 2.0` on the interpolated varying; on the Vita that
apparently fails, and the colour is multiplied by whatever texture is bound (rectangles hide it:
they bind Phaser's white texture). 0.63 rounds the tint effect once (`floor(x + 0.5)`) in every
shader that tests it; `tests/shaders` draws with 0.99/1.99 and checks they come out as 1/2, and that
everything else stays byte for byte. **Not yet seen on the console.**

### Shader rewrites on the console (log48/log49, 0.63, 2026-10-20)
The stats graph is right with the rewrites and wrong with `shaderfix=0`: the inexact tint effect
was the cause. GPU time (`gpuprofile=1`), rewrites on (log48) against `shaderfix=0` (log49): the
field shader's full-screen quad 18.0 ms against 20.3 in battle, 17.2 against 18.9 on the title
screen; Phaser's shader on the title screen 10.5 ms against 12.0 for 5 draws (biggest 4.8 against
5.8). PokeRogue's sprite shader stayed ~11 ms per Pokémon (different Pokémon in the two runs). So
the rewrites save ~10-15%; most of the fragment cost is elsewhere (a plain texture read drew a
whole battle in ~3 ms). Next: a benchmark that draws each shader full-screen with one part removed
at a time, to find which part costs.
