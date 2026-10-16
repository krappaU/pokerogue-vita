# CLAUDE.md

Guidance for Claude Code sessions working on this repository.

## Project
PS Vita homebrew that runs the live pokerogue.net client in an embedded QuickJS-ng runtime with a
minimal web-API shim, plus native fast paths for Phaser's render loop. Read `docs/PLAN.md` (design,
phases, open items), `docs/development.md` (tooling and Vita gotchas) and the recent part of
`docs/phase1-findings.md` (the findings log) before starting work.

Goal: parity with the online mode of Admiral-Billy's PokeRogue App. That means the live client with
auto-updates, the same rendering, gamepad controls, online saves, and the system keyboard for login.
Never fork or patch the game itself. Anything the game needs is provided by the runtime.

## Where things are
- `archive/phase0/`: the original feasibility benchmark (history only; its findings are in
  `docs/phase0-findings.md`).
- `tools/measure/`: the real game in headless Chrome (reference renders, per-frame counts).
- `runtime/`: the actual app: C host (`src/`) + browser shim (`js/`). See `runtime/README.md` for
  building, running headless, the control channel (`scripts/drive.sh`: keys, screenshots, eval) and
  how to play without an account (the offline game in a data folder, `--data .cache/offline`).
- `docs/phase1-findings.md`: the findings log: measurements, device logs and why each change was made.

## How to work
- All toolchains run in Docker (see `docs/development.md`); the host only needs Docker, git, curl
  and python3.
- Runtime loop: `runtime/scripts/build_linux.sh`, run headless with `run_linux.sh --control
  .cache/control.txt` (in the background), then drive it with `drive.sh` and read the
  `.cache/shots/*.png` screenshots and `.cache/data/log.txt`. `[perf]` lines report JS time per frame.
- If Docker builds seem to ignore a source change, `touch` the file (a timestamp mismatch was seen once).
- Loop for Vita builds: build on Linux first for fast iteration → build the .vpk
  (`runtime/scripts/build_vita.sh`) → optionally `runtime/scripts/run_vita3k.sh` as a crash check →
  test on a real Vita for timing. **Never treat Vita3K timings as real performance.**
- vitaGL needs `libshacccg.suprx`, Sony's shader compiler, which each user installs on their own
  console. Never commit, bundle or publish it; for emulator tests keep a personal copy in
  `runtime/.cache/private/` (ignored by git).
- Any native fast path that replaces Phaser code must be checked against Phaser's own output
  (`runtime/scripts/probe_verify_render.js`, `probe_verify_uniforms.js`, `probe_verify_batching.js`).
- Record measured results in the relevant README or `docs/`, with dates and game/Phaser versions.
- Stop heavy containers when done (`docker rm -f pkr-rt pkr-browse pkr-serve`): headless rendering
  with software GL is CPU-heavy, and leftover runtimes skew performance measurements.

## Conventions
- License: AGPL-3.0-only, the same as PokeRogue. New source files start with an
  `SPDX-License-Identifier: AGPL-3.0-only` comment.
- C11. Match the existing style in `runtime/src` (4-space indent, `snake_case`, terse comments
  that explain why).
- Third-party code is fetched by scripts into ignored folders (`third_party/`, `data/`, `.cache/`),
  never committed.
