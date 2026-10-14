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
- Vita3K is for crash checks only: it does not emulate the Vita's CPU speed.

## Measuring the real game
See [phase0/measure/README.md](../phase0/measure/README.md): headless Chrome running a local dev
build of PokeRogue, with hooks that count what Phaser draws each frame.
