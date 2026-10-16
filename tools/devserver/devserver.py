#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Dev server for PokeRogue Vita test builds: the console downloads new builds from it and uploads
its logs to it, so neither needs FTP (the app side is runtime/src/devtools.c; see README.md).

The console makes every connection, so this can run on any machine the console can reach over
the internet. Only the Python standard library is needed.

  GET  /build.json?key=K[&branch=B]  the newest test build: {"id", "commit", "label", "size"}
  GET  /build.vpk?key=K&id=ID        that build's .vpk (supports resuming with Range)
  POST /log?key=K&kind=prev|now      a log from the console, saved under <dir>/logs/

Builds come from the newest CI run (push events) that uploaded a dev .vpk artifact, on any branch or
on --branch / the console's devbranch=. With --vpk, a local file (or the newest .vpk in a folder,
e.g. runtime/build-vita) is served instead, for builds made on this machine.
"""

import argparse
import hashlib
import hmac
import io
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API = "https://api.github.com"
MAX_LOG = 64 << 20
CACHE_S = 60  # how long the newest-build answer is reused


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class GitHub:
    """The newest CI .vpk, downloaded once per build into <dir>/builds/."""

    def __init__(self, repo, token, builds_dir):
        self.repo, self.token, self.dir = repo, token, builds_dir
        self.lock = threading.Lock()
        self.cache = {}  # branch -> (time, build)

    def get(self, path, redirect=True):
        req = urllib.request.Request(API + path, headers={
            "Authorization": "Bearer " + self.token,
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        })
        opener = urllib.request.build_opener() if redirect else urllib.request.build_opener(NoRedirect)
        try:
            return opener.open(req, timeout=60)
        except urllib.error.HTTPError as e:
            if not redirect and e.code in (301, 302, 303, 307, 308):
                return e
            raise

    def newest(self, branch):
        with self.lock:
            hit = self.cache.get(branch)
            if hit and time.time() - hit[0] < CACHE_S:
                return hit[1]
            build = self._find(branch)
            self.cache[branch] = (time.time(), build)
            return build

    def _find(self, branch):
        q = {"event": "push", "per_page": "15"}
        if branch:
            q["branch"] = branch
        runs = json.load(self.get(f"/repos/{self.repo}/actions/workflows/ci.yml/runs?" + urllib.parse.urlencode(q)))
        for run in runs.get("workflow_runs", []):
            arts = json.load(self.get(f"/repos/{self.repo}/actions/runs/{run['id']}/artifacts"))
            for a in arts.get("artifacts", []):
                # Only dev builds: they keep the dev server client, so the next update works too.
                if not a["name"].startswith("pokerogue-dev-") or a.get("expired"):
                    continue
                version = a["name"].split("-")[2]
                sha = run["head_sha"][:7]
                title = (run.get("display_title") or "")[:60]
                build = {
                    "id": f"run{run['id']}",
                    "commit": sha,
                    "label": f"{version} {run['head_branch']} {sha} {title}".strip(),
                    "path": os.path.join(self.dir, f"run{run['id']}.vpk"),
                    "artifact": a["id"],
                }
                self._download(build)
                build["size"] = os.path.getsize(build["path"])
                return build
        return None

    def _download(self, build):
        if os.path.exists(build["path"]):
            return
        log(f"downloading {build['label']}")
        r = self.get(f"/repos/{self.repo}/actions/artifacts/{build['artifact']}/zip", redirect=False)
        url = r.headers["Location"]
        # The artifact itself is on blob storage, which must not get our GitHub token.
        data = urllib.request.urlopen(url, timeout=300).read()
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            name = next(n for n in z.namelist() if n.endswith(".vpk"))
            tmp = build["path"] + ".part"
            with z.open(name) as src, open(tmp, "wb") as dst:
                shutil.copyfileobj(src, dst)
            os.replace(tmp, build["path"])


class LocalVpk:
    """A .vpk built on this machine: a file, or the newest .vpk in a folder."""

    def __init__(self, path):
        self.path = path

    def newest(self, branch):
        p = self.path
        if os.path.isdir(p):
            vpks = [os.path.join(p, n) for n in os.listdir(p) if n.endswith(".vpk")]
            if not vpks:
                return None
            p = max(vpks, key=os.path.getmtime)
        if not os.path.isfile(p):
            return None
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        stamp = datetime.fromtimestamp(os.path.getmtime(p)).strftime("%Y-%m-%d %H:%M")
        # No commit: the console recognises the build by its id once installed.
        return {"id": "local" + h.hexdigest()[:12], "commit": "", "label": f"{os.path.basename(p)} built {stamp}",
                "path": p, "size": os.path.getsize(p)}


def make_handler(key, source, logs_dir, default_branch):
    offered = {}  # id -> build, for the download that follows build.json

    class Handler(BaseHTTPRequestHandler):
        server_version = "pkr-devserver"

        def log_message(self, fmt, *args):
            pass  # requests carry the key in the URL; we log our own lines instead

        def reply(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def parse(self):
            u = urllib.parse.urlsplit(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
            given = self.headers.get("X-Dev-Key") or q.get("key", "")
            if not hmac.compare_digest(given.encode(), key.encode()):
                log(f"refused {u.path} from {self.client_address[0]} (wrong key)")
                self.reply(403, {"error": "wrong key"})
                return None, None
            return u.path, q

        def do_GET(self):
            path, q = self.parse()
            if path is None:
                return
            if path == "/build.json":
                try:
                    build = source.newest(q.get("branch") or default_branch)
                except Exception as e:  # GitHub unreachable, bad token...
                    log(f"finding the newest build failed: {e}")
                    return self.reply(502, {"error": str(e)})
                if not build:
                    return self.reply(404, {"error": "no build found"})
                offered[build["id"]] = build
                log(f"console asked for a build: offering {build['label']} ({build['id']})")
                return self.reply(200, {k: build[k] for k in ("id", "commit", "label", "size")})
            if path == "/build.vpk":
                build = offered.get(q.get("id"))
                if not build or not os.path.isfile(build["path"]):
                    return self.reply(404, {"error": "unknown build; ask build.json first"})
                return self.send_file(build["path"])
            self.reply(404, {"error": "unknown path"})

        def send_file(self, p):
            size = os.path.getsize(p)
            start = 0
            m = re.match(r"bytes=(\d+)-$", self.headers.get("Range", ""))
            if m:
                start = int(m.group(1))
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    return
            self.send_response(206 if start else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size - start))
            if start:
                self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
            self.end_headers()
            with open(p, "rb") as f:
                f.seek(start)
                shutil.copyfileobj(f, self.wfile)
            log(f"sent {os.path.basename(p)} from byte {start}")

        def do_POST(self):
            path, q = self.parse()
            if path is None:
                return
            if path != "/log":
                return self.reply(404, {"error": "unknown path"})
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0 or n > MAX_LOG:
                return self.reply(413, {"error": "bad size"})
            data = self.rfile.read(n)
            kind = q.get("kind", "now")
            kind = kind if kind in ("prev", "now") else "now"
            stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
            name = f"{stamp}-{kind}.txt"
            with open(os.path.join(logs_dir, name), "wb") as f:
                f.write(data)
            # latest.txt: the newest log; latest-prev.txt: the newest log of a run that has ended
            # (crashed, or quit), uploaded by the run after it.
            for alias in ("latest.txt", f"latest-{kind}.txt"):
                with open(os.path.join(logs_dir, alias), "wb") as f:
                    f.write(data)
            build = self.headers.get("X-Build", "?")
            what = "previous run" if kind == "prev" else "sent from the game"
            log(f"log received ({what}, {n // 1024} KB, {build}): {os.path.join(logs_dir, name)}")
            self.reply(200, {"saved": name})

    return Handler


def github_token():
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok
    try:
        return subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--key", default=os.environ.get("DEVSERVER_KEY"), help="shared secret (or DEVSERVER_KEY)")
    ap.add_argument("--dir", default=os.path.expanduser("~/pokerogue-devserver"), help="logs/ and builds/ go here")
    ap.add_argument("--repo", default="krappaU/pokerogue-vita")
    ap.add_argument("--branch", help="only builds of this branch, unless the console asks for another")
    ap.add_argument("--vpk", help="serve this .vpk (or the newest .vpk in this folder) instead of CI builds")
    a = ap.parse_args()
    if not a.key or len(a.key) < 12 or not re.fullmatch(r"[A-Za-z0-9_-]+", a.key):
        sys.exit("give a key of 12+ letters, digits, - or _ with --key or DEVSERVER_KEY, e.g.:\n"
                 "  python3 -c 'import secrets; print(secrets.token_urlsafe(16))'")
    logs_dir, builds_dir = os.path.join(a.dir, "logs"), os.path.join(a.dir, "builds")
    os.makedirs(logs_dir, exist_ok=True)
    os.makedirs(builds_dir, exist_ok=True)
    if a.vpk:
        source = LocalVpk(os.path.abspath(a.vpk))
        log(f"serving local builds from {source.path}")
    else:
        tok = github_token()
        if not tok:
            sys.exit("CI builds need a GitHub token: set GITHUB_TOKEN, log in with `gh auth login`, or use --vpk")
        source = GitHub(a.repo, tok, builds_dir)
        log(f"serving CI builds of {a.repo}" + (f", branch {a.branch}" if a.branch else ""))
    log(f"logs go to {logs_dir}")
    log(f"listening on {a.bind}:{a.port}")
    ThreadingHTTPServer((a.bind, a.port), make_handler(a.key, source, logs_dir, a.branch)).serve_forever()


if __name__ == "__main__":
    main()
