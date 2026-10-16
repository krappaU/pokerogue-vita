#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# From a fresh save of a login-bypass build (see docs/development.md): pick a gender, start a Classic
# run with the first two starters, and wait in the first battle's command menu.
cd "$(dirname "$0")" || exit 1
./drive.sh "key Space" "sleep 4" "key Space" "sleep 3" "key Space" "sleep 6" \
  "key Space" "sleep 1.5" "key Space" "sleep 1.5" "key ArrowRight" "sleep 1" "key Space" "sleep 1.5" "key Space" "sleep 1.5" \
  "key Enter" "sleep 2" "key Space" "sleep 4" "key Space" "sleep 20" "key Space" "sleep 6" "key Space" "sleep 6"
