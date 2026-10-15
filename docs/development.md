# Development guide

Every toolchain runs in Docker, so the host needs only `docker`, `git`, `curl` and `python3`.

| Docker image | Used for | Built from |
|---|---|---|
| `vitasdk/vitasdk:latest` | Vita builds (.vpk) | pulled from Docker Hub |
| `pkrb-linux` | desktop builds and Node/V8 reference runs | `phase0/docker/linux.Dockerfile` |
| `pkrb-vita3k` | headless Vita3K crash checks | `phase0/docker/vita3k.Dockerfile` |
| `pkr-chrome` | headless Chrome for measuring the real game | `phase0/measure/Dockerfile` |

## Phase 0 benchmark
```bash
cd phase0
./scripts/fetch_deps.sh      # QuickJS-ng (pinned), Phaser 3.90, live pokerogue.net chunks, Vita debug-text helper
./scripts/build_linux.sh     # desktop build + run, writes bench-log.txt
./scripts/build_vita.sh      # -> build-vita/pokerogue_bench.vpk
./scripts/run_vita3k.sh      # install + run the vpk in Vita3K and print its log (crash check only)
docker run --rm -v "$PWD":/w -w /w pkrb-linux node scripts/node-driver.js   # same scene in V8
```
`fetch_deps.sh` downloads whatever pokerogue.net serves today, so Test 1 numbers drift as the game grows.

## Installing on a PS Vita
Requirements: HENkaku (or Ensō), VitaShell, and **Settings → HENkaku Settings → Enable Unsafe
Homebrew**. Builds are marked unsafe so they can raise the CPU clock.

1. In VitaShell, press **SELECT** to start FTP. Note the IP address it shows.
2. Upload the file from a computer. You can rename it during upload; VitaShell only cares about `.vpk`:
   ```bash
   curl -T pokerogue_bench.vpk ftp://VITA_IP:1337/ux0:/data/pokerogue_bench.vpk
   ```
3. In VitaShell, open `ux0:data/`, press ✕ on the .vpk, install it, and accept the permissions prompt.
4. Logs are written to `ux0:data/pokerogue-bench/log.txt`:
   ```bash
   curl ftp://VITA_IP:1337/ux0:/data/pokerogue-bench/log.txt -o log.txt
   ```

Later phases will also need `ur0:data/libshacccg.suprx`, extracted on the console with ShaRKF00D.
vitaGL needs it to compile shaders at runtime.

## Vita gotchas
- CMake needs `-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake`.
- Vita newlib's printf has no `%zu`; use `%lu` with a cast.
- Size `_newlib_heap_size_user` and `sceUserMainThreadStackSize` explicitly (see `phase0/src/main.c`).
- Vita3K is for crash checks only: it does not emulate the Vita's CPU speed. It also crashes on its
  own now and then, and loses the app's unflushed file writes when it does; the app's console
  output (`*** TTY:` lines in `.cache/vita3k-shots/vita3k.log`) survives.
- vitaGL's init functions return whether the resolution *fell back*, not success.
- VitaSDK's prebuilt libcurl uses OpenSSL 1.0 struct layouts and crashes with the SDK's OpenSSL
  1.1, so `runtime/scripts/build_vita_curl.sh` builds our own. It also forces curl's own
  `inet_pton`: the Vita's accepts host names as IPv4 addresses, which silently disables SNI.
- OpenSSL on the Vita can't open the CA bundle by path. The runtime parses it once into a shared
  `X509_STORE` attached through `CURLOPT_SSL_CTX_FUNCTION`.
- OpenSSL finds no entropy source on the Vita ("Insufficient randomness" on every TLS connection);
  the runtime installs a `RAND_METHOD` backed by `sceKernelGetRandomNumber`. Vita3K hides this.
- OpenSSL 1.1 creates a pthread rwlock (backed by kernel objects) for every certificate and TLS
  context; the Vita runs out of them after ~90 certificates, reported as "malloc failure". The app
  replaces OpenSSL's lock functions with single-threaded versions (`runtime/src/vita_compat.c`).
- vitaGL compiles GLSL inside `glLinkProgram` (compile status always reads true) and, unpatched,
  crashes in `SceGxm` when a shader fails; `runtime/patches/vitagl-0001-*.patch` makes the link fail
  instead, and the runtime logs the compiler messages and saves the shader sources to the data dir.
- vitaGL translates GLSL to Cg, where words like `texture`, `half`, `fixed` and `sampler` are
  reserved. Phaser's main shader has a variable named `texture`; `js/webgl.js` renames such
  identifiers before handing shaders over.
- Crash dumps (`ux0:data/psp2core-*.psp2dmp`) decode with vita-parse-core and the unstripped
  `build-vita/pokerogue-rt` (the tool needs small Python 3 fixes).
- Don't offer zstd in Accept-Encoding: each stream needs megabytes of buffer.
- LiveArea images must be 8-bit palette PNGs (`runtime/scripts/make_livearea.py` writes them).
- vitaGL uniform locations are negated pointers and often negative; only -1 means "not found".
- vitaGL compiles a sampler array (`uniform sampler2D s[16]`) as one sampler, so only `s[0]` gets
  a texture. Phaser's batch shader and PokeRogue's shaders use one; `js/webgl.js` rewrites each
  array into separate samplers and maps the uniform calls back onto them. (Symptom: every sprite
  whose texture sat on another unit drew as horizontal streaks.)
