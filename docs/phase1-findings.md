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

### What each part of the shaders costs (benchmark log52, 0.64, 2026-10-20; runtime 0.65)
`bench=shaders` on the console: GPU time per full-screen draw (960x544, alpha blending, median of
12, each draw waited for alone; 0.8 ms of that is the wait; a plain texture read 1.5 ms). Field
shader: 19.0 ms as shipped, 14.8 rewritten; without the texture pick 14.9, without day/night 10.8,
without terrain 12.4 (terrain was off), without tint modes 15.1, mediump 16.6, texture x tint only
1.9. Sprite shader: 23.8 as shipped, 22.3 rewritten; without the colour loops 20.7, tera 20.2
(off), day/night 17.3, shadow 17.3, grey/tone 22.1, tint modes 22.8, mediump 24.0, texture x tint
only 1.9. Phaser's: 6.6, rewritten 4.9, one texture 3.8, no tint modes 4.2, mediump 5.8. So the
compiler runs every part for every pixel, whatever a setting says: splitting conditions so a
setting is tested first (0.62) changed nothing (14.8 against 14.9 for the texture pick alone), and
precision isn't the cost.

0.65: shader variants (development.md, "GPU measurements"): copies of PokeRogue's two shaders
without the effects the current settings switch off, chosen at each draw. Expected from the
numbers above: the field quad ~15 -> ~8-9 ms (no terrain), an ordinary Pokémon ~22 -> ~15 ms (no
tera, no palette loops). Day/night (6.5-8 ms) and the shadow (6.5 ms) stay while they are on.
**To measure:** `bench=shaders` again (the "rewritten + variants" lines), and a battle with
`gpuprofile=1`.

### Shader variants on the console (log61, 0.65, 2026-10-20; runtime 0.66)
`bench=shaders` with the variants: field shader 10.9 ms per full screen (19.0 as shipped), sprite
12.0 (23.8). In battle 16-25 fps, up from 7-14; a battle frame is now bound by its ~40 ms of JS,
not the GPU. But each copy took 0.78-0.98 s to build the first time a draw needed it, and the
game stood still meanwhile: the stutters that sometimes swallowed a button press.

0.66: the copies are built ahead of use, and vitaGL keeps compiled shaders on the memory card.
- vitaGL's shader cache (`HAVE_SHADER_CACHE`, `<data>/shader-cache/`, keyed by a hash of the
  source; 512 folders made on the first start): a shader compiled once loads from the card on
  later starts, the game's own included.
- Precompiling (`gl.c`): when a family is linked, the copies it is likely to need are queued
  (terrain, day/night and shadow each on or off, with at most one of tera, shiny palettes and
  fusion palettes on: 16 for the sprite shader, 3 for the field shader). They are built while the
  game's long loading frames run, under the loading screen ("Preparing shaders N/M"), and
  otherwise between frames, at most one slow build per 300 ms, with "Preparing shaders N/M: the
  game may stutter" in the bottom-left corner. A copy no one queued is still built on first use.
  `precompile=0` in config.txt (debug builds) builds them only on first use, to compare.
- On the desktop (Mesa) the 19 copies take 0.3 s; the PokeRogue pipelines are linked inside the
  game's first long frame, so all of them were built under its loading screen.
- `bench=shaders` now waits for the dev build's update check, like the game (it ran first before).
**To measure:** the `[gl] shader variants precompiled: N in S s` line on the first start after
installing (expected ~15-20 s, under the loading screen) and on the next start (from the cache),
and whether the game's own start (after `[phaser] renderer`) got shorter.

### Stutters after precompiling (log65, 0.66, 2026-10-20; runtime 0.67)
The first start built all 19 shader variants under the loading screen (25.6 s in one 42 s frame;
0.3-3.8 s each), and none were built in play. Frame rates were better, but play still stuttered:
in battle, most 10 s windows had a frame of 400-1000 ms, some several of ~600 ms (p95 590 ms).
One was a garbage collection (1.5 s, JS heap 62 MB). The sampler put 30% of JS time under the
game's gamepad handler (`processInput`), and the GPU took 53-57 ms per frame in that battle.
Texture memory was nearly full (video RAM 2.3 of 95 MB free, RAM pool 6.9 of 64), so eviction ran
under pressure: anything not drawn for 90 frames (~7 s at 12 fps) was dropped, however often it
came back, and re-decoded from its PNG in the frame that drew it next. The log shows 1-11 such
re-uploads per 10 s; battle menus used every turn fit that pattern.

0.67, to find and cut the stutters:
- `[slow]` lines (profiler.c, while sampling): for each frame with 250 ms or more of JS, the stacks
  its time went to. Each sample is charged with the time since the one before, so a long native
  call or a garbage collection shows up on the stack right after it.
- `[gl] texture N (WxH) re-uploaded in X ms, F frames after its eviction (back K times)` for each
  re-upload of 30 ms or more.
- Under pressure, a texture that came back now waits 2x, then 4x, 90 frames before it can go again.
- `[prof] (this report took N ms)`: the 30 s report writes profile.txt to the card.
**Next:** if the re-uploads are the stutters, decode them on the other cores (the image thread)
instead of in the frame.

### What the slow frames were (log68, 0.67, 2026-10-20; runtime 0.68)
Second start of 0.67: the 19 shader variants came from vitaGL's cache in 0.5 s (12-22 ms each,
against 0.3-3.8 s on the first start). Play felt smoother, with some stutters left. The `[slow]`
lines and timings named them:
- The profiler's own 30 s report: 0.5-5.0 s (`this report took`), between frames, so the frame
  statistics never showed it. Each function was looked up by a linear search over ~2000 names
  for every frame of ~3000 stacks, and profile.txt was written to the card in small pieces.
- The `[mem]` breakdown every ~40 s counts the JS heap by walking every object: 37-45 ms on the
  desktop for 104 MB, so likely most of a second on the Vita, also between frames.
- One garbage collection mid-battle: 2.4 s (with a 1.4 s `addAt` charged to it).
- The game's own work, on one core: building move animations (`populateMoveAnim`, 300-500 ms
  when a move is first used), saving (`saveAll`, JSON.stringify, 200-450 ms), the party screen's
  text objects (~400 ms), reading system data (JSON.parse, 760 ms, once).
- Texture re-uploads: 31-42 ms each, 17 in the session: not the problem.

0.68:
- profiler.c: functions found through a hash index; a stack's functions counted once with a
  marker instead of comparing names; profile.txt written whole by a thread of its own. The report
  took 0-2 ms on the desktop (5 ms before).
- `[mem] running` lines take the JS heap from QuickJS's GC threshold (1.5x what the last
  collection left) instead of counting it; startup stages still count.
- Under the loading screen (once per long frame), if the JS heap is at least a third of the way to
  QuickJS's next collection, it collects then (`[gc] collected under the loading screen`), which
  also puts the next collection further off.

### 0.68 in play (log71, 2026-10-20; runtime 0.69)
- Garbage collection under the loading screen worked (372 and 908 ms, hidden); no collection
  landed mid-battle in this session.
- The profiler report was down from 0.5-5 s to 0.3-0.7 s, still a stall every 30 s. The rest was
  the log itself: host_log wrote and flushed each line to the memory card from the calling
  thread, several ms per line on the Vita, and the report is ~45 lines.
- Battles at 22-37 fps. The `[slow]` frames left in play are mostly the game's own work: the party
  and summary screens building their text objects (30-60 ms per stack), ability checks (~110 ms).
  One runtime cost: each stat-change animation spent ~320 ms in a single native call while
  Phaser created its TileSprite (no JS ran and no collection happened; the canvas pool and the
  canvas resize are fast on the desktop).
- A bug report: in a trainer battle in the temple, a mirrored copy of a Pokémon's sprite showed
  under it, where its shadow goes. That part of the sprite's quad is stretched below the frame for
  the shadow, and the shader is meant to darken or discard every pixel there. Not reproduced yet.

0.69:
- Log lines go to a writer thread (another core) through a 256 KB ring; host_log_flush() waits for
  it before the log is uploaded and before the app quits, restarts or starts the updater. It is
  also an atexit handler: the first build failed CI's Vita3K boot check (no shader compiler, so
  main returns early) because exit() closed log.txt while the writer was still flushing it.
- `[arena] <name>` when the game loads a battle background, so a bug report can be placed.
- `[gl] texImage2D WxH took N ms` and `[canvas] a WxH canvas took N ms to allocate` for calls of
  30 ms or more, to find the stat-change stall.

### 0.69 in play (log75, 2026-10-20; runtime 0.70)
- The profiler report: 9-77 ms (0.3-0.7 s in 0.68) with the log written by its own thread.
- `[arena]` lines work (default, town, temple in this session).
- Each stat-change animation (stat up/down arrows) stalled ~330 ms: Phaser's WebGL TileSprite
  makes a canvas of its full size (936x1896 here), never draws on it, and uploads it as a texture:
  `[gl] texImage2D 936x1896 took 200 ms`. The rest of the stall is its pattern texture (896x576,
  52-83 ms). 0.70: on the Vita, a canvas that was never drawn on since it was sized goes up as
  `glTexImage2D(..., NULL)`, which vitaGL fills with zeros itself: no 7 MB to allocate, clear,
  convert and copy. (Desktop GL leaves such a texture undefined, so the desktop still uploads.)
- The worst stall left is the game's own end-of-wave save: `saveAll` parses and stringifies the
  system data (0.7-1.9 s), and once a garbage collection landed in it (a 3.1 s frame).
- Battles at 12-20 fps in this session's later waves (GPU 34-38 ms per frame; JS 46-60 ms).

