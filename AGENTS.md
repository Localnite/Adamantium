# AGENTS.md — Project Reboot 3.0 (Season 7 fork with bots and headless match mode)

Everything an agent needs to work on this fork without rediscovering it. It is written from the logs, dumps and crashes of the actual sessions, so the weird parts are the point. Dates are 2026 (Oct 23–28). Items marked **[unverified]** were implemented but never confirmed by a playtest or a log; everything else was seen in a log, a dump or a disassembly.

---

## 0. Read this first (rules that were learned the hard way)

1. **Never touch the user's live server.** A bots-only test script once killed every process newer than a pid watermark and took out the server the user had just started (2026-10-25 10:26). Rules now:
   - Before any test, check that no real server is running (`Shipping.exe` with `host@projectreboot.dev` and no `-BOT_TEST`). Abort if one is up or appears mid-test.
   - Stop only the launcher pid you started yourself (`kill -TERM`, the launcher cleans its own process tree). Never kill by name, by pid watermark, or with `pkill -f`.
   - `pkill -f`/`pgrep -f` with a pattern that also appears in your own command line (`localnite-launcher`, `FortniteLinux/Season 7`, `localnite/proton`) kills your own shell (exit 144). Put such logic in script files.
   - Never install a DLL over a running server's DLL: build to a separate `OutDir` (`Build2/`, `Build3/`...), install by copying to `<name>.tmp-install.dll` and `mv -f` over the target (atomic).
   - `clean_tests.sh` is disabled on purpose. `run_one.sh`/`run_navbuild.sh` already follow the rules.
2. **Bots must look believable.** The user rejected giving every bot the same default cosmetic parts "for performance". Never homogenise bot appearance, names, behaviour variety, or other realism to save CPU; find the cause elsewhere (the real cause was logging, see §9).
3. **The user cannot always playtest and is often away.** Work is log-driven. Prefer changes that are testable with a bots-only match, say plainly what is untested, and never claim a visual/feel result you did not see.
4. **Do not rebuild/install the DLL unless the user wants a DLL change** (they have said "backend only" before).
5. Keep comments in the same density/style as the surrounding code (plain English, explain the *why* of engine quirks). Reboot macros: `LOG_INFO(LogX, ...)` expands to `if (...) stmt;` — brace carefully when using it inside an unbraced `if/else`.

---

## 1. What this is

- **Project Reboot 3.0** (Milxnor, upstream `10c6590` "automate recomp") is a C++ DLL that is injected into a Fortnite *dedicated-server-style* process (the normal game exe patched to run headless) and turns the game into a listen/dedicated server by hooking UE functions.
- This fork targets **Fortnite Season 7** (PC game 7.00 CL 4629139, UE 4.22) and adds:
  1. **Bots** (`bots.h`, `BotBrain.h`, `BotRegistry.h`): up to ~100 simulated players with perception, navigation, looting, combat, healing, building, storm handling, kill-feed integration.
  2. **Headless match mode** (`prodopts.h`, `prodmatch.h`, hooks in `NetDriver.cpp`, `dllmain.cpp`, `FortGameModeAthena.cpp`): `-PROD_MATCH` headless mode reporting to an HTTP callback.
  3. Misc engine-bug fixes (spectate-killer, log flushing, team numbering, etc.).
- Loaded by **`~/localnite-launcher`** (Go; `main.go`) with `--server --server-dll "<dll>"` under Wine/Proton GE-Proton11-11 on Linux.
- Season 7 game build: `/run/media/guestd/General/FortniteLinux/Season 7` (now **7.20**, see §14 — **offsets/bot work were tuned on 7.00 and are untested on 7.20**).

### Source layout (all under `Project Reboot 3.0/`)
| File | Role |
|---|---|
| `bots.h` (~2.9k lines) | Bot spawn/fill, team numbering, test options (`-BOT_*`), navmesh creation, nav probe, session recorder hooks, storm/zone helpers |
| `BotBrain.h` (~10k lines) | The whole brain: observe → decide → apply, perception, combat, healing, building, navigation grid, stuck solver, hearing, etc. |
| `BotRegistry.h` | Maps bot controllers/pawns/player states to bot ids (`BotControllers`, `LabelFor`) |
| `prodopts.h` | `ProdMatch::Options` parsed from the command line; `gAllowStart` |
| `prodmatch.h` | Headless match state machine + backend reporting (included **only** by `NetDriver.cpp`) |
| `NetDriver.cpp` | `TickFlushHook`: bot tick, sprint-style bracketing, `ProdMatch::Tick()` |
| `FortGameModeAthena.cpp` | `ReadyToStartMatch` hook (prod allow-start), PickTeam hook uses the playlist's first team, spawn hooks |
| `FortPlayerController.cpp` | `ClientOnPawnDiedHook` (spectate killer), pickup hooks |
| `FortPickup.cpp` | `CompletePickupAnimationHook` patched so a bot pawn finds its controller via `BotRegistry` |
| `dllmain.cpp` | init; GUI thread skipped under `-PROD_MATCH`; `Globals::bNoMCP=false` under prod |
| `log.h` | spdlog setup — **flush policy matters hugely** (§9) |
| `gui.h` | GUI incl. the "Start Bus Countdown" button whose behaviour prod mode replicates |

Companion tooling in `~/Documents/FortniteLinux/RebootBotWork/` (copy of sources, `reboot-bots.patch` = diff vs upstream `10c6590`, the DLL, and scripts: `run_one.sh`, `run_navbuild.sh`, `run_bot_tests.sh`, `bot_report.py`, `analyze_session.py`, `struggle_report.py`, `ablate.sh`, `navstr.py`, `callers.py`, `kill_game.sh`, `wait_for_finish.sh`, `test-runs/`).

---

## 2. Build, install, run, logs

- Toolchain: msvc-wine. **Only `/p:PlatformToolset=v145` works on this machine.**
  ```
  CL=/FS msbuild "./Project Reboot 3.0/Project Reboot 3.0.vcxproj" /p:configuration=Release /p:platform=x64 /p:PlatformToolset=v145 /p:OutDir=../BuildN /m:1
  ```
  `CL=/FS` and `/m:1` are required: a stale `mspdbsrv` (Oct 24) causes `C1041` PDB lock errors with parallel builds. Build to a fresh `BuildN/` so a running server's DLL is never overwritten.
