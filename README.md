# PokeRogue Vita

An unofficial PlayStation Vita homebrew client for [PokeRogue](https://github.com/pagefaultgames/pokerogue).
It runs the **live pokerogue.net game**, which means automatic updates, online accounts and cloud
saves, much like the online mode of [Admiral-Billy's PokeRogue App](https://github.com/Admiral-Billy/Pokerogue-App)
on desktop and Android.

> **Status: early research.** Phase 0 (feasibility) is done and Phase 1 (desktop runtime) is next.
> Nothing playable exists yet.

## Why this isn't just a browser wrapper
The Vita's built-in browser runs ES5 and has no WebGL. PokeRogue needs about 6 MB of ES2022, WebGL,
Web Audio and more, and no modern browser engine runs on the Vita. So this project is a small,
purpose-built "browser": a native app that embeds the [QuickJS-ng](https://github.com/quickjs-ng/quickjs)
JavaScript engine and implements only the web APIs PokeRogue and Phaser use. WebGL goes to vitaGL,
fetch to libcurl, login fields to the Vita's on-screen keyboard, and so on. The hottest parts of
Phaser's rendering are replaced with native code.

The full design is in [docs/PLAN.md](docs/PLAN.md), and the measurements behind it are in
[docs/phase0-findings.md](docs/phase0-findings.md).

## Phase 0 headline numbers (real PS Vita, 444 MHz)
| | stock Phaser in QuickJS | with native fast path |
|---|---|---|
| Synthetic battle frame (JS only) | 57.8 ms | **27.4 ms** |
| Startup: parse 6 MB of game JS | 14.8 s | 0.9 s from bytecode cache |

## Repository layout
| path | contents |
|---|---|
| `docs/` | plan, Phase 0 findings, development guide |
| `phase0/` | feasibility benchmark: C harness, JS shim, native fast path, build scripts |
| `phase0/measure/` | tools to run the real game in headless Chrome and count what it draws |

## Building
See [docs/development.md](docs/development.md). Everything builds in Docker (VitaSDK for the Vita,
Debian for desktop).

## License
[AGPL-3.0](LICENSE), the same license as PokeRogue.

Third-party components fetched at build time keep their own licenses: QuickJS-ng (MIT),
Phaser (MIT), and the VitaSDK samples' debugScreen helper.

## Disclaimer
This is a fan project. It is not affiliated with or endorsed by Pagefault Games, Nintendo,
The Pokémon Company, Game Freak or Sony. Pokémon and all related names are trademarks of their
respective owners. The app downloads the game client from pokerogue.net at runtime and does not
redistribute game assets.
