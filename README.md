# BLOCKVERSE 2.0 — Windows PC Source Project

C++17 voxel sandbox game project using raylib 5.5 for rendering/input and ENet 1.3.18 for UDP networking.

## Build (Windows 10/11, Visual Studio 2022)
Requirements: Git, CMake 3.20+, Visual Studio 2022 with Desktop development with C++.

From PowerShell in this directory:
```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\Release\BLOCKVERSE.exe
```
First configure fetches raylib and ENet from GitHub, so internet access is required.

## Windows installer
Install [Inno Setup 6](https://jrsoftware.org/isdl.php), then run `build_installer.bat`. It builds Release and creates `dist\BLOCKVERSE-Setup-2.0.0.exe` (Start Menu entries, optional desktop shortcut, optional firewall rule for UDP 27125, uninstaller). The runtime is statically linked, so no VC++ redistributable is needed.

## Fullscreen and settings
Borderless fullscreen starts on by default; toggle with **F11** or **Alt+Enter** (or **W** in Settings). Settings (fullscreen, VSync, FPS, render distance, sensitivity, field of view with `,` / `.`) persist in `%APPDATA%\BLOCKVERSE\settings.cfg`; saves go to `%APPDATA%\BLOCKVERSE\blockverse_world.bvw`.

## Online multiplayer
1. Forward UDP port **27125** on the host router/firewall.
2. Start a dedicated server on the host:
   `.\build\Release\BLOCKVERSE.exe --server`
3. Launch the game client and choose **6 CONNECT ONLINE** from the main menu.
4. The client connects to `127.0.0.1` by default. To use another server, start the game with
   `.\build\Release\BLOCKVERSE.exe --connect <server-ip>` (no rebuild needed). Press **6** on the title screen
   to connect, then start or load a world. Other connected players appear as blue blocks, and block edits are relayed live.

This project includes an ENet dedicated-server mode (Ctrl+C stops it) that assigns player IDs and relays player positions and block edits to the other clients. Edits made before a player joined are not replayed to them. It is an early multiplayer foundation; authoritative world replication, authentication, anti-cheat, NAT traversal, lobby discovery, server persistence and production deployment still require further engineering and security testing. Do not expose an unauthenticated server to the public internet.

## Gameplay
- Modes: Survival, Creative, Hardcore
- Procedural 128×128 voxel world with terrain, trees, coal, iron and crystals
- Chunk mesh rendering and adjustable render distance
- Mining, block placement, hotbar, XP/levels, stamina and movement
- Crafting overlay (`C`, closes with `C`/`ESC`): planks, stone pickaxe (mined blocks drop double), crystal blade (double melee damage)
- Enemy spawning and pursuit; boss summon (`B`) after level 5
- Save (`F5`) and auto-save on exit to `blockverse_world.bvw` (saves terrain and game mode; inventory/XP are not saved yet)
- Settings: V toggles VSync, F FPS counter, [ / ] render distance, - / + mouse sensitivity
- `TAB` opens settings; `ESC` saves and returns to title (`ESC` on the title screen quits)

## Project status and honest scope
This is a substantial, buildable game codebase, but it is not equivalent to a fully content-complete commercial sandbox. In particular, online multiplayer currently provides ENet connectivity and packet relay rather than finished server-authoritative synchronized gameplay; crafting is a small starter recipe set; enemies and boss AI are foundational; there is no asset pack, audio suite, quest campaign or polished animation system yet. Those systems need iterative implementation, playtesting and security review.

## License notes
raylib and ENet are external dependencies with their own licenses. Keep their license notices when redistributing binaries or source.
