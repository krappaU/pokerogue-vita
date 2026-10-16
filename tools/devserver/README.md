# Dev server

Gets test builds onto the PS Vita and logs off it without FTP. The app side is
`runtime/src/devtools.c`; setup and behaviour are described in `docs/development.md`
("Test builds and logs without FTP").

```bash
export DEVSERVER_KEY=$(python3 -c 'import secrets; print(secrets.token_urlsafe(16))')
tools/devserver/devserver.py                          # newest CI build (needs gh auth or GITHUB_TOKEN)
tools/devserver/devserver.py --branch native-renderer # only that branch's builds
tools/devserver/devserver.py --vpk runtime/build-vita # builds made on this machine
```

Logs go to `~/pokerogue-devserver/logs/` (`--dir` to change); CI builds are cached in
`~/pokerogue-devserver/builds/`.
