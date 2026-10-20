#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Log server for PokeRogue Vita dev builds: the console uploads its logs here, anyone can read them
(the app side is runtime/src/devtools.c; see README.md). Only the Python standard library is needed.

  POST /log?key=K&kind=prev|now|bench   a log from the console; each upload is a new entry (logN)
  GET  /logs                      the list, newest first (?format=json for JSON)
  GET  /logs/logN                 one log
  GET  /logs/latest               the newest log

Test builds come from the "dev-build" GitHub pre-release (.github/workflows/publish-dev.yml). For
dev builds from before that move, /build.json and /build.vpk still answer from that release.
"""

import argparse
import hmac
import json
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_LOG = 64 << 20
LOG_ID = re.compile(r"log(\d{1,9})(?:\.txt)?")
STORED = re.compile(r"log(\d{9})\.txt")
OLD_NAME = re.compile(r"(\d{8})-(\d{6})-(prev|now)\.txt")  # the previous server's names
KINDS = {"prev": "prev-run", "now": "in-game", "bench": "benchmark"}  # what the console sends -> what the list shows
DEV_RELEASE = "https://github.com/{repo}/releases/download/dev-build/build.json"


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class Logs:
    """<dir>/log000000057.txt with log000000057.json (time, kind, build, size) next to it."""

    def __init__(self, folder):
        self.dir = folder
        self.lock = threading.Lock()
        os.makedirs(folder, exist_ok=True)
        self.import_old()

    def path(self, n, ext):
        return os.path.join(self.dir, f"log{n:09d}.{ext}")

    def ids(self):
        return sorted((int(m.group(1)) for m in map(STORED.fullmatch, os.listdir(self.dir)) if m), reverse=True)

    def meta(self, n):
        try:
            with open(self.path(n, "json")) as f:
                m = json.load(f)
        except (OSError, ValueError):
            m = {}
        size = os.path.getsize(self.path(n, "txt"))
        return {"id": f"log{n}", "time": m.get("time", "?"), "kind": m.get("kind", "?"),
                "build": m.get("build", "?"), "size": size, "url": f"/logs/log{n}"}

    def add(self, data, kind, build, stamp=None):
        stamp = stamp or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        meta = {"time": stamp, "kind": kind, "build": build}
        with self.lock:
            ids = self.ids()
            n = (ids[0] if ids else 0) + 1
            for ext, body in (("json", json.dumps(meta).encode()), ("txt", data)):
                tmp = self.path(n, ext) + ".part"
                with open(tmp, "wb") as f:
                    f.write(body)
                os.replace(tmp, self.path(n, ext))
        return n

    def import_old(self):
        """Numbers the previous server's YYYYMMDD-HHMMSS-kind.txt logs, oldest first."""
        old = sorted(n for n in os.listdir(self.dir) if OLD_NAME.fullmatch(n))
        for name in old:
            d, t, kind = OLD_NAME.fullmatch(name).groups()
            p = os.path.join(self.dir, name)
            with open(p, "rb") as f:
                data = f.read()
            n = self.add(data, KINDS[kind], "?", f"{d[:4]}-{d[4:6]}-{d[6:]}T{t[:2]}:{t[2:4]}:{t[4:]}Z")
            os.remove(p)
            log(f"imported {name} as log{n}")
        for alias in ("latest.txt", "latest-prev.txt", "latest-now.txt"):
            if old and os.path.exists(os.path.join(self.dir, alias)):
                os.remove(os.path.join(self.dir, alias))