### The Temple reflection, caught on screen (0.69-beta, 2026-10-20; runtime 0.71)
A screenshot from the release: a trainer battle in the Temple, the enemy's Cofagrigus at wave 76.
Under its base, left of the Darkrai info box, a fragment of sprite pixels in the coffin's colours
shows where only the shadow (or nothing) belongs; sometimes, per the player, it is larger.
PokeRogue's sprite pipeline stretches a Pokémon's quad below its frame for the shadow (batchQuad:
y1 and v1 grow; vCutoff keeps the frame's real bottom) and the shader darkens or discards every
pixel of that strip. Pixels of the sprite sheet showing there means that test didn't run or
failed: the copy without the shadow drew (a stale hasShadow), or the comparison came out wrong on
the GPU. The code paths checked (the native uniform setter, the fast paths, the variant choice)
all go through the recorded GL calls, so no cause is known yet.
0.71 (debug builds): L + R + TRIANGLE switches the variants off and on while playing. If the
fragment goes away with them off, they are the cause; if not, the next suspect is the rewritten
or original shader's shadow test at the Vita's precision.

### A long session ran out of memory: the game never destroys its stat-change sprites (log79, 0.70, 2026-10-20; runtime 0.72)
After a long session in the Temple, a stat change failed with `InternalError: out of memory` in
getImageData and the battle stopped (the phase never ended). The game's playStatChangeAnimation
adds a TileSprite of 156x316 times the sprite scale (936x1896 at the Vita's), fades it out and
never destroys it. Each one kept a 7 MB texture, the CanvasTexture's 7 MB copy of its pixels
(Phaser reads the whole canvas back at once) and, in our runtime, the canvas's own 7 MB (that
getImageData allocated a blank canvas's pixels and pinned them). The main heap went from 107 to
236 MB, with 6.8 MB textures spilling out of video memory into the heap.

0.72:
- getImageData of a canvas never drawn on returns zeros without allocating its pixels, and the
  canvas stays unallocated.
- On the Vita, the texture of such a canvas (made without data since 0.70) can be evicted like an
  image's, and comes back as zeros if it's used again.
- js/bootstrap.js: every 120 frames, a TileSprite showing `battle_stats` that has faded to alpha 0
  with no tween on it is destroyed, as the game's code would have done (`[phaser] removed a
  finished stat-change sprite`; PKR_STAT_SPRITE_CLEANUP=0 in localStorage turns it off).

### Local storage saved off the game's thread (log83, 0.72, 2026-10-21; runtime 0.73)
log83: a `[slow]` frame spent 321 ms in storage.js's flush, the once-a-second save of local
storage after the game changed it (the game saves after each wave). The time was the write to the
memory card, not JSON.stringify. 0.73: storage.js still makes the JSON on the game's thread, and
`writeFileLater` copies it to a writer thread that writes it (atomically, as before). A newer copy
of a file still waiting replaces it; the forced flush before a restart and the app's exit wait
for the queue. Writes of 100 ms or more are logged (`[storage] ... written in N ms`).
tests/smoke checks that a forced flush puts the value in the file.

### What else holds the main thread (logs 75, 81, 83; runtime 0.74)
The profiler's main-loop split in battles: the game's JS 83-94% of the time (native work done for
it included), waiting in the buffer swap 5-9% (~4-7 ms per frame), the control channel 1-2%
(dev builds; some windows 3-4 ms per frame), our drawing and after-frame work under 1 ms. The
swap's wait is the GPU (vitaGL already triple-buffers; vglSwapBuffers waits only when the display
queue is full, and the Temple's GPU time is 34-38 ms against 40-55 of JS), not something a thread
could take. 0.74 moves what could be moved:
- Text uploads: plutovg keeps a canvas premultiplied, and Phaser uploads its text canvases with
  UNPACK_PREMULTIPLY_ALPHA. The upload un-premultiplied every partly transparent pixel (three
  integer divisions, which the Vita's CPU does in software), then premultiplied the copy again.
  A premultiplied upload of a canvas is now one swizzle pass of its store, as browsers do. The
  log showed 30-60 ms per text-line upload in battles (828x286, 1626x179, 1674x179).
- The control channel (dev builds): read by a thread of its own four times a second; the main
  thread only runs the lines it queued.
- The GPU timer (dev builds): one measured frame every 5 s instead of every 30 frames (two
  glFinish per measurement, each holding the main thread for a whole GPU frame).

### The Temple reflection is the shader variants (log86, 0.73, 2026-10-21; runtime 0.75)
With L + R + TRIANGLE in a Temple battle: the strip under Cofagrigus showed sprite-sheet pixels
with the variants on and was right with them off (`[gl] shader variants off/on` lines in log86).
So a copy drew where the original would have darkened or discarded the strip: either the copy
without the shadow code was chosen (a stale `hasShadow`), or a copy had stale shadow values
(`vCutoff`, `relPosition`, ...), or the Vita's compiler builds a copy wrongly.
One way the records can go stale only on the Vita: vitaGL's uniform location is a pointer to its
own program's uniform (built without SAFE_UNIFORMS), so a write lands in that program whichever
program is bound, where WebGL and the desktop's GL apply it to the bound program (or refuse it).
The record (gl.c var_record) filed each write under the bound program, so a write to the sprite
shader made while another program was bound reached the Vita's program but not the record. Phaser
binds through its own idea of the current program (setProgram skips useProgram when it believes
the program is bound), so a write like that needs only that idea to be wrong once.
0.75: on the Vita, a write is recorded in the family that owns its location, as vitaGL applies it,
and such writes are logged (`[gl] uniform NAME written to program N while program M is bound`).
If the log shows them and the strip is right, this was it; if the strip still breaks without
them, the next suspect is the compiler (a copy with the shadow code drawing it wrongly).

### No shadows with the variants on: GLSL bools are floats on vitaGL (log90, 0.75, 2026-10-21; runtime 0.76)
0.75 on the console: the Temple strip was still wrong, and with the variants on no Pokémon had a
shadow at all; no `written to program ... while ... is bound` lines, so 0.75's theory was wrong
(its change stays: it matches how vitaGL applies a write). The cause: vitaGL reports a uniform's
type from the compiled program, and Cg compiles a GLSL `bool` to a float, so `hasShadow`,
`yCenter`, `ignoreTimeTint` and `isOutside` came back as GL_FLOAT. The game sets them with
uniform1i, so the record held the int 1; read as a float that is 1.4e-45, a denormal the Vita's FPU
flushes to 0 (Vita3K, on an x86 host, keeps it: the emulator never showed this). So every sprite
looked shadowless and drew with the copy without the shadow code (no shadow; the stretched strip
under it showed the sprite sheet), and copies were synced with glUniform1fv of those bits.
0.76: each write is recorded with how it was made (glUniform*i or *f), and reading the settings and
syncing a copy use that, whatever type the GL reports.

### Where the GPU time goes now that shadows draw (logs 95 and 97, 0.76, 2026-10-21; runtime 0.77)
The shader benchmark (log95, ms of GPU time per full screen of one shader): sprite shader as
shipped 23.7, as 0.76 draws it (rewritten, the copy without tera and palettes) 16.2, without
day/night 17.1, without the shadow 17.2, texture times tint only 1.9; field shader as shipped
18.2, as drawn 11.8, without day/night 10.8, texture times tint only 2.7. 0.65's 12.0 for the
sprite shader as drawn was the copy without the shadow that 0.65-0.75 picked by mistake.
A Ruins battle with gpuprofile=1 (log97, each draw waited for alone, so the sums run high): the
field shader 25 ms a frame in 4 draws (the backdrop alone 11.6), the sprite shader 11-20 ms in 2-4
draws (up to ~10 per Pokémon), Phaser's own 11-37 ms (more with menus open).
So day/night and the shadow are each about 6.5 ms per full screen in the sprite shader, and
day/night ~7 in the field shader. Most of day/night is the if-chain that picks its colour: it
reads settings only, so it is the same for every pixel of a draw, but the GPU runs it per pixel.
0.77: js/webgl.js marks the chain (`/*pkr<tint*/ CHAIN /*pkr|dayNightTint = pkr_dayNightTint;*/`),
gl.c's copies have the uniform read instead, and gl.c works the colour out once per draw from the
recorded uniforms, with the same tests (day_night_tint). A family with such a swap always draws
with a copy (one more precompiled copy of the field shader: 20 in all). The shadow block now tests
`spriteY >= 0.9` on its own first and works out the pixel's x on the sprite only inside it (the
same pixels; whether the Vita's GPU then skips the rest is for the benchmark to show). The
benchmark compares both with 0.76's code, and has rows for the field shader with one texture unit
(the game's field draws are one sprite each, so always unit 0) and the shadow without `discard`.

### Day/night once per draw: a third off both shaders (logs 100 and 102, 0.77, 2026-10-21; runtime 0.78)
The benchmark (log100, ms per full screen, 0.77 against 0.76's code in the same run): field shader
as drawn 7.7 against 11.7 (without day/night at all 4.8, with one texture unit 6.9); sprite shader
11.1 against 17.0 (day/night per pixel alone 15.2, the shadow tests as shipped alone 12.9, without
the shadow 7.9, without day/night 9.1). So the per-draw colour saved ~4 ms per full screen in each
shader and the nested shadow tests ~1.8. Without `discard` the shadow was 0.2 ms cheaper: not worth
changing what other blend modes would see. In the Ruins battle (log102, gpuprofile=1) the backdrop
took 8.3 ms against 11.6 in 0.76; fps there isn't comparable (gpuprofile waits for every draw on the
main thread). What day/night still costs, ~3 ms per full screen, is mostly its blend:
blendHardLight(c, tint) per channel is tint < 0.5 ? 2 tint c : 1 - 2 (1 - tint)(1 - c), which
is c * mul + add with mul and add from the tint alone. 0.78: the copies have that multiply-add
(pkr_tintMul, pkr_tintAdd, worked out with the colour in gl.c). The pixels are within 1/255 of the
original's (the multiply-add rounds differently; tests/shaders allows that for copies).
Measured (logs 106 and 108, 0.78, 2026-10-21): the benchmark has the field shader at 5.5 ms per full
screen (0.77 7.7, 0.76 11.7; without day/night at all 4.8) and the sprite shader at 9.0 (0.77 11.9,
0.76 16.2; without the shadow 5.9). In a Ruins battle (gpuprofile off) the GPU took 30-36 ms a
frame against 38-48 in 0.76's (log93), but fps stayed at 17-22: JS takes 44-55 ms a frame, so the
frames wait on JS and the GPU now finishes inside that time.

### A battle frame's JS: PokeRogue's sprite pipeline (log108, 0.78, 2026-10-21; runtime 0.79)
In the Ruins battle (log108) the profiler put 37% of a battle frame's JS (~45-55 ms) in PokeRogue's
sprite pipelines, the per-sprite code that sets the shaders' uniforms: self time in
SpritePipeline.onBind 9%, onBatch 7%, `map` 6%, FieldSpritePipeline.onBind 3%, `isOutside` 3%.
On the desktop (offline game, first battle, command menu) the same split: 55% of the frame's JS,
with SpritePipeline.onBind ~130 us and onBatch ~120 us per Pokémon sprite, FieldSprite's ~30 us.
Most of it is two palette loops: per sprite and frame, four 128-value arrays are built with 128
`arr.splice(arr.length, 0, ...four)` calls (~0.45 us each on the desktop even with patch 0004's fast
paths: the call itself), then copied with `flat()` for set4fv. For a sprite that isn't shiny and has
no fusion colours, which is nearly every sprite, all the values are 0.
The game can't be changed and its code is compiled without its source (bytecode cache), so this is
done on the source as a chunk is compiled (src/gamefix.c, with the owner's approval): exact pieces
of the minified code with the names left open; the zero cases take a shared array of 128 zeros.
If the game changes that code, nothing matches and its code runs as it is (config.txt gamefix=0
turns it off). Desktop: Sprite.onBind 130 -> 72 us, onBatch 120 -> 46 us (34 of them the draw).
Checked: the GL uniform calls of three frozen frames with and without it have the same colour arrays
(the other differences are animation frames that froze differently between the runs), and
tests/smoke runs both versions of the game's code (as a chunk and as plain text) on shiny, fusion
and plain cases with the same results.
0.80: log108's battle had a shiny Pokémon (`rgbHexToRgba` in the profile, which only shiny sprites
reach): its onBatch parses 64 hex colours and makes 64 splice calls every frame. The cache entry it
reads is a JSON object the game replaces but never changes (src/sprites/variant.ts), so the two
arrays are now kept per entry (a WeakMap) and built once. Desktop, after both: JS per battle frame
(command menu, no shiny) ~2.15 -> ~1.95 ms. The live site's battle chunk (2026-10-21) gets the
same three rewrites as the offline build's, and both still parse.

### Bitmap text: the item counts (logs 93, 102, 108; runtime 0.81)
In late battles on the Vita, Phaser's BitmapTextWebGLRenderer was 15-22% of a frame's JS (on the
stack), with BatchChar under 2%: PokeRogue draws every held item's stack count as a BitmapText of one
or two characters (getIconStackText), so the cost is per text, not per character. 0.81: the
character loop (BatchChar and MultiPipeline.batchQuad) is native everywhere (__native.batchChars), and
a BitmapText in a container, where the item icons keep them, is drawn wholly by the C container walk
(batch_bitmap_text: GetCalcMatrix, tints, setGameObject, the stored text bounds, flushes before the
same characters as Phaser). Desktop, 200 item-count texts in containers: JS per frame 4.8-5.8 ->
3.4-3.6 ms (p50), so about half of a text's cost; a 2000-character text: 705 -> 87 us. Phaser's own
renderer still draws texts with per-character colours or a post pipeline, and pipelines with their own
batchQuad, onBatch or shouldFlush. Checked with probe_verify_bitmaptext.js (CI, tests/phaser: plain,
tinted, letter-spaced, drop shadow, rotated, in a container, and one long enough to flush mid-text
in both passes; pixel rounding on). It found one difference on the way: the native rounding gave +0
where Math.round gives -0 (-0.5 <= x < 0); fastpath.c now has QuickJS's Math.round (js_round),
also used for NineSlice vertices.

### 0.81 on the console, and the arena tints once per frame (log111, 0.81, 2026-10-22; runtime 0.82)
0.81 in the same Ruins battles as log108: 27-32 fps (17-22 in 0.78), JS per frame 25-33 ms at the
median (41-51). The battle reports now put the sprite pipelines at 16-26% (FieldSpritePipeline.onBind
with `isOutside` and `map` ~12%), tweens 13-18% and the game's frame listeners 8-14% (one report 30%:
the game's own button handlers, run from Phaser's gamepad button events). FieldSpritePipeline.onBind
works out the arena's five colours (new arrays, isOutside three times) for every sprite, though they
only change between frames; src/gamefix.c's fourth rewrite keeps them on the pipeline for the frame
(game.loop.frame), so the same arrays also let Phaser's WebGLShader skip the repeated uniform writes.
Checked: the uniform state at every field and Pokémon draw of three frozen frames is the same with
and without it, and tests/smoke runs the game's onBind both ways over two frames (same calls, two
isOutside calls instead of three per bind). Desktop battle: JS per frame ~1.95 -> ~1.55 ms
(FieldSprite.onBind 25 -> 18 us, SpritePipeline.onBind 63 -> 49 us, onBatch 46 -> 31 us with fewer
uniform writes per flush).

### 0.82 on the console: tweens first, and the GPU in the Ruins (log114, 0.82, 2026-10-22; runtime 0.83)
0.82: Forest battles 28-41 fps (JS 21-29 ms, GPU 18-28 ms per frame); the Ruins 27-32 fps in later
waves, but 18-26 fps in one stretch where the GPU took 35-49 ms a frame (swap waits 4-10 ms): there
the GPU, not the JS, sets the frame rate. In the JS, tweens are now the largest group (16-29%), and
almost all of it under Phaser's Tween.update is `call` (callbacks), not the stepping; on the desktop a
turn of the first battle shows tweens at 4-8% and only Phaser's own code. 0.83: the 30 s profile
report adds, for tweens, other updates and frame events (each at 5% and up), the game's own functions
under them (the first frame of the game's code below tweenStep or Game.step), so the next console log
names the callbacks.

### 0.83's logs: where the game logic goes, and transparent arena bases (logs 117-119, 0.83, 2026-10-22; runtime 0.84)
The new report lines (log118): under tweens, 51-83% is Phaser's own stepping and the rest the game's
callbacks (battle animation onRepeat/onComplete, UI onUpdate); under frame events, the game's
gamepadButtonDown handler, which in one menu (remembering a move: every cursor move recomputes the
learnable level moves through the pre-evolutions, ~0.5 s per press) was 43% of the JS. The GPU profile
(log119, Forest): the field shader ~14.5 ms a frame in 3 draws, and one Pokémon-shader draw of 9-11 ms
in many frames, the cost of a whole screen of that shader (not seen on the desktop, where the Pokémon
quads are 2-4% of the screen; maybe a move animation's backdrop). On the desktop the arena's two
bases are 1920x792 images, 73% of the screen each and mostly transparent, drawn with the field shader
like the backdrop: about 2.5 screens of it per frame. 0.84: for a transparent texel without a fill
tint the field shader ends right after `color = texture * texel` (the rest leaves color as it is:
the tint effects need effect 1 or 2, day/night and terrain need color.a > 0); tests/shaders now
matches the original exactly in every field case. The benchmark has a row without it.
0.85: Phaser's tween stepping in C (src/fastpath.c "tweens", js/fastpath.js part 19): Tween.update of
an active, started, unpaused tween, and TweenData.update while playing forward or backward without
finishing in the step (Linear, Sine and Cubic eases computed with the same operations as Phaser's,
which covers nearly all of PokeRogue's; others called; onUpdate dispatched only when someone
listens). Starts, ends, repeats, yoyos, holds, delays and texture tweens stay in Phaser's code.
probe_verify_tweens.js (CI, tests/phaser) steps the same tweens (every ease the game uses, yoyo,
repeat, hold, delays, a number tween, two targets, time scale, update listeners) natively and with
Phaser's code over the same deltas and compares targets, state and callbacks after each step; it
catches a 1e-12 change in an ease. Desktop: 0.55 against ~2.9 us per running tween of two properties;
the first battle's idle tweens 89-99 -> 73 us a frame (the rest: the game's callbacks and setters).

### A long session slows down: the party screen's icons pile up (log123, 0.85, 2026-10-22; runtime 0.86)
A 21-minute session on 0.85: the first battles ran at 28-41 fps with 20-30 ms of JS at the median;
after ~8 minutes many battles took 34-44 ms (18-25 fps), and tweenStep's own share of the samples
went from 2% to 8%. PokeRogue's PokemonIconAnimHelper (the party screen's jumping icons) keeps every
icon added to it, and the party screen destroys its slots on every opening (clearPartySlots) without
taking their icons out: each opening adds the party's icons again, and two endless tweens of the
helper set the y of all of them every 200 ms and on every frame of a jump. Desktop, ten openings
with a party of two: 22 icons kept (all but two destroyed sprites). 0.86: src/gamefix.c's fifth
rewrite drops an icon whose sprite was destroyed (no scene) in those two loops instead of moving it
(the live ones move as before; tests/smoke runs the game's loop both ways); after ten openings the
helper keeps none (two while the screen is open). Each rewrite now has its own marker, so chunks
without the code are skipped quickly, and may apply more than once (this loop is in two places).
The two ~10 s pauses in the log were not garbage collection mid-battle: the collection itself took
1.7 s under the loading screen; the rest was the game resetting for a new run (reset, launchBattle,
the UI screens set up again: MoveInfoOverlay alone ~3 s).

### 0.86 over a long session (log126, 0.86, 2026-10-22; runtime 0.87)
20 minutes in the Jungle and the Temple, battles getting harder: JS per frame stayed at 27-35 ms at
the median from the first battle to the last, mostly 24-32 fps (0.85's log went from 20-30 to 34-44
ms over the same time), so the party icons were the slowdown. Two things are left. An 80 s stretch
in the Jungle where the GPU took 47-60 ms a frame (18-20 fps), the most yet, with nothing in the log
to say what was drawn. And ~10 s pauses when a new run starts (game over): the game resets and sets
its UI up again, and two collections of its ~65 MB of live JS data run in that frame (2 s under the
loading screen, ~3.4 s in MoveInfoOverlay's setup, where the threshold is crossed). 0.87: the GPU
profile (gpuprofile=1) also logs the frame's three slowest draws with their shader, the size of the
texture on unit 0 and the area their vertices cover (desktop: "PokeRogue field 3.6 ms, texture 320x180,
6 vertices over x 0..1920 y 0..1080" is the backdrop), so a costly draw can be named from the log.

### A stall when terastallizing, and shiny sprites on the GPU (logs 130-131, 0.87, 2026-10-22; runtime 0.88)
log130: a one-second loading screen when a Pokémon terastallized: "shader variant: program 13 without
fusion (3274 ms to build)", the sprite shader with tera and shiny colours both on (a shiny Pokémon).
Only combinations with at most one of tera, variants and fusion were precompiled. 0.88 precompiles
every combination (after the likely ones): 32 copies of the sprite shader, 4 of the field's (the
first start after installing prepares ~16 more, a few seconds each; later starts load them from the
cache in milliseconds).
log131 (gpuprofile=1, Temple), with 0.87's slowest-draw lines: the backdrop 6.7 ms (320x180 over the
whole screen), each base 3.3 ms (320x132 over 1920x792), UI draws up to 8 ms, and Pokémon draws of
6.3 ms (a 73x73 texture over ~6% of the screen) and 10.5 ms (514x514, ~12%): about 100 ms per full
screen, ten times a plain Pokémon. Those are shiny sprites: the palette loop compares each pixel
with the variant colours, and the GPU runs its 32 steps for every pixel of the quad, transparent
ones included (most of a sprite's quad, and all of the shadow strip below it). 0.88: the loops only
run for texels that aren't transparent (the match needs texture.a > 0, so the pixels are the same).
The benchmark gets rows with a 12-colour shiny palette, with and without that test.

### Debug options on the start screen (owner's request, 2026-10-22; runtime 0.89)
Editing config.txt on the console for each test was slow. Debug builds' start screen now lists the
shader benchmark as a mode (as `bench=shaders`) and the on/off options as toggles written back to
config.txt (gamefix, shaderfix, precompile, sample, hud, gpuprofile, plainshaders, nettrace). The
log notes each switch (`[start] key=value`). `log=1` isn't there: debug builds always log.
`audio=0` isn't either: sound starts before the start screen.

### A Temple battle's JS (log140, 0.89, 2026-10-22; runtime 0.90)
log140 (Temple, profiler on): 25-35 fps, JS 32 ms and GPU 22 ms per frame, so the JS sets the pace.
Ten reports of the battle (35k samples): update 40% (tweens 16.5%, sprite animations 11.7%, timers
12.3%), rendering 39% (PokeRogue's sprite pipelines 15.2%, Phaser's JS 12.2%, native 9.4%, text
2.9%), frame events 10% (two fifths the game's button handling, the rest Phaser's per-frame
listeners). The game's own work shows up as events (mode changes, messages, summons, move
animations); the steady per-frame parts were looked at with a Temple copy of the saved battle on the
desktop (.cache/session.json with arena.biome 29), timing each piece:
- PokeRogue's sprite pipeline: per Pokémon sprite per frame, four 128-value colour arrays went to
  set4fv/set4iv through `.flat()` (a copy of an array that is flat already), so Phaser's uniform
  cache (it compares the array) never matched and all four were uploaded every time: ~13 of the
  bind's ~18 us on the desktop. gamefix.c's colour rules now hand the arrays over as they are: the
  shared zeros and a shiny Pokémon's kept colours are uploaded once.
- FieldSpritePipeline.onBind: nine uniform calls per field sprite (5 per frame in this battle), most
  writing what the previous sprite wrote. A new rule ("arena uniforms once per frame") returns early
  when the frame, shader and the sprite's own values match the pipeline's previous bind (2 of 5 binds
  in this battle).
- Phaser's DOMElement.preRender: the game's ten login and registration fields, hidden all game,
  each wrote `style.display = "none"` through the DOM shim's style Proxy every frame (~4% of the
  desktop's samples). js/input.js leaves the write out when it is "none" already.
- Gamepad: every frame the runtime built a new pad with 18 button objects and Phaser passed all 18
  buttons and 4 axes to its Button/Axis objects (Gamepad.update: 1.9% of the Vita frame on its own).
  The native layer now returns the same array while nothing changed, js/input.js keeps the same pad
  objects, and Phaser's Gamepad.update is skipped for a pad unchanged since its last update (same
  values give the same state and no events: tests/phaser compares a sequence with Phaser's own).
Desktop, JS per frame without GL time (idle in the command menu / fighting, two runs each): 0.89
1.59, 1.56 / 2.27, 2.21 ms; 0.90 1.46, 1.62 / 1.98, 2.09 ms. The desktop has no gamepad and a
single battle with one Pokémon a side, so the Vita's numbers will say more.

### L and R pressed two buttons each (owner's report, 2026-10-22; runtime 0.91)
The Vita SDK's SCE_CTRL_L2/R2 are the same bits as SCE_CTRL_LTRIGGER/RTRIGGER (the PS TV's names
for them), and both were mapped: L reached the game as L1 and L2 (in starter select: cycle form and
gender at once), R as R1 and R2 (shiny and ability). L and R are now L1 and R1 only, and the rear
touchpad is back as L2/R2: a touch on its upper half, left or right side; off while a game text
field is on screen (js/input.js tells the native layer), as resting fingers once kept pressing the
login form's submit through it. 0.90's CI desktop job failed (a static JSValue initializer the
desktop compiler rejects). Locally build_linux.sh had hidden it (its check found the previous
binary); it now fails on a compile error. 0.90 was never published as a dev build.

### A 38-minute session, and starter select (log141, 0.89, 2026-10-22; runtime 0.92)
log141: Town, then Temple battles at 27-30 fps (JS 33-42 ms, GPU 19-24 ms per frame, as log140),
four game resets (new runs, 7.6 s each) and, from t=1800 s, the starter select screen at 17-20 fps
(JS 50-57 ms, GPU 24-37 ms, up to 50 draw calls: the grid of icons). Two of the game's own costs
there, both now rewritten by gamefix.c (same results; tests/smoke compares them):
- Opening starter select after a reset took one 11 s frame: the screen's setup (run again after
  every reset, which rebuilds the UI) called getAllStarters() once per slot, 81 times, each a walk
  over all species: 4.7 s. Garbage collection was 2.4 s of the rest.
- Each filter change (toggling a generation, type...) took 0.94 s: filterStarters read the filter
  bar ten times per starter; ~70% of the frame went to those reads.
A reset also leaves about six endless tweens running (the icon helpers of the old party and starter
screens, a blinking text): measured on the desktop, 8 tweens before a reset and 14 after. They step
every frame but their work is small (the old icons are dropped since 0.86).

### The game stopped after an hour: the heap ran out (log142, 0.89, 2026-10-22; runtime 0.93)
log142 (the same session as log141, sent after it hung at t=3480 s): a stat change animation threw
"InternalError: out of memory" in getImageData (Phaser's CanvasTexture keeps a copy of the
TileSprite's 936x1896 canvas, 7 MB), the battle phase never ended, and the game sat there drawing
frames. The heap (256 MB) had 247 MB in use. JS objects alive after a collection: 43 MB in the first
battle, 61, 74 and 91 MB later; QuickJS collects when its heap reaches 1.5x that, so up to ~45 MB of
garbage sat on top. The rest of the heap grew as the run went on (decoded sounds 16 -> 29 MB,
textures 1599 -> 1787, sprite atlases): the game keeps what it loads for the whole run. Game
resets add a little (desktop: +2 MB of JS objects, +6 tweens, +42 textures per reset).
0.93: the next collection is due before garbage would leave less than 40 MB of the heap free (the
1.5x rule otherwise, also after QuickJS's own collections), and a frame that finds less than 20 MB
free collects first, under the loading screen (not again until the heap has grown by 8 MB, so live
data alone doesn't cause a collection every few frames). getImageData collects and retries once
when it runs out of memory. With live data growing ~30 MB an hour, collections come more often
late in a long session; live data alone filling the heap stays possible after a few hours.

### 0.93 on the console, and the release (log145, 0.93, 2026-10-22)
log145, a new run (Town, Metropolis, Plains; profiler on): battles 33-45 fps (JS 21-30 ms, GPU
19-25 ms per frame), the menus between them 21-22 fps (party icons' animations and button
handling). All seven gamefix.c rewrites applied; no out-of-memory error. One unhandled rejection
from the game's own code ("cannot read property 'play' of undefined" in Pokemon.playAnim after
loadAssets: the Pokémon's sprite was already gone); the game carried on. Released as v0.93, the
first release without "beta" (docs/releases/v0.93.md).

### Two costs that grow during a run (log145, 0.93, 2026-10-23; runtime 0.94)
log145's battle reports, one per 30 s: Phaser's Clock.update (timer events) rose from 1.5% of the
frame's JS to 11.7% over 25 minutes, and PokemonIconAnimHelper's loops (getCurrentOffset,
updateJumpIcons, onAlternate) took 6-10% from the moment the run started.
- Timers: Phaser 3.90's TimerEvent.destroy() only clears the callback. The event stays in its Clock's
  list and, with nothing to dispatch, never completes, so the Clock steps it every frame. The game
  destroys timers that way: the play time timer at every battle load (a new one each time), a
  fainting Pokémon's cry animation, and others. Desktop: 1 active timer after loading, 6, 9, 14 after
  three battles. js/bootstrap.js now takes a destroyed event out of its Clock's list (at the Clock's
  next preUpdate); a re-added one runs again (tests/phaser). After the same three battles: 1-2.
- Icons: the starter select screen stays set up (hidden) for the whole run, and its helper's two
  endless tweens kept setting the y of its ~90 icons (every 200 ms, and every frame of a jump). The
  helper's loops (gamefix.c, since 0.86 for destroyed icons) now go to js/core.js: an icon that isn't
  drawn (it or one of its containers has visible false) is skipped and remembered with the hidden
  object; on the game's prerender event, a helper whose hidden object is shown again sets those icons
  from the current state, which is what the loop would have left (it runs whenever that state
  changes). A drawn icon always has the game's y (checked on the desktop's starter select: 33 drawn,
  0 different; tests/smoke covers the hidden case).
(The offline build of 2026-10-16 has no `jumpOffset=0` in PokemonIconAnimHelper, so its jumping icons
get NaN there; the live game has the field. Not this runtime's.)

### A 30 fps lock (owner's request, 2026-10-23; runtime 0.95)
Battles on 0.93 ran between 30 and 45 fps; the owner prefers an even 30. `fps30=1` (start screen
toggle in every build, L + R + □ in game, "fps30" on the control channel) sets vitaGL's swap interval
to 2: the display shows a new frame every other vblank and the main loop waits for it, so frames
are 33.3 ms apart. Desktop (paced in the main loop): 60.2 -> 30.1 fps. The start screen's toggle
rows are 22 px (from 26) so nine toggles and four modes fit the screen.

### Shorter screen setup, and collections at saves (logs 142 and 145; runtime 0.96)
Starting a new run rebuilds every screen of the game: one 7.6 s frame on the Vita (log142), with
~1,500 text objects made. Two pieces of that, the same results and less work:
- Phaser 3.90's Text constructor registers its 1x1 canvas with the texture manager, which makes a
  GPU texture (and sets its filter), then deletes it at once. On the Vita each texture made and freed
  goes through vitaGL's allocator: ~0.7 s of the 7.6 s in the delete alone. js/fastpath.js 6b makes no
  texture for the canvas of a Text being built (CanvasPool.create with a Text parent, then addCanvas
  of that canvas); the constructor then deletes null and sets null as before. Desktop, one reset:
  1,426 GL textures made and 1,400 deleted before, 716 and 690 after. tests/phaser checks it.
- TextStyle.setStyle (js/fastpath.js since 0.4x) looked up all ~30 style keys in the config through
  Phaser's path rules; a key the config doesn't have gives the default, so those are now set
  directly (fontSize keeps the full rule: it also reads inherited values). scripts/probe_textstyle.js
  over a reset: 710 configs, 0 different from Phaser's own setStyle.
On the desktop the reset frame changed little (GL is cheap there); the Vita's next log will say.
Collections: QuickJS still collects in the middle of play about every 25 minutes (1.6-4 s; logs 142,
145), and a long run has no loading screens to hide them under. The game saves its run at the start
of every wave (localStorage sessionData..., or the server's savedata API online); after such a save,
when the JS heap is two thirds of the way to its next collection, the next frame collects first
behind the loading screen. QuickJS patch 0006 adds JS_GetMallocSize for that check (the heap's
size without walking it). Desktop: a save per wave reaches it.

### 0.96 on the console: the lock, and a collection at a save (log150, 0.96, 2026-10-23; runtime 0.97)
Construction Site battles: JS 23-35 ms per frame at the median and GPU 22-37 ms, so many frames
don't fit 33 ms. With the 30 fps lock on, frames that fit come every 33.3 ms (a 20 s stretch at a
flat 30.1 fps); a frame that doesn't takes 50 ms, and the lock removes only the frames above 30. The
dips the owner saw are mostly slow frames: 79 over 250 ms in 13 minutes, 15 of them the party screen
being built (populatePartySlots, ~330 ms each; half of it making text objects), others the game's
turn logic (speed sorting, messages), text drawing and texture uploads.
The collection after a save ran once: 1.5 s that freed 1.6 MB. It estimated what the last collection
left as the next threshold / 1.5, but since 0.93 the threshold is sometimes set lower to keep the heap's
reserve, so it fired early. main.c now keeps what each collection left (its own, and QuickJS's, whose
threshold is 1.5x that), and the save-time collection also needs at least 8 MB of growth since.


### The party screen and its texts (log150, 0.96, 2026-10-23; runtime 0.98)
The party screen is built anew each time it opens and destroyed when it closes: six slots with ~6
texts each (36 for a full party), plus sprites, icons and containers. On the Vita that was a ~330 ms
frame, 15 times in 13 minutes (log150). Desktop, Temple battle, six Pokémon, per opening: 36 GL
textures made, 62 canvas uploads, 36 textures deleted at close. Two changes in js/fastpath.js 4:
- Measuring a new text no longer makes its GPU texture. Phaser's updateText ends by uploading the
  canvas; the measuring pass (drawing switched off) therefore made a texture from the blank canvas,
  and the draw uploaded again. Now a text gets its texture when it is first drawn, and texts that
  are never shown (the party screen's hidden HP labels, much of the game's prebuilt UI) get none.
  Desktop, in a battle: 1,218 live GL textures before, ~510 after.
- A destroyed text's texture is kept under the inputs of its last draw (text, style, padding,
  spacing, font generation; up to 4 MB, at most 256 KB each), and a new text drawn with the same
  inputs takes it instead of drawing its canvas and uploading it. Gradients, patterns and wrap
  callbacks are never kept. tests/phaser checks the reuse and that Phaser's own drawing of the same
  inputs gives the same pixels; screenshots of the party screen are identical.
Desktop, per opening after the first: 0 textures made (36), 2 uploads (62), 24 texts drawn from a
kept texture; the opening frame 13.7 and 10.3 ms of update (19.4, 12.8 before), render 3.1-3.7 ms
(5.0). What is left is the game making its objects (texts ~0.13 ms each on the desktop). Other
screens built again with the same words (the shop's items, menus) take kept textures too: a
100 s fight loop reused 88 of 1,156 drawn texts.

### 0.98 on the console (log154, 0.98, 2026-10-24)
17 minutes online (Town, Construction Site, Power Plant), 30 fps lock off, sampling off (so only
frames over 1 s are reported one by one). The owner found it smooth. Live GPU textures at the end:
677 (log150: 1,272) for about the same memory. Windows of 20 s whose slowest frame was 150-249 ms:
27 (log150: 4 in a shorter session), with no more in 250-399 than before. That fits party-screen
openings dropping from ~330 ms to about 200 ms, though without samples this log can't name the frames.
One collection after a save took 1.5 s and freed 1.5 MB: the heap had grown 19 MB since the last
collection, all of it kept data, so the 8 MB-growth rule let it through. QuickJS would have collected
at 66 MB a little later anyway.

### The 1.0 release (log154, 0.98, 2026-10-25)
The owner found 0.98 stable on the console (log154 above) and asked for 1.0. runtime/VERSION 1.0
(package version 01.00), the same code as 0.98; notes in docs/releases/v1.0.md.

### Wi-Fi lost at the end of a wave (log163, 1.0, 2026-10-28; runtime 1.1)
Online, the game saves at the start of every wave (gameData.saveAll): it writes the save to
localStorage, then asks the server to check it (GET savedata/system/verify) before the wave
continues. The Wi-Fi had dropped: DNS failed three times (the first try and our two GET retries),
fetch rejected as in a browser, and PokeRogue's verify() doesn't catch that ("unhandled
rejection"), so the promise the wave waits on never settled. The game kept running at 50 fps with
nothing to do, and it stayed stuck after the Wi-Fi came back, because nothing asked again.
A save request (any /savedata/ URL) that gets no response at all no longer rejects. It waits, and a
choice over the game (src/dialogs.c, like the import file picker but with no cancel) offers
**Retry** (sends it again, with any others waiting) or **Back to the title screen** (location.reload:
the app restarts into the same mode, and the run continues from its last save on the server).
Error responses (HTTP 4xx/5xx) reach the game as before; the game's other requests (title stats,
rankings) keep failing as in a browser, since the game handles those. tests/smoke checks that such
a request waits behind the dialog until aborted. Desktop: DNS failure -> dialog; Retry -> sent again
(failed again -> dialog again); Back to the title screen -> restart into the game.

### The egg gacha, and pixels drawn for nothing (logs 166-167, 1.1, 2026-10-28; runtime 1.2)
The connection dialog worked on the console: savedata/updateall failed ("Connection reset by
peer"), the owner chose Retry, the save went through and the wave went on.
The egg gacha ran at ~25 fps with the GPU at 38-40 ms a frame (JS ~20 ms, a few frames of 400-490 ms;
the owner also found it slow to react). What it draws (desktop dump): the battle scene behind it in
full (the arena backdrop with the field shader, its two ground layers, the trainer), a screen-filling
opaque background, then three machines of three 320x132 layers each (1920x792 on screen), of which
11-33% has visible texels: about 9.5 screens of blended pixels a frame. Two changes, both drawing
the same pixels:
- Trimmed quads: image textures get a map of their visible 4x4 blocks at upload (src/gl.c
  gl_alpha_box), and a sprite on MultiPipeline (tint effects 0-1) or PokeRogue's FieldSprite (effect
  0), in normal blending with one tint, is drawn as the part of its quad that can have visible texels
  (2-texel margin), in C (src/fastpath.c sprite_quad.trim) and in batchFast. Gacha layers drop to
  11-33% of their quads; in every battle the arena's two ground layers (FieldSprite shader, 73% of the
  screen each) drop to 4-20% (temple_a: 4%).
- Covered objects: when a top-level child of the scene holds an opaque image covering the camera (all
  texels at alpha 255 by the same map, alpha 1 down its containers, normal blending, no mask, post
  pipeline, rotation or crop), the top-level children before it aren't drawn; they still go in the
  camera's render list (input orders objects under the pointer by it). The menu backgrounds are such
  images (party, summary, starter select, egg list and the gacha's: 100% opaque), so the battle under
  them (arena, Pokémon with their shader, battle info) is no longer drawn. Looked for every 8 frames
  (a walk of ~110 visible objects in a battle, 44 us on the desktop), checked again every frame.
Desktop, screenshots with each change on and off: gacha, Temple battle (animations paused) and the
party screen over it identical, but for the gacha's clock and the message's bobbing arrow.
tests/phaser checks both (pixels in a frame; the skipped objects in the render list; no skip under
a 0.99-alpha cover). Where a pixel centre falls exactly on a texel edge (a scale such as 1.25),
float rounding may pick the other texel for a smaller quad; PokeRogue draws 3 pixels per texel.
The gacha's slow reactions are not explained yet: the log had no samples (the profiler was off).

### 1.2 on the console (log170, 1.2, 2026-10-30)
Online, 9 minutes (Town, Metropolis, Graveyard, then the egg gacha), profiler on. The owner: the egg gacha
went from slow to react to "super responsive". GPU per frame in battles is now 14-19 ms (Metropolis,
Graveyard; logs 150 and 154 had 22-38 ms on other maps), so JS (25-33 ms at the median) is what sets the
frame rate now. The egg gacha at the end: 50-60 fps, GPU 10-18 ms, JS 10-19 ms (log167: ~25 fps, GPU
38-40 ms). Party screen openings reported as slow frames (over 250 ms): 4, at 252-289 ms (log150: 13
in 13 minutes, 298-333 ms); the rest stayed under the report's threshold. Slow frames left are the
game's own work: a move animation's first use (populateMoveAnim, 300-575 ms), wave saves (saveAll's
JSON.stringify, ~300 ms), loading sprite atlases (Phaser's JSONArray parsing), screen setup.

### Versions (owner's scheme, 2026-10-30; runtime 1.0.2)
Releases are major.minor; the dev builds between them are major.minor.N on top of the last release,
and the next release collects them. The dev builds published as "1.1" (the connection dialog) and
"1.2" (trimmed quads, covered objects) are 1.0.1 and 1.0.2; this entry's build is 1.0.2 by name. The
next release is 1.1. The LiveArea version has two numbers only, so 1.0.N shows 01.00 there.

### 1.0.2's full session (log171, 1.0.2 dev "1.2", 2026-10-30; runtime 1.0.3)
The whole session behind log170 (17 minutes, online: Town, Metropolis, Graveyard, egg gacha). No errors
or hangs. Battles 28-40 fps: JS 25-33 ms a frame at the median, GPU 15-20 ms, so JS sets the rate.
The profiler's 12 battle reports, averaged: Phaser's render JS 21%, tweens 20%, PokeRogue's sprite
pipelines 16% (FieldSprite's onBind alone 6% of samples), input and frame events 8%. The cover search
(js/fastpath.js part 20) took 5% of the samples (findCover 3.1%, covers 2.3%), though on the desktop a
search of a battle's ~190 objects takes 0.055 ms, so most of that is the sampler's bias toward JS loops.
1.0.3: searched every 30 frames instead of 8, and what can't change between searches (type, pipeline,
frame and its opacity) is judged once per object and frame.

### Four of the review's findings (review-2026-10.md 3, 4, 5, 9; runtime 1.0.4)
- Renderbuffer leak (3): Phaser's WebGLFramebufferWrapper.destroy deletes what
  getFramebufferAttachmentParameter(..., OBJECT_NAME) returns; the native call answered a number, so
  every destroyed render target left its depth/stencil renderbuffer behind (and deleted its texture
  only through the texture's own wrapper). js/webgl.js now records each framebuffer's attachments and
  answers with the objects (or null). Deleted GL objects are dead in gl.c (id 0, as in WebGL), so the
  texture's second delete can't hit a texture that got its name meanwhile, and is*() answers false for
  them (vitaGL's glIsTexture(0) says yes). tests/phaser checks that a destroyed framebuffer frees both.
- Disk cache cleaning (4): `<data>/http-cache/` kept every file forever, and each game release adds
  newly named chunks. A cached file read again gets its modification time set to now (at most once a
  week per file: utime, no data written; not the unversioned fonts, whose age decides refetching).
  Once a week (`http-cache/cleaned` marks the last pass) the cache thread deletes files unused for 60
  days, one of the 256 folders at a time while idle, from two minutes after launch, paced like the
  old cache's removal. Desktop: a 90-day-old file and a 2-day-old .tmp deleted, a fresh one kept; an
  online start marked all 808 files it read.
- Missing shader compiler (5): the app closed back to LiveArea with the reason only in the log. It now
  shows the reason and how to install libshacccg.suprx, drawn into a display buffer with the 8x8 font
  (vitaGL can't start without the compiler), until a button press or 120 s (config.txt fatal_wait;
  0: not shown). Vita3K 4201 crashes on its own side presenting a display buffer without libgxm (main
  RAM or video RAM alike), so its boot check sets fatal_wait=0; on the console it is still to be seen.
- Download check (9): a downloaded offline game replaced the installed one unchecked (a resume answered
  416 counts as complete). Its size must now be the release's and it must open as a zip with files;
  otherwise it is deleted, the installed game stays, and a message says to try again. Not run end to
  end here (this session can't reach GitHub's API for that repository).

### A session across sleep mode (logs 173-174, 1.0.2 dev "1.2", 2026-10-31)
Online, the app put to sleep at the title (t=200 s; the app's clock stops while the Vita sleeps) and
played on after waking, all in the Graveyard; profiler off. The owner felt it got slower. JS per frame
follows what is on screen, not the time played: grouped by draw calls per frame (t=260-565 s), 13-18
calls: 22-27 ms at the median; 19-24: 30-37 ms; 26-32: 41-48 ms (more sprites: double and trainer
battles). Windows late in the session with 16-18 calls (t=460-476 s) ran at 22-24 ms, as fast as the
first battles after waking. Memory as usual for a run (JS after collections 44 -> 64 MB, GPU textures
55-64 MB, canvases 735 -> 843), no network errors after waking. So no slowdown over time; what costs
is per sprite (each Pokémon sprite is drawn with its own pipeline bind and flush).

### 1.0.4 on the console (logs 175-184, 2026-10-31 to 11-01; runtime 1.0.5)
Seven online sessions on 1.0.4 (three with the profiler: logs 177/178, 180, 181), Town, Graveyard and
Abyss. No errors, no connection losses, collections as before (one after a wave save, 1.6 s).
- The cover search is gone from the profiles (log171: findCover + covers 5.4% of battle samples; logs
  178 and 181: under the top 20).
- The disk cache's first weekly pass ran: 3,160 files, none unused for 60 days (the cache is weeks old),
  9.6 s of work spread over idle time.
- Battles: JS at the median 22-34 ms depending on what is on screen, as in logs 171 and 174 (by draw
  calls a frame: 13-18 calls 21-27 ms, 19-22 calls 27-34 ms, 23 or more 31-38 ms). The Abyss sessions
  (181, 182) were the slowest at 19-22 calls (32-34 ms against 27 in log171's Graveyard and Metropolis);
  no sign of a change from 1.0.3-1.0.4's code (log178, also 1.0.4: 22 ms at 13-18 calls).
- The profiles put 6-8.5% of battle samples in FieldSpritePipeline.onBind, 2% in Arena.isOutside and
  2-3% in Array.map, though the "arena uniforms once per frame" and "arena tints per frame" rules
  applied (the [gamefix] line), so those run once a frame. They are the first JS after a pipeline
  flush (a GL draw in C): the sampler takes its sample at the next interrupt check, so native time
  just before tends to land on them. Read per-function shares near native work with care.
- Logs 182-184 stop without a last line (182 in a battle, 183 and 184 at the title). Quitting with
  START + SELECT wrote nothing, so such an end couldn't be told from closing the app in LiveArea or
  a crash. 1.0.5 logs "[app] quit (START + SELECT)", and the cache pass also logs the size it keeps.

### PokeRogue's sprite pipelines (desktop, Temple battle, game of 2026-11-01; runtime 1.0.6)
Timed directly (wall clock; the sampler puts native time before a function on that function):
render ~1.1 ms a frame on the desktop. The Pokémon pipeline's onBind is 9 us a sprite in a loop
(5 of them its ~13 uniform calls, 4 the game's JS; the field part via super.onBind 0.5 us);
hiding the three Pokémon sprites (front, back, substitute) saves 0.15-0.2 ms a frame, 50-70 us a
sprite with its pipeline switches, onBatch, batchQuad and its own draw. Those are spread out; no
one piece is large, and each sprite needs its own uniforms and draw.
The field pipeline drew each arena sprite separately: FieldSpritePipeline.onBatch flushes after
every sprite. In a Temple battle that is 5 draws a frame (backdrop, four ground layers), and the
four layers have the same uniforms (terrainColorRatio 1). Rule "arena sprites drawn together"
(src/gamefix.c): the field pipeline leaves its sprites batched, and a bind that sets other values
flushes first (added to "arena uniforms once per frame"); Phaser flushes on a pipeline switch and
at the frame's end. Not for the Pokémon pipeline, which extends the field pipeline and calls the
same onBatch. Temple: 5 -> 2 field draws a frame, screenshots identical (animations paused,
batching switched off and on by the pipeline's name), render 1.17 -> 1.13 ms on the desktop.
tests/smoke checks that each sprite is drawn with the uniforms it was bound with, in fewer draws,
and that the Pokémon pipeline keeps its draw per sprite.

### Later waves (log187, 1.0.6, 2026-11-02)
Online, two runs into later waves (Abyss, then Cave), profiler off. JS at the median by draw calls a
frame: Abyss 13-18 calls 30 ms, 19-24 36.7, 25-32 41.9, 33 or more 45.3 (10 windows at 38-44 calls,
19-20 fps); Cave 13-18 calls 23.1 ms, 19-24 31.7. GPU 14.5-24 ms. The hitch between waves with the
loading screen was the collection after a wave save: 2.2 s, JS heap 84.7 -> 74.8 MB (the other one,
in the Abyss, 1.5 s for 2 MB). The heap grows with the run (JS after collections up to 75 MB, heap in
use up to 183 MB), so collections take longer late in a run; placing them after saves keeps them out
of battles. Each run's end took one ~6 s frame (back to the title: the game rebuilds its screens).
The Abyss is ~7 ms slower than the Cave at the same draw calls, but on the desktop an Abyss battle
(the saved Temple run with its arena set to the Abyss) draws the same objects as the Temple (backdrop,
four ground layers, the Pokémon) at the same cost (render 1.2 ms against 1.1): the difference is in
the run (later waves), not the arena's drawing. Needs a profiled log to say more.

### From the pokerogue-nx fork (reviewed 2026-11-03; runtime 1.0.7)
silvershadowkat/pokerogue-nx merged our runtime up to 1.0.6 and ports it to the Switch and Android
(SDL, touch controls, mbedTLS, storage that survives the Switch's rename-can't-overwrite). Taken over:
- Storage writes that fail are tried again: the writer thread retries a failed write a second later,
  up to 5 times (unless a newer copy of the file is queued), and storage.js keeps a file it couldn't
  hand over dirty for the next flush. Before, the change waited for the game's next write to it.
- net.c: a cached 404's status line was added with length 28 for 26 bytes (2 bytes read past the
  literal); a cache entry whose rename fails no longer leaves its .tmp.
- Where the game is, in logs (the fork logs every phase change; this is lower volume): a "[game] wave
  N: <arena>, wild|trainer[, double] | party P, items I" line when the wave changes, and "wave N" on
  every [perf] line (js/bootstrap.js, N.setPerfTag; only when there is a log).
Kept in mind (docs/PLAN.md): their finding that a fractional corrected Date.now() makes the save API
reject saves (unconfirmed on the Vita: our correction only applies past 30 s of clock error), and their
D-pad navigation for the login and register screens.

### The item transfer menu (log189, 1.0.7, 2026-11-03)
Late in a run (waves 114-119, Cave, 6 Pokémon, 39-45 held items) every up or down press in the party
screen's "move items" list was a ~400 ms frame (up to 730 ms when the list scrolled). The profile:
PartyUiHandler.processModifierTransferModeUpDownInput calls updateOptions on every press, which
destroys and makes again every option's BBCodeText (TextStyle.setStyle, setText's tag parsing) and
looks the held items up again, then moves the cursor. The rebuild exists to put the quantity of the
item being left back to its maximum, so it only changes anything when that quantity was lowered with
left/right. gamefix rule "item list cursor without a rebuild" now rebuilds only then; scrolling still
rebuilds (setOptionsCursor does that itself), and the window otherwise is the one already shown.
Smoke check: the same quantities, cursor and window as the game's code over a sequence of presses and
quantity changes, with 6 rebuilds instead of 12. On the desktop (offline game, Abyss save, transfer
screen) presses now rebuild nothing; the save has no held items, so the speed-up itself needs a log.
Also, covered objects (fastpath.js): the children skipped under a screen-covering image were walked
whole every frame to fill the camera's render list (listSkipped, 4.7% of the party screen's frames).
Only the input plugin reads that list (sortGameObjects orders the objects under a pointer by it), so
now only the skipped interactive objects (the input plugin's list) go in, in drawing order; the
phaser test checks their order against what is drawn.

### A slow network: the egg gacha, log sends and wave starts (log194-196, 1.0.8, 2026-11-03)
Played on busy 2.4 GHz hotel Wi-Fi. What the user saw, and what the log shows:
- The egg gacha "hung" after a ticket: the pull waits for the game's system save
  (`savedata/system/update`, the whole system data), and that request stayed pending ~50 s (t=190 to
  241 s). The frames were fine (38 ms max); the game itself waits, as it does in a browser. The log
  uploads in the same minutes ran at ~10 KB/s (51 KB in ~5 s), so the save's upload is the likely time.
- Slow wave starts were downloads the game waits for: a special battle's music has to load before
  the wave starts (battle_legendary_ub.mp3, 2 MB, before wave 141; the Delibird-y encounter's
  2.5 MB before wave 144). sea.mp3 (2.9 MB) took ~80 s, about 37 KB/s. 8.6 MB came over the network
  in waves 136-145.
- After a log send the game felt unresponsive. The result was a dialog, which takes the controls
  until closed, and it came only when the upload finished (5 s here). The release check after a
  dialog closes also counted the rear touchpad's upper half (L2/R2), so fingers resting there could
  keep the game's controls muted. And START of L + R + START reached the game.
Changes (1.0.9): the log send shows a line over the game ("Sending the log...", then the result for
4 s) and leaves the controls to the game (the benchmark's end keeps its dialog); with L + R held,
START and TRIANGLE (the combos' buttons) no longer reach the game; the rear touchpad doesn't hold a
dialog's input mute. And `[net] slow:` lines for any request over 5 s: the method, the URL without
its query, upload size and speed, time to the first byte, download speed, whether the connection
was new (connect and TLS times) or reused, and the HTTP version, so the next log says where such
waits go (the upload, the server, or the download).

### The item list's rebuild: rex BBCodeText drawn when rendered (log200, 1.0.9; runtime 1.0.10, 2026-11-03)
1.0.8 stopped the rebuild on up/down within the item transfer list (log200: no more
processModifierTransferModeUpDownInput stacks), but scrolling still rebuilds it, ~385 ms a press on the
Vita (log200, waves 147-149, ~50 items). The rebuild makes ~10 option labels anew, rex BBCodeTexts
(phaser3-rex-plugins 1.80.20), and each new one is changed ~5 times before the game reads its width
(constructor, setShadow, setLineSpacing, setText twice); every change parsed the tags, laid out the
lines, drew the canvas (96 px fonts, scaled down) and uploaded it. js/fastpath.js 4b does for them what
4 does for Phaser's Text: a change only marks the text; it is laid out and measured with rex's own code
(drawing off) when its size or lines are first read, and drawn and uploaded once, when rendered; a
destroyed label's texture goes to a new one with the same text and style (textCache), as when the list
scrolls by one. The class comes from window.RexPlugins.GameObjects.BBCodeText when the game registers
the plugin; the game's code is precompiled without source, so its shape is checked instead (methods,
updateText's one argument, the text's canvasText on first use), and anything else leaves rex alone.
Checks: tests/phaser (rex 1.80.20 from npm, built as the game builds its options: same sizes, lines
and canvas pixels as rex's own code, one draw per text, textures reused); scripts/probe_rex_text.js
in the running game (ten labels in the game's own style at its size: same sizes, 0 of 2943 changed
pixels differ, also with kept textures). Desktop, build plus the frame that draws them: 22-32 ms with
rex's own drawing, 13-19 ms now; per label one measure instead of ~5 layouts, one draw instead of ~5.
The Vita's number needs a log with the item list.
Measured again on the desktop with a real late run (the owner's save, wave 163, 43 held items; kept in
runtime/.cache/saves/, not committed): scrolling the item transfer list (Mega Darkrai's items), the
game's updateOptions took 24.2 ms a rebuild with rex's own drawing and 9.3 ms with 4b (the labels'
drawing moves to the next frame, where scrolling mostly reuses their textures).

### Battle retries and the system save (log203, 1.0.10; runtime 1.0.11, 2026-11-03)
The owner often retries battles (back to the title, Continue): each one was a 5-6.4 s frame, then
1.1 and 2 s frames (log203: ~80 s over 8 resets), and the system save's parse took 650-900 ms at each
load (6.7 s in log203). Two changes, and a better report for such frames:
- The slow-frame report keeps 4096 samples (1024 cut a 6.4 s frame at ~3 s) and adds a line "by
  function": the time each function was on the stack, under 85% of the frame. Frames that build a
  screen spread over many stacks and showed nothing before.
- With it, on the desktop with fixtures/run-wave163.json: the reset frame was 300 ms, 187 of them
  UI.setup (every screen handler's setup again). js/lazy_ui.js now also defers the five settings
  screens (96 ms of it), achievements, game stats, the egg gacha, challenges and the Pokedex scan
  (~135 ms, ~45% of the frame). A reset destroys every handler (UI.freeUIData) and
  GameChallengesUiHandler's destroy() reads what its setup() made: a never-built handler is now
  destroyed without its own destroy() (UiHandler's is empty). After the change the reset frame is
  under the 250 ms report threshold on the desktop; with lazy UI off it is 915 ms. Settings,
  achievements, stats and the egg gacha look the same either way (screenshots: only the play time,
  the gacha's clock and the title's animation differ). tests/phaser checks the deferral.
- gamefix "system save parsed with fewer calls": parseSystemData's reviver ran for ~21,000 members;
  it now runs only for gameStats, eggs and ribbons, and the *Attr members get the reviver's
  BigInt(value ?? 0) in C (src/js_host.c parseRevived; the rule matches the reviver's exact text).
  Desktop, a 218 KB save: 11.7 ms with the reviver, 6.5 ms now (plain JSON.parse 3.1). A JS walk
  instead was slower than the engine's reviver (16.5 ms). Smoke test: the same data (classes, BigInts,
  a member revived to undefined deleted) and the same error for a fractional *Attr.
The Vita's numbers need a log with a retry.

### Out of memory at wave 200 after a long session (log205-209, 1.0.10/1.0.11; runtime 1.0.12, 2026-11-04)
One app session played waves 163-199 with many battle retries (log203-207, ~2 h). Late in it the
256 MB heap was full five times (an emergency collection of ~3 s each, 191 -> 231 MB still in use
after it; JS heap after collections 61 MB at wave 164, 115 MB at 200). At wave 200 a stat change's
TileSprite asked for its canvas copy (Phaser's CanvasTexture reads its canvas back when made:
936x1896, 7 MB, for the boss's sprite size) and got "out of memory" inside a battle phase's start():
the phase never ended and the battle stopped, while frames went on (the "hang"). After restarting the
app, wave 200 went through: 141 MB in use, JS 47 MB (log209).
Causes found on the desktop with fixtures/run-wave199.json (reset + Continue, as a retry does):
- BattleScene.reset ends with this.children.removeAll(true): on a display list the argument skips
  the removal callbacks; nothing is destroyed. Each retry left the old battle's top-level objects
  alive (trays, arenas, bars, candy bar, time-of-day widget: their sprites still in the update list,
  their texts' textures kept): +35 updated objects, +43-70 textures, ~1.3 MB of JS heap per retry.
  gamefix "a reset destroys the old battle" destroys them first (launchBattle makes them all anew),
  and afterwards drops tweens left on destroyed objects (stopped kept ones and endless ones; one still
  running to its end is left). Six retries: textures flat at 690 (were +43-70 each), JS heap +0.25 MB
  each (was ~1.3), tweens +5 each (was +8: the rest are counters and target-less tweens); the battle
  plays on after them.
- js/fastpath.js 6c: a canvas texture copies its canvas only when its pixel copy is read. On this
  runtime the copy was all transparent anyway (Phaser uploads the texture first, and src/canvas2d.c
  frees an uploaded canvas's pixels), so it was up to 7 MB of zeros per stat change. tests/phaser
  compares with Phaser's own code both ways (blank and drawn canvases, update()).
log209 (1.0.11, the wave-200 battle from a fresh start): 36 fps, JS 30 ms, 14 draw calls. Every turn:
the enemy's move choice (getNextMove: stats and abilities per candidate move) 250-350 ms, the
turn-end phase (held items, an item transfer) 300-380 ms, and the item bar rebuilt from scratch on
each item change (ModifierBar.updateModifiers: all ~46 icons with their sprites, counts and pointer
handlers made again) ~220 of 260 ms.

### A regression of 1.0.12 and stacks for long frames (log212, 1.0.12; runtime 1.0.13, 2026-11-04)
log212 (sampler off): twice "Supplied items must be elements of the same array" at a new run's first
encounter. TimeOfDayWidget listens to the game's onEncounterPhase event and never stops; 1.0.12's
reset rule destroys old battles' widgets, whose listeners then tweened destroyed icons when the time
of day changed (the event target's dispatch caught the error; the run went on). gamefix "time-of-day
widget unsubscribes when destroyed" gives it a preDestroy that removes the listener (then
Container.preDestroy, as before): across three retries the game's event target keeps one listener per
event (it gained one per retry). Before 1.0.12 the old widgets were alive and animated hidden icons.
The other listeners on that target belong to ArenaFlyout and BattleFlyout, which remove theirs.
Also in log212, not found on the desktop: a 21 s frame in "promise jobs after frame" right after wave
10, with frames at 55-65 ms of JS afterwards (20-25 before); 0.7-1 s frames in the starter screen's
menus (some exactly ~1.03 s, several in a row); texture re-uploads of 30-115 ms there. The desktop
gives the starter menus 13-20 ms frames and wave 10 -> 11 no long frame. Since logs usually have the
sampler off, a frame (or promise jobs) still running after 0.5, 5 and 20 s now logs one JS stack each
("[stuck] ... s into <phase>: <stack>"), at no cost otherwise.
The 21 s after wave 10 in log212 was a big egg hatch (the owner, "skip" chosen). Desktop, 40 eggs
added to a test save (.cache/session-eggs.json): the hatch is one 564 ms run of promise jobs between
frames (the slow-frame report only covers frame JS, so it showed nothing); ~37x on the Vita matches
21 s. Method timers: Egg.generatePlayerPokemon 476 ms for 40, of which PlayerPokemon.initBattleInfo
425 ms (89%): each hatched Pokemon builds a full battle info box (~40 objects: name, level, HP bar,
stat panel; 2.9-4.5 ms each on the desktop) that the hatch and summary screens never show; it is
destroyed with the Pokemon after the summary. The slower frames after the hatch were the egg summary
screen (sprites loaded as the cursor moves); after leaving it the desktop is back to its frame times
and object counts.

### Egg hatches and the item bar (runtime 1.0.14, 2026-11-04)
- Hatches: gamefix "hatched Pokemon build their battle info on use" (two rules: the egg's
  generatePlayerPokemon runs under js/core.js __pkrHatch; Pokemon.init asks __pkrInfoInit, which,
  while hatching, makes battleInfo an accessor that builds it on first use exactly as init does, and
  makes destroy() skip a never-built one). Desktop, 40 eggs with skip: 40 boxes deferred, none built;
  the hatch's longest stretch between frames 81 ms (was 564 ms). The hatched Pokemon are the same with
  the change on and off (species, form, IVs, nature, ability, shininess, variant, gender, tera type,
  luck; 12 eggs under the hatch's own seed), and the summary screen shows them as before.
- Item bar: gamefix "item bar icons kept" (one rule over ModifierBar.updateModifiers): an icon is kept
  for its item while what getIcon() draws from is unchanged (the item's plain fields and stack counts,
  the holder's icon atlas/frame/fusion and texture, treasure count); a kept icon is shown again and
  loses the two pointer listeners the method adds again; one under a pointer is made anew (a new
  one there would get a pointerover). Desktop, wave-199 fixture (31 icons): 10.3 -> 2.4 ms per rebuild;
  screenshots of the bar identical to the game's own for the same items, a changed count and a
  removed item.
- js/fastpath.js 21: the input plugin's object list (1,936 interactive objects in that battle) is
  checked with sets when an object is made interactive or removed, instead of indexOf over it: ~6%
  of the item bar's rebuild, and every other screen that makes interactive objects.

### The egg hatch on the Vita with 1.0.14 (log215, 2026-11-04)
log215 (sampler off, waves 16-26, ~14 eggs hatched with skip between waves 25 and 26; no errors):
- The hatch: no stretch of frame JS or promise jobs reached 0.5 s (none logged a `[stuck]` stack);
  the window holding it has a 370 ms longest frame. log212's bigger hatch was one 21.2 s stretch.
- The egg summary screen after it: frames at 26-31 ms of JS at the median (log212: 53-55 ms), and
  no long frame when leaving it (log212: 2.1 s). Both are the battle info boxes that are no longer
  built: they sat hidden in the field UI during the summary and were destroyed after it.
  The summary's own texture re-uploads (the 249x747 and 492x404 atlases, ~35 ms each) are the same.
- The item bar: with 7-11 items early in a run there is nothing to see (sampler off); a late run
  is needed for its Vita number.
- What remains slow in battles: the enemy's move choice in double battles, 1.1 s frames twice
  (`[stuck]` stacks: getNextMove -> getAttackDamage -> getBaseDamage -> calculateBattlePower and the
  ability hooks it applies, for every move against every target), the speed sort at a turn's start
  (0.7 s: getEffectiveStat -> applyAbAttrs), the save at each wave (0.5-0.8 s, JSON.stringify), and
  one 1.6 s collection after a save.

### The enemy's move choice (runtime 1.0.15, 2026-11-04)
log215's 1.1 s frames in double battles were EnemyPokemon.getNextMove (wild and trainer AI): it
checks every usable move against every opponent for a KO, then scores every move and target, each
with the game's damage formula, simulated. Desktop, offline game 1.12.1.1, a double battle made from
fixtures/run-wave163.json (a second wild Pokemon added; `.cache/session-double.json`): ~31-36 ms for
the two enemies' choices per turn (~1.1-1.3 s at the Vita's ~35x), 5 ms in a single battle. Profile:
35% canApplyAbility (~1,150 calls per choice, applyAbAttrs asks it for every Pokemon's ability and
passive at every hook), 22% inSpeedOrder (a priority queue that shuffles and sorts again at every pop,
sortBySpeed's comparator computing both Pokemon's speed with all stat and ability hooks at every
comparison), getAbility 13% of its own.
- gamefix "enemy move choice remembers what it asks again": getNextMove runs under js/core.js
  __pkrAIChoice, which swaps memoizing versions of canApplyAbility, getAbility, getPassiveAbility and
  hasPassive onto Pokemon's prototype for the choice (answers per Pokemon and flag) and puts the
  originals back after (also when it throws), so the rest of the game pays nothing.
- gamefix "enemy move choice keeps the speed order": within a choice sortInSpeedOrder returns a copy
  of the order it gave for the same list (its shuffle is seeded from turn and wave and restores the
  random state).
- gamefix "speed order sorts with each speed once": sortBySpeed computes each speed once per sort
  (once per choice inside one) and sorts with the same comparisons; also at each turn's start.
Desktop: the double battle's two choices 36 -> 16 ms per turn (-56%), single battle 4.2 -> 3.2 ms.
Same moves, targets and the game's printed move scores both ways for every choice compared (60 in
the double battle, 30 in the wave-199 single battle, each pair from the same battle random state).

### A shifted clock breaks online saves; the AI on the Vita (log219, log220; runtime 1.0.16, 2026-11-05)
- log220 (1.0.15, the owner moved the console's clock ~2 minutes back to test the pokerogue-nx
  fork's finding): "[clock] the console's clock is off by -0.03 hours", then at wave 32's save the
  server answered "failed to decode request body: json: cannot unmarshal number 1793852076922.301
  into Go struct field CombinedSaveData.system.timestamp of type int", and the game went back to
  the title. The fork was right: net.c's offset had a fraction (microseconds from gettimeofday), so
  Date.now() did too. 1.0.16 rounds the offset to whole milliseconds (the fork truncated in nowMs).
  Only consoles whose clock is more than 30 s off were affected.
- The enemy's move choice (1.0.15) on the Vita, log219 waves 29-30 (double battles): no `[stuck]`
  stack in getNextMove and no frame over 0.5 s in battle (log215, 1.0.14: seven, up to 1.15 s, in
  double battles at waves 16-22).
- log219's first start of 1.0.15 had one 28.4 s frame at startup: the game's code compiled again
  for the new gamefix set (once per update; log220's start: 2.3 s for that part).

### A freeze after an hour: memory, and one error that stopped the game (log225; runtime 1.0.17, 2026-11-05)
log225 (1.0.15, ~64 minutes): a run to wave 48 (game over), ~16 minutes in the starter select screen,
then a new run that froze at wave 31: the picture stopped, the music played on. At 226 of 256 MB of
heap in use, "[canvas] out of memory for a 1224x357 canvas"; that canvas's native calls answered
undefined, canvas2d.js destructured one (getTransform in fillText's shadow), the TypeError went up
through Phaser's render, and Phaser's step, which asks for its next frame at its end, never asked
again: 60 empty frames a second from then on. No collection ran before the failure: the heap-low one
waits for 236 MB, and 30 MB free wasn't enough for 1.7 MB (garbage, fragmentation).
1.0.17:
- A canvas or image whose pixels can't be allocated makes the runtime collect garbage right there
  (main.c host_gc_for_alloc, logged "[gc] collected when ... found no memory") and try once more.
- A canvas without pixels answers its transform and text metrics with defaults (identity, zeros) in
  canvas2d.js, so drawing on it does nothing instead of throwing.
- A requestAnimationFrame callback that throws without asking for its next frame is called again
  next frame under the same id (up to 300 frames in a row); the same error is logged 5 times, then
  every 600th. One failing frame no longer stops the game.
Memory over that session (JS left after each collection): 47 MB at wave 32, 59, then 69 -> 81 MB in
the starter screen, 92 MB by the next run's wave 13; heap in use 119 -> 227 MB; Phaser textures
516 -> 1017 (+350 in the starter screen). Desktop (wave-163 save): loading the sprites of 81 species
(what the starter screen does for each species the cursor visits) keeps 21.7 MB more JS after a
collection (0.27 MB, ~1,240 objects and ~8,500 properties per species: the atlas's ~88 Phaser Frame
objects, its animation, and a copy of the atlas JSON in customData, ~18% of it, which neither Phaser's
WebGL path nor the game reads). Nothing unloads them, in the browser either; on the Vita a long visit
to the starter screen adds tens of MB for the rest of the session.

### Why log225 ran out when longer sessions didn't; the starter screen (runtime 1.0.18, 2026-11-06)
Memory, earlier sessions (JS left after a collection / most heap in use): log203 40 min, one run, 80 /
214 MB; log207 128 min, one run, 84 / 235 MB (out of memory at wave 200, fixed in 1.0.12); log212,
log216 under 70 / 175 MB. log225: 92 / 227 MB, the first session with a game over, 16 minutes in the
starter select screen and a second run. Every gamefix rule still applied to the game's new build
(battle-scene-DzzynR15, loading-scene-CKnNUEJ8), so nothing of ours stopped working; what that
session did differently was load ~350 more species sprites (kept for the session, see above) and
spend 16 minutes in a screen that accumulates listeners.
The starter screen in log225 (81 `[stuck]` stacks) and on the desktop (wave-163 save, the screen
opened over the battle with ui.setMode):
- Idle, it costs 45-75 ms of JS a frame on the Vita (10-20 fps) and 2.4 ms on the desktop, nearly all
  of it drawing (Phaser's container walk, the sprite pipelines, texts): the general render cost.
- A cursor move: 2.6 ms of JS on the desktop (setStarter, setStarterDetails, the instructions), plus
  loading the species' sprite. Phaser 3.90's Container.removeHandler takes the child's DESTROY
  listener off this.remove where addHandler put this.onChildDestroyed, so every child taken out and
  added back keeps one more; the screen's instructions do that to their icons twice per move (+1.5
  listeners per move each, 82 after 40 moves), and each removal scans and copies the whole list. With
  1,500 stale listeners a move's instructions cost 0.28 instead of 0.06 ms on the desktop. Fixed in
  js/fastpath.js 7b (the listener addHandler made is taken off, as later Phaser does; tests/phaser).
- The game's own leak, same screen: StarterSummary takes the old pointerover off its ability and
  passive texts but not the pointerout, and its candy container (and the Pokedex page's) adds both
  and removes neither: +1 closure per move each. gamefix "starter tooltips replaced, not added"
  (three rules; tests/smoke, the tooltips shown the same both ways).
  After 40 moves with both fixes, no object in the screen has more than 3 listeners.
- A filter change: 19 ms on the desktop (~0.7 s on the Vita): filterStarters 13 ms (every filter's
  values worked out for each of the ~600 starters, whatever is selected) and updateScroll 5 ms (each
  of up to 81 slots destroys its icon sprite and makes a new one). Not changed yet.
Also: src/gamefix.c's "[gamefix]" line was cut at 400 characters (the battle scene chunk's 19 rule
names); it now has room for all.

### Pokemon sprites unloaded when unused (runtime 1.0.19, 2026-11-06)
js/sprite_gc.js, like a garbage collector for the species atlases (see log225 above: ~0.27 MB of JS
each on the desktop, kept for the session). A pass unloads "pkmn__" atlas textures and their
animations that no object uses and that loaded over a minute ago. "In use": any object on a scene's
display or update list (into containers), every live sprite through its animation state
(js/fastpath.js 9 keeps them all, on a list or not), objects the battle scene and UI handlers hold
directly, what each plays or will play, and every sprite key of a Pokemon in either party (the
fusion palette reads front and back atlases by name). A pass runs at a new biome or a new run (the
old route's Pokemon won't come back soon), and under memory pressure: JS left by the last collection
over 27% of the heap (69 MB on the Vita), at most once a minute and only after 20 more sprites were
loaded. Never while a loader is busy. The game loads every Pokemon sprite through loadPokemonAtlas
(Phaser's loader skips existing keys) and makes its animation when missing, so an unloaded sprite
loads again from the disk cache when shown. Animations leave the manager's map directly:
AnimationManager.remove would call every sprite's animation state for each one, and none plays them.
Desktop (wave-163 save, 81 species loaded as the starter screen would): one pass unloaded 88 of 96
atlases in 14 ms, 130.3 -> 115.3 MB of JS after a collection (21.7 MB had come with the 81); the
battle drew the same; the starter screen reloaded and showed an unloaded species; a double-battle
turn played after a pass; with the party's keys pinned, 14 of 15 kept in that battle.
Switch: __rt.spriteGc = false. Log: "[sprites] <reason>: unloaded N of M Pokemon sprites ...".

### The start screen waiting on background work (runtime 1.0.20, 2026-11-06)
The owner: since some time after 1.0 the start screen ("Play online / Play offline") ignores input
for ~30 s after launch, with nothing on screen saying why. In the 40 most recent logs the choice
always comes after the previous log's upload finished (test builds send it at start: 350 KB by
now, with the [stuck] and [perf] lines of an hour, over busy hotel Wi-Fi), never before. The upload
thread and the offline release check (a TLS request to GitHub) were the only helper threads that
didn't lower their priority (plat_thread_setup): the Vita's kernel doesn't time-slice threads of
equal priority, so a busy one can keep the start screen's thread from running.
1.0.20: both run below the main thread on core 1, like the network, cache and decoder threads; the
start screen says what runs in the background in place of its help line ("Sending the last
session's log to the log server...", "Checking for a new offline game release..."); and the log now
has the evidence that was missing: "[start] chosen after N s on the start screen", a line for any
stretch over 0.5 s the screen didn't run, and each job's duration ("prev log upload: ... (N s)",
"latest release on GitHub: ... (N s)").

### Sprite unloading on the Vita (log229, 1.0.19; runtime 1.0.21, 2026-11-06)
log229 (waves 31-41, ~18 minutes, Continue then a return to the title): three passes, 85 ms at the
start (Continue: 0 of 16, 15 in use), 142 ms at wave 41's new biome (graveyard to abyss: 15 of 32
unloaded, 17 in use), 65 ms at the title (14 of 17: the party isn't shown there; Continue loads
it again). No JS errors, no missing sprites; JS left by a collection 47-60 MB. The game makes a new
Arena for each biome as well as each run, so the wave-41 pass was logged as "new run"; 1.0.21 names
it by what changed ("new biome", "new run", "title screen").

### Starter screen filter changes (runtime 1.0.22, 2026-11-06)
Desktop, wave-163 save with every species marked caught (572 starters shown, as in a long-played
save), the starter select screen opened with ui.setMode: one filter change (updateStarters) took
30.7 ms (about 1 s on the Vita): updateScroll 18.4 ms, of it setSpecies 12.8 ms (each of the 81 slots
destroys its icon sprite and makes a new one); filterStarters 11.1 ms; the rest ~1 ms. log225's Vita
stacks inside updateStarters: updateScroll 14 of 28, tryUpdateValue 12, filterStarters 1 (stacks are
taken 0.5 s into a frame, so the later steps are over-represented).
- gamefix "starter icons kept, not remade" + js/core.js __pkrStarterIcon: the slot's icon is reset to
  what the game's new sprite would be (position, scale, origin, alpha, visible, angle, flip, depth,
  texture, frame, tint), its tweens stopped, dropped from the screen's icon animation helper; it stays
  in the slot when only the badges brought to the top next follow it (the rule's text names them).
  Every slot's child order and icon state compared equal both ways after refreshes (81 slots, kept
  and remade in both orders); tests/phaser checks the same on a copy of the code with real sprites.
  updateStarters 31.9 -> 20.4 ms (-36%).
- Tried and dropped: answering the type filter once per pair of types (isOfType reads only type1 and
  type2): no change, the filter's ~10 ms are spread over many small checks per starter (the
  Unlocks/Misc options' `some`, getStarterData, getStarter, the passive, cost and egg checks).
- Found on the way: a dynamic import() from code resumed after an await in a frame callback (tests'
  scene.js) or from a control-channel eval fails with "cannot read property 'length' of undefined"
  before loading anything. The game's own lazy imports work; not looked into further.

### The starter screen on the Vita after 1.0.22; texts set to what they show (runtime 1.0.23, 2026-11-06)
log232 (1.0.22, the starter screen after the title): filter changes 0.88-0.90 s on the Vita (log225:
1.05-1.08 s), so -15% there for the -36% on the desktop; the screen idles at 42-70 ms of JS a frame.
The start screen: "[start] chosen after 7.4 s" with no stall line, but that start sent a 1 KB log
(the previous run was the updater's), so the slow case is still to be seen.
Several of log232's long frames were in drawing texts at render (textRender -> drawText ->
updateText): each refresh sets every slot's cost label to its text and colours again (setText,
setColor, setShadowColor), Phaser marks a text changed on every setColor even to the same colour, and
js/fastpath.js 4 drew each again at the next render, canvas and GPU upload (texts are drawn at 6x).
On the desktop one filter change gave 170 text updates and 88 draws. Now a text whose inputs (the
signature 4 already kept for reusing destroyed texts' textures: words, font, colours, shadow,
padding, spacing...) are those its texture was last drawn with isn't drawn again: 0 draws for the
same change. A text measured in another state since its last draw (its sizes are that state's now)
is drawn as before. tests/phaser checks: unchanged, not drawn; another colour, drawn once; measured as
other words and set back, drawn once with Phaser's size. Switch: __rt.textSkipSame = false.

### The start screen ignoring input; a log toggle for players (runtime 1.1.1, 2026-11-07)
The start screen still took no input for a while after launch in log235 (47.5 s) and log240 (27 s,
1.1), though the new lines showed it running all along (no "didn't run" stall) and its background
jobs done in 0.6-1.9 s. Its input guard waited for a frame with every button released before any
press counted (against cross held from launching the app), and buttons 6 and 7 are the rear
touchpad's upper half (L2 / R2): fingers resting there kept it waiting until they moved, the same
thing 1.0.9 fixed for the dialogs. Now the first reading only records what is held and later presses
count (a button held from launching still doesn't, until pressed again), and the log says when the
first press came ("[start] first button press after N s").
Player builds' start screen gets a "Write a log (log.txt)" toggle beside the 30 fps lock: it sets
config.txt log=1 (the README's way to write a log for a bug report), and turned on there it starts
log.txt at once (main.c host_log_open, as at startup), so that run is logged too.

### Memory over returns to the title: unloaded animations kept alive (log243; runtime 1.1.2, 2026-11-08)
log243 (1.1, waves 71-103 in 48 minutes, four returns to the title): JS left by a collection 47 ->
60 -> 67 -> 74 -> 77 MB, heap in use up to 218 of 256 MB. Desktop, wave-163 save, six resets with
Continue (no waves between): +0.5 MB of JS each, +1,660 objects, +600 strings, +5 tweens and +26
listeners on Phaser's AnimationManager. Those listeners were the leak: every Phaser Animation listens
to the manager's PAUSE_ALL and RESUME_ALL from its constructor, and js/sprite_gc.js (1.0.19) took an
unloaded animation out of the manager's map without them. The manager then kept each one alive with
its ~88 frames, and through them the destroyed texture's frames: at every new biome and return to
the title, the sprites "unloaded" stayed in memory (83 such listeners after a few resets). Unloading
now removes both listeners and the animation's frames, as Phaser's Animation.destroy does (still
without the REMOVE_ANIMATION event, which would call every sprite's animation state for each one).
tests/phaser checks that an unloaded animation leaves no listener. Still left per reset: 5 endless
tweens of the game's PokemonIconAnimHelper (the screens are rebuilt with new helpers; small).

### What else grows over waves; cries kept compressed (log246; runtime 1.1.3, 2026-11-09)
log246 (1.1.2, waves 103-121, 30 minutes): the start screen took its first press after 2.5 s (the
1.1.1 fix). Heap in use peaked at 181 MB (log243, 1.1: 218); unloaded sprites are freed now (17-20
per biome, a "memory" pass at 71 MB), but JS left by a collection still rose 47 -> 69 MB, and the
decoded audio 13 -> 27 MB (C memory, inside the same 256 MB).
Desktop, wave-163 save, the bot for 14 waves with a collection and counts after each wave: JS
101.9 -> 123.9 MB (+9 MB in the first wave, then ~1 MB a wave; the biome change at 171 gave 2.3 MB
back), arrays 96k -> 155k, objects 330k -> 427k, move animation textures ("PRAS-") 58 -> 104,
Phaser's audio cache 145 -> 294 entries, decoded audio 8.6 -> 25.1 MB. By kind, decoded: move
animation sounds 11.6 MB (220, mostly WAV: "PRSFX- ..."), cries 8.2 MB (33, M4A), sound effects 5.2 MB
(37, WAV). All 86 move animation textures came to 3.1 MB of JS (removing them in an experiment):
the rest of the JS growth is the move animations' data the game parses once per move and keeps
(AnimConfig frames), which only a change to the game could drop. The move textures could only go at
the title screen and would need the game's list of common animations (loaded once at startup), so
they stay.
1.1.3: M4A buffers (every cry, some move sounds) keep their file (~1/8 of their samples), and their
decoded samples are kept within 6 MB (config.txt audiokeep=<MB>): past it, the least recently played
that aren't playing are dropped; one played again is decoded on the decode thread and its voice
starts once the samples are back (tens of ms late; the game's frame doesn't wait). WAVs stay decoded
(their files are as big). Same 14 waves: decoded audio grew 9.6 MB (was 16.5), M4A held at 5.7 MB
(18 dropped, 1 decoded again). With audiokeep=0 (every M4A dropped after each play): 53 dropped, 27
decoded again in 5 waves, no voice left waiting, no errors. The [mem] audio line reports both.

### Damaged files on the memory card: saves, cached code, the offline download (runtime 1.1.4, 2026-11-12)
From a code review (no device log): five places where a failed write or a damaged file lost data
or kept the app from starting. Desktop runtime, each case made by hand:
- `config_set` (the start screen's toggles, the fps lock) removed config.txt before checking that
  its new copy was written: a full card left none. Now the old file goes only when the new one is
  whole (temp file on /dev/full: config.txt unchanged, a `[config]` log line).
- localStorage and cookies (`js/storage.js`): a file that didn't parse loaded as empty, and the next
  save replaced it. The offline game's saves exist nowhere else. Now every write keeps the file it
  replaces as `<file>.bak` (a rename, on the writer thread); a file that is damaged, empty or
  unreadable is moved to `<file>.bad`, the backup is loaded, and a dialog says so. The backup is
  also what is left if the app dies between a write's two renames. tests/smoke checks it across its
  two runs (the first leaves a damaged file, the second must start from the backup).
- Compiled code (`<data>/bytecode/`, now `.qbc4`): QuickJS trusts bytecode as it reads it, so a
  damaged file could crash every start. Each file ends with a checksum (Adler-32, miniz), checked
  where it is read (the prefetch thread, mostly); a mismatch deletes the file and the module is
  compiled again. All 13 files of the smoke page damaged or emptied: 13 "is damaged" lines, the
  page's 39 checks pass. Every module is compiled once more after this update (the new extension).
- HTTP disk cache: entries start with a 16-byte header (magic, length, Adler-32); a mismatch
  deletes the entry and it is downloaded again. Entries from before have no header and are served
  as they are (no re-download of the cache). And source from the cache that doesn't compile is
  dropped and fetched once more in the same start (`net_cache_forget`), which also covers the old
  entries. The smoke page over HTTP: 11 entries damaged -> 11 re-downloaded; 11 old-format entries
  holding text that isn't JavaScript -> 11 "doesn't compile; downloading it again"; same results
  as an undamaged run each time.
- Offline game.zip: checked against the SHA-256 GitHub gives for the release's file (`digest` in
  the release JSON), worked out as the download arrives; a part left by an earlier attempt is read
  once first ("Checking the part downloaded before"). Size and the zip's directory can't see damage
  inside the file. With a 4 MB stand-in release (`PKR_RELEASE_API=<url>` on the desktop): right
  digest installs; wrong digest, and a resumed part with one changed byte, are rejected and
  deleted; a release without a digest is checked by size and as a zip, as before. Not measured on
  the Vita: the hash of 530 MB as it downloads, and the extra rename per save.
- `build_vita.sh` ended with `; true`: after a compile error an older .vpk passed its check. It
  now fails with the build's status (the full output is in `<build dir>/build.log`).
tests/smoke and tests/phaser pass. tests/shaders fails two variant pixel checks on this machine's
Mesa (Ubuntu 24.04, outside the Docker image) with and without these changes.
