#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# The extended test: the desktop runtime plays the offline game for a while in several scenarios
# (tests/extended/autoplay.js presses the game's buttons), then tests/extended/report.py reads each
# log for errors, stalls and memory and writes a report. For releases, not every build: it takes
# most of an hour (.github/workflows/extended.yml runs it on demand).
#
# Usage: scripts/test_extended.sh [scenario...]   (default: all; see SCENARIOS below)
#   PKR_GAME_ZIP   the offline game (default .cache/offline/game.zip; scripts/fetch_offline_game.sh)
#   PKR_EXT_SCALE  multiplies every scenario's waves and time limit (default 1)
# Needs the desktop build (scripts/build_linux.sh). Results: .cache/ext/<scenario>/ (log.txt,
# end.png) and .cache/ext/report.md; the exit status is 1 if a scenario failed.
set -uo pipefail
cd "$(dirname "$0")/.."
ZIP=${PKR_GAME_ZIP:-.cache/offline/game.zip}
SCALE=${PKR_EXT_SCALE:-1}
OUT=.cache/ext
NAME=${PKR_CONTAINER:-pkr-ext}
[[ -f $ZIP ]] || { echo "no offline game at $ZIP (scripts/fetch_offline_game.sh)" >&2; exit 2; }
[[ -x build-linux/pokerogue-rt ]] || { echo "no desktop build (scripts/build_linux.sh)" >&2; exit 2; }

# name | snapshot ("" = a fresh save; "double" = the wave-163 run with a second wild Pokemon) |
# waves | heal | config.txt lines | minutes allowed | what it covers
SCENARIOS=(
  "fresh||6|1||25|a new save: title, New Game, Classic, starter select, the first waves"
  "wave163|fixtures/run-wave163.json|12|1||45|a late run (12 waves, items, a biome change and its sprite unloading)"
  "wave199|fixtures/run-wave199.json|3|1||25|the last wave: the final boss, the end of the run, back to the title"
  "double|double|4|1||25|double battles (the enemy AI's move choice for two)"
  "nogamefix|fixtures/run-wave163.json|4|1|gamefix=0|25|the game as published: config.txt gamefix=0"
  "gameover|fixtures/run-wave163.json|8|0||30|no healing: a lost battle, the game over, back to the title"
)
want=("$@")

mkdir -p "$OUT"
docker rm -f "$NAME" >/dev/null 2>&1
ran=()
for s in "${SCENARIOS[@]}"; do
  IFS='|' read -r name snap waves heal conf minutes what <<< "$s"
  if [[ ${#want[@]} -gt 0 && ! " ${want[*]} " =~ " $name " ]]; then continue; fi
  waves=$(( waves * SCALE )); minutes=$(( minutes * SCALE ))
  dir=$OUT/$name
  rm -rf "$dir"; mkdir -p "$dir"
  ln "$ZIP" "$dir/game.zip" 2>/dev/null || cp "$ZIP" "$dir/game.zip"
  printf 'fps30=0\nsample=0\n%s\n' "$conf" > "$dir/config.txt"
  echo offline > "$dir/launcher.txt"
  case "$snap" in
    "") ;;
    double) python3 tests/extended/make_double.py fixtures/run-wave163.json "$dir/localStorage-offline.json" ;;
    *) cp "$snap" "$dir/localStorage-offline.json" ;;
  esac
  : > "$dir/control.txt"
  echo "== $name: $what ($waves waves, up to $minutes min)"
  PKR_CONTAINER=$NAME nohup scripts/run_linux.sh --data "$dir" --control "$dir/control.txt" > "$dir/stdout.txt" 2>&1 &
  for _ in $(seq 60); do [[ -f $dir/log.txt ]] && break; sleep 1; done
  # the start screen (offline preselected), then wait until the game's scene and UI exist
  sleep 3; echo "key Enter" >> "$dir/control.txt"
  ready=0
  for i in $(seq 120); do
    echo "eval (()=>{try{const s=__rt.renderer.game.scene.getScene('battle');return 'pkrready'+$i+'='+(s&&s.ui&&s.ui.handlers&&s.ui.handlers.length?1:0)}catch(e){return 'pkrready'+$i+'=0'}})()" >> "$dir/control.txt"
    sleep 3
    if grep -aq "pkrready$i=1" "$dir/log.txt" 2>/dev/null; then ready=1; break; fi
  done
  if [[ $ready = 1 ]]; then
    printf '%s\n' "eval (globalThis.__botConfig={waves:$waves,heal:$([[ $heal = 1 ]] && echo true || echo false)},'cfg')" \
      "evalfile tests/extended/autoplay.js" >> "$dir/control.txt"
    end=$(( SECONDS + minutes * 60 ))
    while (( SECONDS < end )); do
      grep -aq '^\[bot\] done' "$dir/log.txt" && break
      docker ps -q -f "name=^$NAME\$" | grep -q . || break # the runtime ended (a crash)
      sleep 5
    done
  else
    echo "   the game didn't come up"
  fi
  echo "shot $dir/end.png" >> "$dir/control.txt"; sleep 3
  docker rm -f "$NAME" >/dev/null 2>&1
  rm -f "$dir/game.zip"
  grep -a '^\[bot\] done' "$dir/log.txt" | cut -c1-200 || echo "   no [bot] done line"
  ran+=("$name")
done
[[ ${#ran[@]} -gt 0 ]] || { echo "no such scenario: ${want[*]}" >&2; exit 2; }
python3 tests/extended/report.py "$OUT" "${ran[@]}"
