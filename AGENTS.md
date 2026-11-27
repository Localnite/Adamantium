# AGENTS.md

## What this is
A Windows C++ **DLL** injected into a Fortnite game exe to turn it into a dedicated server ("Project Reboot"; this fork is "Adamantium": bots + Save the World + backend match flow). The DLL is loaded by an external launcher that is **not in this repo**. There is no app to run locally and no package manager. Verification = build + run against a target game build + read `reboot.log`. The one exception is `tests/bot_rules_test.cpp`, a standalone g++ test for the pure bot decision rules in `Project Reboot 3.0/BotRules.h` (no game/DLL needed): `g++ -std=c++17 -O1 -Wall -Wextra -I"Project Reboot 3.0" -o /tmp/bot_rules_test tests/bot_rules_test.cpp && /tmp/bot_rules_test`. Do not invent other test/lint/typecheck/codegen scripts.

## Build (exact commands)
- Primary: Visual Studio 2022 / MSVC **v143**, `x64`. CI source of truth is `.github/workflows/msbuild.yml`:
  - `nuget restore .`
  - `msbuild . /t:Clean,Build /p:Configuration=Release`  (≤ S20 build)
  - `msbuild . /p:AboveS20=TRUE /t:Clean,Build /p:Configuration=Release`  (≥ S20 build)
  - Artifacts from `x64/Release/`; `Build/` is the local output dir (both gitignored).
- Linux: `./build-linux.sh` (msvc-wine). It **pins `/p:PlatformToolset=v145`** because the committed vcxproj says v143 and local MSBuild 18 resolves 14.51 → MSB8052. Requires msvc-wine under `~/msvc-wine`.
- `ABOVE_S20` is a compile-time switch. Without it, the DLL requires Fortnite ≤ 20; with it, ≥ 20; a mismatch pops a MessageBox and aborts in `dllmain.cpp` `Main()`. The `aboveS20` branch is auto-generated from `master` by `.github/workflows/aboveS20.yml` (prepends `#define ABOVE_S20` to `inc.h`). CI builds `master`.
- vcxproj quirk: `AboveS20` defines bare `ABOVE_S20` in 3 configs but `ABOVE_S20=$(AboveS20)` in `Release|x64` (the config CI uses).
- Only x64 configs link fmt/spdlog/curl/zlib/MinHook from `vendor/`; Win32 configs link just MinHook. C++ latest, `/utf-8` on x64, no PCH. `UnrealEngine.cpp` is listed under `<None>` — edits to it do not affect the build.

## Architecture / entry points
- `dllmain.cpp` `DllMain` only does `CreateThread(..., Main, ...)`; all work happens in `Main()`. Order matters: `InitLogger()` → MinHook init → `RebootConfig::Get()` → `Addresses::SetupVersion()` → `Offsets::FindAll()` → `Addresses::FindAll()/Init()/Print()` (Offsets must run first: `FindCantBuild` needs `FortAIController.CreateBuildingActor`) → version guard → GUI thread (unless prod match) → travel/level switch.
- There are no hardcoded game addresses: `finder.h` + `addresses.cpp` scan the running exe with Memcury (`vendor/memcury.h`) using byte patterns/string refs, branched on `Engine_Version` (416…427, 500) and `Fortnite_Version` (1.7…24.40). Supporting a new game version means adding patterns/finder functions there, not changing call sites. Reverse-engineering scripts referenced in comments (`fnmod/re/d730.py`) are **not in this repo**.
- Most `.cpp/.h` files are reimplementations of UE/Fortnite engine classes; `vcxproj.filters` shows their original engine paths (`Engine\Source\Runtime\...`, `FortniteGame\Source\...`). Game-specific behavior/hooks live mainly in `dllmain.cpp` and the `Fort*`/`Zone*` files.
- STW ("Zone") code (`Zone*.cpp/h`, `FortGameModeZone.cpp`) contains hardcoded exe offsets and vtable-slot call-site patches (e.g. `Zone_PatchAIDirectorCallers` in dllmain.cpp). Long comments explain why; read them before "simplifying".
- `.clangd` has absolute `/home/guestd/msvc-wine/...` include paths and a `~/.cache/reboot-clangd` symlink dir for case-insensitive `<imgui/...>` includes (`vendor/ImGui`). Update those paths if the toolchain moves; clangd does not expand `~`.

## Runtime config & modes (hard-earned, not obvious)
- Root `reboot.ini` is only a **partial/default sample**. The real file is read from **next to the game exe** (`FortniteGame/Binaries/Win64/reboot.ini`, where `reboot.log` also lives), auto-created from `RebootConfig.h` `DefaultText()`. Config is read **once at DLL start** — edit then restart.
- Override per server without editing the shared file: `-reboot.<Section>.<Key>=<value>` (e.g. `-reboot.Bots.Enabled=false`, case-insensitive, quote spaces) or `-REBOOT_INI=<path>`. Older `-BOT_*` flags still work. Defaults live in `RebootConfig.h`, not the root ini.
- `-PROD_MATCH` (see `prodopts.h`/`prodmatch.h`) = headless backend-driven mode: no GUI, MCP on, HTTP event reports to `-PROD_CALLBACK`. Without it an ImGui/DX9 GUI thread runs. Default `Backend=http://127.0.0.1:5000` (LawinServerV2); STW matchmaking registers there.
- Logging: spdlog, `reboot.log`; file sink ≥ info, console sink ≥ warn (console is slow under Wine); named loggers `LogDev`, `LogZone`, `LogBots`, `LogMatchmaker`, etc.

## Repo facts
- Local `main` == `origin/master`; CI runs on `master`. Generated dirs/artifacts (`x64/`, `Build/`, `*.pdb`, `*.tlog`) are gitignored — never commit or edit them.
