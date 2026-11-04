# PokeRogue for PS Vita: plan

## Goal
A Vita homebrew app (.vpk) that runs the **live** pokerogue.net client, the same way the
"online mode" of Admiral-Billy's Electron app does. That means automatic updates, the same
rendering, controls and online saves, the Vita buttons as a gamepad, and an on-screen keyboard
for login (the app's own: the system keyboard never opened on the console, and its memory is used
for the GPU).

## Why a plain browser wrapper is impossible on Vita
Survey of the live site (Oct 2026, game v1.12.1):

| Game needs | Vita built-in browser (NetFront, ~2012 WebKit) |
|---|---|
| ES2022 JS (`#private` fields, `??=`, `?.`, `.at()`), ES modules | ES5 only |
| WebGL 1 (Phaser is **forced** to `Phaser.WEBGL`, with 3 custom GLSL shaders in `src/pipelines/glsl`) | no WebGL |
| Web Audio (`AudioContext`, mp3 BGM and wav SE) | no |
| `FontFace` (.ttf pixel fonts), Canvas 2D (Phaser draws all text through it) | partial or none |
| `localStorage`, `document.cookie` (auth token), `navigator.getGamepads` | partial |
| DOM `<input>` for login (rex InputText plugin) | yes, but useless without the rest |

There is no modern browser engine ported to Vita, and porting WebKit or Chromium is out of reach.
The startup JS is about 6.2 MB across 8 chunks, plus more chunks loaded on demand.

## Approach: a tiny purpose-built "browser"
A native C app (VitaSDK) that embeds a modern JS engine and implements **only the web APIs that
PokeRogue and Phaser actually use**:

```
pokerogue.net (index.html, JS chunks, assets)  ←→  api.pokerogue.net
          │ fetch (libcurl + TLS, browser UA/Origin; Cloudflare returns 403 without them)
┌─────────▼──────────────────────────────────────────────────────┐
│ Host app (C)                                                    │
│  QuickJS-ng ── runs the game bundle as served (ES2023, modules;  │
│                a few exact speed rewrites: src/gamefix.c)        │
│  Web API shim:                                                  │
│   WebGL1        → vitaGL (GLSL translated at runtime)           │
│   Canvas2D/text → plutovg (fonts with per-character fallback)   │
│   Image         → stb_image / libpng → GL texture               │
│   Web Audio     → own mixer on sceAudio (mp3 streamed, not      │
│                   pre-decoded to float PCM, to save RAM)        │
│   fetch/XHR     → libcurl, disk cache keyed by hashed filename  │
│   localStorage, document.cookie → files in ux0:data/pokerogue   │
│   Gamepad API   → Vita buttons as a "standard" pad              │
│   Pointer/touch → front touchscreen                             │
│   <input> focus → built-in on-screen keyboard for login         │
│   minimal fake DOM (document, body, createElement, style, events)│
└─────────────────────────────────────────────────────────────────┘
```

**Auto-updates:** at every launch, fetch `index.html` (8 KB) and resolve its hashed chunk names.
Chunks already in the disk cache are not downloaded again. QuickJS bytecode is cached per hash,
so the compile cost is paid once per game release. If a new release breaks the shim, show the
error and fall back to the last version that booted.

**Dev loop:** the same codebase also builds for Linux (SDL2 + GLES2). Most shim work happens on
desktop, where it is fast to run and screenshot. The Vita build uses the `vitasdk/vitasdk`
Docker image and is tested in Vita3K (does it boot?) and on the real Vita (performance, memory).

## Risks (honest)
1. **CPU performance (biggest).** QuickJS is an interpreter with no JIT, and the Vita's
   Cortex-A9 runs at 444 MHz at most. Phaser does sprite batching in JS every frame. The
   frame rate could land anywhere from fine to unplayable. *Gate it first (Phase 0).*
   Mitigations: cap at 30 fps, skip redraws when nothing changed, replace hot Phaser methods
   (batch vertex fill, matrix math) with native C at runtime, overclock.
