#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Prints the git commit the build comes from ("abc1234", or "abc1234-modified" with uncommitted
# changes to tracked files), for the version shown in the app and its log.
cd "$(dirname "$0")/.." || exit 1
id=$(git rev-parse --short=7 HEAD 2>/dev/null) || { echo unknown; exit 0; }
git diff --quiet HEAD -- . 2>/dev/null || id="$id-modified"
echo "$id"
