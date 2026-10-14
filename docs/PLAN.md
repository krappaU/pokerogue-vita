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
│   Canvas2D/text → plutovg or cairo + freetype                   │
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
0. **Feasibility spike: done (go).** See [phase0-findings.md](phase0-findings.md). On real
   hardware, the synthetic battle frame went from 57.8 ms (stock Phaser) to 27.4 ms (native
   batching fast path). Compiling the 6 MB of JS takes 14.8 s; loading the bytecode cache takes 0.9 s.
1. **Desktop runtime: largely done.** See [phase1-findings.md](phase1-findings.md) and `runtime/`.
   The live client boots to its login screen, and a login-bypass build plays into battles.
   The real game turned out much heavier than the Phase 0 benchmark (busy battle ~12–15 ms JS per
   frame on x86, estimated ~200 ms on the Vita before optimizations).
2. **Vita runtime:** the same runtime on Vita (SDL2 + vitaGL, curl), then measure the *real* game's
   frame cost on the device. That number decides how much native rendering work Phase 4 needs.
3. **Playable:** audio, gamepad, touch, IME login, online saves.
4. **Polish:** performance (native container walk, uniform batching, 30 fps cap), memory work,
   offline cache and last-good fallback, LiveArea assets, settings.

## Needed from the Vita side
- HENkaku/Ensō, VitaShell (for FTP installs)
- `libshacccg.suprx` extracted (ShaRKF00D), which vitaGL needs to compile shaders. Needed from Phase 2.
- Firmware: the test Vita is on 3.65 with HENkaku + VitaShell
- Phase 0 and later builds write logs to `ux0:data/pokerogue/log.txt`; send those back
