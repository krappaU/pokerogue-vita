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

## Measuring the real game
See [phase0/measure/README.md](../phase0/measure/README.md): headless Chrome running a local dev
build of PokeRogue, with hooks that count what Phaser draws each frame.
