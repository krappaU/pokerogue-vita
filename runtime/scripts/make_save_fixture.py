#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Turns a PokeRogue run (an exported session, sessionData*.prsv, or its decrypted JSON) into a test
# fixture for scripts/run_saved_battle.sh: a localStorage snapshot of the offline game holding the run
# as the Guest's, with what could tell whose run it was scrambled. The run's random seed and every
# Pokemon id become fresh random ones (each id replaced wherever the run refers to it: items, battle
# tags...), its timestamp a fixed date, play time, money and score are rounded. Nicknames are dropped.
# The game rebuilds everything else (stats, the dex) from what is left, so the run plays the same:
# same party, items, wave and arena. No system data is included: the offline game makes a new Guest.
#
# Usage: scripts/make_save_fixture.py <run.prsv|run.json> <fixture.json> [--base snapshot.json]
#   --base: a snapshot whose settings, tutorials, key mapping and language go along (tutorials seen:
#           no tutorial messages on the way into the battle). Default: none.
# A .prsv is decrypted with the game's save key, read from the offline game's source maps
# (<data>/game.zip, PKR_DATA, default .cache/offline) with openssl.
import argparse
import base64
import json
import os
import random
import re
import string
import subprocess
import sys
import urllib.parse
import zipfile

FIXED_TIMESTAMP = 1793491200000  # 2026-11-01 00:00 UTC


def save_key(data_dir):
    z = zipfile.ZipFile(os.path.join(data_dir, "game.zip"))
    for name in z.namelist():
        if not name.endswith(".js.map"):
            continue
        m = json.loads(z.read(name))
        for src in m.get("sourcesContent") or []:
            k = re.search(r'export const saveKey = "([^"]+)"', src or "")
            if k:
                return k.group(1)
    sys.exit("no saveKey in the offline game's source maps")


def load_run(path, data_dir):
    raw = open(path, "rb").read().strip()
    if raw.startswith(b"{"):
        return json.loads(raw)
    out = subprocess.run(["openssl", "enc", "-d", "-aes-256-cbc", "-md", "md5", "-a", "-A", "-k", save_key(data_dir)],
                         input=raw.replace(b"\n", b""), capture_output=True, check=True).stdout
    return json.loads(out)


def scramble(run, rng):
    ids = {p["id"] for p in run.get("party", []) + run.get("enemyParty", []) if isinstance(p.get("id"), int)}
    new = {}
    for i in sorted(ids):
        while True:
            n = rng.randrange(1, 2**32)
            if n not in ids and n not in new.values():
                break
        new[i] = n

    def walk(v):
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return new.get(v, v)
        if isinstance(v, list):
            return [walk(x) for x in v]
        if isinstance(v, dict):
            return {k: walk(x) for k, x in v.items()}
        return v

    run = walk(run)
    alphabet = string.ascii_letters + string.digits
    if isinstance(run.get("seed"), str):
        run["seed"] = "".join(rng.choice(alphabet) for _ in run["seed"])
    run["timestamp"] = FIXED_TIMESTAMP
    for k, step in (("playTime", 3600), ("money", 100), ("score", 100)):
        if isinstance(run.get(k), int):
            run[k] = run[k] // step * step
    for p in run.get("party", []):
        p["nickname"] = None
    return run, len(new)


def offline_encode(obj):
    # the offline game's own encoding: base64 of encodeURIComponent(JSON)
    s = json.dumps(obj, separators=(",", ":"))
    return base64.b64encode(urllib.parse.quote(s, safe="-_.!~*'()").encode()).decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("out")
    ap.add_argument("--base")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    data_dir = os.environ.get("PKR_DATA", ".cache/offline")
    run, n = scramble(load_run(a.run, data_dir), random.Random(a.seed))
    snap = {}
    if a.base:
        base = json.load(open(a.base))
        snap = {k: base[k] for k in ("prLang", "mappingConfigs", "tutorials", "settings") if k in base}
    snap["sessionData_Guest"] = offline_encode(run)
    json.dump(snap, open(a.out, "w"), indent=1)
    print(f"{a.out}: wave {run.get('waveIndex')}, party {len(run.get('party', []))}, items {len(run.get('modifiers', []))}, "
          f"{n} ids replaced, game {run.get('gameVersion')}")


main()
