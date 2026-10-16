# Runtime

A small purpose-built "browser" that runs the PokeRogue web client: QuickJS-ng plus just the web APIs
the game and Phaser use. The same code builds for the PS Vita (vitaGL, sceCtrl, sceAudio) and for
Linux (SDL2 + OpenGL ES 2), where most development and testing happens headless in Docker.

## Layout
| file | what it does |
|---|---|
| `src/main.c` | config, frame loop, `[perf]`/`[mem]` log lines, watchdog, test control channel, app restart |
| `src/platform_vita.c`, `src/platform_sdl.c` | screen, input (W3C "standard" gamepad), audio output, threads |
| `src/js_host.c` | QuickJS setup, ES module loader with bytecode cache, URL resolution, `__native` helpers |
| `src/net.c` | HTTP on libcurl in a worker thread; disk cache for versioned assets; the offline game's zip |
| `src/gl.c` | WebGL 1 as native functions; screen scaling; texture eviction when GPU memory runs short |
| `src/canvas2d.c` | images (decoded on a worker thread), fonts with per-character fallback, Canvas 2D on plutovg |
| `src/fastpath.c` | Phaser's container walk, sprite batching, uniforms and flushes in C (see `js/fastpath.js`) |
| `src/audio.c`, `src/audio_m4a.c` | Web Audio backend: decoder thread (WAV, MP3, AAC) and mixer |
| `src/savecrypt.c` | native AES for the game's save encryption (crypto-js compatible) |
| `src/launcher.c`, `src/offline_install.c` | start screen; downloading/updating the offline game |
| `src/osk.c`, `src/dialogs.c`, `src/overlay.c` | built-in keyboard; QR codes, notices, file picker; status screens |
| `src/profiler.c` | sampling profiler for logs from the console |
| `js/bootstrap.js` | loads the shim, fetches `index.html`, runs it like a browser; Phaser adjustments |
| `js/core.js` | console, timers, rAF, events, URL, TextEncoder, base64, crypto, Intl polyfill |
| `js/dom.js` | DOM tree, HTML parser, selectors, elements, document/window, FontFace, downloads, file inputs |
| `js/canvas2d.js`, `js/webgl.js` | CanvasRenderingContext2D and WebGLRenderingContext on the natives |
| `js/net.js`, `js/storage.js` | fetch, XMLHttpRequest, Blob, FileReader; localStorage and cookies |
| `js/input.js`, `js/audio.js` | navigator, keyboard/pointer events, Gamepad API, text fields; Web Audio |
| `js/fastpath.js`, `js/lazy_ui.js`, `js/i18n_cache.js`, `js/savecrypt.js` | performance: native rendering paths, UI built on first use, translation cache, native save encryption |

## Build and run (all in Docker)
```bash
./scripts/fetch_deps.sh      # pinned third-party sources into third_party/
./scripts/build_linux.sh     # -> build-linux/pokerogue-rt
./scripts/build_vita.sh      # -> build-vita/pokerogue-<VERSION>.vpk
./scripts/run_linux.sh --control .cache/control.txt                        # live pokerogue.net
./scripts/run_linux.sh --data .cache/offline --control .cache/control.txt  # offline game in .cache/offline
./scripts/run_vita3k.sh 240  # crash check in Vita3K (its timings mean nothing)
```
Everything the runtime writes (HTTP cache, `localStorage.json`, `cookies.json`, `log.txt`) goes to the
data folder (`.cache/data/` by default, `ux0:data/pokerogue/` on the Vita).

While it runs, drive it with `scripts/drive.sh`, which appends to the control file:
```bash
./scripts/drive.sh "key Space" "sleep 2" "shot .cache/shots/title.png"
./scripts/drive.sh "eval document.title"
./scripts/drive.sh "evalfile scripts/probe_verify_render.js"
```

### Playing without an account (testing)
The live site needs a login, and the login API shouldn't be hammered from a server. For testing,
use the offline game: put Admiral-Billy's `game.zip` (or let the start screen download it) in a data
folder such as `.cache/offline/` and run with `--data .cache/offline`. It plays as a guest.
`scripts/drive_to_battle.sh` drives a fresh save into the first battle (timing-sensitive; check with
a screenshot).

### Checking native fast paths
Every replacement of Phaser code is checked against Phaser's own output with animations frozen:
`scripts/probe_verify_render.js` (vertex data per flush), `probe_verify_uniforms.js` (GL uniform
calls) and `probe_verify_batching.js` (draw calls, texture binds, uploaded vertex bytes).
[../docs/render-coverage.md](../docs/render-coverage.md) lists which draw features and screens take
the native paths.

## Status
See the [root README](../README.md) and the open items in [../docs/PLAN.md](../docs/PLAN.md).
Measurements and the reasons behind each change are in [../docs/phase1-findings.md](../docs/phase1-findings.md).
