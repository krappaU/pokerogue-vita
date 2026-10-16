# PokeRogue for PS Vita: plan

## Goal
A Vita homebrew app (.vpk) that runs the **live** pokerogue.net client, the same way the
"online mode" of Admiral-Billy's Electron app does. That means automatic updates, the same
rendering, controls and online saves, the Vita buttons as a gamepad, and the system on-screen
keyboard for login.

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
│  QuickJS-ng ── runs the unmodified game bundle (ES2023, modules) │
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
│   <input> focus → sceImeDialog (system keyboard) for login      │
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
3. **Playable: done (beta).** Sound, gamepad, touch, built-in keyboard login, online saves,
   offline mode with downloads and updates, save import/export. Released as betas
   (v0.37-beta onward).
4. **Performance and polish: in progress.** See the findings log for what has been done (native
   container walk, sprite batching, uniforms and flushes; lazily built UI; translation cache;
   native save encryption; work moved off the main thread) and the open items below.

## Open items (2026-10-16)
Performance (the main remaining work):
- Battles run at ~10-20 fps on the Vita; the game's own sprite pipelines (per-sprite uniforms and
  flushes, colour arrays rebuilt with `splice(...spread)`) are now the largest cost. Planned: a
  native Phaser renderer behind Phaser's API (branch `native-renderer`), engine-level speedups in
  QuickJS (fast paths for `splice`/spread), and checking whether a JIT is possible.
- Loads: startup ~17 s with a warm cache (over a minute after each game update while its code is
  compiled), wave and trainer-battle transitions of several seconds, hitches when evicted textures
  are re-uploaded. The `[perf]` line now names slow main-loop steps; check the next logs.

Reliability:
- Downloads occasionally stall for ~16 s and fail with 0 bytes (one per session in the last logs,
  down from batches before 0.39). Consider retrying failed asset GETs once.
- Memory over long sessions: watch heap and GPU pools in `[mem]` lines (0.41 evicts harder under
  pressure).
- No fallback to the last working game version if a new release breaks the shim (risk 4 above).
- Cloudflare served stale copies of pokerogue.net to requests with an `Origin` header (fixed by
  matching browser headers); watch for similar cache variants.

Features and checks:
- Untested on the console yet: save import/export and the restart after an import (0.41),
  texture eviction under pressure (0.41), the input-repeat fix and QR links (0.40).
- The system keyboard (IME dialog) fails on the test console (0x800204c4); the built-in keyboard
  is used instead. Discord/Google sign-in can't work (a QR code explains it).
- Most game settings are untested; the rear touchpad is off (resting fingers pressed "submit").
- Video elements show a stand-in frame (no video decoding).
## Requirements on the Vita
- HENkaku or Ensō, with unsafe homebrew enabled; VitaShell to install builds (over FTP or USB)
- `libshacccg.suprx` in `ur0:data/` (or `ur0:data/external/`), which vitaGL needs to compile shaders.
  Each user installs their own copy, for example with PIB-Configuration-Tool, ShaRKBR33D or ShaRKF00D.
- Wi-Fi: the app loads the live game from pokerogue.net
- Logs go to `ux0:data/pokerogue/log.txt`