- Installed (stable) DLL = `src/Build/Project Reboot 3.0.dll` **and** a copy at `~/localnite-launcher/Project Reboot 3.0.dll` (launcher dir; old copy backed up at `/tmp/claude-1000/prodbackup/`). Latest installed was Build15 (prod match). **Build16 is not built**: pending = remove the 5400 s default `MaxSeconds` cap (source already edited) and fix the `FilledAt` stale-time bug (§11).
- Run (normal server): `./localnite-launcher --buildpath "<Season 7>" --dll ./Tellurium.dll --inject-delay 3000 --helpers --debug --server --server-dll "<dll>" --email host@projectreboot.dev --password 12345678 -- <your own server args>`. Everything after `--` is passed to the game (use it for `-BOT_*`/`-PROD_*`).
- Logs: `<Season 7>/FortniteGame/Binaries/Win64/reboot.log` — read with `grep -a` (bot names are binary garbage). Game logs: `FortniteGame.log` / `FortniteGame_2.log` in the Wine prefix (`~/.local/share/localnite/pfx/.../FortniteGame/Saved/Logs`); to tell server from client, the client log has `XAudio2 using`. Crash dumps land in `.../FortniteGame/Saved/Crashes/` (minidumps have no usable callstack).
- Headless exe patch: the Season 7 exe needs Reboot Launcher's headless binary patch (UTF-16 `-invitesession -invitefrom -party_joininfo_token -replay` replaced with `-log -nosplash -nosound -nullrhi -useolditemcards       `); Seasons 3/5 exes were patched, Season 7's status was unknown.

### Test options (command line, parsed in `Bots::TestOptions`)
`-BOT_TEST` (start the bus by itself: `ReadyToStartMatch` returns true once bots exist, then `GameMode->StartAircraftPhase()` after `-BOT_BUS_DELAY=20` s of warmup), `-BOT_COUNT=N`, `-BOT_MAX_SECONDS=900`, `-BOT_EXIT` (ExitProcess 5 s after `BotTest: finished`, with an explicit log flush), `-BOT_SKILL=N`, `-BOT_FILL=N`, `-BOT_PLAYLIST=<path>` (e.g. `/Game/Athena/Playlists/50v50/Playlist_50v50.Playlist_50v50`), `-BOT_NAVBUILD` (explore the island and write the nav cache; `run_navbuild.sh`, ~5 min).
Test scripts: `run_one.sh <name> [bots] [max_s]` (env `DLL=...`, `EXTRA="..."`), `ablate.sh <name> "<cfg;cfg>"`, `bot_report.py <logs>` (events, perf, deaths/credit/weapon, stuck hotspots, "match pace"), `struggle_report.py`, `analyze_session.py` (real players vs recorded bots).

### Tuning
There is no runtime config any more (`bot_config.txt` was removed 2026-10-29). Every tuning value is a constant/global with its default in the source (top of `BotBrain.h`); change it there and rebuild. Test options still come from the command line (`-BOT_*`, `-PROD_*`).

---

## 3. Engine facts for this build (UE 4.22, Fortnite 7.00)

- **ProcessEvent on a UFunction with parameters needs a real params buffer.** Passing `nullptr` makes the engine `memcpy` from address 0 and crash (hit via `OnRep_IsSkydiving(bPreviousState)`).
- **`FString::Set()` stores the pointer without copying.** Passing a temp `std::wstring` leaves a dangling pointer (garbled names). Use `BotBrain::StableWide` / `MakeBotName`.
- **Never call `FString::ToString()` on a possibly empty FString**: Reboot's version does `wcslen(Data.Data)` and a null `Data` crashes (build39 crash at first bot death). `FString` wraps `TArray<TCHAR> Data`.
- `GetPlayerName()` returns garbage here; read `PlayerNamePrivate` by offset.
- **Dump param order is REVERSE declaration order** (`ForceKill(DeathReason, KillerController, KillerActor)` was verified against the dump). The user dumped Season 7: `Win64/ObjectsDump.txt` ("Fortnite Version 7.000000", 614k lines) — use it, not the old 3.5 dump in `reboot/bert`. Property names are case-exact: it is `PlayerID` (capital ID); a lookup of `PlayerId` silently failed.
- **UniqueId layout (UE 4.22)**: `FUniqueNetIdRepl [0]=vtable [8]=object ptr [16]=control block`; object `[0]=vtable [8]=self (TSharedFromThis weak this) [16]=weak ctrl [24]=FString Data [32]=Num [36]=Max [40]=FName Type`.
- **Raw-pointer caches go stale and still pass validity checks.** `IsValidLowLevel`, `IsActorBeingDestroyed`, `IsDestroyed` all passed on a freed-and-reused prop; `K2_GetActorLocation` then crashed on a non-canonical RootComponent (exe+0x2b452a5). Use `IsLiveBuilding()` (IsValidLowLevel + `IsA(ABuildingSMActor)` + not destroyed) and cache positions so no engine call touches the prop. Same risk: `gChestActors`, `gWorldQueue`, `gPickups`, `gHarvestables`, `gDoors`.
- Non-instanced gameplay abilities (jump) have no instance, so ending them does nothing and `Spec->ActiveCount` climbs to 255 → log spam "Jump called when Spec->ActiveCount (255) >= UINT8_MAX". Fixed by zeroing `Spec.ActiveCount` before each jump activation.
- Hits/shots in S7: **the shooter's CLIENT computes hits and sends target data**; there is no server fire RPC on `FortWeapon`. The ranged ability `GA_Ranged_GenericDamage_C` is a thin BP over native `FortGameplayAbility_RangedWeapon` (members `WeaponFireCue`, `CurrentWeapon`, `FiringNoiseRange`); it stays active waiting for client hit data, so 64% of bot activations failed until the ability is released/cancelled (§6). Client-style `ServerSetReplicatedTargetData` is accepted without error but ignored → `client_shots` off.
- Teams: `TeamsArray` container is null on S7 / has 102 slots; usable teams are 2..101 (102 rejected, 101 works). Reboot assumes first team 3. See §8.
- Season 7 POI sublevel names come from container paths `Athena_POI_<Name>_`; there is no "Tilted" sublevel. `GenericPlot_DCE_001` is **Snobby Shores, not Tilted** (user confirmed).
- The 7.00 build ships the full UE4 AI stack + Fort/Athena AI classes (`AthenaAIController` with ranged+melee ability slots, `AthenaAIDropper/Director/System/Settings`, `AthenaPathFollowingComponent`, `AthenaNavSystem/AthenaNavMesh/AthenaNavSystemConfig(+Override)`); only BT in memory is `BT_AthenaHuskBase` (goal-via-EQS + melee; not worth forking). `FortBotController/FortBotPawn/FortBotStructureBuilder` are class stubs. Forks (Bertogim, gumba21, lovexash) only spawn bots on S7; lovexash combat is Chapter-2 AI-controller only. Web search found nothing for S7 attack logic.
- The server runs `LIGHTWEIGHT_TIME_GUARD` hitches (Athena_GameState_C tick 70–125 ms, GameMode 9760 ms at start) regardless of bots; they are not necessarily bot-caused.