2. **Memory.** Roughly 300+ MB is usable. Browsers decode audio to float32 (about 40 MB per BGM
   track), so audio must stay compressed. Textures go in CDRAM.
3. **Startup time.** Parsing 6 MB of JS on the A9 is slow. The bytecode cache handles it.
4. **Upstream drift.** A new game release may use a web API we don't implement. The
   last-good-version fallback handles it, then we patch the shim.
5. **Cloudflare.** If bot protection gets stricter (for example a JS/Turnstile challenge),
   non-browser clients break. Today, browser-like headers are enough.

## Phases
0. **Feasibility spike: done (go).** See [phase0-findings.md](phase0-findings.md) (the benchmark
   itself is in `archive/phase0/`). On real hardware the synthetic battle frame went from 57.8 ms
   (stock Phaser) to 27.4 ms with native batching; the bytecode cache cut compiling 6 MB of JS from
   14.8 s to 0.9 s.
1. **Desktop runtime: done.** The live client and offline builds run headless in Docker; this is
   where most work is developed and checked.
2. **Vita runtime: done.** vitaGL, libcurl/OpenSSL (with Vita-specific fixes), memory budget,
   LiveArea assets.
3. **Playable: done.** Sound, gamepad, touch, built-in keyboard login, online saves,
   offline mode with downloads and updates, save import/export. Released as betas
   (v0.37-beta onward), then v0.93 and v1.0.
4. **Performance and polish: in progress.** See the findings log for what has been done (native
   container walk, sprite batching, uniforms and flushes; lazily built UI; translation cache;
   native save encryption; work moved off the main thread) and the open items below.

## Open items (2026-11-03, runtime 1.0.10)
Performance:
- Battles run at ~28-40 fps on the Vita (log171: JS 25-33 ms a frame at the median, GPU 15-20 ms), so
  JS sets the frame rate. Where it goes (12 reports, samples): Phaser's render JS 21%, tweens 20%,
  PokeRogue's sprite pipelines 16%, input and frame events 8%, the rest updates and game logic.
  Next steps in [native-renderer.md](native-renderer.md) (the render walk and updates in C).
  What is native today: [render-coverage.md](render-coverage.md).
- Pauses that remain are the game's own work: a move animation's first use (300-575 ms), the save
  at each wave (JSON.stringify ~300 ms), atlas loading, screen setup, and collections (1.5-2 s,
  placed under loading screens and after saves where possible).
