# Dev server

Gets test builds onto the PS Vita and logs off it without FTP, for dev builds only (`PKR_DEV=1`). The app side is
`runtime/src/devtools.c`; setup and behaviour are described in `docs/development.md`
("Test builds and logs without FTP").

```bash
export DEVSERVER_KEY=$(python3 -c 'import secrets; print(secrets.token_urlsafe(16))')
tools/devserver/devserver.py                      # offers the newest published build
tools/devserver/publish.sh RUN_ID "what to test"  # publish a CI run's dev artifact (needs gh auth or GITHUB_TOKEN)
tools/devserver/publish.sh pokerogue-0.42-dev.vpk "what to test" --commit SHA --branch B   # or a local build (PKR_DEV=1)
```

Consoles are offered the newest published build, whatever its branch; plain pushes and CI runs
are never offered. Publishing the same or an older build again makes it the newest (a rollback).
A console with `devbranch=B` in `config.txt`, or the server's `--branch B`, gets the newest CI dev
build of that branch instead (needs a GitHub token).

Logs go to `~/pokerogue-devserver/logs/` (`--dir` to change), published builds to `published/` and
downloaded CI builds to `builds/` next to it.

With `--logs-key L` (or `DEVSERVER_LOGS_KEY`), `GET /logs/?key=L` lists the saved logs and
`GET /logs/latest.txt?key=L` returns one. That key only reads logs, so it can be given to Claude
sessions on other machines (the project chat) without letting them upload or fetch builds.

Cloud sessions can't reach that port. If `<dir>/gist-id` holds the id of a (secret) GitHub gist, each
uploaded log is also copied into it as `latest.txt` and `latest-prev.txt` using `gh api`, whose login
must be available to the service (`gh gist create latest.txt latest-prev.txt` makes the gist).
