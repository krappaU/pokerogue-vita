#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Publishes a dev build to the dev server: consoles are offered the newest published build.
#   publish.sh RUN_ID|RUN_URL|pokerogue-<version>-dev.vpk ["what to test"] [--commit SHA] [--branch B]
# Run it on the machine where the dev server runs (see README.md).
exec python3 "$(dirname "${BASH_SOURCE[0]}")/devserver.py" publish "$@"
