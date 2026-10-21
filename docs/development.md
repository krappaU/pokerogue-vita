# Development guide

Every toolchain runs in Docker, so the host needs only `docker`, `git`, `curl` and `python3`.

| Docker image | Used for | Built from |
|---|---|---|
| `vitasdk/vitasdk:latest` | Vita builds (.vpk) | pulled from Docker Hub |
| `pkr-rt-linux` | desktop builds of the runtime | `runtime/docker/linux.Dockerfile` |
| `pkrb-vita3k` | headless Vita3K crash checks | `runtime/docker/vita3k.Dockerfile` |
| `pkr-chrome` | headless Chrome running the real game (reference renders) | `tools/measure/Dockerfile` |

The original feasibility benchmark (Phase 0) is kept in `archive/phase0/` for reference; its own
README explains how it was built and run.

## Installing on a PS Vita
Requirements: HENkaku (or Ensō), VitaShell, and **Settings → HENkaku Settings → Enable Unsafe
Homebrew**. Builds are marked unsafe so they can raise the CPU clock.

1. In VitaShell, press **SELECT** to start FTP. Note the IP address it shows.
2. Upload the file from a computer. You can rename it during upload; VitaShell only cares about `.vpk`:
   ```bash
   curl -T runtime/build-vita/pokerogue-0.41.vpk ftp://VITA_IP:1337/ux0:/data/pokerogue.vpk
   ```
3. In VitaShell, open `ux0:data/`, press ✕ on the .vpk, install it, and accept the permissions prompt.
4. Logs are written to `ux0:data/pokerogue/log.txt` (dev builds; a player build only with `log=1`
   in `ux0:data/pokerogue/config.txt`, see "Debug features" below):
   ```bash
   curl ftp://VITA_IP:1337/ux0:/data/pokerogue/log.txt -o log.txt
   ```

The app also needs `ur0:data/libshacccg.suprx` (Sony's shader compiler, extracted on the console
with ShaRKBR33D or ShaRKF00D); vitaGL compiles shaders with it at runtime. Never commit or share it.

## Test builds and logs without FTP (dev builds)
Dev builds only: `PKR_DEV=1 runtime/scripts/build_vita.sh` (`build-vita-dev/pokerogue-<VERSION>-dev.vpk`;
CI uploads it as the `pokerogue-dev-*` artifact). Normal builds, the ones handed to players, don't
contain this code at all: they never replace their own files or send logs anywhere. Dev builds
show "dev" after the version. The app side is `runtime/src/devtools.c`.

**Test builds (GitHub):** before the start screen a dev build reads `build.json` of the
[`dev-build` pre-release](https://github.com/krappaU/pokerogue-vita/releases/tag/dev-build). If it
names another build than the one running, the app asks and downloads the .vpk from the release. A
running app can't write its own `ux0:app/PKRV00001` folder (0.50 on the console: "couldn't write
ux0:app/PKRV00001/eboot.bin.new"), so a second app installs it, the way VitaShell updates
itself: **PokeRogue Updater** (`runtime/updater/updater.c`, title `PKRVUPD01`), which the dev build
carries as `updater.vpk`. The game unpacks the build to `ux0:data/pokerogue/update/files/` and
restarts; early in that start, before its graphics take the memory the installer's PAF module
needs, it installs the updater with the system's installer (ScePromoterUtil,
`runtime/updater/promoter.h`), starts it and quits. The updater installs the build the same way (a
full install: LiveArea and param.sfo too), writes `update/result.txt` and starts the game again,
which uninstalls the updater early in that start and shows an error if there was one. A marker
file (`<data>/installer.txt`) brackets every installer call: if a start finds it, the call never
came back (crash or hang), and the installer stays off for that build. The installer needs
`sce_sys/package/head.bin` in the folder: every .vpk carries one, made at build time by
`runtime/scripts/make_head_bin.py` from VitaShell's template (the same bytes VitaShell would write).
If something fails the download stays in `ux0:data/pokerogue/dev-update.vpk` for VitaShell. A
build that was declined isn't offered again.
`devbuilds=off` in `config.txt` turns the check off (`devbuilds=<url>` reads another `build.json`).
The updater (like VitaShell's VSUPDATER) is built with every dev build.

Publishing: every push to `main` whose CI run passes is published by itself ("Publish dev build"
runs after CI, with the commit title as the label); pull requests and other branches never are. It
replaces the release with that run's dev .vpk and its `build.json`. By hand (Actions -> "Publish dev
build" -> Run workflow, with a label and optionally a CI run; empty: the newest successful CI run on
`main`) it publishes another run: an older one is a rollback. From a shell:
`gh workflow run publish-dev.yml -f label="what to test" [-f run=<run id>]`.

