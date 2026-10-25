# PokeRogue Vita

An unofficial PlayStation Vita homebrew client for [PokeRogue](https://github.com/pagefaultgames/pokerogue).
It runs the **live pokerogue.net game**, which means automatic updates, online accounts and cloud
saves, much like the online mode of [Admiral-Billy's PokeRogue App](https://github.com/Admiral-Billy/Pokerogue-App)
on desktop and Android. It can also play an offline build of the game from the memory card.

> **Status: released (1.0).** Online play (login, cloud saves) and offline play work on a real
> Vita, with sound, gamepad controls, the touchscreen and save import/export. Battles run at about
> 25-45 fps depending on the map and how far into a run (or an even 30 with the 30 fps lock), and
> some moments still pause briefly.
> Download the latest build from [Releases](https://github.com/krappaU/pokerogue-vita/releases).

## What works
- **Online:** the live game from pokerogue.net (always the current version), login with a PokeRogue
  account on the built-in keyboard, cloud saves.
- **Offline:** downloads the offline build used by Admiral-Billy's app (~530 MB) and plays it from
  the memory card; no login, local saves. The start screen offers updates when a new build is out.
- Sound (music, effects, cries), the Vita's buttons as a gamepad (the rear touchpad as L2/R2), front
  touchscreen.
- Save import/export (Manage Data): files go to `ux0:data/pokerogue/exports/`, usable across modes.
- Links in the game's menus are shown as QR codes to open on a phone.

## Why this isn't just a browser wrapper
The Vita's built-in browser runs ES5 and has no WebGL. PokeRogue needs about 6 MB of ES2022, WebGL,
Web Audio and more, and no modern browser engine runs on the Vita. So this project is a small,
purpose-built "browser": a native app that embeds the [QuickJS-ng](https://github.com/quickjs-ng/quickjs)
JavaScript engine and implements only the web APIs PokeRogue and Phaser use. WebGL goes to vitaGL,
fetch to libcurl, Canvas 2D to plutovg, Web Audio to a native mixer, and so on. The hottest parts of
Phaser's rendering, and some of the game's own heavy work (save encryption), run as native code.
The game itself is never modified.

The design is in [docs/PLAN.md](docs/PLAN.md); measurements and decisions are in
[docs/phase1-findings.md](docs/phase1-findings.md); the latest project review is
[docs/review-2026-10.md](docs/review-2026-10.md).

## Repository layout
| path | contents |
|---|---|
| `runtime/` | the app: C host (QuickJS, WebGL, Canvas 2D, HTTP, audio) and JS browser shim |
| `docs/` | plan, findings log, development guide, project review |
| `tools/measure/` | runs the real game in headless Chrome (reference renders, per-frame counts) |
| `archive/phase0/` | the original feasibility benchmark (history only) |

## Building
See [docs/development.md](docs/development.md) and [runtime/README.md](runtime/README.md).
CI builds the desktop runtime and the .vpk on every push, runs the tests on both (the .vpk in
Vita3K), and keeps each commit's .vpk as a downloadable artifact.
Everything builds in Docker (VitaSDK for the Vita, Debian for the desktop version used for development).

## License
[AGPL-3.0](LICENSE), the same license as PokeRogue.

Third-party components fetched at build time keep their own licenses: QuickJS-ng (MIT), plutovg
(MIT), stb_image (MIT or public domain), miniz (MIT), minimp3/minimp4 (CC0), faad2
(GPL-2.0-or-later), QR-Code-generator (MIT), libcurl (curl license), OpenSSL, vitaGL (LGPL-3.0),
Mozilla's CA bundle (MPL-2.0), the 8x8 bitmap font (public domain) and VitaShell's package header
template for dev builds' installs (GPL-3.0).

The game's LiveArea images use the rocky coast arena backdrop by Vari from PokeRogue's assets,
CC BY-NC-SA 4.0 (see [runtime/sce_sys/README.md](runtime/sce_sys/README.md)).

## Disclaimer
This is a fan project. It is not affiliated with or endorsed by Pagefault Games, Nintendo,
The Pokémon Company, Game Freak or Sony. Pokémon and all related names are trademarks of their
respective owners. The app downloads the game client from pokerogue.net (or the offline build from
GitHub) at runtime and does not redistribute game assets.
