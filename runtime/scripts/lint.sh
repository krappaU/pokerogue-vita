#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Fast checks without building anything (CI runs them first): JS syntax, shell scripts, license
# headers, the version format, and that Sony's shader compiler is never committed.
# Needs node (18+) and shellcheck.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
rc=0
fail() { echo "lint: $*"; rc=1; }

# JS syntax (the runtime's shim, probes, test pages and tools); node detects ES modules itself.
for f in $(git ls-files '*.js'); do
  node --check "$f" 2>/tmp/lint-node.txt || fail "syntax error in $f: $(head -5 /tmp/lint-node.txt)"
done
# Shell scripts.
mapfile -t sh < <(git ls-files '*.sh')
shellcheck -S warning "${sh[@]}" || fail "shellcheck warnings (see above)"
# Every source file carries the license identifier (data files and images can't).
for f in $(git ls-files runtime tools archive | grep -vE '\.(md|png|json|patch|xml|txt|pem)$|/VERSION$'); do
  head -3 "$f" | grep -q 'SPDX-License-Identifier: AGPL-3.0-only' || fail "no SPDX-License-Identifier in $f"
done
grep -qE '^[0-9]+\.[0-9]+(\.[0-9]+)?$' runtime/VERSION || fail "runtime/VERSION must look like 1.1 or 1.0.2"
# libshacccg.suprx is Sony's; each user installs their own (CLAUDE.md). Never in the repository.
[ -z "$(git ls-files -- '*.suprx' '*.skprx')" ] || fail "Sony modules are committed: $(git ls-files -- '*.suprx' '*.skprx')"

[ $rc = 0 ] && echo "lint: ok"
exit $rc
