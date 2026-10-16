#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Start the desktop runtime on the offline game with a saved run and drive it into that run's battle
# command menu ("What will ... do?"), for profiles and probes. Unlike drive_to_battle.sh (a fresh save
# through starter select, where slow desktop frames lose presses in menu animations), this needs two
# presses plus one per battle message, and checks the game's UI mode instead of trusting sleeps.
#
# Usage: scripts/run_saved_battle.sh [snapshot.json] [extra pokerogue-rt arguments]
#   snapshot: a localStorage-offline.json that holds a saved run (sessionData_Guest), default
#   .cache/session.json. Make one by playing any offline run to its first battle and copying
#   <data>/localStorage-offline.json; it is copied in again before every start, so runs repeat.
# The runtime keeps running in the background (container pkr-rt, or PKR_CONTAINER) with the control
# file .cache/control.txt; drive it further with drive.sh. Data folder: PKR_DATA, default .cache/offline
# (needs game.zip).
set -euo pipefail
cd "$(dirname "$0")/.."
SNAP=.cache/session.json
if [[ $# -gt 0 && $1 == *.json ]]; then SNAP=$1; shift; fi
DATA=${PKR_DATA:-.cache/offline}
[[ -f $SNAP ]] || { echo "no snapshot $SNAP (see the comment at the top)" >&2; exit 1; }
[[ -f $DATA/game.zip ]] || { echo "no $DATA/game.zip" >&2; exit 1; }
grep -q sessionData "$SNAP" || { echo "$SNAP holds no saved run" >&2; exit 1; }

cp "$SNAP" "$DATA/localStorage-offline.json"
: > .cache/control.txt
LOG=$DATA/log.txt
rm -f "$LOG"
nohup scripts/run_linux.sh --data "$DATA" --control .cache/control.txt "$@" > .cache/run_saved_battle.out 2>&1 &

# The start screen reads the control file too: "Play offline" (remembered choice, else preselected).
until [[ -f $LOG ]]; do sleep 1; done
scripts/drive.sh "sleep 2" "key Enter"

# Then: on the title screen (mode 1) Continue is preselected; in battle, each press moves the
# messages on (tutorial, "Go! ...") until the command menu (mode 2).
mode() {
    local tag=$1
    printf '%s\n' "eval (()=>{try{return 'pkrmode$tag='+__rt.renderer.game.scene.getScene('battle').ui.getMode()}catch(e){return 'pkrmode$tag=none'}})()" >> .cache/control.txt
    for _ in $(seq 40); do
        sleep 0.5
        local m
        m=$(grep -ao "pkrmode$tag=[a-z0-9]*" "$LOG" | tail -1 | cut -d= -f2) || true
        [[ -n $m ]] && { echo "$m"; return; }
    done
    echo timeout
}
start=$SECONDS
n=0
while true; do
    n=$((n + 1))
    m=$(mode $n)
    case $m in
        2) echo "in the command menu after $((SECONDS - start)) s"; exit 0 ;;
        1|0) scripts/drive.sh "key Space" "sleep 3" ;;
        *) sleep 3 ;;  # still loading (none), a transition, or a long frame
    esac
    if (( SECONDS - start > 300 )); then echo "no command menu after 300 s (last mode $m)" >&2; exit 1; fi
done
