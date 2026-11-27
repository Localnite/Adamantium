

Adamantium is a fork of the widely used Project Reboot game server (commit 10c659028ad9d6816f78226483f11a884bf81f57), featuring QOL features and Save The World Support.

Below are the key points about this project:

## Features

1. **Experimental Bot Support**: The project includes experimental rule-based AI bots. (only tested on S7 v7.00-.30, may not work well or at all on older seasons.)
2. **Experimental Save The World Support**: Experimental STW support is included, results may vary.


All existing features from Project Reboot carry over.


## Installation

1. Clone this repository: `git clone https://github.com/Localnite/Adamantium`
2. Build the project using your preferred build tools. (Use Visual Studio 2022)
3. Run the server using your preferred launcher.
## Contributing

Contributions are welcome! Feel free to create pull requests or report issues on the GitHub repository.

## Credits

Bot AI design ideas in this fork were informed by the public [Syndicate Source Code](https://github.com/Sacttt/Syndicate-Source-Code) project (MIT licensed, commit `2035ea1`). In particular:

- Separating *observed travel* from a successfully requested path or build commit, so a stalled bot cannot renew its own progress deadline (`BotNavigation::Progress`).
- Detecting loops by distance walked rather than by a fixed time window (`BotRoute::Memory`).
- Capping build-fight escalation with a height budget so two bots cannot ramp above each other forever (`CombatSkillPolicy`).
- Stateless, per-bot deterministic difficulty gating (`BotDifficulty::Allow`).
- Tracing from the muzzle before firing or editing through a bot's own builds (`ShotObstacle`).
- Keeping decision rules in pure, engine-free code so they can be unit tested (`game-server/Validation/`).
- A single bounded investigation objective that cannot be replaced by every successive gunshot (`BotTactics::Offer`).
- Deriving a search bearing from damage knockback instead of using a hidden shooter's live position (`PlayerBots::RecordDamage`).
- Continuous braking when approaching a ledge or stair crest (`BotSkillPolicy::CrestInput`).
- Refusing an ammo pickup that sits beside a nearby threat (`BotTactics::AmmoEscapeTarget`).
- Separate storm margins for starting to rotate and for finishing a fight (`BotTactics::ShouldRotate` / `CanCommitFight`).
- A spatial hash grid so local actor queries scale with nearby density rather than world size (`BotSpatial::Index`).
- A combat goal point that combines approach/retreat with a sustained sideways step (`BotTactics::CombatGoal`).
- A retained crank / 90 sequence: latched quarter-turn stories, alternating shoot pauses, and story/time/material caps (`BotCombatBuild::GroundClimb`).
- Distance-accumulated footstep noise (`BotTactics::Footfall`), aim alignment gating (`AimAligned`), incoming-shot tube geometry (`IncomingShot`).
- Safe-use-time and cancel rules for healing (`BotSustain::SafeUseTime` / `PlanHeal`), loot/weapon scoring (`BotLootPlan` / `BotMission`), storm destination policy (`BotTactics`), bus-departure projection (`BotLanding`), successive-hit pressure and squad allocation (`CombatSkills` / `team-test.cpp`).

These decisions live as pure, engine-free rules in `Project Reboot 3.0/BotRules.h` and are covered by `tests/bot_rules_test.cpp`.

Also credited: retained high-ground selection in late circles (`SpectraBotsIntegration` endgame height sampling), validated watchdog escape directions (`BotGroundRescue`), shotgun→rifle follow-up swaps (`CombatPlan::Followup`), and weapon-class hearing radii (`BotAwareness`).

Also credited: velocity-based target lead instead of retrospective aim lag (`CombatSkillPolicy::Prediction`); burst fire and aim alignment (`PlayerBots.h`); stepping out from behind own cover (`BotGroundAI::SupportedCombatPeek`); proactive terrain probes (`BotTerrainAI::TerrainNeedsRamp`); travel-time-based storm rotation (`BotTactics::ShouldRotate`); time-to-contact healing (`BotSustain::SafeUseTime`); real-player gunfire as an investigation cue (`BotFireInput.h`); role/height-aware loot scoring (`BotLootPlan::LootCost`); crouch posture (`BotTactics::Posture`); failed-goal memory (`BotRoute::Memory`); retreat-spot scoring (`BotSurvival::Pick`); panic-box gates (`BotAwareness::React`); and shooting at the cover an enemy hid behind (`PlayerBots::AttackEnemyCover`).

Our implementations of those ideas live in `Project Reboot 3.0/BotRules.h` and are covered by `tests/bot_rules_test.cpp`. They were written for this build and are not copied code.

## License

This project is licensed under the BSD-3-Clause license.

---

**Note**: Adamantium is a work in progress, and further updates and enhancements are planned. Explore, experiment, and contribute to make it even better! 🚀
