# Phase 0: QuickJS-on-Vita feasibility benchmark

> **Archived.** This was the feasibility benchmark that decided the project could work (see
> [docs/phase0-findings.md](../../docs/phase0-findings.md)). It is kept for history and is not
> maintained: paths below are as they were (`phase0/`), and its measuring tools now live in
> [tools/measure](../../tools/measure).

Answers one question before the real port is built: is the Vita's CPU fast enough to run
PokeRogue's JavaScript in QuickJS-ng (an interpreter, no JIT)?

- **Test 1:** compiles the real, current pokerogue.net startup code (about 6 MB), then
  saves and reloads it as bytecode. That is the planned cache for fast startup.
- **Test 2:** runs Phaser 3.90 (the engine PokeRogue uses) on a fake WebGL context with a
  synthetic battle screen, and reports milliseconds per frame. GL calls do nothing, so this
  is the JavaScript-only cost of a frame.

## Build (on the dev machine)
```
./scripts/fetch_deps.sh     # QuickJS-ng, Phaser, live PokeRogue chunks, Vita debug-text helper
./scripts/build_linux.sh    # desktop build + run (reference numbers)
docker run --rm -v "$PWD":/w -w /w pkrb-linux node scripts/node-driver.js   # same scene in V8
./scripts/build_vita.sh     # -> build-vita/pokerogue_bench.vpk
```

## Run on the Vita
1. HENkaku Settings (in the Settings app) → **Enable Unsafe Homebrew**.
2. VitaShell → press **SELECT** to start FTP. Note the address it shows.
3. Upload from the Mac (Finder's FTP is read-only, use curl):
   `curl -T pokerogue_bench.vpk ftp://<VITA_IP>:1337/ux0:/data/`
4. VitaShell → `ux0:data/` → X on the vpk → install, and accept the extended-permissions warning.
5. Start **PokeRogue Bench** from the home screen. It takes about a minute, then shows "Press START".
6. Get the log: `curl ftp://<VITA_IP>:1337/ux0:/data/pokerogue-bench/log.txt -o log.txt`
   (or just take a photo of the screen).

## Results

### Round 1 (2026-10-14): pure JS, Thumb `-O2` build
| | x86 QuickJS | x86 Node/V8 | Vita3K (emulated, NOT representative) | **real Vita** |
|---|---|---|---|---|
| compile 6 MB | 0.89 s | n/a | 5.9 s | ? |
| idle frame (166 objs) | 3.9 ms | 0.37 ms | 18 ms | **57 ms** |
| battle frame | 4.0 ms | 0.32 ms | 20 ms | **59.5 ms** |
| heavy frame (480 objs) | 11.3 ms | 0.80 ms | 60 ms | **166 ms** |

The real Vita was about 15x slower than x86 QuickJS. The per-frame breakdown showed game
logic at about 3% and Phaser's render path at about 95%, mostly per-sprite batching.

### Round 2: native fast path + ARM `-O2 -mcpu=cortex-a9` build
`js/fastpath.js` + `src/fastpath.c` move sprite matrix math and vertex writes to C, and skip
Phaser's per-child setAlpha/setScrollFactor round trip in containers. A check compares the
vertex output bit for bit against Phaser's own code on the same scene: 0 differences.
The "js" rows measure the same workload as round 1, so they isolate the effect of the compiler flags.

| real Vita, round 2 | idle | battle | heavy (480 objs) |
|---|---|---|---|
| js (Phaser as shipped) | 55.4 ms | 57.8 ms | 161 ms |
| native fast path | **25.1 ms** | **27.4 ms** | **73.5 ms** |

On x86 QuickJS the battle frame was 3.7 → 1.65 ms; Vita3K showed 26.7 → 11.3 ms.
The ARM flags alone were worth about 3% (59.5 → 57.8 ms). The fast path was worth 2.1x.
Frame times on the real Vita are very stable (p95 within 1 ms of the average).
Startup on the real Vita: parsing the 6 MB of game JS takes **14.8 s**, while loading the cached
bytecode takes **0.94 s**, so the bytecode cache is essential.

In the profiled "native" breakdown, batchSprite shows about 0 calls. That's expected: containers
now call the native batcher directly.

How to read the real-Vita battle frame time (the GPU and text drawing will add cost on top):
- **≤ 25 ms:** good. 30 fps looks achievable.
- **25–50 ms:** workable, but needs more native C fast paths for Phaser's hot functions.
- **> 60 ms:** serious trouble. Rethink before building the full runtime.
