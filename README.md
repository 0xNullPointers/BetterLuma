# BetterLuma

BetterLuma is a maintained fork of LumaCore - the Steam client hook layer designed to handle family-sharing bypass, depot key injection, achievement spoofing, Denuvo authorization, legacy CD-key suppression, and seamless multiplayer proxying.

This project is focused on continuing the development and adding key features and architectural improvements that were desired for a long time, including simultaneous multi-game OnlineFix support (Kindof), complete process watcher attachment preservation, robust lifecycle tracking, and compiler-resilient hook resolution.

It ships as four files placed in the Steam installation directory:

- `dwmapi.dll` - thin DWM proxy that Steam loads on startup; immediately loads `BetterLuma.dll`
- `xinput1_4.dll` - thin XInput 1.4 proxy; backup load gate for `BetterLuma.dll`
- `BetterLuma.dll` - the main hook library and patch orchestration engine
- `BetterLumaPayload.dll` - injected directly into game processes for OnlineFix multiplayer (EOS bridge and lobby redirection)

## How it works

At Steam startup, the proxy DLLs load before any game code and load `BetterLuma.dll`. The core engine then:

1. Copies `steamclient64.dll` to `bin\lcoverlay.dll` so hooks and captures run in an isolated environment independently of the live client.
2. Reads the current Steam build ID from `steam.exe!GetBootstrapperVersion` so byte-pattern searches pick the most accurate signature for the running build.
3. Fetches per-build pattern TOMLs from the network mirror chain, caches them locally, and primes the runtime pattern map.
4. Installs over 40 Detours hooks plus Vectored Exception Handler (VEH) captures into the loaded `lcoverlay.dll` copy, covering IPC dispatch, package ownership, license patching, Denuvo auth, manifest binding, network packet rewriting, and OnlineFix game language synchronization.
5. Starts a Lua directory watcher that monitors `config/stplug-in/` for `.lua` files.
6. For OnlineFix titles, injects `BetterLumaPayload.dll` into game processes via `CreateProcess` hooks to maintain the Epic Online Services (EOS) bridge and handle Spacewar (480) multiplayer redirection.

When a Lua file appears or changes, BetterLuma parses it, loads depot decryption keys and ownership records, and injects the new ownership data into Steam without restarting. For OnlineFix games, BetterLuma synchronizes the game's language setting to Spacewar (480) and `BetterLumaPayload.dll` is injected into the game process via `CreateProcess` hooks to handle EOS bridge and lobby redirection.

## Features

See [docs/BetterLuma.md](docs/BetterLuma.md) for a full description of every hook and feature.

## Known Issues

- **Concurrent OnlineFix Lobby & Matchmaking Collisions**: While BetterLuma fully supports launching, attaching, and tracking playtime for multiple `-onlinefix` games simultaneously on the local client, Steamworks backend architecture limits a single Steam account to one active Spacewar (480) lobby at a time. Creating a lobby in one title overwrites your Steam profile's joinable rich presence (`+connect_lobby`) for the other title, breaking friend invites for the earlier lobby. Legacy P2P networking channels may also conflict if both games attempt to host or listen on identical virtual ports under the same account. Running one title in single-player while playing another in multiplayer, or playing games that utilize the Epic Online Services (EOS) bridge for matchmaking, functions without issue.

## Building

### Requirements
* Windows 10/11 (64-bit)
* Visual Studio 2022 (MSVC with C++20 support)
* CMake 3.20 or newer

### Build Command
```bat
build.bat
```

Dependencies (Lua 5.4, Microsoft Detours, spdlog, protobuf, toml++) are downloaded automatically via CMake FetchContent on the initial build. Subsequent runs are fully incremental. Output binaries are generated in `build/Debug/` or `build/Release/` and copied to `Releases/`.

## Credits

The original work was done by Midrags, and the copyright of all the files is hence held by them.
All subsequent modifications, architectural enhancements, and continued development are done by 0xBadCod3.

See [CREDITS.md](CREDITS.md) for full attribution details.

## License

BetterLuma is distributed under the GNU General Public License v3 or later. See the root [LICENSE](LICENSE) file for the full text.
