# Log server

Receives the logs that dev builds (`PKR_DEV=1`) upload, and shows them to anyone. The app side is
`runtime/src/devtools.c`; the console part is described in `docs/development.md` ("Test builds and
logs without FTP"). Test builds don't come from here any more: they are published to the
`dev-build` GitHub pre-release by `.github/workflows/publish-dev.yml`.

| request | |
|---|---|
| `POST /log?key=K&kind=prev\|now` | an upload from the console (the key can also be the `X-Dev-Key` header); each is a new entry |
| `GET /logs` | the list, newest first (`?format=json` for JSON) |
| `GET /logs/log57` | one log |
| `GET /logs/latest` | the newest log (its id is in the `X-Log-Id` header) |

Ids are `log1`, `log2`, ... in upload order, never reused. Time (UTC), kind (`prev-run`: the log of
a run that ended, sent by the next start; `in-game`: L + R + START) and the build that sent it are
in the list. Logs are stored as `<data>/logs/log000000057.txt` with a `.json` beside it. If the
upload key appears in a log (a URL in an error line), it is replaced by `<devkey>` before saving,
since reading is public.

Dev builds from before builds moved to CI (0.49 and older) still ask `GET /build.json` and
`/build.vpk` here; the server answers from the `dev-build` release, so they update themselves.

## Running it (Docker Compose with a Cloudflare tunnel)
1. In Cloudflare Zero Trust: Networks -> Tunnels -> Create a tunnel (cloudflared). Copy the token
   from the install command. Add a public hostname (e.g. `vita-logs.example.com`) with the service
   `http://devserver:8765`.
2. Here: `cp env.example .env`, fill in `DEVSERVER_KEY` (make one with
   `python3 -c 'import secrets; print(secrets.token_urlsafe(16))'`) and `TUNNEL_TOKEN`.
   `DEVSERVER_DATA` is where the logs go; pointing it at the old server's `~/pokerogue-devserver`
   imports its logs (numbered by time) on the first start. Create the folder first (`mkdir -p data`)
   and set `DEVSERVER_UID`/`DEVSERVER_GID` to its owner (`id -u`, `id -g`): a folder Docker creates
   itself belongs to root, and the server can't write to it.
3. `docker compose up -d --build`, then `curl https://vita-logs.example.com/logs`.
4. On the console, `ux0:data/pokerogue/config.txt`: `devserver=https://vita-logs.example.com/` and
   `devkey=<the key>`.

The compose file also publishes port 8765 on the host for direct access (LAN, or a console still
set to `http://<host>:8765/`); remove it once nothing uses it. If uploads get HTTP 403 from
Cloudflare (not from the server, whose 403 says "wrong key"), turn off bot challenges (Bot Fight
Mode, Browser Integrity Check) for that hostname.

Without Docker: `DEVSERVER_KEY=<key> tools/devserver/devserver.py --dir ~/pokerogue-devserver`.