def listing(entries):
    rows = [("id", "uploaded (UTC)", "kind", "KB", "build")]
    rows += [(e["id"], e["time"].replace("T", " ").rstrip("Z"), e["kind"], str((e["size"] + 1023) // 1024), e["build"])
             for e in entries]
    w = [max(len(r[i]) for r in rows) for i in range(4)]
    return "".join("  ".join(r[i].ljust(w[i]) for i in range(4)) + "  " + r[4] + "\n" for r in rows)


def make_handler(key, logs, repo):
    def dev_build():
        with urllib.request.urlopen(DEV_RELEASE.format(repo=repo), timeout=30) as r:
            return json.load(r)

    class Handler(BaseHTTPRequestHandler):
        server_version = "pkr-devserver"

        def log_message(self, fmt, *args):
            pass  # uploads carry the key in the URL; we log our own lines instead

        def reply(self, code, body, ctype="application/json", extra=()):
            data = body if isinstance(body, bytes) else (body.encode() if isinstance(body, str) else json.dumps(body).encode())
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")  # Cloudflare must not keep an old "latest"
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def text(self, code, s):
            self.reply(code, s, "text/plain; charset=utf-8")

        def keyed(self, q):
            given = self.headers.get("X-Dev-Key") or q.get("key", "")
            return hmac.compare_digest(given.encode(), key.encode())

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
            path = u.path.rstrip("/") or "/"
            if path == "/":
                return self.text(200, __doc__)
            if path == "/logs":
                entries = [logs.meta(n) for n in logs.ids()]
                return self.reply(200, entries) if q.get("format") == "json" else self.text(200, listing(entries))
            if path.startswith("/logs/"):
                name = path[len("/logs/"):]
                ids = logs.ids()
                m = LOG_ID.fullmatch(name)
                n = (ids[0] if ids else None) if name == "latest" else int(m.group(1)) if m else None
                if n is None or n not in ids:
                    return self.text(404, "no such log\n")
                with open(logs.path(n, "txt"), "rb") as f:
                    return self.reply(200, f.read(), "text/plain; charset=utf-8", [("X-Log-Id", f"log{n}")])
            # Dev builds from before the builds moved to CI ask here; answer from the GitHub release.
            if path in ("/build.json", "/build.vpk"):
                if not self.keyed(q):
                    return self.reply(403, {"error": "wrong key"})
                try:
                    build = dev_build()
                except (OSError, ValueError) as e:
                    log(f"dev-build release unreachable: {e}")
                    return self.reply(404, {"error": "no build found"})
                if path == "/build.json":
                    return self.reply(200, {k: build.get(k) for k in ("id", "commit", "label", "size")})
                if q.get("id") != build.get("id"):
                    return self.reply(404, {"error": "unknown build; ask build.json first"})
                return self.reply(302, b"", extra=[("Location", build["url"])])
            self.text(404, "unknown path\n")

        def do_POST(self):
            u = urllib.parse.urlsplit(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
            if u.path.rstrip("/") not in ("/log", "/logs"):
                return self.text(404, "unknown path\n")
            if not self.keyed(q):
                log("refused an upload (wrong key)")
                return self.reply(403, {"error": "wrong key"})
            size = int(self.headers.get("Content-Length") or 0)
            if size <= 0 or size > MAX_LOG:
                return self.reply(413, {"error": "bad size"})
            data = self.rfile.read(size)
            # Logs are public: a key that ended up in one (a URL in an error line) is blanked.
            data = data.replace(key.encode(), b"<devkey>")
            build = re.sub(r"[^ -~]", "?", self.headers.get("X-Build", "?"))[:120]
            n = logs.add(data, KINDS.get(q.get("kind"), "in-game"), build)
            log(f"log{n}: {size // 1024} KB, {KINDS.get(q.get('kind'), 'in-game')}, {build}")
            self.reply(200, {"saved": f"log{n}", "url": f"/logs/log{n}"})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=int(os.environ.get("DEVSERVER_PORT", 8765)))
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--key", default=os.environ.get("DEVSERVER_KEY"), help="upload key (or DEVSERVER_KEY)")
    ap.add_argument("--dir", default=os.environ.get("DEVSERVER_DIR", os.path.expanduser("~/pokerogue-devserver")),
                    help="the logs go to <dir>/logs (or DEVSERVER_DIR)")
    ap.add_argument("--repo", default="krappaU/pokerogue-vita", help="where the dev-build release is")
    a = ap.parse_args()
    if not a.key or len(a.key) < 12 or not re.fullmatch(r"[A-Za-z0-9_-]+", a.key):
        sys.exit("give a key of 12+ letters, digits, - or _ with --key or DEVSERVER_KEY, e.g.:\n"
                 "  python3 -c 'import secrets; print(secrets.token_urlsafe(16))'")
    logs = Logs(os.path.join(a.dir, "logs"))
    log(f"logs in {logs.dir} ({len(logs.ids())} so far); listening on {a.bind}:{a.port}")
    ThreadingHTTPServer((a.bind, a.port), make_handler(a.key, logs, a.repo)).serve_forever()


if __name__ == "__main__":
    main()
