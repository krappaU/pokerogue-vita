# CLAUDE.md

Guidance for Claude Code sessions working on this repository.

## Project
PS Vita homebrew that runs the live pokerogue.net client in an embedded QuickJS-ng runtime with a
minimal web-API shim, plus native fast paths for Phaser's render loop. Read `docs/PLAN.md` (design
and phases) and `docs/phase0-findings.md` (measurements and gotchas) before starting work.

Goal: parity with the online mode of Admiral-Billy's PokeRogue App. That means the live client with
auto-updates, the same rendering, gamepad controls, online saves, and the system keyboard for login.
Never fork or patch the game itself. Anything the game needs is provided by the runtime.

## The user
- A backend web developer in a non-JS language who knows a bit of JS, and is new to C, VitaSDK and
  graphics. Explain Vita and graphics specifics in plain terms.
- Owns a real PS Vita (firmware 3.65, HENkaku, VitaShell) and runs builds on it, then sends back
  logs or photos. `libshacccg.suprx` has not been extracted yet; it is needed from Phase 2.
- Works through the Claude desktop app against a remote Linux server. They cannot download files
  from the server directly. To hand over a build, publish an Artifact download page (the `downloads`
  capability allows `.zip` but not `.vpk`, so offer the .vpk as `.zip` and have the upload command
  name it `.vpk` on the Vita).

## Where things are
- `phase0/`: the feasibility benchmark (finished; reference only).
- `runtime/`: the actual app: C host (`src/`) + browser shim (`js/`). See `runtime/README.md` for
  building, running headless, the control channel (`scripts/drive.sh`: keys, screenshots, eval) and
  how to play without an account (a login-bypass build served on a Docker network).
- `docs/phase1-findings.md`: real-game performance profile and the optimization plan.

## How to work
- All toolchains run in Docker (see `docs/development.md`). The host has no gcc or node. `gh` is installed
  and logged in as the user (krappaU), and pushes to github.com/krappaU/pokerogue-vita.
- Runtime loop: `runtime/scripts/build_linux.sh`, run headless with `run_linux.sh --control
  .cache/control.txt` (in the background), then drive it with `drive.sh` and read the
  `.cache/shots/*.png` screenshots and `.cache/data/log.txt`. `[perf]` lines report JS time per frame.
- If Docker builds seem to ignore a source change, `touch` the file (a timestamp mismatch was seen once).
- Loop for Vita builds: build on Linux first for fast iteration → build the .vpk → `phase0/scripts/run_vita3k.sh`
  as a crash check → the user runs it on the real Vita for timing. **Never treat Vita3K timings as
  real performance.**
- Any native fast path that replaces Phaser code must be checked against Phaser's own output
  (see `bench.verifyFastpath()` in `phase0/js/bench.js`).
- Record measured results in the relevant README or `docs/`, with dates and game/Phaser versions.
- This server also runs unrelated services. Stop heavy containers when done
  (`docker rm -f pkr-browse`, since headless Chrome with software GL is CPU-heavy).

## Conventions
- License: AGPL-3.0-only, the same as PokeRogue. New source files start with an
  `SPDX-License-Identifier: AGPL-3.0-only` comment.
- C11. Match the existing style in `phase0/src` (4-space indent, `snake_case`, terse comments
  that explain why).
- Third-party code is fetched by scripts into ignored folders (`third_party/`, `data/`, `.cache/`),
  never committed.
