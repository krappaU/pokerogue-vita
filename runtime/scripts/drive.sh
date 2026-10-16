#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Send commands to a running runtime (started with --control .cache/control.txt).
# Usage: scripts/drive.sh "key Space" "sleep 2" "shot .cache/shots/a.png" ...
# A sleep is also sent as "wait": the runtime keeps that pause after the previous command even when
# a long frame made the commands pile up.
cd "$(dirname "$0")/.." || exit 1
mkdir -p .cache/shots
for c in "$@"; do
  case "$c" in
    sleep\ *) printf 'wait %s\n' "${c#sleep }" >> .cache/control.txt; sleep "${c#sleep }" ;;
    *) printf '%s\n' "$c" >> .cache/control.txt ;;
  esac
done