- vitaGL's GLSL header had `lessThan`/`lessThanEqual` swapped (`runtime/patches/vitagl-0003-*`).
- Shaders that stop using their texture result crash libshacccg ("fatal internal error"); keep a
  small dependency on it in debug shaders (e.g. `+ color * 0.002`).
- Memory: the newlib heap and vitaGL's pools are fixed, and Phaser's defaults don't fit. The runtime
  disables Phaser's pre-FX (one full-screen render target per camera), frees canvas pixels once
  they're uploaded to the GPU, and allocates full-screen render targets at half size. The `[mem]`
  lines in the log break memory use down; `eval __native.textureDump(100)` lists big textures.

## Versions
`runtime/VERSION` holds the version ("0.18"). Bump it for every build handed out for testing. It
sets the .vpk name (`build-vita/pokerogue-0.18.vpk`) and the LiveArea version (00.18), and the
app shows it with the git commit (`scripts/build_id.sh`; "-modified" means uncommitted changes) in
the first log line and in the corner of the startup and error screens. Commit before building a
test build so the commit id identifies the code.

In the game, SELECT while holding L + R toggles a debug line in the top-right corner: version,
frame rate, JS time per frame and heap use (`hud=1` in `<data>/config.txt` starts with it on; `hud`
on the control channel toggles it). While it is on, Phaser is told to restore its GL state after
the line is drawn, which costs about a millisecond per frame.

## Text entry
The login and register forms open the system keyboard (Vita IME dialog). On the test console
`sceImeDialogInit` fails with 0x800204c4 (cause unknown; it works in Vita3K), so after one failure
the app uses its own keyboard (`runtime/src/osk.c`), drawn over the game: D-pad/stick and cross to
type, circle delete, triangle space, square capitals, START done, SELECT back, or touch the keys.
`osk=1` in `<data>/config.txt` forces it (on the desktop runtime too: arrows, Enter, Backspace,
Escape, typed characters). The rear touchpad is unused: resting fingers held "submit" down.
The game's text fields are HTML `<input>` elements over the canvas; `js/input.js` draws their text
(the field's font and colour, passwords as asterisks) and the native layer puts it over the game.

## Profiling
- The built-in profiler (`runtime/src/profiler.c`) samples the JS call stack while the debug line
  is on (SELECT while holding L+R, or `hud=1` in `<data>/config.txt`). Every 30 s the log gets the
  busiest functions and how the main loop's time splits (game JS, swap/GPU wait, drawing...);
  `<data>/profile.txt` gets all stacks in collapsed form for flame graph tools. In Vita3K, sampling
  during normal play caused one-second stalls, hence the opt-in.
- `scripts/probe_phases.js` (update vs render time per frame), `probe_selfprof.js` (per-function
  self time on desktop; its wrappers inflate the totals) and `probe_textcost.js` complement it.
- Compiled game code is cached in `<data>/bytecode/` (key: URL, source, QuickJS version). On the
  desktop a cache hit loads 4-6x faster than compiling.

## The game's API and Cloudflare
api.pokerogue.net sits behind Cloudflare. Requests without a browser-like `Origin`/`Referer`
(`https://pokerogue.net`) get a 403 "Sorry, you have been blocked" HTML page, and many failed
logins in a short time get 429 (error 1015). `nettrace=1` in `<data>/config.txt` logs the headers
of API requests. Don't test logins in a loop from a server: Cloudflare starts blocking its address.

## Debugging on the Vita and in Vita3K
- `PREFILL_FROM=.cache/data ./scripts/run_vita3k.sh 300` copies the desktop runtime's HTTP cache
  into the emulator, so the game loads without going through Vita3K's network emulation.
- Vita3K compiles shaders in the background and skips draws until they're ready, so the first run
  after a shader change shows a black screen. Run it twice. Its rendering of the game is not
  reliable either: treat it as a crash and memory check, not a picture of what the Vita shows.
- Control commands go in `<data>/control.txt` (on Vita3K: `ux0/data/pokerogue/control.txt` under
  its file system), e.g. `evalfile ux0:data/pokerogue/probe_gl.js`. `scripts/probe_*.js` are
  diagnostic probes; comparing a probe's output on the desktop runtime and on Vita3K narrows down
  where they differ.
- `<data>/debug.js` runs before the game loads; a `__shaderHook(source)` function defined there
  rewrites every shader, e.g. to output intermediate values as colours. Comparing the same hook on
  the desktop runtime and on Vita3K narrows down where the two differ.
- Vita3K's shader translation is not exact: for PokeRogue's field shader it disagrees with the
  real Vita. Confirm graphics findings on the console.
- `interrupt=N` in `<data>/config.txt` stops JS that runs N s in one frame and logs its stack.
- An empty file named `dump-shaders` in the data folder makes vitaGL write each shader as handed
  to the Cg compiler (after GLSL translation) to `<data>/shaders/`
  (`runtime/patches/vitagl-0002-*.patch`).

## Measuring the real game
See [phase0/measure/README.md](../phase0/measure/README.md): headless Chrome running a local dev
build of PokeRogue, with hooks that count what Phaser draws each frame.
