# Runtime

A small purpose-built "browser" that runs the PokeRogue web client: QuickJS-ng plus just the web APIs
the game and Phaser use. This folder currently builds for Linux (SDL2 + OpenGL ES 2); the Vita build
(SDL2 + vitaGL) is Phase 2.

## Layout
| file | what it does |
|---|---|
| `src/main.c` | config, 60 Hz frame loop, `[perf]` stats, test control channel |
| `src/platform_sdl.c` | window, GL ES context, keyboard/mouse/gamepad (W3C "standard" layout) |
| `src/js_host.c` | QuickJS setup, ES module loader over HTTP, URL resolution, `__native` helpers |
| `src/net.c` | libcurl HTTP (non-blocking, polled per frame); disk cache for versioned assets; browser-like headers |
| `src/gl.c` | WebGL 1 as native functions, with screen scaling/letterboxing of the 1920x1080 canvas |
| `src/canvas2d.c` | image decoding (stb_image), fonts and Canvas 2D drawing (plutovg) |
| `src/fastpath.c` | native sprite batching for Phaser (from Phase 0) |
| `js/bootstrap.js` | loads the shim, fetches the live `index.html`, runs it like a browser |
| `js/core.js` | console, timers, rAF, events, URL, TextEncoder, base64, crypto, Intl polyfill |
| `js/dom.js` | DOM tree, HTML parser, selectors, elements, document/window, FontFace, DOMParser |
| `js/canvas2d.js` | CanvasRenderingContext2D (state, colors, fonts, text layout, shadows) |
| `js/webgl.js` | WebGLRenderingContext around the native functions |
| `js/net.js` | fetch, XMLHttpRequest, Blob, FileReader, Headers |
| `js/storage.js` | localStorage and cookies, persisted in the data dir |
| `js/input.js` | navigator, keyboard/pointer events, Gamepad API |
| `js/audio.js` | silent Web Audio stand-in (real audio is Phase 3) |
| `js/fastpath.js` | swaps Phaser's sprite batching and container rendering for native-backed versions |

## Build and run (Linux, headless, all in Docker)
```bash
./scripts/fetch_deps.sh      # QuickJS-ng, plutovg, stb (pinned) into third_party/
./scripts/build_linux.sh     # -> build-linux/pokerogue-rt
./scripts/run_linux.sh --control .cache/control.txt          # live pokerogue.net
```
Everything the runtime writes (HTTP cache, `localStorage.json`, `cookies.json`, `log.txt`) goes to `.cache/data/`.

While it runs, drive it with `scripts/drive.sh`, which appends to the control file:
```bash
./scripts/drive.sh "key Space" "sleep 2" "shot .cache/shots/title.png"
./scripts/drive.sh "eval document.title"
./scripts/drive.sh "evalfile scripts/probe_selfprof.js"
```

### Playing without an account (for testing and measurements)
The live site needs a login. A local production build of PokeRogue with `VITE_BYPASS_LOGIN=1`
plays as a guest. To build and serve one (see `../phase0/measure/README.md` for checking out the game):
```bash
VITE_BYPASS_LOGIN=1 VITE_BYPASS_TUTORIAL=1 pnpm exec vite build --mode production   # in the pokerogue checkout
docker network create pkr-net
docker run -d --name pkr-serve --network pkr-net -v <phase0/measure>:/tools:ro -v <pokerogue>/dist:/dist:ro \
  -v <cache dir>:/cache pkr-chrome node /tools/serve.js /dist /cache 8000
DOCKER_ARGS="--network pkr-net" ./scripts/run_linux.sh --url http://pkr-serve:8000/ --control .cache/control.txt
./scripts/drive_to_battle.sh   # timing-sensitive; check with a screenshot
```
`serve.js` fills in images, audio and locales from pokerogue.net on demand.

## Status (2026-10-14)
- The live pokerogue.net client boots to its login screen. A login-bypass production build of the
  current game source plays through title, starter select and into battles (including a wave-120
  double battle with ~40 items), with correct rendering, text and PokeRogue's custom shaders.
- Not done yet: audio (silent stand-in), login text input (system keyboard on Vita), pointer
  coordinates, per-glyph font fallback for mixed CJK text, video (stand-in frame), bytecode cache.
- Performance findings are in [../docs/phase1-findings.md](../docs/phase1-findings.md).
