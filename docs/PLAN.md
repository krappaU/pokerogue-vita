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

## Open items (2026-10-30, runtime 1.0.4)
Performance:
- Battles run at ~28-40 fps on the Vita (log171: JS 25-33 ms a frame at the median, GPU 15-20 ms), so
  JS sets the frame rate. Where it goes (12 reports, samples): Phaser's render JS 21%, tweens 20%,
  PokeRogue's sprite pipelines 16%, input and frame events 8%, the rest updates and game logic.
  Next steps in [native-renderer.md](native-renderer.md) (the render walk and updates in C).
  What is native today: [render-coverage.md](render-coverage.md).
- Pauses that remain are the game's own work: a move animation's first use (300-575 ms), the save
  at each wave (JSON.stringify ~300 ms), atlas loading, screen setup, and collections (1.5-2 s,
  placed under loading screens and after saves where possible).

Reliability and maintenance (from [review-2026-10.md](review-2026-10.md); findings 3, 4, 5 and 9
done in 1.0.4: renderbuffer leak, disk cache cleaning, missing-compiler message, download check):
- Unpinned build inputs: `vitasdk/vitasdk:latest` (finding 8; Vita3K is pinned since 2026-10-28,
  with a weekly scheduled check for newer builds).
- Cg reserved-word renaming doesn't rename uniform/attribute lookups (finding 7; latent).
- No fallback to the last working game version if a new PokeRogue release breaks the shim (risk 4
  above); no scheduled check that runs the offline game to the title screen.

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