**Logs (log server):** `tools/devserver/` (README there; Docker Compose with a Cloudflare tunnel).
With `devserver=` set, each start uploads the previous run's log (kept as `log-prev.txt`, so the
log of a crash is sent by the next start), and L + R + START in game uploads the current one. Each
upload is a new entry; reading needs no key:
```
curl https://<host>/logs           # the list, newest first: id, time, kind, size, build
curl https://<host>/logs/latest    # the newest log
curl https://<host>/logs/log57     # one log
```
`kind` is `prev-run` (the log of a run that ended, crashed or not, sent by the next start) or
`in-game` (L + R + START). In `ux0:data/pokerogue/config.txt` on the console:
```
devserver=https://<host>/
devkey=<the server's DEVSERVER_KEY>
```
Without `devserver=`, a dev build sends no logs.

## Vita gotchas
- CMake needs `-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake`.
- Vita newlib's printf has no `%zu`; use `%lu` with a cast.
- Size `_newlib_heap_size_user` and `sceUserMainThreadStackSize` explicitly (see `runtime/src/main.c`).
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
  `build-vita/pokerogue-rt` (or `build-vita-dev/`) of the same commit. The tool needs small Python 3
  fixes (`elftools.common.py3compat` is gone, `xrange`, `string.letters`, bytes in `c_str`) and
  runs in the VitaSDK image, which has `arm-vita-eabi-objdump` and `addr2line`.
- Don't offer zstd in Accept-Encoding: each stream needs megabytes of buffer.
- LiveArea images must be 8-bit palette PNGs (`runtime/scripts/make_livearea.py` writes them, for
  the game and the updater; the game's use a CC BY-NC-SA arena backdrop, see
  `runtime/sce_sys/README.md`). The Vita reads a bubble's images when the app is installed (by
  VitaShell or the dev build's updater).
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

**Debug features** are in dev builds (`PKR_DEV=1`), profiling builds and the desktop runtime only
(CMake `PKR_DEBUG`, on by default except for Vita builds for players): the log file by default, the
control channel (`ux0:data/pokerogue/control.txt` on the Vita, read every frame), the built-in
profiler, the debug line, the watchdog thread, `<data>/debug.js` and config.txt's debugging options
(`url`, `frames`, `interrupt`, `hud`, `profile`, `sample`, `profile_rate`, `sample_ms`, `nettrace`).
A player build writes no log and its JS `console.*` does nothing, unless `log=1` is in config.txt
(for a bug report). `-DPKR_DEBUG=OFF` builds the player variant on the desktop for testing.

