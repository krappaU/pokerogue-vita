#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Reads the extended test's logs (.cache/ext/<scenario>/log.txt) and writes report.md next to them:
# per scenario how far the bot got and why it stopped, JS errors, the game's own error lines,
# out-of-memory lines, stalls, the slowest frames, memory, sprite unloading and the gamefix rules
# that applied. Exit status 1 if a scenario failed: no "[bot] done" line, the bot stuck or erring,
# an uncaught JS error or rejection, an out-of-memory line, or a scenario that should have reached
# its target and didn't (the game over one may also end at the title), or a wave-199 run that didn't
# end in a victory.
# Usage: report.py <dir> <scenario>...
import collections
import json
import os
import re
import sys

base, names = sys.argv[1], sys.argv[2:]
# reasons a scenario may end with: the target wave, or the title (a game over, the end of the run)
ALLOWED = {"wave199": {"target", "title"}, "gameover": {"target", "title"}}
# the game's own error lines seen in every log, not the runtime's doing
KNOWN_GAME_ERRORS = [r"^Texture key already in use: PRAS- ", r"^Manifest not found"]

rows, details, failed = [], [], []
for name in names:
    path = os.path.join(base, name, "log.txt")
    try:
        lines = open(path, errors="replace").read().split("\n")
    except OSError:
        rows.append(f"| {name} | FAIL | no log | | | | |")
        failed.append(name)
        continue
    bot = None
    for l in lines:
        if l.startswith("[bot] done "):
            try:
                bot = json.loads(l[len("[bot] done "):])
            except ValueError:
                bot = {"reason": "unreadable"}
    start = next((i for i, l in enumerate(lines) if l.startswith("[bot] started")), len(lines))
    after = lines[start:]
    js = [l for l in lines if l.startswith("[js] uncaught") or l.startswith("[js] unhandled")]
    oom = [l for l in lines if "out of memory" in l and not l.startswith("[bot]")]
    errs = collections.Counter(l[8:].strip() for l in lines if l.startswith("[error] "))
    game_errs = {k: v for k, v in errs.items() if any(re.search(p, k) for p in KNOWN_GAME_ERRORS)}
    other_errs = {k: v for k, v in errs.items() if k not in game_errs}
    watchdog = [l for l in after if l.startswith("[watchdog]")]
    slow = sorted((float(m.group(1)) for l in after for m in [re.match(r"\[perf\] slow frame: (\d+) ms", l)] if m), reverse=True)
    mem = [l for l in lines if l.startswith("[mem] running: heap in use")]
    gcs = [l for l in after if l.startswith("[gc]")]
    sprites = [l for l in lines if l.startswith("[sprites]")]
    fixes = sorted({x.strip() for l in lines if l.startswith("[gamefix]") for x in l.split(":", 1)[1].split(",")})
    reason = bot.get("reason") if bot else "no [bot] done line"
    ok_reasons = ALLOWED.get(name, {"target"})
    problems = []
    if not bot:
        problems.append("the bot never finished (crash or time limit)")
    elif reason not in ok_reasons:
        problems.append(f"ended with '{reason}'" + (f" ({bot.get('handler')}, mode {bot.get('mode')}: {bot.get('message', '')!r})" if reason == "stuck" else "")
                        + (f": {bot.get('error')}" if bot.get("error") else ""))
    if name == "wave199" and bot and bot.get("victory") is not True:
        problems.append("the final boss wasn't beaten (no victorious game over)")
    if js:
        problems.append(f"{len(js)} uncaught JS errors/rejections")
    if oom:
        problems.append(f"{len(oom)} out-of-memory lines")
    status = "FAIL" if problems else "pass"
    if problems:
        failed.append(name)
    waves = f"{bot.get('firstWave')} → {bot.get('lastWave')}" if bot else "-"
    mins = f"{bot.get('seconds', 0) / 60:.1f}" if bot else "-"
    last_mem = re.search(r"heap in use (\d+) MB \| JS (\d+) MB", mem[-1]) if mem else None
    rows.append(f"| {name} | {status} | {reason} | {waves} | {mins} | {len(js)} | "
                f"{(last_mem.group(2) + ' / ' + last_mem.group(1) + ' MB') if last_mem else '-'} |")
    d = [f"### {name}: {status}", ""]
    if problems:
        d += [f"- **{p}**" for p in problems]
    if bot and bot.get("waves"):
        per = ", ".join(f"{w}: {s} s/{t} turns" for w, s, *rest in bot["waves"] for t in [rest[0] if rest else "?"])
        d.append(f"- waves: {per}")
    if bot and bot.get("presses"):
        d.append(f"- button presses by UI mode: {bot['presses']}")
    for l in js[:5]:
        d.append(f"- `{l[:300]}`")
    for l in oom[:5]:
        d.append(f"- `{l[:200]}`")
    for k, v in sorted(other_errs.items(), key=lambda kv: -kv[1])[:8]:
        d.append(f"- error line ({v}x): `{k[:200]}`")
    if game_errs:
        d.append(f"- the game's known error lines: {sum(game_errs.values())}")
    if watchdog:
        d.append(f"- watchdog: {len(watchdog)} (first: `{watchdog[0][:150]}`)")
    if slow:
        d.append(f"- slow frames after the start: {len(slow)}, longest {', '.join(f'{x:.0f}' for x in slow[:5])} ms (desktop timings)")
    if gcs:
        d.append(f"- collections: {len(gcs)}; last `{gcs[-1][:150]}`")
    for l in sprites:
        d.append(f"- `{l[:200]}`")
    d.append(f"- gamefix rules applied: {len(fixes)}")
    d.append("")
    details += d

report = ["# Extended test", "", "| scenario | result | ended | waves | minutes | JS errors | JS / heap at the end |",
          "|---|---|---|---|---|---|---|"] + rows + [""] + details
open(os.path.join(base, "report.md"), "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if failed else 0)
