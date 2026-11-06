#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Downloads the offline game for the extended test (scripts/test_extended.sh): game.zip from the
# latest release of Admiral-Billy's PokeRogue App (github.com/Admiral-Billy/pokerogue, the same file
# the start screen's "Download offline game" gets; ~530 MB). Kept if the tag is unchanged.
# Usage: scripts/fetch_offline_game.sh [dir]   (default .cache/offline-game; prints the zip's path)
# GH_TOKEN, if set, is sent to GitHub's API (CI: more requests allowed).
set -euo pipefail
cd "$(dirname "$0")/.."
DIR=${1:-.cache/offline-game}
mkdir -p "$DIR"
auth=()
[[ -n ${GH_TOKEN:-} ]] && auth=(-H "Authorization: Bearer $GH_TOKEN")
json=$(curl -sfL "${auth[@]}" https://api.github.com/repos/Admiral-Billy/pokerogue/releases/latest)
tag=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])' <<< "$json")
url=$(python3 -c 'import json,sys; print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if a["name"] == "game.zip"))' <<< "$json")
if [[ -f $DIR/game.zip && "$(cat "$DIR/game.zip.tag" 2>/dev/null)" == "$tag" ]]; then
  echo "$DIR/game.zip" # $tag, kept
  exit 0
fi
echo "downloading the offline game $tag ($url)" >&2
curl -sfL --retry 3 -o "$DIR/game.zip.part" "$url"
mv "$DIR/game.zip.part" "$DIR/game.zip"
echo "$tag" > "$DIR/game.zip.tag"
echo "$DIR/game.zip"
