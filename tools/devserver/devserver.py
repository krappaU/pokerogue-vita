#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Dev server for PokeRogue Vita test builds: the console downloads new builds from it and uploads
its logs to it, so neither needs FTP (the app side is runtime/src/devtools.c; see README.md).

The console makes every connection, so this can run on any machine the console can reach over
the internet. Only the Python standard library is needed.

  GET  /build.json?key=K[&branch=B]  the newest test build: {"id", "commit", "label", "size"}
  GET  /build.vpk?key=K&id=ID        that build's .vpk (supports resuming with Range)
  POST /log?key=K&kind=prev|now      a log from the console, saved under <dir>/logs/
  GET  /logs/[NAME]?key=K|L          the saved logs (newest first), or one of them; L is --logs-key, a
                                     second key that only reads logs, for sharing with Claude sessions

Consoles are offered the newest *published* build, whatever branch it came from. Publishing is an
explicit step (publish.sh): it copies a dev .vpk, from a CI run or a file built on this machine, into
<dir>/published/ with a label saying what to test. Plain pushes and CI runs are never offered, so
work in progress on a branch can't replace the build under test. A console with devbranch=B in
config.txt (or the server's --branch B) gets the newest CI dev build of that branch instead.

  publish.sh RUN_ID|RUN_URL|FILE.vpk ["what to test"] [--commit SHA] [--branch B]
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
LOG_NAME = re.compile(r"(\d{8}-\d{6}-(prev|now)|latest(-prev|-now)?)\.txt")


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


class Published:
    """Builds put in <dir>/published/ by `publish`: <id>.vpk with <id>.json (label, commit, ...)."""

    def __init__(self, folder):
        self.dir = folder

    def newest(self, branch):
        best = None
        for n in os.listdir(self.dir):
            if not n.endswith(".json"):
                continue
            try:
                with open(os.path.join(self.dir, n)) as f:
                    meta = json.load(f)
                meta["id"] = n[:-5]  # from the file name, never from its content
                vpk = os.path.join(self.dir, meta["id"] + ".vpk")
                if os.path.isfile(vpk) and (best is None or meta["published_at"] > best["published_at"]):
                    best = {**meta, "path": vpk, "size": os.path.getsize(vpk)}
            except (OSError, ValueError, KeyError, TypeError):
                continue
        return best


class Sources:
    """Published builds by default; CI builds of a branch when one is asked for."""

    def __init__(self, published, github):
        self.published, self.github = published, github

    def newest(self, branch):
        if branch:
            return self.github.newest(branch) if self.github else None
        return self.published.newest(None)


def one_line(text, n=150):
    """The console's JSON reader has no escapes: plain printable ASCII only."""
    return re.sub(r"[^ -~]", "?", text).replace('"', "'").replace("\\", "/")[:n]


def publish_main(argv):
    ap = argparse.ArgumentParser(prog="publish.sh", description="Publish a dev build: consoles are offered the newest published one.")
    ap.add_argument("src", help="CI run id or URL (its pokerogue-dev-* artifact), or a pokerogue-<version>-dev.vpk file")
    ap.add_argument("label", nargs="*", help="what to test, shown on the console")
    ap.add_argument("--commit", help="commit of a local file (CI runs know theirs)")
    ap.add_argument("--branch", help="branch of a local file (CI runs know theirs)")
    ap.add_argument("--dir", default=os.path.expanduser("~/pokerogue-devserver"))
    ap.add_argument("--repo", default="krappaU/pokerogue-vita")
    a = ap.parse_args(argv)
    pub = os.path.join(a.dir, "published")
    builds = os.path.join(a.dir, "builds")
    os.makedirs(pub, exist_ok=True)
    os.makedirs(builds, exist_ok=True)
    m = re.fullmatch(r"(\d+)", a.src) or re.search(r"/actions/runs/(\d+)", a.src)
    if m and not os.path.exists(a.src):
        tok = github_token()
        if not tok:
            sys.exit("a CI run needs a GitHub token: set GITHUB_TOKEN or log in with `gh auth login`")
        gh = GitHub(a.repo, tok, builds)
        run_id = m.group(1)
        run = json.load(gh.get(f"/repos/{a.repo}/actions/runs/{run_id}"))
        arts = json.load(gh.get(f"/repos/{a.repo}/actions/runs/{run_id}/artifacts"))["artifacts"]
        art = next((x for x in arts if x["name"].startswith("pokerogue-dev-") and not x.get("expired")), None)
        if not art:
            sys.exit(f"run {run_id} has no pokerogue-dev-* artifact (is it finished? has it expired?)")
        build = {"id": f"run{run_id}", "label": art["name"], "path": os.path.join(builds, f"run{run_id}.vpk"), "artifact": art["id"]}
        gh._download(build)
        src, bid, version = build["path"], build["id"], art["name"].split("-")[2]
        commit, branch = run["head_sha"][:7], run["head_branch"]
    else:
        base = os.path.basename(a.src)
        m = re.fullmatch(r"pokerogue-(\d+\.\d+)-dev\.vpk", base)
        if not os.path.isfile(a.src) or not m:
            sys.exit(f"{a.src}: not a run id and not a pokerogue-<version>-dev.vpk file (normal builds can't update themselves)")
        h = hashlib.sha256()
        with open(a.src, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        src, bid, version = a.src, "local" + h.hexdigest()[:12], m.group(1)
        commit, branch = (a.commit or "")[:7], a.branch or ""
    head = " ".join(x for x in (version, branch, commit) if x)
    label = one_line(head + (" - " + " ".join(a.label) if a.label else ""))
    tmp = os.path.join(pub, bid + ".vpk.part")
    shutil.copyfile(src, tmp)
    os.replace(tmp, os.path.join(pub, bid + ".vpk"))
    meta = {"commit": commit, "branch": branch, "version": version, "label": label, "published_at": time.time()}
    with open(os.path.join(pub, bid + ".json"), "w") as f:
        json.dump(meta, f)
    print(f"published {bid}: {label}")
    print("consoles are now offered it at their next start")


def make_handler(key, source, logs_dir, default_branch, logs_key=None):
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
            u = urllib.parse.urlsplit(self.path)
            if u.path.startswith("/logs/") and logs_key:
                given = self.headers.get("X-Dev-Key") or urllib.parse.parse_qs(u.query).get("key", [""])[0]
                if hmac.compare_digest(given.encode(), logs_key.encode()):
                    return self.send_log(u.path[len("/logs/"):])
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
            if path.startswith("/logs/"):
                return self.send_log(path[len("/logs/"):])
            self.reply(404, {"error": "unknown path"})

        def send_log(self, name):
            names = sorted((n for n in os.listdir(logs_dir) if LOG_NAME.fullmatch(n)), reverse=True)
            if not name:
                aliases = [n for n in names if n.startswith("latest")]
                index = "".join(f"{n}\n" for n in aliases + [n for n in names if n not in aliases])
                return self.reply(200, index.encode(), "text/plain; charset=utf-8")
            if name not in names:  # only names listed above, so no path tricks
                return self.reply(404, {"error": "no such log"})
            with open(os.path.join(logs_dir, name), "rb") as f:
                self.reply(200, f.read(), "text/plain; charset=utf-8")
            log(f"sent log {name} to {self.client_address[0]}")

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
            threading.Thread(target=push_gist, args=(os.path.dirname(logs_dir),), daemon=True).start()
            self.reply(200, {"saved": name})

    return Handler


GIST_LOCK = threading.Lock()


def push_gist(folder):
    """Copy the newest logs to the private gist named in <dir>/gist-id, so cloud sessions can read
    them (their sandbox can't reach this server). Best effort: a failure only costs a log line."""
    try:
        with open(os.path.join(folder, "gist-id")) as f:
            gist = f.read().strip()
    except OSError:
        return
    files = {}
    for alias in ("latest.txt", "latest-prev.txt"):
        try:
            with open(os.path.join(folder, "logs", alias), "rb") as f:
                f.seek(0, 2)
                f.seek(max(0, f.tell() - (900 << 10)))  # the gist API truncates big files
                files[alias] = {"content": f.read().decode("utf-8", "replace") or "(empty)"}
        except OSError:
            pass
    try:
        with GIST_LOCK:
            r = subprocess.run(["gh", "api", "-X", "PATCH", "gists/" + gist, "--input", "-"],
                               input=json.dumps({"files": files}), capture_output=True, text=True, timeout=60)
        log("gist updated" if r.returncode == 0 else "gist update failed: " + r.stderr.strip()[:200])
    except (OSError, subprocess.SubprocessError) as e:
        log(f"gist update failed: {e}")


def github_token():
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok
    try:
        # systemd services get no HOME, where gh keeps its login.
        env = {**os.environ, "HOME": os.path.expanduser("~")}
        return subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=True, env=env).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "publish":
        return publish_main(sys.argv[2:])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--key", default=os.environ.get("DEVSERVER_KEY"), help="shared secret (or DEVSERVER_KEY)")
    ap.add_argument("--dir", default=os.path.expanduser("~/pokerogue-devserver"), help="logs/ and builds/ go here")
    ap.add_argument("--repo", default="krappaU/pokerogue-vita")
    ap.add_argument("--branch", help="offer the newest CI dev build of this branch, not the published one (a console can ask for another with devbranch=)")
    ap.add_argument("--logs-key", default=os.environ.get("DEVSERVER_LOGS_KEY"),
                    help="a second key that can only read the logs at /logs/ (or DEVSERVER_LOGS_KEY)")
    a = ap.parse_args()
    if not a.key or len(a.key) < 12 or not re.fullmatch(r"[A-Za-z0-9_-]+", a.key):
        sys.exit("give a key of 12+ letters, digits, - or _ with --key or DEVSERVER_KEY, e.g.:\n"
                 "  python3 -c 'import secrets; print(secrets.token_urlsafe(16))'")
    if a.logs_key and (len(a.logs_key) < 12 or not re.fullmatch(r"[A-Za-z0-9_-]+", a.logs_key) or a.logs_key == a.key):
        sys.exit("--logs-key needs 12+ letters, digits, - or _, and must differ from the key")
    logs_dir, builds_dir, pub_dir = (os.path.join(a.dir, n) for n in ("logs", "builds", "published"))
    for d in (logs_dir, builds_dir, pub_dir):
        os.makedirs(d, exist_ok=True)
    tok = github_token()
    gh = GitHub(a.repo, tok, builds_dir) if tok else None
    if a.branch and not gh:
        sys.exit("--branch serves CI builds, which need a GitHub token: set GITHUB_TOKEN or log in with `gh auth login`")
    source = Sources(Published(pub_dir), gh)
    log(f"offering " + (f"CI dev builds of branch {a.branch}" if a.branch else "the newest published build") +
        f" (publish with publish.sh; {'CI branches on request' if gh else 'no GitHub token: devbranch= is ignored'})")
    log(f"logs go to {logs_dir}")
    log(f"listening on {a.bind}:{a.port}")
    ThreadingHTTPServer((a.bind, a.port), make_handler(a.key, source, logs_dir, a.branch, a.logs_key)).serve_forever()


if __name__ == "__main__":
    main()
