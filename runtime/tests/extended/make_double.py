#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# A double battle for the extended test: a saved run (localStorage-offline.json snapshot) whose wild
# battle gets a second enemy, a copy of the player's third Pokemon at the enemy's level. The game
# starts a saved wild battle as a double when its enemy party has two Pokemon.
# Usage: make_double.py <snapshot.json> <out.json>
import base64
import copy
import json
import sys
import urllib.parse

src, out = sys.argv[1], sys.argv[2]
d = json.load(open(src))
# the offline game's saves: base64 of the URI-encoded JSON
s = json.loads(urllib.parse.unquote(base64.b64decode(d["sessionData_Guest"]).decode()))
e = s["enemyParty"][0]
p = copy.deepcopy(s["party"][2 % len(s["party"])])
p.update(player=False, id=e["id"] + 7, boss=False, bossSegments=0, level=e["level"])
s["enemyParty"] = [e, p]
enc = urllib.parse.quote(json.dumps(s, separators=(",", ":")), safe="~()*!.'")
d["sessionData_Guest"] = base64.b64encode(enc.encode()).decode()
json.dump(d, open(out, "w"))
