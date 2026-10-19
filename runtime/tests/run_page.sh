#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Runs a test page (tests/<name>/) in the desktop runtime as an offline game and checks its result.
# The page logs "[test] PASS/FAIL ..." lines and finally "[test] DONE passed=N failed=M".
# Usage: tests/run_page.sh <name> [timeout seconds]   (inside an environment that has the runtime's
# dependencies and xvfb-run: the pkr-rt-linux image, see scripts/test_linux.sh)
# Served from .cache/test/<name>/www: the page's files, the verification probes (scripts/probe_verify_*.js)
# and test libraries fetched by tests/fetch_test_deps.sh (third_party/test/). Log, screenshot and
# data in .cache/test/<name>/.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
NAME=${1:?usage: tests/run_page.sh <name> [timeout]}
TIMEOUT=${2:-120}
RT=${RT:-build-linux/pokerogue-rt}
OUT=.cache/test/$NAME
[ -d "tests/$NAME" ] || { echo "no test page tests/$NAME"; exit 2; }
[ -x "$RT" ] || { echo "no runtime at $RT (build it first)"; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT/www" "$OUT/data"
cp -r "tests/$NAME/." "$OUT/www/"
cp scripts/probe_verify_*.js "$OUT/www/"
[ -d third_party/test ] && cp -r third_party/test/. "$OUT/www/"
# Offline mode from the page folder (no start screen, no network), no profiler.
printf 'local=%s\nprofile=0\n' "$PWD/$OUT/www" > "$OUT/data/config.txt"
LOG=$OUT/data/log.txt

# Twice: the second start loads the compiled code the first one wrote, as every start after the
# first does on the console (0.49-0.54 lost their fast paths only there: Phaser without source).
for pass in 1 2; do
: > "$OUT/data/control.txt"
rm -f "$LOG"
SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} xvfb-run -a -s "-screen 0 960x544x24" \
  "$RT" --js js --data "$OUT/data" --control "$OUT/data/control.txt" >"$OUT/stdout.txt" 2>&1 &
PID=$!

status=timeout
for _ in $(seq 1 $((TIMEOUT * 4))); do
  if grep -q '^\[test\] DONE' "$LOG" 2>/dev/null; then status=finished; break; fi
  if ! kill -0 $PID 2>/dev/null; then status=exited; break; fi
  sleep 0.25
done
if [ "$status" != exited ]; then
  echo "shot $PWD/$OUT/screen.png" >> "$OUT/data/control.txt"
  sleep 1
  echo quit >> "$OUT/data/control.txt"
  for _ in $(seq 1 40); do kill -0 $PID 2>/dev/null || break; sleep 0.25; done
  kill $PID 2>/dev/null
fi
wait $PID 2>/dev/null

echo "== $NAME: test results$([ "$pass" = 2 ] && echo " (from compiled code)")"
grep -E '^\[test\]' "$LOG" 2>/dev/null
# Errors the page didn't catch also fail the run.
errors=$(grep -cE '^\[js\] (uncaught|unhandled)|^\[boot\] failed|^bootstrap failed' "$LOG" 2>/dev/null)
if [ "$status" != finished ] || ! grep -q '^\[test\] DONE .*failed=0$' "$LOG" || [ "${errors:-0}" != 0 ]; then
  echo "== $NAME: FAILED ($status, $errors uncaught JS errors); last log lines:"
  tail -40 "$LOG" 2>/dev/null || cat "$OUT/stdout.txt"
  exit 1
fi
# A page that loads Phaser must get every checked fast path (js/fastpath.js logs which).
if grep -q "^\[fastpath\].*left to Phaser" "$LOG"; then
  echo "== $NAME: FAILED: fast paths not installed:"
  grep "^\[fastpath\]" "$LOG"
  exit 1
fi
done
echo "== $NAME: ok"