---

## 4. Making bots simulated at all (the foundational hack)

- A pawn possessed by a bot *player controller* is **never simulated server-side** (frozen mid-air, no gravity). `RemoteRole` writes and forcing `MovementMode` did nothing.
- Working recipe (from Bertogim/Project-Reboot-3.0-bots): `SetIsBot(false)` + `Controller->UnPossess()` + restore `Pawn->PlayerState` by hand + `CharacterMovement.bRunPhysicsWithNoController = true` (+ tick/activate CMC, clear `bSimGravityDisabled` / `bDisableMovementAndTurnInPlace`, set `bAllowMovement`, `NetUpdateFrequency >= 50`). Bots then stand/fall/jump.
- AI-controlled pawns (`AthenaAIController`, no PlayerState) are also simulated; not used.
- Consequence: because bots are unpossessed, **the engine never runs its death/damage/aircraft flow for them** — we reimplement it (§6, §7).
- Upstream bug: `static inline AllPlayerBotsToTick` in `bots.h` made one list per .cpp, so `Tick()` never saw spawned bots. Must be plain `inline`.
- `bots.h` `FillBots` / `SpawnBotsAtPlayerStarts`; bots take human-looking names (deterministic across runs because `rand()` is never seeded: bot #12 is always GoldenTitan_729).

### Bus / aircraft quirks
- The engine marks every alive player (bots included) `bInAircraft` and at bus end calls `SpawnDefaultPawnFor` for all (22 calls, 16 failures; a real player lost his pawn). Fix: clear `bInAircraft` on bot player states every tick, and `SpawnDefaultPawnForHook` returns nullptr for registered bot controllers (`BotRegistry::BotControllers`).
- Bus-exit teleport: `Pawn->TeleportTo(BusLoc)` is refused if another pawn occupies the spot → 26 of 94 bots stayed on the spawn island and killed each other with pickaxes. Now: random exit spot (500+200·try units, ±250 z), up to 8 tries, then `K2_SetActorLocation` without sweep (`BotJump #N: every teleport ... refused`).
- Global jump slot limiter: 0.35 s between bus exits (clients hitched ~130 ms per newly arriving pawn: customization/mesh build; 15 hitches in 20 s when 62 bots left within 4 s).
- Bots must also clear spawn-island loot when boarding (`ClearIslandItems` at Phase==Aircraft, then re-equip the pickaxe); only humans did that via `EnterAircraftHook`.
- Players joining a match already past the bus get **no pawn** (HUD shows 999, camera under map): restart the server before joining.

---

## 5. Identity, teams, kill feed

- **All bots shared one `PlayerID`/`WorldPlayerId`** and the client resolves players by it (killer/victim names, kill feed, "eliminated by"). Reboot only sets `WorldPlayerId=PlayerID` for real players (`FortGameModeAthena.cpp` ~1611). Each bot now gets `PlayerID = WorldPlayerId = 5000 + id`.
- **Empty/identical `UniqueId` on bots** made the kill feed show the *last created* bot for everyone. `AssignBotUniqueIds` clones a human's `FUniqueNetIdRepl` (shared ptr = string object + control block) per bot **once a human joins**; logs `UniqueId template` (hex) + `UniqueId: gave N bots`. It also finds the human via `BotBrain::gPawns` and logs `UniqueId: waiting for a real player...` every 30 s. Fill-mode spawns after the human joined, so it works there too.
- **The human is often NOT in `GameMode->GetAlivePlayers()`.** Anything that needs "the humans" must use `BotBrain::gPawns`. (This was the shared root cause of: kills credited to nobody / "died to the storm", UniqueIds not assigned, kill feed showing one bot.)
- Kill feed: `HandleBotDeath` fills the victim's replicated `DeathInfo` (`FinisherOrDowner`/`Downer`, `DeathLocation`, `Distance`, `bInitialized`) via `FillBotDeathInfo` + `OnRep_DeathInfo`; the `FinisherOrDowner` offset is resolved by name (it was never set in dllmain). Death cause from the weapon path via `EDeathCause` enum names (logged once as `EDeathCause:`); only ~8 of 39 values handled. `EDeathCause::FallDamage = 1` (falls used to show as storm: `bStorm` must exclude the `fall` mark and `FallDamage` is tried first). The `\x01fall` marker in `gLastWeaponPath` only counts within 4 s of the fall (`gFallTime`), otherwise human kills of fallen bots were logged as fall.
- Kill credit: `gLastDamager` with a 12 s window (`gLastDamageTime`); human kills of bots via `gHumanShotTime` (human weapon `AmmoCount` drop within 4 s, <30000u, else nearest <6000u) and, at hit time, `AttributeDamageToHuman` (pawn `LastFiredTime`/ammo).
- Bot death (`RemoveFromAlivePlayers` natively; `PlayersLeft` verified dropping 21→20). Known gaps vs real players: no health siphon, no match report; loot drop on death was added (build90: all `CanBeDropped` stacks except the pickaxe via `PickupCreateData`) **[unverified]**.
- **Team numbering** (the 100-player mystery): GUI bot count of 99 made the human the 100th team; Reboot's `PickTeam` gave index n+2 and index 102 is rejected, so the human ended with `TeamIndex 0`, was never in the alive list (PlayersLeft started at 99 = bots only, hit 0 while the human lived). Current design: `PlaylistFirstTeam` reads the first team from the playlist (`DefaultFirstTeam` 2..3) or derives it from `TeamsArray.Num() - MaxTeamCount`; `first_team=2` can force it; highest accepted team = 101 (`HighestAcceptedTeam`); in solo bots take teams from the **top** (`PickBotTeam`, direct write) while humans get from the bottom via Reboot's `PickTeam` (which now uses the same first-team); `EvictBotOnTeam` kills a bot sharing a team with a joining human (only possible with >100 players; skipped when the playlist has teams >1, `PlaylistSquadSize = max(MaxSquadSize, MaxTeamSize)`). Verified bots-only: solo fill 100 → teams 2..101; 50v50 fill 100 → exactly 2×50. Two-human + 98-bot case was fixed and tested in bots-only only. Reboot's large-team spread wrap-around was fixed (it cycled 3 teams instead of 2). Why the native path rejects 102 is still unexplained (probably a range check).
- Distinct teams are why bots ignored each other originally (`bBotsFightBots` default now on).

---

## 6. Combat, damage, effects

All of this exists because the server never runs a bot's own weapon logic.

- **Ranged**: bots activate `GA_Ranged_GenericDamage` through `ASC.InternalTryActivateAbility` with a valid prediction key (Reboot's own RPC handler sets `spec.InputPressed` and passes `Current>0`; a zero key made it return false). The ability must be released first (`ServerReleaseWeaponPrimaryAbility`; S7 names, not `ServerReleaseWeaponAbility`) and cancelled 0.12 s after each shot (`FortWeapon.CancelActiveAbility`) → 71/71 activations succeed (92 false/54 true before). Exclusion list gotcha: do not use `Default` as an ability-name exclusion — it matches every `Default__` CDO path; use `DefaultPlayer`.
- **Hit resolution is done by the bot** (`ResolveShot`, skill model): casts the real bullet line (aim + wobble + gaussian·sigma, ×1.3 when moving, recoil +10%/shot) against a body box (±42 wide, −80..85 tall; head zone `|lateral|<16`, vertical>58, plus a 50% luck roll; multiplier 1.75×, sniper 2.5×); misses are possible; damage applied with `SetShield`/`SetHealth` and `ForceKill(bot controller as killer)`. If `client_shots` is on, the bot sends client-style target data and verifies 0.4 s later; after 6 ignored with 0 worked it falls back to direct damage (the game ignores it; default off).
- **Skill model** (`Brain.Skill` 0–100, `gSkillMin/gSkillMax`): `AimSigma = 14·exp(-S/28)°`, `TurnRate = 45 + 1.6·S °/s`, reaction time 0.35–1.0+ s, trigger needs aim within `6+(100−S)/4°`, aim uses a **lagged target position** (`AimHistory`, lag `0.5−0.004·S`), `FireRange` cap `3000·(0.6+0.4·S/100)` (shotgun 1100). 7% "strong" tier (skill 72–95) from PUBG solo data (real: 56% zero kills, 3% 5+).
- **Melee**: pickaxe through `GAB_Melee_ImpactCombo_Athena` (needs the same key handling); damage 20/hit (the user's session shows bots hitting them for 19–20), lands 0.22 s after the swing only if the target is still within reach+90; each swing plants the bot 0.3 s and applies the game's own `FortPlayerPawnAthena.MeleeCombatSlowSpeedMultiplier/Duration`; chase speed 0.92× sprint; swing bursts of 2–5 then 1.1–2.4 s pause (cadence 0.8–1.05 s). Real-player pickaxe damage is **unmeasured**.
- **Effects** (what the victim/others see) — hardest area:
  - Held weapon invisible → `EnsureWeaponShown` (set `bWeaponActivated`, clear `bWeaponHolstered`); hide it during emotes with `FortWeapon.SetWeaponVisibility(bForceUpdate, bDesiredVisibility)`.
  - Replicated aiming stance: `FortPlayerPawn.bIsTargeting` bit + `OnRep_IsTargeting` while in combat (`Brain.bTargetingShown`); `FortPlayerPawnAthena LastFiredTime/Direction/Location` written per shot. S7 has **no OnRep for `BurstFireCounter`**, so no remote fire FX through it.
  - The S7-correct damage cue path: `FortPawn.NetMulticast_Athena_BatchedDamageCues(SharedData{bIsValid,bWeaponActivate,bIsBallistic,bIsCritical,bIsFatal,bIsShield,Magnitude,Normal,Location}, NonSharedData{HitActor})` on the **shooter** pawn each shot (`SendShotCue`, `bWeaponActivate` = muzzle flash/tracer). `FortPlayerPawnAthena.NetMulticast_Athena_BatchedDamageCues` does **not** exist in S7 (the 3.5 dump was wrong). `HitActor` is a raw ObjectProperty (not a weak ptr; treating it as one crashed `ShotCue`). Victim effects also via `ASC.NetMulticast_InvokeGameplayCueExecuted_WithParams(GameplayCue.Damage, ...)` on the victim ASC (`SendHitCue`; pawn BP handles `GameplayCue_Damage` → `OnDamagePlayEffects` = arrow, particles, sound, shake). Victim damage-direction arrow and tracers were still **[unverified]/unsolved** at the last report.
- **Healing**: items classified by path word (bandage 15/cap75 4 s, medkit 100 10 s, shieldsmall 25/cap50 2.5 s, shield 50 5 s, chug 100/100 12 s, slurp); started via the item's own use ability once in hand (`Attack(..., bConsumable=true)`), applied server-side after the use time (`SetHealth`/`SetShield` + `Inventory->RemoveItem`, only if removal succeeds), manual fallback after `HealUse+2 s`; healing stops when a sound is heard <3500u or an enemy is <1200u (`NoHealUntil +4 s`).
- **Storm**: the engine storm does not hurt unpossessed bots → own `ApplyStormDamage`; `BotZone` log shows indicator/radius. Early circle shrinks ~580 u/s (about run speed) and its centre drifts across the map, so a bot waiting for the edge never catches up → bots read `FortSafeZoneIndicator NextCenter/NextRadius` (sanity: inside current circle) and rotate to a point inside the next circle after a random delay (`Brain.RotateAt`). Storm goal is the nearest part of the circle just inside the edge (`max(0.55R, R−3500)`), not a random far spot (a bot died climbing a slope at 124/550 u/s). In 50v50 there is **no safe zone/storm** (phase jumps to 5 EndGame right after the bus; `PickSafeZoneLocation` errors with MaxRadius 0 in client logs).
- **Fall damage**: peak Z tracked while `Mode==3`; 0.3 s after landing compare engine damage against `FallDamageFor = (h/100 − 8)·4.5` and apply the rest; `GuessFallDamage` with per-bot `FallBias 0.75–1.3` makes bots avoid drops that would kill them.
- **Building/harvest**: `StepWorldScan` (1500 building actors per tick, rescans every 240 s) keeps wooden non-player-placed props (`gHarvestables`) and doors (class name contains `door` and has `bDoorOpen` → `gDoors`; 139 found, 9699 props). Doors opened by setting `bDoorOpen`/`bDoorCollisionDisabled` + OnReps. **`ApplyDamage` does NOT lower a building's health, and `OnDamageServer` alone gives wood but no damage.** Working harvest: call the hook (wood) + `BuildingActor.SetHealth(health − damage)` + `OnDeathServer`/`SilentDie` at 0. Harvest targets exclude anything >1500 hp or named `car_`/`truck`. Walls: `ServerCreateBuildingActorHook` replicated (tile centres at multiples of 512, wall 256 off, `PBWA_W1_Solid_C`, 10 wood; yaw guessed) **[unverified yaw]**. Breakable railings/fences are smashed immediately instead of hop-jumped (jumps never cleared them).
- Vehicles: **not implemented** (`BotVehicle:` log only). Reboot spawns vehicles; driving is client-authoritative (`ServerVehicleUpdate`).

---

## 7. Movement (this took the longest)

- **Friction regression**: CMC braking friction cut real speed to ~45% of the asked speed (asked 620 → moved 270). `Apply` zeroes `GroundFriction`, `BrakingDecelerationWalking`, `BrakingFrictionFactor`, `BrakingFriction` → ~93% of asked. Measured real-player speeds (live `HumanMove`): run MaxWalkSpeed **440**, sprint **550** (not 850): defaults Walk 440 / Run 550 / Sprint 550, calibrated from humans at full speed.
- **`movement_input` (real `AddMovementInput`) measured only ~100 u/s on the ground** → off; bots write Velocity (with `RampVelocity`: accel 2600 / decel 3600 u/s², toggled by `ramp_motion`, to avoid client-visible velocity discontinuities that looked like teleporting). `move_assist` adds a light `AddMovementInput` (accel=150 verified) so replicated `AccelerationPack` is non-zero — otherwise the walk animation slides (CMC zeroes a bot's Acceleration each tick).
- **Sprint animation**: the game's `IsSprinting == (CurrentMovementStyle == 3)` and the game resets the byte every tick. **Writing `CurrentMovementStyle` every tick regressed speed to ~54%** (fixed: write only when the state changes). Holding style 3 during the CMC update collapses velocity to ~105 u/s regardless of ability/MaxWalkSpeed/move_assist → style 3 is set **only around `ServerReplicateActors`** (`BotBrain::SprintStyleBegin/End`, `gSprinters`, called in `NetDriver.cpp TickFlushHook`); clients see sprint, the CMC never does (ratio 0.80). Pawn `IsSprinting()` therefore reads false by design. Sprint also presses full input scale. Client-visual result **[unverified]**. `IsSprinting()` is true even when standing (unreliable).
- **Jump and sprint via abilities**: `ActivateAbilityNamed FortGameplayAbility_Jump / _Sprint` on the PlayerState ASC (`BotJump: 200 of 200 worked`, reaches mode 3, vz 630); `Character.Jump` alone almost never worked. `JumpZVelocity 600 → apex ≈183`; jump physics `rise = 600t − 492t²`.
- **Navigation, three layers** (all because the BR world originally has no navmesh):
  1. **Detailed grid** (`bot_nav_cache7.bin`, magic `NAV7`, 100u cells, 3 floors/cell, 32×32 tiles in a hash map, capsule sweeps r28 h66, hop edges with jump marks, drops ≤1000u with A* skipping drops over 60% health, climb ≤70/100u, railing vaulting clearance 135). Built by `run_navbuild.sh` (`-BOT_NAVBUILD`, ~5 min, ~36M rays, ~2.9M nodes, 104 MB). Unfinished scans save to `*.partial.bin`, never overwrite a finished one. **Rebuild the cache (delete bin, bump `NavCacheMagic`) whenever `NavMaxClimb/NavMaxDrop`/capsule values or the map change.** Loads at first bot tick (~1 s hitch). Bots re-seed the map where they stand; unreachable goals are dropped (mainland only; enclosed pockets <900 nodes give no verdict).
  2. **Coarse whole-island map** (`bot_nav_coarse7.bin`, 400u cells, ~245k nodes, explored in ~30 s) for long trips (storm, far POIs) and fallback.
  3. **Real UE navmesh** (below) tried first when `navmesh=1`; `TryMeshRoute` accepts partial paths if they gain ≥600u and ≥30% of the distance, **validates the first 3 segments with knee/chest rays and rejects routes through unjumpable obstacles** (mesh does not carve railings/props: 87% of obstacle-crossing mesh routes were straight through the obstacle). Mesh adds ~900 ms worst-frame hitches (no-mesh worst ≈290 ms). Mesh gave chests/bot 3.7–3.8 vs 3.3–3.5 but ~25% of queries answered.
- **Creating the navmesh on S7 (all reverse engineered)**: the S7 BR world has **no NavigationSystem object** (`World.NavigationSystem` offset 312 null; reflection cannot create one). The exe on disk is **encrypted** (`.text` uniform noise), so Ghidra/r2 on the file is useless; with `nav_experiment=1` the DLL dumps the decrypted *loaded* image to `Win64/exe_image.bin` (runtime base e.g. 0x6ffff5db0000, **raw file offset == RVA**; `r2 -n -a x86 -b 64 -c 'pd 60 @ 0x<rva>' exe_image.bin`, or `objdump -b binary -mi386:x86-64`). `RebootBotWork/nav_vtables.txt`, `navstr.py` (function→strings via `.pdata`), `callers.py`.
  - `UNavigationSystemConfig` vtable slot 71 (0x238) = `CreateAndConfigureNavigationSystem(UWorld&)`; slot 73 = Configure. `UNavigationSystemBase`: 71 Tick, 72 CleanUp, 73 Configure, 76 ApplyWorldOffset, 77 `InitializeForWorld(World, Mode)` → 95 `OnWorldInitDone`, 89 Build, 98 `SpawnMissingNavigationData`.
  - Working sequence: call the real `AddNavigationSystemToWorld(World, 1 /*GameMode*/, Default__AthenaNavSystemConfig, 1)` (RVA **0x24ae500**; Reboot's 4.21 signature matches in this 4.22 image; 4-arg form; skips if `World.NavigationSystem` already set) → set `bGenerateNavigationOnlyAroundNavigationInvokers` + `bInitialBuildingLocked=false` (via `SetPawnBitOn`) → `SpawnMissingNavigationData` (slot 98) spawns the RecastNavMesh/AthenaNavMesh actor → spawn `/Script/NavigationSystem.NavMeshBoundsVolume` (no brush model on the server, extent 0) and write a **huge `FBoxSphereBounds` straight into its RootComponent at offset 328** (found by scanning a pawn's capsule for its position; not a reflected property) → `NavigationSystemV1.OnNavigationBoundsUpdated(volume)` → `RegisterNavigationInvoker(bot, 4000, 7000)` per bot makes tiles appear around them. `K2_ProjectPointToNavigation` then finds the mesh within ~40 s; `FindPathToLocationSynchronously` 0.01–0.09 ms/query.
  - Spawning `AthenaNavSystemConfigOverride` deferred does not create the system; `AthenaAISettings.NavigationSystemConfig` is null on the CDO.
  - **Known failure**: in the user's live session the mesh was created 09:26:48 *after the bus left* and never came up ("not up yet 0 of 12" for 8 min) although tests that created it before the bus worked. Nudges (re-announce bounds, re-register invokers) were added; the bus-before-creation case should be tested with `EXTRA='-BOT_TEST -BOT_COUNT=99 -BOT_BUS_DELAY=4 ...'`.
- **Stuck handling** (many layers, because the world is full of railings/props/roofs):
  - `TraceBlocked` returns hit fraction/actor/ImpactPoint; probes ignore pawns, the held weapon, names with Door/Pawn; **trace starts inside the bot's own capsule/weapon made the solver say "no way out" almost always** → `Obs.SelfPawn/SelfWeapon` are passed as ignore context.
  - `ScanVision` (16-ray fan, 900u) with goal alignment/room/visit memory (400u cells)/hysteresis; feeler wall-following (AvoidAngle persists 0.9 s); cornered→escape (12-direction open-air probe, 1.6 s commit, jump pulses); pacing detector (`BotPace`: net <350u but >1200u walked in 8 s → drop goal, 3 s random escape, every 3rd a 7000u wander); `BadPickups` blacklist (300 s; 30 s for sight-blocked); `MaybeUnstick` ladder (35/65/100 s, storm pace ×0.4): run+jump off the nearest 100–1500u drop, then warp to open ground found by tracing down (prefers ≥150u lower) if no human within 6000u (`BotUnstick`, `NextWarpTry` retry); `StruggleCheck/SolveEscape` (24 headings, 80u steps, rise ≤150 hop, drop ≤420, jumping only at planned spots, hop timing from jump physics, run-up, `BadEscapes` memory 150 s ±25°); obstacle-top measurement (`BotHop`, jump over anything <150 above the feet, "jump and pray" when no top found, limit 5 per blocker); first stuck event on a breakable blocker smashes it (`BotBreak`, `BotBlocker` log shows building/health/breakable/hopOver). Struggle stats: plans without jumps free the bot ~75–85% in 6 s, with jumps ~40% (some walls, e.g. `S_WaterWall_*`, block movement but no visibility ray sees them). Remaining hotspots: Diner area (44000,−4000), Mansion (28000..32000,88000), Mine (−4000,−60000, multi-level), Bunker (84000,56000), Temple (80000,4000..12000). ~60% of blocked events are "no progress" with no blocker; only ~10% have another pawn within 120u (bots blocking each other is NOT the cause).
  - Rooftop landings were a trap (z 853/1229/2389 vs street −299); the last bots sitting boxed in died to the storm.
- Pace: a real game halves around 240 s and is at ~10% by ~540 s; bots-only baseline was far faster. `FightGovernor` (target alive curve `100·exp(−t/560)` from bus start) scales fight appetite → 50% left at ~350–400 s, ~60 at 5 min. Per-POI landing caps in `ChooseLanding` (`gLandingCount`, room = max(4, containers/3)); landing corridor 45000u around the bus line; dive 3200/s; jump at closest approach (MaxReach 115k).

---

## 8. Perception, behaviour, personality

- Awareness model in `Observe`: up to 3 nearest candidates; head/body/feet visibility traces; field of view 100% within 70°, tapering to 0 at 150°, min 0.35 inside 900u; scan windows (0.6 s every 1.5–4 s); notice time `0.2 + dist/2400` s scaled by visibility, target speed, recent shots, per-bot `Alertness`; being hurt → instant awareness; `EnemyLastPos` only updated from what is seen.
- Hearing: the game's `AthenaAISettings` read at runtime — footsteps 150..4000 (speed scale 440), crouch ×0.1, damaging shot 1500, NPC max 5500. Bots turn at half range and investigate once per 8 s; `gSounds` = bot shots, chest opens, human shots (detected via `AmmoCount` drops in a 0.4 s pawn scan); `footsteps=0` disables.
- Personality: per-bot `Aggression` (0.05..0.7, mostly loot-first; ×1.35 and humans ×2.5 appetite for armed bots), `Alertness`, bravery 0.75–1.35 in `ShouldFlee` (hp+shield+0.3·weapon score), `Curiosity` 0.3–0.9. Fights gated by a once-per-sighting roll with `IgnoreUntil`, provocation by `HurtAt`/very close (armed only). Engage range 2250 (unarmed 700; first version 1300/3000 caused a bloodbath). Unarmed bots shot at turn on the shooter with the pickaxe (flee rule off for 12 s after being hurt). Pickups ignored 8 s after seeing an enemy or being hurt (unless unarmed and item within 1000u). Pickup selection requires line of sight (`HasSight`, 1.5 s memo).
- Reactions: hit with no visible enemy → 1.5 s look-around spin, then box-up (4 walls via `PlaceBuilding`) or run (`BotReact`); cover seeking when shot at range (`BotCover`, 12 bearings × 3 reaches, hidden-from-enemy ray test, 6 s run, then peek/heal); hide behind built wall (`BotHide`); rocket dodge (`BotDodge`, `FortProjectileBase` scan); defence: wall build when taking fire (every 2.5–4.5 s), jump under fire, box-up at low health (`BotDefend`); memory: go investigate the last position of a lost enemy.
- Spawn-island activities (`DecideIsland`): walk, emote (`PlayEmote`/`GAB_Emote_Generic`, `BotEmote: N of M emotes exist`), look around, hop, grab a floor weapon, fire into the air.
- Realism work driven by the user's recorded matches (`session_record.csv`, `analyze_session.py`): real player heading change median 22°/s (bots were 0 then flipping 100–250°/s), jumps ~5–7/min (bots 0.8), sprints 38% of moving (bots 66–85%), ~5 shots/min at median 15 m, 7 item switches/min; bots strafed 99% sideways at 5 m. Fixes: hops/glances/drift/fight variety/sprint-run mix; body facing follows heading at 600°/s (turns looked snappy otherwise); `BotFlip` log tracks >100° heading flips. Open: heading flips from avoid deflection/BotBreak/sidestep/escape/chest dithering.
- Rejected after A/B (18 matches): a tactics/learning layer — no gain, removed. Husk leftovers parked for 50v50: attack slots (`FortAIHotSpot`), `StepAside`, speed by urgency.

---

## 9. Performance (the big lesson)

- The "bots teleport/stutter" problem was **mostly logging**: Reboot's spdlog `flush_on(info)` flushed both the Wine console sink and the file on every line (~2 ms each under Wine) while bots wrote 50–100 lines/s. Fix in `log.h`: `flush_on(warn)`, console sink level warn, `spdlog::flush_every(1s)`, explicit flush before `ExitProcess`. Bot code went 20–60 ms/frame → 0.3–4 ms; server 12–17 fps → 27–30 fps with 90 bots. **Check log volume first when something stutters.**
- `UFortKismetLibrary::FindGroundLocationAt` (4×/s per airborne bot) was another large cost → a plain downward `TraceBlocked` (interval 1.0/0.4/0.15 s by height).
- Per-frame decision budget `gDecisionBudgetMs=8` (`bot_budget_ms`), rotating start bot, bots >30000u from any human decide every 0.45 s instead of 0.16 s; ray budget `gTraceBudget=120`/frame; `BotPerf` log every 10 s (fps, longest gap, bot ms/frame, section timers, rays). Per-bot `Bot #N pos=` lines every 15 s. Ray casts are ~0.4 ms/frame.
- With 100 bots in 50v50 bot code is ~7 ms/frame (26.8 fps server).
- Windows/Wine: leaked UDP socket in the shared Wine prefix's wineserver can keep port 7777 after a server is killed while a client still runs → server silently falls to 7778 while everything expects 7777. Stop everything and start the server first. X server (Xwayland) hit 256 clients from leaked wine `explorer.exe`/`winedevice.exe` orphans → new windows failed; kill orphans from *old* prefixes only.
- Crash investigation method: `CRASHLOG` lines in `reboot.log` come from a vectored exception handler (also `BOT_CRUMB` breadcrumbs). `IsBadReadPtr` probes show up as benign first-chance exceptions (kernel32). Resolve `rip`: exe addresses → disassemble `/tmp/claude-1000/exe_image.bin`; DLL RVAs → `.pdata` function bounds + `llvm-objdump` (ImageBase 0x180000000), string refs show the UFunction name; DLL frames symbolised with `llvm-pdbutil publics`. Stack scan lists `.rdata/data` pointers too — only `.text` offsets are real return addresses.

---

## 10. Spectating and other engine fixes

- Human death did not spectate the killer in S7: Reboot only did it for `Fortnite_Version < 6`; now `< 8` with a killer-pawn fallback from `PlayerState.PawnPrivate` (`FortPlayerController.cpp ClientOnPawnDiedHook`, log `Spectating the killer`), and `PlayerToSpectateOnDeath = killer` (the game's auto-spectate picked a random player).
- `ReadyToStartMatch` returns true under `-BOT_TEST` once bots exist and under `-PROD_MATCH` once `gAllowStart`.
- Fill mode (`bot_fill=N` / `-PROD_FILL`): bots are spawned at the countdown so the server knows exactly who joined; `bot_fill_lead` (default 20 s before the countdown ends); if the GUI starts the bus with no countdown, fill happens at "the bus has already left" (late-drop path). Tick() runs with zero bots in fill mode. `PlaylistCapacity` uses `UFortPlaylist.MaxPlayers` (50v50 = 100). 50v50 is **groundwork only**: no safe zone, no teammate coordination, solo-calibrated pace.
- Session recorder: the recorder (`RecordSession`, `session_record.csv`; rows `S` 5 Hz per real player + ~1/18 of bots with `zoneEdgeDist,zoneRadius` appended, `E` for hurt/heal/shot/equip) is how you compare bots with humans.

---

## 11. Automatic match mode (`-PROD_MATCH`)

Flags (`prodopts.h`): `-PROD_MATCH` (headless: no GUI thread, `Globals::bNoMCP=false` so skins load, match flow in `prodmatch.h`), `-PROD_MATCH_ID=`, `-PROD_CALLBACK=<url>`, `-PROD_TOKEN=`, `-PROD_JOIN_WINDOW=15`, `-PROD_BUS_PAUSE=3.5`, `-PROD_COUNTDOWN=10`, `-PROD_FILL=N`, `-PROD_DRAIN_GRACE=10`, `-PROD_IDLE_TIMEOUT=600`, `-PROD_END_GRACE=90`, `-PROD_ORPHAN_LIMIT=6`. Parsing note: `-prod_match` must not match the start of `-prod_match_id=` (boundary check).

State machine: Booting → Waiting → JoinWindow → Filling → BusPause → Countdown → InMatch → Closing, ticked every 0.25 s from `TickFlushHook`.
- A player joining starts/resets the join window (15 s); at expiry bots fill the rest (`FillBots`), then waits `BusPause` after the last bot, then replicates the GUI's **"Start Bus Countdown"** exactly: `bStartedBus`, `AmountOfPlayersWhenBusStart`, `WarmupCountdownEndTime/Duration/EarlyDuration`, giving the visual countdown.
- `CountConnectedHumans()` = client connections with a PlayerController. When all humans have left (after bots are in) the server closes after `DrainGrace`; also closes `EndGrace` after a winner, `IdleTimeout` if nobody ever joined, and after `OrphanLimit` undelivered reports in a row with nobody connected. There is intentionally **no maximum match time**.
- Reporting: WinHTTP worker thread with a queue; `POST <callback>/event` with header `X-Prod-Token`; heartbeat every 10 s; HTTP 410 = backend does not know the match (→ close); `closing` is posted before `ExitProcess`.
- Edge cases handled: `Target>1` guard (full-lobby skip), early `if (!bUp) return;`, backend gone, double-close.
- **KNOWN BUG (unfixed, source)**: in the Filling case `gRt.FilledAt = Now` uses `Now` captured before `FillBots` (~10 s) → should be `Clock()` after `FillBots`; effect: the countdown starts ~1 s after the last bot instead of 3.5 s.
- **Status**: live-verified once by the user (ready → join window → bots → countdown → bus), but not re-run since; full headless bots-only validation needs the user's manual server stopped. Build15 DLL still has the old 5400 s cap.

---

## 12. Weird behaviours and gotchas (quick list)

- Bots with a PC are frozen unless unpossessed (§4). Bots' `bInAircraft`/default-pawn spawn flood at bus end (§4).
- `FString::Set` dangling; `FString::ToString` on empty crashes; ProcessEvent with null params crashes (§3).
- Name `PlayerID` is capitalised; the dump lists params in reverse order.
- The human is not in `GetAlivePlayers()`.
- Writing movement-related replicated bytes every tick can change physics (`CurrentMovementStyle`).
- `ApplyDamage` doesn't damage buildings; `OnDamageServer` only pays out.
- The S7 `HitActor` batch-cue param is a raw pointer; many params were wrong in the 3.5 dump (e.g. non-existent functions).
- Reading a file path "next to reboot.log" means the Wine cwd, i.e. `<Season 7>/FortniteGame/Binaries/Win64`.
- `localnite-launcher` windows: orphaned processes from a dead server can trigger false aborts in `run_one.sh` (check stale injector pids).
- Ground hitches seen on clients were GC (10–33 ms), streaming unloads and party/replay logs, not bots.
- Per-run noise in bot tests is large (different landings): compare several runs, use `ablate.sh`.
- The bot name lookup is deterministic because `rand()` isn't seeded (not a bug, but don't expect new names).
- Stale `static inline` per-translation-unit globals (§4) are a recurring trap in header-only code here — keep shared state `inline`, not `static inline`.

---

## 13. Verified vs untested (as of 2026-10-28)

Verified in logs/playtests: unpossess recipe, bot fall/jump/loot/pickup, melee+ranged kills, end-to-end bot→human kill, kill credit, UniqueId clone, PlayerID fix, team layout for solo and 50v50 fill, nav cache build/load, navmesh creation (bus-after-creation case failed once), friction/speed fix, spawn flood fix, log flush fix, landing caps, pace curve, prod flow (once).
**[unverified]** at the last report: hit-effect visuals (arrow/tracers/muzzle) from build80–81; sprint animation on clients; loot-on-death; wall yaw; railing hopping/breaking (builds 13–14); mesh with bus-before-creation; two-human 98-bot team layout live; storm next-zone rotation (`BotZone ... next=` line); pickaxe feel; all of build90/91 tuning; DLL Build16 (not built).

---

## 14. Game version

- The Season 7 build used for testing is now **7.20**. Reboot offsets/signatures and all bot work were tuned and tested on **7.00**; 7.20 is **untested** (expect to re-find offsets such as the navigation `AddNavigationSystemToWorld` RVA 0x24ae500, and verify dump-derived property names).

---

## 15. Suggested next steps

1. Get Reboot running on 7.20 (headless exe patch, re-find offsets), then re-verify prod match and bots there.
2. Rebuild Build16 (remove `MaxSeconds`, fix `FilledAt`) when the user allows; re-run a full headless `-PROD_MATCH` test once their manual server is stopped.
3. Verify the unverified effect/animation items with the user (ask what felt different), then tune.
4. Possible improvements: loot-first behaviour, unarmed bots avoiding armed ones, avoiding drops/falls at low health, heal in storm, landing toward the likely centre, shared stuck memory in the nav maps, human paths as proven walkable, vehicles, 50v50 safe zone and teammate coordination, per-player memory.
