#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Send commands to a running runtime (started with --control .cache/control.txt).
# Usage: scripts/drive.sh "key Space" "shot .cache/shots/a.png" ...
cd "$(dirname "$0")/.."
mkdir -p .cache/shots
for c in "$@"; do
  case "$c" in
    sleep\ *) sleep "${c#sleep }" ;;
    *) printf '%s\n' "$c" >> .cache/control.txt ;;
  esac
done
