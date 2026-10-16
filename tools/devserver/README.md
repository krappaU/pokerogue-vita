# Dev server

Gets test builds onto the PS Vita and logs off it without FTP, for dev builds only (`PKR_DEV=1`). The app side is
`runtime/src/devtools.c`; setup and behaviour are described in `docs/development.md`
("Test builds and logs without FTP").

```bash
export DEVSERVER_KEY=$(python3 -c 'import secrets; print(secrets.token_urlsafe(16))')
tools/devserver/devserver.py                          # newest CI dev build (needs gh auth or GITHUB_TOKEN)
tools/devserver/devserver.py --branch native-renderer # only that branch's builds
tools/devserver/devserver.py --vpk runtime/build-vita-dev # dev builds made on this machine (PKR_DEV=1)
```

Logs go to `~/pokerogue-devserver/logs/` (`--dir` to change); CI builds are cached in
`~/pokerogue-devserver/builds/`.

With `--logs-key L` (or `DEVSERVER_LOGS_KEY`), `GET /logs/?key=L` lists the saved logs and
`GET /logs/latest.txt?key=L` returns one. That key only reads logs, so it can be given to Claude
sessions on other machines (the project chat) without letting them upload or fetch builds.