**Releases for players:** write the notes as `docs/releases/v<version>-beta.md`, commit, and push a
tag `v<version>-beta` on that commit (`runtime/VERSION` must match), or run the "Release" workflow
by hand with that tag name (and optionally a commit of main; default the newest), which tags the
commit itself (cloud sessions can't push tags). CI runs on main are never cancelled by a newer
push, so a release can wait for an older commit's run. `.github/workflows/release.yml` waits for the commit's CI run to pass and publishes its
player .vpk (the file CI ran in Vita3K) as a release with those notes.

In debug builds, TRIANGLE while holding L + R switches the shader variants (gl.c) off and on at
once, with a notice in the corner and a `[gl] shader variants` line in the log (`variants` on the
control channel): when something is drawn wrong, this tells whether the variants are the cause.

In the game, SELECT while holding L + R toggles a debug line in the top-right corner: version,
frame rate, JS time per frame and heap use (`hud=1` in `<data>/config.txt` starts with it on; `hud`
on the control channel toggles it). While it is on, Phaser is told to restore its GL state after
the line is drawn, which costs about a millisecond per frame.

## Text entry
The login and register forms open the app's own keyboard (`runtime/src/osk.c`), drawn over the game:
D-pad/stick and cross to type, circle delete, triangle space, square capitals, START done, SELECT
back, or touch the keys. The system keyboard (Vita IME dialog) is not used (0.59): it never opened
on the test console (`sceImeDialogInit` 0x800204c4; it worked in Vita3K), most likely because
vitaGL had taken part of the system dialogs' memory budget, which now serves as GPU memory. On the
desktop runtime a field is answered through the control channel (`ime <text>`, `ime-cancel`)
unless `osk=1` (then arrows, Enter, Backspace, Escape, typed characters). The rear touchpad is
unused: resting fingers held "submit" down.
The game's text fields are HTML `<input>` elements over the canvas; `js/input.js` draws their text
(the field's font and colour, passwords as asterisks) and the native layer puts it over the game.

## Profiling
- The built-in profiler (`runtime/src/profiler.c`) samples the JS call stack while the debug line
  is on (SELECT while holding L+R, or `hud=1` in `<data>/config.txt`). Every 30 s the log gets the
  busiest functions and how the main loop's time splits (game JS, swap/GPU wait, drawing...);
  `<data>/profile.txt` gets all stacks in collapsed form for flame graph tools. In Vita3K, sampling
  during normal play caused one-second stalls, hence the opt-in.
- On the desktop, `PKR_PROFILE_RATE=1000 scripts/build_linux.sh` builds with ten times as many
  samples, and the control command `profile` samples without the debug line (whose drawing makes
  Phaser rebuild its GL state every frame, which then shows in the profile). `<data>/profile.txt`
  is rewritten every 30 s: copy it in between to keep more than one window.
- `scripts/probe_phases.js` (update vs render time per frame), `probe_selfprof.js` (per-function
  self time on desktop; its wrappers inflate the totals) and `probe_textcost.js` complement it.
- Compiled game code is cached in `<data>/bytecode/` (key: URL, source, QuickJS version). On the
  desktop a cache hit loads 4-6x faster than compiling.

## Offline game (local mode)
The start screen (`runtime/src/launcher.c`) chooses between the live site and an offline build of
PokeRogue in `<data>/game/`; if none is installed it offers to download one
(`runtime/src/offline_install.c`: GitHub release API, resumable download to `game.zip.part`,
renamed to `<data>/game.zip`). The game is played straight from the zip (miniz, `net.c`):
unpacking its 34,000 files ran at ~0.1 MB/s on the Vita's memory card. An unpacked folder in
`<data>/game/` also works. The choice is remembered in `<data>/launcher.txt`. The offline game is
played served under the made-up origin `https://offline.invalid`, no login,
saves in `<data>/localStorage-offline.json`. `online=1` in `config.txt` ignores the folder;
`local=<dir>` names another one. The build used for testing is the one Admiral-Billy's PokeRogue
App downloads for its offline mode (`game.zip` from github.com/Admiral-Billy/pokerogue/releases,
~530 MB, 735 MB unpacked; the `.map` files can be deleted).

## Memory budget
Extended memory mode (`ATTRIBUTE2=12`). Heap 256 MB (`_newlib_heap_size_user`), vitaGL pools: 95 MB
CDRAM, 64 MB RAM, 16 MB phycont and 4 MB of the system dialogs' 8.8 MB budget (no system dialog is
used, but the system's installer in dev builds needs the rest for its PAF module: 0.59 took all of
it, and PAF failed to load). An app gets 112 MB of CDRAM; libgxm takes 16 MB of it for the GPU's parameter buffer
(vitaGL's default size), so ~1 MB stays free. vitaGL puts textures in the first pool with room in
that order (CDRAM is the fastest for the GPU), then in the heap; debug builds log where the
textures are (`[mem] ...: textures in video RAM ... MB, main RAM ..., heap ...`). On the test
console 0.58 left ~15 MB of main memory to the system. `getImageData` hands its pixels to JS
without a copy.

## GPU measurements (debug builds)
- The `[perf]` line's `GPU N ms/frame`: one frame every 5 s is timed with glFinish before its
  swap, after the frame before was waited for (0.59; until then the reading included the previous
  frame and was about twice the real time). Until 0.73 it was every 30th frame; each glFinish holds
  the main thread for a whole GPU frame.
- Phaser is told of 4 texture units (its batch shader picks a sprite's texture with an `if` per
  unit; 16 cost ~2.5 ms per frame of GPU time on the title screen in 0.56). `maxtextures=N` in
  config.txt sets another number (0: as many as the GPU has).
- `plainshaders=1` in config.txt: every textured fragment shader only samples its texture (wrong
  colours, no effects). The GPU time it saves is what the shaders cost; what's left is pixels and
  texture reads.
- `gpuprofile=1` in config.txt: every 10 s one frame waits for the GPU after each draw call and the
  log gets a `[gpu] per shader` line: GPU time, draws and vertices per shader program (Phaser's by
  their `SHADER_NAME`, PokeRogue's as "PokeRogue sprite" / "PokeRogue field"). Each wait also ends
  the GPU's scene, which every draw pays for too (about the smallest draw's time, in the line):
  compare programs with it, not with the `[perf]` line's GPU time.
- `js/webgl.js` rewrites the game's fragment shaders so the Vita's shader compiler gets cheaper code
  for the same pixels (`rewriteShader`, 0.62): Phaser's texture pick reads every unit and then
  chooses (no texture read inside an if-chain), PokeRogue's colour loops are skipped when their
  palette is empty, and a condition that mixes a per-pixel test with a setting tests the setting
  first, on its own (tera, day/night, terrain). The tint effect (0, 1 or 2) is rounded before it
  is compared (0.63): the Vita's GPU interpolated 2.0 inexactly, so solid-colour shapes without a
  texture of their own (the summary's stats graph) showed whatever texture was bound. The log names
  what each shader got (`[shaderfix]`).
  `tests/shaders` renders each rewritten shader against the original (PokeRogue's sprite and field
  shaders of a pinned commit, Phaser's multi-texture shader) with the settings the game uses and
  compares every byte. `shaderfix=0` in config.txt keeps the originals, to compare on the console.
- Shader variants (`gl.c`, 0.65): the Vita's shader compiler runs all of a shader's code for every
  pixel, effects a setting switches off included. `js/webgl.js` marks such effects in PokeRogue's
  shaders (`/*pkr+terrain*/ ... /*pkr-terrain*/`: terrain, day/night, tera, the two palette loops,
  shadow); at each draw `gl.c` checks the current uniform values with the shader's own conditions
  and draws with a copy of the program built without the effects that are off (built on first use,
  logged as `[gl] shader variant: ...`). The game only sees its own program: uniform writes to it
  are recorded and copied into a copy before it draws. `tests/shaders` compares the pixels; a copy
  may differ by at most 1/255 (it is compiled as a different shader, so rounding can move; leaving
  out an effect that is on changes far more, which the test catches).
  Building a copy takes ~1 s on the console, so since 0.66 the likely copies are queued when the
  program is linked and built ahead of use: under the game's loading screen during its long
  frames ("Preparing shaders N/M"), else between frames with a notice in the bottom-left corner
  (`precompile=0` turns this off). vitaGL keeps every compiled shader in `<data>/shader-cache/`
  (`HAVE_SHADER_CACHE` in `build_vita_vitagl.sh`), so later starts load them from the card; delete
  that folder to measure a first start.
- `bench=shaders` in config.txt (dev builds, 0.64): instead of the game, the shader benchmark
  (`runtime/bench/`, packed into the dev .vpk with PokeRogue's shaders) draws the full screen with
  PokeRogue's field and sprite shaders and Phaser's multi-texture shader, as shipped, as rewritten
  and with one part removed at a time (day/night, terrain, tint modes, texture pick, colour loops,
  tera, shadow, tone, mediump floats), then a plain texture read and a constant colour. Each line
  is the median GPU time of 12 full-screen draws, each waited for alone (the "empty" line is that
  wait's own cost). The variants are for measuring only. When it's done the log goes to the log
  server by itself, as kind `bench` ("benchmark" in the list) with "| shader benchmark" after the
  build. Remove the line to play again.

## Memory: texture eviction
Phaser never frees textures. When GPU texture memory exceeds 64 MB, image-backed textures not drawn
for 600 frames are shrunk to 1x1 and re-uploaded from the image's PNG data when next used (`gl.c`).
Each re-upload doubles that texture's idle time (up to 16x), so textures used every few waves stop
bouncing; when memory is short (over 96 MB, or vitaGL's pools nearly full, after which it would put
textures in the main heap) any texture idle for 90 frames goes. The `[perf]` line counts re-uploads.

## Threads
The game's JavaScript runs on one thread (CPU core 0) and can't be split. Off that thread: HTTP,
the disk cache and the offline game's zip (network thread, core 1), image decoding (core 1), audio
mixing (core 1), audio decoding (core 2), and writing the log and the profiler's profile.txt to the
card (0.69; each line written from the game's thread cost several ms). Decoders run below the network thread's priority: the
Vita doesn't time-slice threads of equal priority, and a busy decoder used to stall downloads.

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

## Tests
CI (`.github/workflows/ci.yml`) runs on every push and pull request; the same scripts run locally:

| script | what it checks |
|---|---|
| `runtime/scripts/lint.sh` | JS syntax (`node --check`), shellcheck, SPDX headers, `VERSION` format, no Sony modules committed |
| `runtime/scripts/test_linux.sh` | the test pages on the desktop runtime (build with `build_linux.sh` first) |
| `runtime/scripts/test_vita3k.sh` | the test pages on the .vpk in Vita3K (build with `build_vita.sh` first) |

Test pages are folders in `runtime/tests/`, loaded by the runtime as an offline game (no network,
no start screen). They log `[test] PASS/FAIL <name>` lines and finally `[test] DONE passed=N
failed=M`; `tests/run_page.sh` (desktop) and `tests/run_vita3k_page.sh` (Vita3K) wait for that line
and fail on any failed check, uncaught JS error or timeout. Logs and screenshots go to
`.cache/test/<name>/` and `.cache/test-vita3k/<name>/` (CI uploads them as artifacts).
- `tests/smoke/`: the browser shim: DOM, timers, fetch/XHR, `import()`, storage, PNG decoding,
  Canvas 2D, WebGL (incl. the Cg identifier renaming), WAV decoding.
- `tests/phaser/`: a still Phaser 3.90 scene built from what PokeRogue draws, checked with the
  `scripts/probe_verify_*.js` probes (native fast paths against Phaser's own code). Phaser comes
  from npm (`tests/fetch_test_deps.sh`, pinned hash).

In Vita3K the pages need a shader compiler: your own copy in `runtime/.cache/private/libshacccg.suprx`
(without it only the boot is checked: the eboot loads, links and runs `main()`). CI uses the PSM
runtime's copy from AnimMouse/SceShaccCg, pinned to a commit and hash-checked, only inside the job.
Vita3K doesn't write its rendering back to the Vita's memory, so `readPixels` returns zeros there;
the smoke test skips pixel comparisons on the Vita's GPU.

## Measuring the real game
See [tools/measure/README.md](../tools/measure/README.md): headless Chrome running a local dev
build of PokeRogue, with hooks that count what Phaser draws each frame.

## Sound
`js/audio.js` implements the part of the Web Audio API that Phaser's WebAudioSoundManager uses, on
top of `src/audio.c`:
- `decodeAudioData` hands the bytes to a decoder thread: WAV to 16-bit PCM, M4A/AAC (the cries,
  some battle sounds) via minimp4 + faad2 (`src/audio_m4a.c`), short MP3s decoded whole. Music
  (MP3s over 384 KB) stays compressed and is decoded while it plays, with its own decoder per
  playing voice (seeking for the game's loop points uses minimp3's frame index).
- The mixer (48 kHz stereo, 32 voices, linear-interpolation resampling) runs on the platform's
  audio thread: `sceAudioOut` on CPU core 1 on the Vita, SDL on the desktop (or a silent thread
  when there is no audio device; `SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=<file>` records the mix).
- Source start times are on the mixer's sample clock (`AudioContext.currentTime`), so Phaser's
  scheduled loop starts are sample-accurate. Gain and panner node chains are folded into a
  left/right gain per voice once per frame (only changes are sent).
- `audio=0` in config.txt turns sound off (the game then gets a silent Web Audio).
Desktop measurements (2026-10-15, offline build e4e9b53): JS time per frame and JS heap are the
same with and without sound; native memory for audio ~9 MB of decoded effects and cries plus the
current music track compressed (1-3 MB).
