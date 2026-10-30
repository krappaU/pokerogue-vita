#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Runs build-vita/pokerogue-<VERSION>.vpk in Vita3K (headless) with a test page as the offline game
# and checks the app's log, like tests/run_page.sh does on the desktop runtime.
# Usage: tests/run_vita3k_page.sh <name> [timeout seconds]
# Needs the Vita3K runtime libraries and Xvfb (the pkrb-vita3k image's packages; CI installs them on
# its Ubuntu 24.04 runner). Results say nothing about speed: Vita3K is a crash and correctness check.
#
# Without .cache/private/libshacccg.suprx (each developer's own copy, never committed) vitaGL can't
# compile shaders, so only the boot is checked: the vpk installs, the eboot loads and links, main()
# runs and stops at the shader-compiler check with its message.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
NAME=${1:?usage: tests/run_vita3k_page.sh <name> [timeout]}
TIMEOUT=${2:-240}
VPK=build-vita/pokerogue-$(cat VERSION).vpk
V3K=.cache/vita3k                       # emulator (scripts/get_vita3k.sh)
OUT=$PWD/.cache/test-vita3k/$NAME       # fresh emulator home per run: log, screenshot
[ -f "$VPK" ] || { echo "no $VPK (build it first)"; exit 2; }
[ -d "tests/$NAME" ] || { echo "no test page tests/$NAME"; exit 2; }
[ -x "$V3K/squashfs-root/AppRun" ] || scripts/get_vita3k.sh || { echo "no Vita3K (scripts/get_vita3k.sh)"; exit 2; }

rm -rf "${OUT:?}"
FS=$OUT/home/.local/share/Vita3K/Vita3K
DATA=$FS/ux0/data/pokerogue
mkdir -p "$FS/ur0/data" "$DATA"
if [ -f .cache/private/libshacccg.suprx ]; then
  MODE=page
  cp .cache/private/libshacccg.suprx "$FS/ur0/data/"
  mkdir -p "$DATA/$NAME"
  cp -r "tests/$NAME/." "$DATA/$NAME/"
  cp scripts/probe_verify_*.js "$DATA/$NAME/"
  [ -d third_party/test ] && cp -r third_party/test/. "$DATA/$NAME/"
  printf 'local=%s\nprofile=0\nlog=1\n' "$NAME" > "$DATA/config.txt"
else
  MODE=boot
  echo "(no .cache/private/libshacccg.suprx: checking the boot only)"
  # fatal_wait=0: no missing-compiler message (Vita3K crashes on its side showing a display buffer
  # without libgxm; on the console the message waits for a button)
  printf 'log=1\nfatal_wait=0\n' > "$DATA/config.txt"
fi
# log=1: the .vpk under test is the player build, which writes no log otherwise.
LOG=$DATA/log.txt

export HOME=$OUT/home XDG_RUNTIME_DIR=/tmp SDL_AUDIODRIVER=dummy
DISP=:$((90 + RANDOM % 100))
Xvfb "$DISP" -screen 0 1280x800x24 >/dev/null 2>&1 &
XPID=$!
export DISPLAY=$DISP
sleep 1
(cd "$V3K" && exec squashfs-root/AppRun --frontend sdl -B OpenGL "$PWD/../../$VPK") >"$OUT/vita3k.log" 2>&1 &
EPID=$!

status=timeout
for _ in $(seq 1 $((TIMEOUT * 2))); do
  if grep -qa '^\[test\] DONE' "$LOG" 2>/dev/null; then status=finished; break; fi
  if ! kill -0 $EPID 2>/dev/null; then status=exited; break; fi
  sleep 0.5
done
command -v xwd >/dev/null && xwd -root -silent -display "$DISP" | convert xwd:- "$OUT/screen.png" 2>/dev/null
kill $EPID 2>/dev/null; sleep 1; pkill -P $EPID 2>/dev/null; kill $XPID 2>/dev/null
wait 2>/dev/null
# The results to keep sit next to the screenshot; Sony's module doesn't stay in the emulator's files.
rm -f "$FS/ur0/data/libshacccg.suprx"
cp "$LOG" "$OUT/app-log.txt" 2>/dev/null

fail() {
  echo "== vita3k $NAME: FAILED: $1; app log:"
  cat "$LOG" 2>/dev/null || echo "(no app log)"
  echo "== last emulator log lines:"
  grep -aE '\|[EC]\||exited with code' "$OUT/vita3k.log" | grep -v 'Missing file\|remove_file' | tail -20
  exit 1
}
grep -qa '^PokeRogue Vita ' "$LOG" 2>/dev/null || fail "the app never started"
grep -qa '^\[mem\] start:' "$LOG" || fail "no memory report"
if [ "$MODE" = boot ]; then
  grep -qa 'libshacccg.suprx not found' "$LOG" || fail "did not reach the shader compiler check"
  grep -qa 'The app exited with code 1' "$OUT/vita3k.log" || fail "the app did not exit cleanly"
  echo "== vita3k $NAME: boot ok (eboot loads, links and runs main(); stops without libshacccg.suprx as expected)"
  exit 0
fi
echo "== vita3k $NAME: test results"
grep -aE '^\[test\]' "$LOG"
errors=$(grep -caE '^\[js\] (uncaught|unhandled)|^\[boot\] failed|^bootstrap failed' "$LOG")
[ "$status" = finished ] || fail "$status before the page finished"
grep -qa '^\[test\] DONE .*failed=0$' "$LOG" || fail "failed checks"
[ "${errors:-0}" = 0 ] || fail "$errors uncaught JS errors"
echo "== vita3k $NAME: ok"