- Next candidates, ranked (2026-11-03, runtime 1.0.10; log203: a late run, waves 163-175, 216 s of
  frames over 250 ms in ~45 minutes):
  (1.0.11 did a first part of 1 and 2: the reviver runs for ~1 in 20 members, and the settings,
  achievements, stats, egg gacha, challenges and Pokedex scan screens are built on first use;
  Vita numbers pending.)
  1. Loading the system save: GameData.parseSystemData is a JSON.parse whose reviver runs for every
     key (~21,000 in a 220 KB save), 650-900 ms each time it runs (title, Continue, each battle
     retry, the egg gacha): 6.7 s in log203. The reviver changes only gameStats, eggs, ribbons and
     the *Attr keys (BigInt); on the desktop the plain parse is a quarter of the time (3.0 of 11.6
     ms). A native walk that calls the game's reviver only for those keys (a gamefix rule on its
     exact text) should save ~0.6 s per load. (The BigInt replacer of the saves' JSON.stringify
     costs only ~10%: not worth it.)
  2. Back to the title and into the run again (each "retry battle", Continue): one 5-6.4 s frame
     rebuilding the screens, then 1.1 and 2 s frames; ~80 s over 8 resets in log203. Its profile is
     cut off (the slow-frame sampler keeps 1024 samples: 5.3 s "stack not stored"), so first a longer
     sample buffer for such frames, then a desktop profile with fixtures/run-wave163.json
     (scene.reset(true) from the control channel).
  3. The reward shop opening, ~1 s each wave (13 frames, 12 s: its option texts, TM compatibility,
     the item pool's thresholds).
  4. Battle frames: 27 fps on average, JS 40 ms, 17 draw calls a frame: still the main lever
     (native-renderer.md). The item screens: ~45 ms of JS a frame while open, with nothing changing.
  5. Collections grow with the run: 1.4-2.6 s late in it (JS heap 95 MB before one), already placed
     after saves and under loading screens.
  6. The item bar (ModifierBar.updateModifiers) rebuilds all ~46 icons on every item change: ~220 ms,
     several times a turn late in a run (log209). Reusing the icons of unchanged items is the next
     per-turn win; the enemy's move choice (250-350 ms a turn) is the game's own logic.
  7. Hatching many eggs at once (skip chosen): ~0.5 s per egg on the Vita, 89% of it building a
     battle info box per hatched Pokemon that is never shown (log212: 21 s). A battle info built on
     first use for hatched Pokemon would need three gamefix rules (the hatch, Pokemon.init, destroy).
  (1.0.14: 6 and 7 done: item bar icons kept, hatched Pokemon's battle info built on use. log215:
  ~14 eggs hatched with no stretch over 0.5 s (log212: 21 s), the summary screen at half the frame
  time; the item bar's Vita number needs a late-run log.)
  8. The enemy's move choice in double battles: 1.1 s frames (log215), the game's damage estimate
     for each move and target with its ability hooks. (1.0.15: answers and speeds kept within one
     choice; desktop -56% in a double battle; Vita numbers pending.)
  (1.0.12: memory over long sessions: retries no longer leave the old battle alive, and canvas
  textures no longer copy their canvas when made; log207 ran out of memory at wave 200.)
  Network, not ours to fix: the save after each wave uploads 255-265 KB (game and session data);
  on busy Wi-Fi 17-74 s (5-31 KB/s). Smaller uploads would need the server to accept compression.

Reliability and maintenance (from [review-2026-10.md](review-2026-10.md); findings 3, 4, 5 and 9
done in 1.0.4: renderbuffer leak, disk cache cleaning, missing-compiler message, download check):
- Unpinned build inputs: `vitasdk/vitasdk:latest` (finding 8; Vita3K is pinned since 2026-10-28,
  with a weekly scheduled check for newer builds).
- Cg reserved-word renaming doesn't rename uniform/attribute lookups (finding 7; latent).
- No fallback to the last working game version if a new PokeRogue release breaks the shim (risk 4
  above); no scheduled check that runs the offline game to the title screen.

From the pokerogue-nx fork (Switch/Android, merged our 1.0.6; reviewed 2026-11-03), to keep in mind:
- Unconfirmed for the Vita: when the console's clock is more than 30 s off, Date.now() is corrected
  by a fractional offset (net.c note_server_date uses microseconds), and the fork found PokeRogue's
  save API rejecting the fractional timestamps (`json: cannot unmarshal number ... of type int`;
  their fix: Math.trunc in core.js's nowMs). To check on a console with a shifted clock.
- Login and register screens without D-pad navigation (login still needs a tap or SELECT-and-cross
  after the password); the fork's runtime/js/modal-controls.js adds focus navigation. Not ported yet.

Features and checks:
- Untested on the console: save import/export and the restart after an import (0.41), QR links (0.40).
- Login uses the built-in keyboard; Discord/Google sign-in can't work (a QR code explains it).
- Most game settings are untested; the game's "submit" button (touchpad click) is unmapped.
- Video elements show a stand-in frame (no video decoding).

## Requirements on the Vita
- HENkaku or Ensō, with unsafe homebrew enabled; VitaShell to install builds (over FTP or USB)
- `libshacccg.suprx` in `ur0:data/` (or `ur0:data/external/`), which vitaGL needs to compile shaders.
  Each user installs their own copy, for example with PIB-Configuration-Tool, ShaRKBR33D or ShaRKF00D.
- Wi-Fi: the app loads the live game from pokerogue.net
- Logs go to `ux0:data/pokerogue/log.txt` (dev builds; player builds only with `log=1` in
  `ux0:data/pokerogue/config.txt`)
