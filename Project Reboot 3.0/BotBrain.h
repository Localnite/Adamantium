#pragma once

// Scripted brain for the simulated player bots (see bots.h).
//
// Structure: Observe() reads the engine, Decide() is pure logic (observation in, action out, no engine calls),
// Apply() writes the engine. Keeping Decide() pure means it can be swapped for a learned policy later.
// Movement is driven by writing the CharacterMovement velocity directly, because the bots run without a controller
// (bRunPhysicsWithNoController), so there is no input pipeline to feed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "FortGameModeAthena.h"
#include "FortSafeZoneIndicator.h"
#include "FortKismetLibrary.h"
#include "BuildingContainer.h"
#include "AbilitySystemComponent.h"
#include "FortPlayerStateAthena.h"
#include "GameplayAbilitySpec.h"
#include "BuildingSMActor.h"
#include "FortPlayerControllerAthena.h"
#include "FortGameStateAthena.h"
#include "FortWeapon.h"
#include "BuildingActor.h"
#include "FortPlayerPawn.h"
#include "FortPawn.h"
#include "FortPickup.h"
#include "FortInventory.h"
#include "FortItem.h"

inline bool bBotGliderVisuals = true; // set bIsSkydiving / bIsParachuteOpen on the pawn so clients see a dive and a glider
inline double gUpdParts[4] = {};        // Update: before Observe, Observe call, after Observe
inline std::unordered_map<int, int> gLandingCount;   // bots already heading for each POI, so a drop does not put thirty of them in one village
inline double gObsSection[16] = {};     // milliseconds spent in each part of Observe, for the report
inline int gTraceBudget = 120;          // ray casts the bots may make in one server frame (the expensive scans wait when it is used up)
inline int gTracesFrame = 0, gTracesTotal = 0;
inline double gTraceTime = 0.0, gTraceWhole = 0.0;
struct TraceWholeTimer;
inline float gDecisionBudgetMs = 8.0f;   // most time per server frame the bots may spend on new decisions
inline double gTimeObserve = 0.0, gTimeDecide = 0.0, gTimeApply = 0.0, gTimeOther = 0.0; // summed over the frames of one report
inline int gDecisions = 0, gDecisionsFar = 0, gDecisionsSkipped = 0;
inline int gBotFillTotal = 0;          // when the bus countdown is near, top the match up with bots to N players in all (0 = the old way: the bots in the GUI are spawned at the start)
inline float gBotFillLead = 20.0f;    // how many seconds before the bus the bots are added
inline bool gNavExperiment = false;  // try to bring a navmesh into the map at runtime (see NavProbe in bots.h)
inline bool gClientShots = false;     // send the game target data like a client (the game ignored it in tests, so off by default)
inline float gMeleeSlowMult = 0.75f, gMeleeSlowTime = 0.5f; // read from the pawn: how much a swing slows the swinger, and for how long
inline bool bRampMotion = true;       // false turns the acceleration ramps off (see RampVelocity)
inline float gSprintMaxWalk = 1.0f;    // MaxWalkSpeed while sprinting as a share of the sprint speed (a real player's reads 440 for 550)
inline bool gSprintAbility = true;     // activate the sprint ability
inline bool gMoveStyle = true;        // write the pawn's replicated movement style (sprint animation)
inline bool gHopOver = true;          // measure and jump over low obstacles
inline int gStruggles = 0, gStruggleExits = 0, gStruggleNone = 0, gStruggleFreed = 0, gStruggleNotFreed = 0; // struggle reports (see StruggleCheck)
inline bool gFootsteps = true;         // bots hear footsteps
inline bool gUnstick = true;          // the long-stuck ladder
inline bool gNavMeshTest = false;     // log whether the mesh has links down from ledges and through railings
inline bool gNavMesh = false;         // build the game's own navigation mesh around the bots and route with it (the grid stays as the fallback)
inline bool gMeshReady = false;       // the mesh exists under at least one bot
inline int gMeshRoutes = 0, gMeshFallbacks = 0;
inline bool gMoveAssist = true;        // also press a very light movement input while walking, so the game replicates a nonzero acceleration (other players' screens then animate walking instead of idle)
inline float gMoveAssistScale = 0.15f; // strength of the move-assist input
inline bool gMovementInput = false;   // move with the game's movement input (measured: only ~100 units/s on the ground, so off) instead of writing velocities
inline float gSkillMin = 8.0f;   // bots get a skill between these, or all the same with -BOT_SKILL=N
inline float gSkillMax = 92.0f;
inline float gSkillFixed = -1.0f;
inline bool bBotsFightBots = true;    // true: everybody on another team is a target (in solo that is everyone); false: bots only attack real players

enum class EBotBrainState : uint8_t
{
	Idle,    // waiting (on the bus, or before the first update)
	Wander,  // warmup: stroll around the spawn point
	Skydive, // free fall towards the landing target
	Glide,   // glider open
	Ground,  // landed: storm avoidance, looting, wandering
	Engage,  // chasing / fighting an enemy
	Retreat, // losing a fight: run away and break line of sight
	Heal     // out of sight, using a healing item
};

inline const char* BotBrainStateName(EBotBrainState State)
{
	switch (State)
	{
	case EBotBrainState::Idle: return "Idle";
	case EBotBrainState::Wander: return "Wander";
	case EBotBrainState::Skydive: return "Skydive";
	case EBotBrainState::Glide: return "Glide";
	case EBotBrainState::Ground: return "Ground";
	case EBotBrainState::Engage: return "Engage";
	case EBotBrainState::Retreat: return "Retreat";
	case EBotBrainState::Heal: return "Heal";
	}

	return "?";
}

enum class EBotItemKind : uint8_t { Other, Pickaxe, Gun, Heal };

enum class EBotTarget : uint8_t { None, Wander, Storm, Chest, Pickup, Poi, Investigate, Harvest, Hunt };

struct BotAction
{
	bool bDriveXY = false;  // set horizontal velocity to Dir*Speed
	float DirX = 0.0f;
	float DirY = 0.0f;
	float Speed = 0.0f;
	bool bStop = false;     // zero horizontal velocity (only used on the ground)
	bool bDriveZ = false;
	float VerticalSpeed = 0.0f;
	bool bFace = false;
	float Yaw = 0.0f;
	bool bSetPitch = false;
	float Pitch = 0.0f;
	bool bJump = false;
	bool bWarp = false;          // last resort for a bot trapped for minutes: put it down on the ground nearby
	FVector WarpPos{};
	bool bAttackMelee = false;
	bool bAttackRanged = false;
	bool bSearchChest = false;
	bool bPickup = false;
	bool bEquip = false;
	UObject* EquipDef = nullptr;
	FGuid EquipGuid{};
	bool bHarvest = false;   // swing the pickaxe at Brain.HarvestActor
	bool bBuildWall = false; // put a wall between the bot and BuildToward
	bool bBoxUp = false;     // four walls round the bot
	bool bBoxRoof = false;   // ...and a roof over it (an enemy close by can shoot down into an open box)
	FVector BuildToward{};
	// single pieces asked for by a build plan (ramps and the walls in front of them, see StepRampPlan); Apply places them
	struct BuildPiece { int Kind; FVector Location; float Yaw; bool bPlanRamp; };
	std::vector<BuildPiece> Pieces;
	bool bEmote = false;     // play an emote (spawn island fun)
	int EmoteIndex = 0;
	bool bUseHeal = false;   // finish using a healing item: apply it and spend it
	FGuid HealGuid{};
	float HealHealth = 0.0f;
	float HealShield = 0.0f;
	float HealCapHealth = 100.0f;
	float HealCapShield = 100.0f;
	float MaxWalkSpeed = 600.0f;
};

struct BotBrainState
{
	EBotBrainState State = EBotBrainState::Idle;
	EBotBrainState AppliedState = EBotBrainState::Idle; // last state whose side effects (visuals, speeds) were applied
	float StateSince = 0.0f;

	FVector Anchor{};          // warmup wander centre
	bool bHasAnchor = false;

	// current goal
	FVector Target{};
	bool bHasTarget = false;
	EBotTarget TargetKind = EBotTarget::None;
	ABuildingContainer* TargetChest = nullptr;
	AFortPickup* TargetPickup = nullptr;
	float TargetPickedAt = 0.0f;
	float TargetStartDist = 0.0f;   // chest goals: how far away it was when picked (height counted four times)
	float PauseUntil = 0.0f;   // standing still (e.g. while loot drops)

	// landing
	bool bLandingChosen = false;
	FVector LandingTarget{};
	float PrevBusDist = 1e9f;
	float JumpAt = -1.0f;

	float NextDecisionTime = 0.0f;
	float NextGroundTraceTime = 0.0f;
	float GroundZ = 0.0f;
	bool bHaveGroundZ = false;

	// blocked detection
	FVector LastDecisionPos{};
	float LastDecisionTime = 0.0f;
	float BlockedAccum = 0.0f;
	int BlockedEvents = 0;
	float LastBlockedEventTime = 0.0f;
	float SidestepUntil = 0.0f;
	FVector SidestepDir{};
	float NextJumpAllowed = 0.0f;
	float NextBlockLog = 0.0f;
	int WallLogs = 0;

	// idle watchdog
	FVector WatchPos{};
	float WatchTime = 0.0f;
	int IdleChecks = 0;
	int WatchdogResets = 0;

	// combat
	FVector EnemyLastPos{};
	float EnemyLastSeen = -100.0f;
	float NextAttackTime = 0.0f;
	float NextEquipTime = 0.0f;
	float StrafeUntil = 0.0f;
	float StrafeSign = 1.0f;
	UObject* PickaxeDef = nullptr;
	FGuid PickaxeGuid{};
	int Attacks = 0;
	AController* Controller = nullptr;             // the bot's own controller (it is not attached to the pawn), used as the killer when a shot kills
	AFortPlayerPawnAthena* TargetPawn = nullptr;  // who the last attack was aimed at
	float TargetDist = 0.0f;
	bool bHaveFaceYaw = false;
	float FaceYaw = 0.0f;
	int Shots = 0;
	int ShotsHit = 0;
	float ReleaseAt = 0.0f;        // when to release the weapon's primary ability after an activation
	int FailedActivations = 0;
	float NextWeaponShowCheck = 0.0f;
	bool bLoggedWeaponFix = false;
	float NextStormTick = 0.0f;
	bool bLoggedStorm = false;
	int ClipGuess = 0;

	// personality (rolled once per bot)
	float Curiosity = -1.0f;  // how likely it is to go and check out a sound
	float Bravery = -1.0f;    // below 1 it runs from fights sooner, above 1 it stays in them

	// hearing and memory
	float LastSoundHandled = 0.0f;
	float NextInvestigate = 0.0f;

	// retreating and healing
	float NoFleeUntil = 0.0f;    // cornered or committed: no running away until then
	float FleeLogAt = 0.0f;
	int RoofBreaks = 0;                       // roofs smashed through to get at a chest underneath
	float NextRoofBreak = 0.0f;
	int KillsSeen = 0;                        // kills already reacted to (see the dance in DecideWalking)
	float DanceAt = 0.0f, DanceUntil = 0.0f;  // dancing over a kill: when it starts, and how long the weapon stays away
	float HealStart = 0.0f;      // when the current heal began, 0 = not healing
	float HealUse = 0.0f;
	FGuid HealGuid{};
	float HealHealth = 0.0f;
	float HealShield = 0.0f;
	float HealCapHealth = 100.0f;
	float HealCapShield = 100.0f;
	float HealStartHealth = 0.0f;
	float HealStartShield = 0.0f;
	float NoHealUntil = 0.0f;
	bool bStyleSprint = false;       // the movement style written to the pawn last (only written when it changes)
	float HopUntil = 0.0f;           // jumping over a railing: keep going straight
	int HopTries = 0;
	void* HopBlocker = nullptr;
	FVector StuckPos{};              // where the bot was when it last made real progress, and for how long it has been going nowhere
	float StuckSince = 0.0f;
	int StuckStage = 0;
	float NextWarpTry = 0.0f;        // the last-resort put-down found no spot or a player was watching: when to look again
	float NextScan = 0.0f;           // a player turns the camera all the time: now and then the bot 'looks around' and sees all round for a moment
	float ScanUntil = 0.0f;
	float RotateAt = 0.0f;           // when this bot sets off for the next circle (players do not all leave on the same second)
	float SeenNextRadius = 0.0f;     // which next circle RotateAt was drawn for
	float StormDropAt = -100.0f;     // when a storm goal last trapped it, so the next one comes from another side
	int StormTries = 0;
	UObject* HealDefPtr = nullptr;   // the item being used, to know when it is in hand
	bool bHealAbilityFired = false;  // its own use ability was started (it then plays the animation, heals and spends the item itself)
	bool bHealAbilityOk = false;
	int Heals = 0;
	UObject* ClipDef = nullptr;
	float ReloadAt = 0.0f;

	// obstacle avoidance and navigation
	float AvoidUntil = 0.0f;       // keep steering around an obstacle until then
	float AvoidAngle = 0.0f;       // degrees away from the goal direction
	int AvoidCount = 0;
	float AvoidWindowStart = 0.0f;
	FVector SteerGoal{};           // where the bot is really heading right now (a path waypoint or the goal); the probes look that way
	bool bSteerGoal = false;
	std::vector<FVector> NavPath;
	size_t NavIndex = 0;
	FVector NavGoal{};
	bool bNavWanted = false;
	float NavQueryAt = 0.0f;
	float NavPathTime = 0.0f;

	// vision: where the bot has been (400 unit cells -> visits), so a scan prefers ground it has not just covered
	std::unordered_map<int64_t, uint8_t> Cells;
	float CellStamp = 0.0f;
	float CellDecayAt = 0.0f;
	float VisionYaw = 0.0f;        // absolute heading chosen by the last scan, for hysteresis
	float NextLookScan = 0.0f;
	int VisionScans = 0;

	// getting out of corners
	bool bWantEscape = false;      // Decide asks Observe to look for an open direction
	float EscapeUntil = 0.0f;
	FVector EscapeDir{};
	float NextEscapeJump = 0.0f;
	bool bEscapePlanned = false;          // the escape was worked out by SolveEscape: it jumps only where the plan says
	FVector EscapeStart{};
	std::vector<float> EscapeHops;        // distances along the escape direction where a jump is needed (the obstacle's face)
	std::vector<float> EscapeRises;       // and how high each one is
	float NextSolveAt = 0.0f;
	struct BadEscape { FVector Pos; float Heading; float Time; };
	std::vector<BadEscape> BadEscapes;    // headings that did not get this bot out (some walls only block movement and no ray sees them)
	bool bEscapeBackoff = false;          // stepping back to get a run-up for a jump
	int EscapeBackoffs = 0;
	int EscapeJumps = 0;                  // jumps made on the current plan, and how far the plan was meant to go (for the report)
	float EscapeReach = 0.0f;
	float EscapeFace0 = 0.0f;
	float NextFootHeard = 0.0f;           // footsteps make a bot look once in a while, not every tick
	float StruggleRefTime = 0.0f;         // window for "has this bot got anywhere in the last few seconds"
	FVector StruggleRefPos{};

	// a run of ramps it is building and walking up (see StartRampPlan)
	bool bRampPlan = false;
	int RampPhase = 0;                    // 0: getting to where the first ramp starts, 1: building and climbing
	float RampDx = 0.0f, RampDy = 0.0f;   // which way the ramps rise (along a grid axis)
	FVector RampTile0{};                  // centre of the first ramp's tile; Z is the ramp's foot
	FVector RampStand{};                  // where it stands to start
	int RampPlaced = 0, RampWanted = 0, RampFails = 0;
	bool bRampWalls = false;              // a wall at the top of every ramp (taking fire)
	float RampUntil = 0.0f, RampNextPiece = 0.0f, RampBest = -1e9f, RampProgressAt = 0.0f, RampJumpAt = 0.0f;
	float NextRampPlan = 0.0f;            // earliest time for another plan
	const char* RampWhy = "";

	// shut in on a raised floor (a roof with a rim, see RoofCheck)
	FVector RoofAnchor{};
	float RoofSince = 0.0f;
	float RoofWantAway = 0.0f;            // seconds spent there while wanting to be somewhere else
	float RoofLastTick = 0.0f;
	float NextRoofTry = 0.0f;
	int RoofTries = 0;
	bool bRoofPending = false;            // a roof escape is running (to report whether it worked)
	std::vector<float> RoofBadYaws;       // edges already tried from this roof
	bool bLandingChecked = false;         // the glide target was checked for a roof over it
	float StruggleAt = 0.0f;              // when the last escape was planned, and from where (to report whether it worked)
	FVector StrugglePos{};
	int Struggles = 0;
	float LastEscapeAt = -100.0f;
	int EscapeCount = 0;
	float EscapeWindowStart = 0.0f;

	// aim skill and the bot's real aim (it turns towards its target at a limited speed and the bullets go where it actually points)
	float Skill = -1.0f;
	float AimYaw = 0.0f;
	float AimPitch = 0.0f;
	bool bHaveAim = false;
	float AimLastTime = 0.0f;
	AFortPlayerPawnAthena* AimTarget = nullptr;
	float AimTargetTime = -100.0f;
	float AcquireTime = 0.0f;
	float Reaction = 0.5f;
	bool bSawEnemy = false;
	bool bTargetingShown = false;    // the aiming stance (bIsTargeting) is currently switched on for the other players to see
	float LastShotTime = 0.0f;
	int Spray = 0;
	float WobbleYaw = 0.0f;
	float WobblePitch = 0.0f;
	float NextWobble = 0.0f;

	bool bClearedIsland = false;     // the spawn island loot was taken away when the bus started

	// statistics for the test reports
	int ChestsOpened = 0;
	int Kills = 0;
	int ItemsPicked = 0;

	// reacting to being hit from nowhere, and falls
	float PrevTotal = 0.0f;
	float HurtFrom = 0.0f;
	float ConfusedUntil = 0.0f;
	float LookYaw = 0.0f;
	float LookDir = 1.0f;
	float LookTarget = 0.0f;
	bool bWeaponHidden = false;
	float NextBoxUp = 0.0f;
	float NextSeedAt = 0.0f;
	FVector EnemyVel{};            // how the enemy was moving when last seen (to guess where it went)
	float MeleeSlowUntil = 0.0f, PlantUntil = 0.0f, MeleeDamageAt = 0.0f;
	AFortPlayerPawnAthena* MeleeTarget = nullptr;
	float MoveLastTime = 0.0f;
	float PrevHeading = 0.0f, PrevHeadingTime = 0.0f;
	bool HavePrevHeading = false;
	int JumpWatch = 0;
	UObject* SprintInstance = nullptr;
	UObject* JumpInstance = nullptr;    // the jump ability that is still active: a client's ends on landing, and the game warns once a spec has been activated 255 times without ending
	float JumpEndAt = 0.0f;             // when to end it (-1: set at the next update)
	int StrafeMode = 1;            // 0 standing, 1 sideways, 2 towards/away
	float SprintShare = -1.0f;     // how much of its travelling this bot runs flat out
	bool bTravelSprint = false;
	float NextSprintToggle = 0.0f;
	float LookOffset = 0.0f, LookUntil = 0.0f, NextLook = 0.0f;   // glancing around while walking
	float NextHop = 0.0f;
	float WeavePhase = 0.0f;
	bool bStrong = false;        // one of the few strong players
	float DodgeUntil = 0.0f;
	float DodgeDirX = 0.0f, DodgeDirY = 0.0f;
	float LastHealthTotal = 0.0f;
	bool bSprintFlag = false;
	float NextMoveLog = 0.0f;
	FVector SpeedPrevPos{};
	FVector RealPrevPos{};          // where the pawn was at the last movement update, for the speed it really has (see ShownVelocity)
	float RealPrevTime = 0.0f;
	int HeldUpFrames = 0;
	float SpeedPrevTime = 0.0f;
	float SpeedSum = 0.0f, AskedSum = 0.0f;
	int SpeedSamples = 0;
	FVector HidePos{};           // just behind the wall it built
	float HideUntil = 0.0f;
	float HideYaw = 0.0f;
	FVector CoverPos{};          // somewhere out of the enemy's sight it is running to
	float CoverUntil = 0.0f;
	float NextCoverAt = 0.0f;
	// How far it has got to noticing each enemy near it (0 nothing, 1 has noticed). One value per enemy: with a single value that restarted
	// whenever the candidate changed, two enemies close by wiped each other's progress every frame and a bot next to a real player took
	// ~20 s to notice them (2026-11-09).
	struct FAwareOf { void* Pawn; float Value; float Updated; FVector LastSeenPos{}; float LastSeenAt = -100.0f; };
	std::vector<FAwareOf> AwareOf;
	float PerceptLast = 0.0f;
	float Alertness = -1.0f;     // how sharp this bot's senses are
	std::vector<uint8_t> NavJump;   // parallel to NavPath: the way to that waypoint is a hop
	float NearTarget = 0.0f, NearBest = 1e9f, NearSince = 0.0f;
	float NextFightJump = 0.0f;
	float NextPeek = 0.0f;
	float WeaponHiddenUntil = 0.0f;
	bool bLookAtAttacker = false;
	bool bBoxPending = false;
	float FallBias = -1.0f;          // how this bot misjudges heights (0.75-1.3 of the real thing)
	bool bFalling = false;
	float FallPeakZ = 0.0f;
	float FallStartTotal = 0.0f;
	float FallCheckAt = 0.0f;
	float FallHeight = 0.0f;
	int FallsLogged = 0;

	// walking back and forth without getting anywhere (slopes, railings, anything the probes do not explain)
	FVector PaceRefPos{};
	FVector PaceLastPos{};
	float PaceRefTime = 0.0f;
	float PacePath = 0.0f;
	int Paces = 0;

	// breaking through what is in the way (railings, fences, thin walls)
	bool bBreaking = false;
	float BreakUntil = 0.0f;
	int BreakCount = 0;
	void* HarvestTracked = nullptr;  // progress check on the prop being hit: a swing that takes nothing off it (a bus, say) is given up on
	float HarvestLastHealth = 0.0f;
	int HarvestNoProgress = 0;
	std::vector<AActor*> BuiltPieces; // what this bot built (to get out of its own box again)

	// Building is done the way a player has to do it: one piece at a time, each only once the bot has turned to face where it goes (see StepBuildQueue).
	struct BuildOrder
	{
		int Kind = 0;
		FVector Location{};
		float Yaw = 0.0f;         // the piece's own yaw
		float FaceYaw = 0.0f;     // where the builder has to look to place it
		bool bFace = true;        // false: overhead or underfoot, no turning needed
		bool bPlanRamp = false;
		bool bHide = false;       // a wall to stand behind once it is up
		bool bBox = false;
		FVector HidePos{};
		FVector Toward{};
		float QueuedAt = 0.0f;
	};
	std::vector<BuildOrder> BuildQueue;
	float NextPieceAt = 0.0f;
	int BoxPlaced = 0;

	// Editing: a window opened in the wall it hides behind, shot through, and closed again
	AActor* HideWall = nullptr;
	float WindowAt = 0.0f;          // when the edit it has started is finished (0 = none under way)
	float WindowUntil = 0.0f;
	float NextWindowPlay = 0.0f;
	bool bWindowOpen = false;
	float BoxedAt = 0.0f;
	float NextWallCheck = 0.0f;
	float FightBreakUntil = 0.0f;     // smashing a wall that is between it and the enemy
	float DropAvoidWindow = 0.0f;
	int DropAvoids = 0;
	float DropRelaxUntil = 0.0f;

	// temperament and memory of fights
	float Aggression = -1.0f;                 // how readily it picks a fight with somebody it merely sees (0..1)
	float IgnoreUntil = 0.0f;                 // having decided not to fight the enemy it sees, it leaves them alone until then
	float IgnoredAtDist = 0.0f;               // how far away the enemy was when it decided that
	float IgnoredAt = -100.0f;                // when
	bool bIgnoredLostSight = false;           // the ignored enemy has been out of sight since
	float DecidedAt = -100.0f;                // when it last made that decision
	float HurtAt = -100.0f;                   // last time something other than the storm hurt it
	void* FoughtPawn = nullptr;               // who it last fought or was shot by...
	float FoughtUntil = 0.0f;                 // ...and until when seeing them again means picking the fight back up
	bool bIgnoredUnarmed = false;             // the enemy it is ignoring was let go while it had no gun
	float NextGrudgeHunt = 0.0f;
	std::vector<std::pair<AFortPickup*, float>> BadPickups; // pickups it could not reach (until when to ignore them)
	struct SightMemo { void* Item; float Until; bool bSeen; };
	std::vector<SightMemo> PickupSight;                      // recent line-of-sight answers for loot on the ground (a trace per item per tick would be too much)
	void* AimHistoryTarget = nullptr;
	std::deque<std::pair<float, FVector>> AimHistory;       // where its target was a moment ago: a hand can only follow what the eye saw

	// materials, building and hunting
	AActor* HarvestActor = nullptr;
	float NextHarvestHit = 0.0f;
	int HarvestHits = 0;
	int HarvestBurst = 0;
	int HarvestBurstLen = 3;
	std::vector<void*> SkipHarvest;  // props that could not be reached
	float FirstGroundTime = 0.0f;    // when the bot first stood on the island
	float NextBuild = 0.0f;
	int Builds = 0;
	float NextDoorCheck = 0.0f;
	bool bLoggedReady = false;

	// spawn island: what the bot is up to while waiting for the bus
	int IslandMode = 0;            // 0 walk about, 1 emote, 2 look around, 3 hop about, 4 grab a weapon, 5 shoot into the air
	float IslandUntil = 0.0f;
	float IslandNextTurn = 0.0f;
	float IslandYaw = 0.0f;

	// travelling between points of interest
	int PoiIndex = -1;
	std::vector<int> RecentPois;

	std::vector<FVector> VisitedChests; // chests done with, or given up on because they could not be reached
	BotAction LastAction; // decisions run at ~6Hz, the last one is re-applied every tick
};

struct BotObservation
{
	float Now = 0.0f;
	EAthenaGamePhase Phase = EAthenaGamePhase::None;
	float Governor = 1.0f;   // how keen the lobby is to fight right now (see FightGovernor)
	FVector Pos{};
	AActor* SelfPawn = nullptr;   // for ray casts that must not hit the bot itself or the weapon in its hand
	AActor* SelfWeapon = nullptr;
	int MovementMode = -1; // EMovementMode: 1 walking, 3 falling
	float HeightAboveGround = 1e9f;
	bool bHaveHeight = false;
	bool bZoneKnown = false;
	FVector ZoneCenter{};
	float ZoneRadius = 0.0f;
	bool bNextZoneKnown = false;
	FVector NextZoneCenter{};
	float NextZoneRadius = 0.0f;

	// obstacle probes along the last commanded heading (walking only)
	bool bProbed = false;
	bool bLowBlocked = false;
	bool bHighBlocked = false;
	bool bLeftClear = true;
	bool bRightClear = true;
	int WoodCount = 0;
	int SelfTeam = -1;
	UObject* BlockerActor = nullptr; // what the wall probes hit (for breaking through)
	FVector BlockerPoint{};
	bool bSoftDeflect = false;   // the deflection comes from the long look ahead (an obstacle is coming), not from touching a wall
	bool bHasDeflection = false; // the wall ahead can be walked around
	float DeflectionAngle = 0.0f;
	bool bHopOver = false;       // the obstacle ahead is low enough to jump over (a railing): its top is HopHeight above the bot's feet
	float HopHeight = 0.0f;
	bool bHasEscape = false;     // an open direction found because the bot is cornered
	FVector EscapeDir{};

	// nearest enemy
	bool bHasEnemy = false;
	FVector EnemyPos{};
	float EnemyDist = 1e9f;
	bool bEnemyVisible = false;
	bool bEnemyIsHuman = false;
	bool bEnemyAimingAtMe = false;   // a real player is pointing at this bot
	bool bProjectile = false;        // something fast is flying at this bot
	float ProjTime = 99.0f;          // seconds until it gets here
	float ProjDirX = 0.0f, ProjDirY = 0.0f;
	AFortPlayerPawnAthena* EnemyPawn = nullptr;
	float EnemyHealth = 100.0f;
	float EnemyShield = 0.0f;
	int EnemyWeaponScore = 0;

	// a sound worth checking out
	bool bHeardSound = false;
	FVector SoundPos{};
	float SoundTime = 0.0f;

	// condition
	float Health = 100.0f;
	float Shield = 0.0f;
	int HealCount = 0;           // healing items carried
	int FreeSlots = 5;
	bool bCanHeal = false;       // there is an item that would help now
	bool bNeedsHeal = false;     // and it is worth using it
	UObject* HealDef = nullptr;
	FGuid HealGuid{};
	float HealUseTime = 3.0f;
	float HealHealth = 0.0f;
	float HealShield = 0.0f;
	float HealCapHealth = 100.0f;
	float HealCapShield = 100.0f;
	bool bHealIsWeapon = false;  // can be put in hand for the animation
	int SelfWeaponScore = 0;

	// weapons
	bool bHasGun = false;
	UObject* BestGunDef = nullptr;
	FGuid BestGunGuid{};
	int BestGunScore = 0;
	bool bHoldingGun = false;
	bool bHoldingBestGun = false;
	float HeldRange = 0.0f;
	float HeldInterval = 0.5f;
	bool bHeldShotgun = false;

	// loot worth walking to
	bool bHasPickup = false;
	FVector PickupPos{};
	AFortPickup* PickupPtr = nullptr;
};

struct BotPawnInfo
{
	AFortPlayerPawnAthena* Pawn = nullptr;
	FVector Pos{};
	int Team = -1;
	bool bIsBot = false;
	float Health = 100.0f;
	float Shield = 0.0f;
	int WeaponScore = 0;
	float Speed = 0.0f;      // how fast it moved since the last scan
	float LastFired = -100.0f; // when its weapon last made a noise
	float AimYaw = 0.0f;       // where a real player is looking (not read for bots)
	bool bAimKnown = false;
};

struct BotPickupInfo
{
	AFortPickup* Pickup = nullptr;
	FVector Pos{};
	int Score = 0;
	EBotItemKind Kind = EBotItemKind::Gun;
};

struct BotPoi
{
	std::string Name;
	FVector Center{};
	int Count = 0;
};

struct BotItemInfo
{
	EBotItemKind Kind = EBotItemKind::Other;
	int Score = 0;
	float Range = 0.0f;
	float Interval = 0.5f;
	bool bShotgun = false;
	int Tier = 1;
	float Damage = 20.0f; // body-shot damage per bullet before the tier bonus (a shotgun's is for a point blank hit)
	bool bSniper = false;
	float HealHealth = 0.0f;    // heals
	float HealShield = 0.0f;
	float HealCapHealth = 100.0f;
	float HealCapShield = 100.0f;
	float UseTime = 3.0f;
	bool bWeaponDef = false;
	bool bWood = false;
};

namespace BotBrain
{
	inline constexpr float RadToDeg = 180.0f / 3.14159265358979323846f;
	inline constexpr float DegToRad = 3.14159265358979323846f / 180.0f;
	inline float WalkSpeed = 440.0f;   // a normal run: below this the walk animation plays
	inline float RunSpeed = 550.0f;    // travelling to loot
	inline float SprintSpeed = 550.0f; // storm, chasing
	inline constexpr float SkydiveHorizontal = 3200.0f;
	inline constexpr float SkydiveVertical = -2400.0f;
	inline constexpr float GlideHorizontal = 1100.0f;
	inline constexpr float GlideVertical = -700.0f;
	inline constexpr float GlideDeployHeight = 4000.0f;
	inline constexpr float JumpReach = 60000.0f; // jump once the bus is this close (horizontally) to the landing target
	inline constexpr float MaxReach = 115000.0f; // the farthest a dive from bus height can cover (about 33s of fall at 3200 a second)
	inline constexpr float ChestSearchRadius = 6000.0f;
	inline constexpr float PickupSearchRadius = 2500.0f;
	inline constexpr float EngageRange = 3000.0f;
	inline constexpr float DisengageRange = 5000.0f;

	// Sight model after the game's own AI (7.30: FortAIPawnStats Dormant/Alert sight, AISenseConfig_Sight auto-success, AthenaAISettings
	// de-aggro). The game's per-AI values live in server-only data, so the radii and angles here are ours; the de-aggro numbers are the game's.
	inline constexpr float AlertMemory = 10.0f;              // seconds after seeing an enemy or being hurt that a bot stays alert
	inline constexpr float AlertFullFieldDeg = 110.0f;       // alert: sees in full this far off its facing (dormant: 70)
	inline constexpr float AlertNoticeScale = 0.6f;          // alert: notices this much faster
	inline constexpr float AutoSuccessRange = 250.0f;        // an enemy that ducks out of sight this close to where it was seen is still tracked
	inline constexpr float AutoSuccessTime = 1.5f;           // ... for this long
	inline constexpr float ReducedDeAggroRange = 4000.0f;    // AthenaAISettings.ReducedDeAggroRange
	inline constexpr float ReducedDeAggroAfter = 30.0f;      // AthenaAISettings.DurationReduceAggroLimits
	inline constexpr float MeleeRange = 240.0f;
	inline constexpr float ProbeLength = 260.0f;
	inline constexpr float PoiMinDistance = 1000.0f;   // a travel goal is a point of interest at least this far away...
	inline constexpr float PoiMaxDistance = 100000.0f; // ...and at most this far
	inline constexpr float PoiArriveRadius = 2500.0f;

	// ---- shared perception (refreshed once per tick for all bots) ----
	inline std::vector<FVector> gChests;
	inline std::vector<ABuildingContainer*> gChestActors; // parallel to gChests
	inline std::vector<int> gChestPoi;                    // parallel to gChests, index into gPois (-1 unknown)
	inline std::vector<BotPoi> gPois;
	// Which containers are already opened (by anybody, bots and humans alike) and how many unopened ones each POI still has. Rebuilt by every
	// ScanContainers (20 s) and kept up to date in between by SearchChest, so bots stop walking to chests that are empty.
	inline std::vector<char> gChestLooted;                // parallel to gChests
	inline std::vector<int> gPoiRemaining;                // parallel to gPois
	// Chests that stalled bots (watchdog, pacing, timeout), shared by all bots; keyed by position because the arrays above are rebuilt. Three
	// failures and the chest is left alone, otherwise the next ninety bots walk into the same railing.
	inline std::unordered_map<int64_t, int> gChestFails;
	inline int64_t ChestKey(const FVector& P) { return ((int64_t)((int)std::floor(P.X / 100.0f)) << 32) ^ (uint32_t)((int)std::floor(P.Y / 100.0f)); }
	inline float gNextChestScan = 0.0f;

	// What becomes of the chests the bots set off for: every outcome is counted by name, the first ones of each kind are logged with where the bot and
	// the chest were, and the totals go to the log once a minute (BotChests). This is how to find out why bots come away from a town with nothing.
	inline std::map<std::string, int> gChestOutcomes;
	inline float gChestOutcomeLog = 0.0f;

	inline void ChestOutcome(const char* Why, const BotBrainState& Brain, const FVector& Pos, float Now, int Id)
	{
		const int Count = ++gChestOutcomes[Why];

		if (Count <= 12 && Brain.TargetKind == EBotTarget::Chest)
		{
			const float Dx = Brain.Target.X - Pos.X, Dy = Brain.Target.Y - Pos.Y;
			LOG_INFO(LogBots, "BotChest #{} {}: chest at ({:.0f}, {:.0f}, {:.0f}), bot at ({:.0f}, {:.0f}, {:.0f}), {:.0f}u away and {:+.0f} up, {:.0f}s after setting off from {:.0f}u",
				Id, Why, Brain.Target.X, Brain.Target.Y, Brain.Target.Z, Pos.X, Pos.Y, Pos.Z, std::sqrt(Dx * Dx + Dy * Dy), Brain.Target.Z - Pos.Z, Now - Brain.TargetPickedAt, Brain.TargetStartDist);
		}

		if (Now >= gChestOutcomeLog)
		{
			gChestOutcomeLog = Now + 60.0f;
			std::string Line;

			for (auto& Entry : gChestOutcomes)
				Line += std::format("{} {}, ", Entry.first, Entry.second);

			LOG_INFO(LogBots, "BotChests so far: {}", Line);
		}
	}
	inline bool gZoneKnown = false;
	inline FVector gZoneCenter{};
	inline float gZoneRadius = 0.0f;
	// How fast the safe zone's edge is coming in (units a second): the shrink plus the centre sliding towards the next circle. A bot waiting
	// inside the circle needs to leave earlier the faster this is (a fixed 600u warning was ~3 s in a late circle; a bot died that way, 2026-11-09).
	inline float gZoneCloseSpeed = 0.0f;

	// how close to the edge a bot lets the storm come before it heads in: 600u, plus ~12 s of the edge's own speed (time to stop what it is
	// doing and get going); capped so a fast early circle does not pull every bot off its loot at once
	inline float StormMargin() { return 600.0f + (std::min)(gZoneCloseSpeed * 12.0f, 4000.0f); }
	inline bool gNextZoneKnown = false;   // the circle the storm closes to next, as the players see it on the map
	inline FVector gNextZoneCenter{};
	inline float gNextZoneRadius = 0.0f;
	inline float gNextZoneRead = 0.0f;
	inline std::vector<BotPawnInfo> gPawns;

	// A real lobby thins out slowly: about 90% left a minute after the bus, 70% at three minutes, half at six. If the bots are killing each
	// other faster than that, they get more peaceful (0 = nobody starts a fight, 1 = business as usual); fights already under way still finish.
	inline float gFightClockStart = 0.0f;
	inline float gPaceSeconds = 560.0f;
	inline float gGovernor = 1.0f;
	inline float gGovernorAt = -10.0f;
	inline int gGovernorAlive = 0;

	inline float FightGovernor(float Now, EAthenaGamePhase Phase)
	{
		if (Phase == EAthenaGamePhase::Aircraft && gFightClockStart <= 0.0f)
			gFightClockStart = Now;
		else if (Phase == EAthenaGamePhase::Warmup || Phase == EAthenaGamePhase::None)
			gFightClockStart = 0.0f;

		if (gFightClockStart <= 0.0f)
			return 1.0f;

		if (Now - gGovernorAt < 1.0f)
			return gGovernor;

		gGovernorAt = Now;
		int Alive = 0;

		for (auto& Other : gPawns)
		{
			if (Other.Pawn && Other.Health > 0.0f)
				++Alive;
		}

		const float Elapsed = Now - gFightClockStart;
		const float Target = (std::max)(6.0f, 100.0f * std::exp(-Elapsed / gPaceSeconds));
		gGovernorAlive = Alive;
		gGovernor = (std::max)(0.0f, (std::min)(1.0f, ((float)Alive - Target) / 8.0f + 0.15f));
		return gGovernor;
	}
	inline float gNextPawnScan = 0.0f;
	inline std::vector<BotPickupInfo> gPickups;
	inline float gNextPickupScan = 0.0f;
	inline std::vector<void*> gBotPlayerStates; // so a real player can be told apart from a (unpossessed) bot
	inline std::unordered_map<UObject*, BotItemInfo> gItemInfo;
	inline int gNavState = 0;          // 0 not known yet, 1 the map has a navmesh, -1 it has none
	inline int gNavFailures = 0;
	// rockets and other projectiles in flight, so a bot can see one coming
	struct BotProjectile { FVector Pos; FVector Vel; float Time; };
	inline std::vector<BotProjectile> gProjectiles;
	inline float gNextProjectileScan = 0.0f;

	struct BotSound { FVector Pos; float Time; float Radius; void* Source; int Team; };
	inline std::vector<BotSound> gSounds; // recent gunshots and chest openings, for the bots to hear

	inline void AddSoundNow(const FVector& Pos, float Radius, void* Source, int Team)
	{
		gSounds.push_back(BotSound{ Pos, UGameplayStatics::GetTimeSeconds(GetWorld()), Radius, Source, Team });

		if (gSounds.size() > 64)
			gSounds.erase(gSounds.begin(), gSounds.begin() + 16);
	}

	inline std::unordered_map<void*, AFortPlayerStateAthena*> gLastDamager; // victim pawn -> who hurt it last (credit for bot kills)
	inline std::unordered_map<void*, float> gFallTime;                      // victim pawn -> when fall damage was last applied (the 'fall' weapon mark is only good for a few seconds)
	inline std::unordered_map<void*, float> gHumanShotTime;                 // real player's pawn -> when its weapon last fired (to credit a kill the engine never told us about)
	inline std::unordered_map<void*, float> gLastDamageTime;                // when that was, so an old attacker is not credited with a storm or fall death
	inline std::unordered_map<void*, std::string> gLastWeaponPath;          // victim pawn -> lower case path of the weapon that hurt it last (death cause for the kill feed)

	inline void RegisterBotPlayerState(void* PlayerState)
	{
		gBotPlayerStates.push_back(PlayerState);
	}

	inline bool IsBotPlayerState(void* PlayerState)
	{
		for (auto Registered : gBotPlayerStates)
		{
			if (Registered == PlayerState)
				return true;
		}

		return false;
	}

	inline float Dist2D(const FVector& A, const FVector& B)
	{
		const float Dx = A.X - B.X;
		const float Dy = A.Y - B.Y;
		return std::sqrt(Dx * Dx + Dy * Dy);
	}

	inline float RandRange(float Min, float Max)
	{
		return Min + (Max - Min) * (float(rand() % 10000) / 10000.0f);
	}

	inline std::string Lower(std::string Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char C) { return (char)std::tolower(C); });
		return Text;
	}

	inline bool Contains(const std::string& Text, const char* Needle)
	{
		return Text.find(Needle) != std::string::npos;
	}

	// ---- item classification (cached per item definition) ----
	inline const BotItemInfo& GetItemInfo(UObject* Def)
	{
		static BotItemInfo Nothing;

		if (!Def)
			return Nothing;

		auto Found = gItemInfo.find(Def);

		if (Found != gItemInfo.end())
			return Found->second;

		BotItemInfo Info;
		const std::string Path = Lower(Def->GetPathName());
		Info.bWood = Contains(Path, "wooditemdata");
		static auto WeaponDefClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeaponItemDefinition");

		// healing items (the names are matched on the path; the first few are logged so a wrong match is easy to spot)
		if (Contains(Path, "/consumables/"))
		{
			auto SetHeal = [&](float Hp, float Sh, float CapHp, float CapSh, float Time, int Score)
			{
				Info.Kind = EBotItemKind::Heal;
				Info.HealHealth = Hp;
				Info.HealShield = Sh;
				Info.HealCapHealth = CapHp;
				Info.HealCapShield = CapSh;
				Info.UseTime = Time;
				Info.Score = Score;
			};

			if (Contains(Path, "bandage")) SetHeal(15.0f, 0.0f, 75.0f, 100.0f, 4.0f, 30);
			else if (Contains(Path, "medkit")) SetHeal(100.0f, 0.0f, 100.0f, 100.0f, 10.0f, 45);
			else if (Contains(Path, "shieldsmall") || Contains(Path, "smallshield") || Contains(Path, "minishield")) SetHeal(0.0f, 25.0f, 100.0f, 50.0f, 2.5f, 35);
			else if (Contains(Path, "shield")) SetHeal(0.0f, 50.0f, 100.0f, 100.0f, 5.0f, 50);
			else if (Contains(Path, "chug")) SetHeal(100.0f, 100.0f, 100.0f, 100.0f, 12.0f, 70);
			else if (Contains(Path, "purplestuff") || Contains(Path, "slurp")) SetHeal(25.0f, 25.0f, 100.0f, 100.0f, 2.5f, 55);

			Info.bWeaponDef = WeaponDefClass && Def->IsA(WeaponDefClass);

			static int LoggedItems = 0;

			if (++LoggedItems <= 14)
				LOG_INFO(LogBots, "BotItem: {} -> {} (heals {:.0f} health / {:.0f} shield, use {:.1f}s, weapon definition: {})", Def->GetPathName(), Info.Kind == EBotItemKind::Heal ? "healing item" : "not recognised", Info.HealHealth, Info.HealShield, Info.UseTime, Info.bWeaponDef);

			return gItemInfo.emplace(Def, Info).first->second;
		}

		if (WeaponDefClass && Def->IsA(WeaponDefClass))
		{
			if (Contains(Path, "harvest") || Contains(Path, "pickaxe"))
			{
				Info.Kind = EBotItemKind::Pickaxe;
			}
			else
			{
				int TypeBonus = 0;

				if (Contains(Path, "shotgun")) { Info.Kind = EBotItemKind::Gun; Info.bShotgun = true; Info.Range = 1300.0f; Info.Interval = 0.95f; TypeBonus = 4; Info.Damage = 90.0f; }
				else if (Contains(Path, "sniper")) { Info.Kind = EBotItemKind::Gun; Info.Range = 5000.0f; Info.Interval = 1.7f; TypeBonus = 2; Info.Damage = 100.0f; Info.bSniper = true; }
				else if (Contains(Path, "assault") || Contains(Path, "rifle")) { Info.Kind = EBotItemKind::Gun; Info.Range = 3500.0f; Info.Interval = 0.22f; TypeBonus = 6; Info.Damage = 30.0f; }
				else if (Contains(Path, "smg") || Contains(Path, "submachine")) { Info.Kind = EBotItemKind::Gun; Info.Range = 2500.0f; Info.Interval = 0.17f; TypeBonus = 5; Info.Damage = 19.0f; }
				else if (Contains(Path, "pistol") || Contains(Path, "revolver")) { Info.Kind = EBotItemKind::Gun; Info.Range = 2500.0f; Info.Interval = 0.4f; TypeBonus = 1; Info.Damage = 26.0f; }

				if (Info.Kind == EBotItemKind::Gun)
				{
					static int TierOffset = Def->GetOffset("Tier", false);
					const int Tier = TierOffset != -1 ? (int)Def->Get<uint8_t>(TierOffset) : 0;
					Info.Tier = Tier > 0 ? Tier : 1;
					Info.Score = 10 + Tier * 10 + TypeBonus;
				}
			}
		}

		return gItemInfo.emplace(Def, Info).first->second;
	}

	// A line trace that frees its parameter buffer (the stock wrapper in KismetSystemLibrary.h leaks one per call).
	// Returns true when something blocks the segment. The pawn is the world context (ignored via bIgnoreSelf), and so is
	// IgnoreExtra, which should be the pawn's held weapon: that is a separate actor attached to the pawn and would
	// otherwise block every trace that starts inside the capsule.
	inline double PerfNow()
	{
		static LARGE_INTEGER Frequency = [] { LARGE_INTEGER F; QueryPerformanceFrequency(&F); return F; }();
		LARGE_INTEGER Counter;
		QueryPerformanceCounter(&Counter);
		return (double)Counter.QuadPart * 1000.0 / (double)Frequency.QuadPart;
	}

	struct TraceWholeTimer
	{
		double Start = PerfNow();
		~TraceWholeTimer() { gTraceWhole += PerfNow() - Start; }
	};

	inline bool TraceBlocked(AActor* Context, AActor* IgnoreExtra, const FVector& Start, const FVector& End, std::string* OutHitName = nullptr, float* OutFraction = nullptr, UObject** OutHitActor = nullptr, FVector* OutHitPoint = nullptr, std::vector<uint8_t>* OutHitBytes = nullptr)
	{
		TraceWholeTimer WholeTimer;

		static auto TraceFn = FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.LineTraceSingle");
		static auto LibraryDefault = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");

		if (!TraceFn || !LibraryDefault)
			return false;

		static auto ParamsSize = TraceFn->GetPropertiesSize();
		static auto WorldContextOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "WorldContextObject");
		static auto StartOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "Start");
		static auto EndOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "End");
		static auto ChannelOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "TraceChannel");
		static auto ComplexOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "bTraceComplex");
		static auto IgnoreOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "ActorsToIgnore");
		static auto OutHitOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "OutHit");
		static auto IgnoreSelfOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "bIgnoreSelf");
		static auto ReturnOffset = FindOffsetStruct("/Script/Engine.KismetSystemLibrary.LineTraceSingle", "ReturnValue");
		static auto HitActorOffset = FindOffsetStruct("/Script/Engine.HitResult", "Actor", false);
		static auto HitTimeOffset = FindOffsetStruct("/Script/Engine.HitResult", "Time", false);
		static auto HitPointOffset = FindOffsetStruct("/Script/Engine.HitResult", "ImpactPoint", false);

		std::vector<uint8_t> Params(ParamsSize + 64, 0);

		*(UObject**)(Params.data() + WorldContextOffset) = Context;
		*(FVector*)(Params.data() + StartOffset) = Start;
		*(FVector*)(Params.data() + EndOffset) = End;
		*(uint8_t*)(Params.data() + ChannelOffset) = 0; // TraceTypeQuery1 (Visibility)
		*(bool*)(Params.data() + ComplexOffset) = false;
		*(bool*)(Params.data() + IgnoreSelfOffset) = true;

		TArray<AActor*> IgnoreList;

		if (IgnoreExtra)
			IgnoreList.Add(IgnoreExtra);

		memcpy(Params.data() + IgnoreOffset, &IgnoreList, sizeof(IgnoreList));

		const double TraceStart = PerfNow();
		LibraryDefault->ProcessEvent(TraceFn, Params.data());
		gTraceTime += PerfNow() - TraceStart;
		++gTracesFrame;
		++gTracesTotal;

		const bool bBlocked = *(bool*)(Params.data() + ReturnOffset);

		if (OutFraction)
		{
			// how far along the ray the hit was (0..1); 1 when nothing was hit. Without the property, assume halfway.
			*OutFraction = !bBlocked ? 1.0f : (HitTimeOffset != -1 ? *(float*)(Params.data() + OutHitOffset + HitTimeOffset) : 0.5f);
		}

		if (bBlocked && OutHitActor && HitActorOffset != -1)
		{
			const int ObjectIndex = *(int*)(Params.data() + OutHitOffset + HitActorOffset);

			if (auto Item = GetItemByIndex(ObjectIndex))
				*OutHitActor = (UObject*)Item->Object;
		}

		if (OutHitBytes)
		{
			static auto HitStruct = FindObject<UStruct>(L"/Script/Engine.HitResult");
			const int HitSize = HitStruct ? HitStruct->GetPropertiesSize() : 0;
			OutHitBytes->assign(Params.data() + OutHitOffset, Params.data() + OutHitOffset + HitSize);
		}

		if (bBlocked && OutHitPoint && HitPointOffset != -1)
			*OutHitPoint = *(FVector*)(Params.data() + OutHitOffset + HitPointOffset);

		if (bBlocked && OutHitName && HitActorOffset != -1)
		{
			const int ObjectIndex = *(int*)(Params.data() + OutHitOffset + HitActorOffset);

			if (auto Item = GetItemByIndex(ObjectIndex))
			{
				if (auto Hit = (UObject*)Item->Object)
					*OutHitName = Hit->GetPathName();
			}
		}

		IgnoreList.Free();

		return bBlocked;
	}

	// Asks the engine's navigation system for a route. Returns false when there is no navmesh or no route.
	// ---- our own navigation map ----
	// This map has no navigation system (see NavProbe), so the bots keep a walkability map of their own. It is a grid of 100 unit cells, each able to
	// hold a few floors, grown by flood fill from spots a bot really stands on (every container, and wherever a bot stands on ground the map does not
	// know yet). Each step traces down for the floor and sweeps a player-shaped capsule sideways, so what is recorded is what a bot can walk: stairs,
	// ramps, doorways, bridges and caves included, and fences it can hop over are marked as jumps. The map lives in a cache file between server runs.
	// Routes are found with A* over the part of the map between the bot and its goal; far goals are reached in stages.
	constexpr float NavCell = 100.0f;
	constexpr int NavTile = 32;                 // cells per tile side
	constexpr int NavL = 3;                     // floors one cell can hold
	constexpr int NavTileNodes = NavTile * NavTile * NavL;
	constexpr float NavMaxClimb = 70.0f;        // per step: stairs and steep ramps, not a ledge
	constexpr float NavMaxDrop = 1000.0f;       // a drop this far is walked off (about 10 m, which the game's fall damage barely notices)
	constexpr float NavInterestCell = 1000.0f;
	constexpr int NavInterestSide = 500;        // covers +-250000 units around the origin

	struct NavTileData
	{
		std::vector<float> Z;           // floor height of each node (cell * NavL + layer)
		std::vector<uint8_t> State;     // 0 empty, 1 floor known, 2 reached by walking, 3 reached and expanded
		std::vector<uint8_t> Edge;      // bit k: the neighbour in direction k can be walked to
		std::vector<uint16_t> Layer;    // 2 bits per direction: the floor of that neighbour
		std::vector<uint8_t> Jump;      // bit k: that step is a hop over something low
	};

	inline std::unordered_map<int64_t, NavTileData> gNavTiles;
	inline std::vector<uint8_t> gNavInterest;     // 1 where the map is wanted (around containers and stuck bots)
	inline std::deque<uint64_t> gNavFrontier;     // reached nodes still to expand
	inline int gNavScanBudget = 260;              // ray casts per server frame spent on exploring
	inline bool gNavGridEnabled = true;           // false switches the detailed map off
	inline bool gNavGridUnreachable = false;      // set by GridPath: the goal is covered by the finished map and cannot be walked to
	inline bool gNavScanDone = false;             // the frontier ran dry at least once
	inline bool gNavDirty = false;
	inline int gNavSaveTick = 0;
	inline AActor* gNavTracePawn = nullptr;       // any live pawn to cast the rays from
	inline long long gNavRays = 0;
	inline int gNavNodes = 0;
	inline int gGridCalls = 0, gGridRoutes = 0, gGridUnreach = 0, gGridNoOpinion = 0, gGridTimeUs = 0;
	inline int gGridNoWindow = 0, gGridNoStart = 0, gGridSame = 0, gGridNotDone = 0, gGridRimless = 0;
	inline bool gGridNoStartFlag = false;
	inline bool gNavEnclosed = false;
	inline char gNavWhy[160] = "";

	inline const int NavDx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
	inline const int NavDy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

	inline int NavFloorDiv(int A, int B) { int Q = A / B; return ((A % B != 0) && ((A < 0) != (B < 0))) ? Q - 1 : Q; }
	inline int NavCellOf(float V) { return (int)std::floor(V / NavCell); }
	inline int64_t NavTileKey(int Tx, int Ty) { return ((int64_t)Tx << 32) ^ (int64_t)(uint32_t)Ty; }
	inline uint64_t NavPack(int Gx, int Gy, int L) { return ((uint64_t)(uint32_t)(Gx + (1 << 20)) << 34) | ((uint64_t)(uint32_t)(Gy + (1 << 20)) << 4) | (uint64_t)L; }
	inline void NavUnpack(uint64_t P, int& Gx, int& Gy, int& L) { L = (int)(P & 15); Gy = (int)((P >> 4) & 0x3FFFFFFF) - (1 << 20); Gx = (int)(P >> 34) - (1 << 20); }

	inline NavTileData* NavGetTile(int Gx, int Gy, bool bCreate)
	{
		const int64_t Key = NavTileKey(NavFloorDiv(Gx, NavTile), NavFloorDiv(Gy, NavTile));
		auto It = gNavTiles.find(Key);

		if (It != gNavTiles.end())
			return &It->second;

		if (!bCreate)
			return nullptr;

		NavTileData& T = gNavTiles[Key];
		T.Z.assign(NavTileNodes, 0.0f);
		T.State.assign(NavTileNodes, 0);
		T.Edge.assign(NavTileNodes, 0);
		T.Layer.assign(NavTileNodes, 0);
		T.Jump.assign(NavTileNodes, 0);
		return &T;
	}

	inline int NavIndex(int Gx, int Gy, int L)
	{
		const int Lx = Gx - NavFloorDiv(Gx, NavTile) * NavTile, Ly = Gy - NavFloorDiv(Gy, NavTile) * NavTile;
		return (Ly * NavTile + Lx) * NavL + L;
	}

	inline bool NavInterestAt(int Gx, int Gy)
	{
		const int Ix = (int)std::floor((Gx + 0.5f) * NavCell / NavInterestCell) + NavInterestSide / 2, Iy = (int)std::floor((Gy + 0.5f) * NavCell / NavInterestCell) + NavInterestSide / 2;
		return Ix >= 0 && Iy >= 0 && Ix < NavInterestSide && Iy < NavInterestSide && !gNavInterest.empty() && gNavInterest[Iy * NavInterestSide + Ix];
	}

	inline void NavAddInterest(float X, float Y, float Radius)
	{
		if (gNavInterest.empty())
			gNavInterest.assign(NavInterestSide * NavInterestSide, 0);

		const int R = (int)std::ceil(Radius / NavInterestCell);
		const int Cx = (int)std::floor(X / NavInterestCell) + NavInterestSide / 2, Cy = (int)std::floor(Y / NavInterestCell) + NavInterestSide / 2;

		for (int Dy = -R; Dy <= R; ++Dy)
		{
			for (int Dx = -R; Dx <= R; ++Dx)
			{
				const int Ix = Cx + Dx, Iy = Cy + Dy;

				if (Ix < 0 || Iy < 0 || Ix >= NavInterestSide || Iy >= NavInterestSide || Dx * Dx + Dy * Dy > R * R + R)
					continue;

				gNavInterest[Iy * NavInterestSide + Ix] = 1;
			}
		}
	}

	inline bool NavCovered(float X, float Y) { return NavGetTile(NavCellOf(X), NavCellOf(Y), false) != nullptr; }

	inline bool NavNameIsPassable(const std::string& Name)
	{
		return Name.find("Door") != std::string::npos || Name.find("Pawn") != std::string::npos || Name.find("door") != std::string::npos;
	}

	// a player-shaped capsule swept from one point to another; blocked unless it only hits a door or another player
	inline bool NavCapsuleBlocked(const FVector& From, const FVector& To, float Radius, float HalfHeight)
	{
		static auto TraceFn = FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.CapsuleTraceSingle");
		static auto LibraryDefault = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");
		const char* Path = "/Script/Engine.KismetSystemLibrary.CapsuleTraceSingle";
		static int ParamsSize = TraceFn ? TraceFn->GetPropertiesSize() : 0;
		static int WorldOffset = FindOffsetStruct(Path, "WorldContextObject", false);
		static int StartOffset = FindOffsetStruct(Path, "Start", false);
		static int EndOffset = FindOffsetStruct(Path, "End", false);
		static int RadiusOffset = FindOffsetStruct(Path, "Radius", false);
		static int HalfOffset = FindOffsetStruct(Path, "HalfHeight", false);
		static int ChannelOffset = FindOffsetStruct(Path, "TraceChannel", false);
		static int ComplexOffset = FindOffsetStruct(Path, "bTraceComplex", false);
		static int IgnoreOffset = FindOffsetStruct(Path, "ActorsToIgnore", false);
		static int OutHitOffset = FindOffsetStruct(Path, "OutHit", false);
		static int IgnoreSelfOffset = FindOffsetStruct(Path, "bIgnoreSelf", false);
		static int ReturnOffset = FindOffsetStruct(Path, "ReturnValue", false);
		static int HitActorOffset = FindOffsetStruct("/Script/Engine.HitResult", "Actor", false);
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "BotNav: capsule sweep fn {} size {} world@{} start@{} end@{} radius@{} half@{} channel@{} ignore@{} hit@{} return@{}", TraceFn != nullptr, ParamsSize, WorldOffset, StartOffset, EndOffset, RadiusOffset, HalfOffset, ChannelOffset, IgnoreOffset, OutHitOffset, ReturnOffset);
		}

		if (!TraceFn || !LibraryDefault || WorldOffset == -1 || StartOffset == -1 || EndOffset == -1 || RadiusOffset == -1 || HalfOffset == -1 || ReturnOffset == -1 || OutHitOffset == -1)
			return false;

		std::vector<uint8_t> Params(ParamsSize + 64, 0);
		*(UObject**)(Params.data() + WorldOffset) = gNavTracePawn;
		*(FVector*)(Params.data() + StartOffset) = From;
		*(FVector*)(Params.data() + EndOffset) = To;
		*(float*)(Params.data() + RadiusOffset) = Radius;
		*(float*)(Params.data() + HalfOffset) = HalfHeight;
		if (ChannelOffset != -1) *(uint8_t*)(Params.data() + ChannelOffset) = 0;
		if (ComplexOffset != -1) *(bool*)(Params.data() + ComplexOffset) = false;
		if (IgnoreSelfOffset != -1) *(bool*)(Params.data() + IgnoreSelfOffset) = true;

		TArray<AActor*> IgnoreList;
		if (IgnoreOffset != -1) memcpy(Params.data() + IgnoreOffset, &IgnoreList, sizeof(IgnoreList));

		LibraryDefault->ProcessEvent(TraceFn, Params.data());
		++gNavRays;
		++gTracesTotal;

		if (!*(bool*)(Params.data() + ReturnOffset))
			return false;

		if (HitActorOffset != -1)
		{
			const int ObjectIndex = *(int*)(Params.data() + OutHitOffset + HitActorOffset);

			if (auto Item = GetItemByIndex(ObjectIndex))
			{
				if (auto Hit = (UObject*)Item->Object)
				{
					if (NavNameIsPassable(Hit->GetPathName()))
						return false;
				}
			}
		}

		return true;
	}

	// What would stop this pawn's body, as opposed to what a ray sees: a capsule swept with the collision profile of the pawn's own capsule. Every other
	// probe here uses the visibility channel, and plenty of things block walking without blocking sight (railings, some fences, invisible walls), so a
	// bot could stand pushing against one and "see" nothing in its way. Other players are not counted. Used only when a bot is not getting anywhere.
	inline bool PawnSweep(AActor* Pawn, AActor* IgnoreExtra, const FVector& From, const FVector& To, float Radius, float HalfHeight, UObject** OutActor, FVector* OutPoint)
	{
		static auto TraceFn = FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.CapsuleTraceSingleByProfile");
		static auto ProfileFn = FindObject<UFunction>(L"/Script/Engine.PrimitiveComponent.GetCollisionProfileName");
		static auto LibraryDefault = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");
		const char* Path = "/Script/Engine.KismetSystemLibrary.CapsuleTraceSingleByProfile";
		static int ParamsSize = TraceFn ? TraceFn->GetPropertiesSize() : 0;
		static int WorldOffset = FindOffsetStruct(Path, "WorldContextObject", false);
		static int StartOffset = FindOffsetStruct(Path, "Start", false);
		static int EndOffset = FindOffsetStruct(Path, "End", false);
		static int RadiusOffset = FindOffsetStruct(Path, "Radius", false);
		static int HalfOffset = FindOffsetStruct(Path, "HalfHeight", false);
		static int ProfileOffset = FindOffsetStruct(Path, "ProfileName", false);
		static int IgnoreOffset = FindOffsetStruct(Path, "ActorsToIgnore", false);
		static int OutHitOffset = FindOffsetStruct(Path, "OutHit", false);
		static int IgnoreSelfOffset = FindOffsetStruct(Path, "bIgnoreSelf", false);
		static int ReturnOffset = FindOffsetStruct(Path, "ReturnValue", false);
		static int HitActorOffset = FindOffsetStruct("/Script/Engine.HitResult", "Actor", false);
		static int HitPointOffset = FindOffsetStruct("/Script/Engine.HitResult", "ImpactPoint", false);
		static int RootOffset = Pawn ? Pawn->GetOffset("RootComponent", false) : -1;
		static uint8_t Profile[16] = {};   // the FName of the pawn capsule's collision profile, as the game hands it out
		static bool bProfile = false, bLogged = false;

		if (!TraceFn || !ProfileFn || !LibraryDefault || !Pawn || WorldOffset == -1 || StartOffset == -1 || EndOffset == -1 || RadiusOffset == -1 || HalfOffset == -1 || ProfileOffset == -1
			|| ReturnOffset == -1 || OutHitOffset == -1 || HitActorOffset == -1 || RootOffset == -1)
		{
			if (!bLogged)
			{
				bLogged = true;
				LOG_INFO(LogBots, "BotUnseen: no pawn-shaped sweep in this build (fn {} profile fn {})", TraceFn != nullptr, ProfileFn != nullptr);
			}

			return false;
		}

		if (!bProfile)
		{
			auto Root = Pawn->Get<UObject*>(RootOffset);

			if (!Root)
				return false;

			std::vector<uint8_t> Ret(ProfileFn->GetPropertiesSize() + 16, 0);
			Root->ProcessEvent(ProfileFn, Ret.data());
			memcpy(Profile, Ret.data(), 8);
			bProfile = true;
			bLogged = true;
			LOG_INFO(LogBots, "BotUnseen: pawn-shaped sweeps use the pawn capsule's collision profile (name index {})", *(int*)Profile);
		}

		std::vector<uint8_t> Params(ParamsSize + 64, 0);
		*(UObject**)(Params.data() + WorldOffset) = Pawn;
		*(FVector*)(Params.data() + StartOffset) = From;
		*(FVector*)(Params.data() + EndOffset) = To;
		*(float*)(Params.data() + RadiusOffset) = Radius;
		*(float*)(Params.data() + HalfOffset) = HalfHeight;
		memcpy(Params.data() + ProfileOffset, Profile, 8);
		if (IgnoreSelfOffset != -1) *(bool*)(Params.data() + IgnoreSelfOffset) = true;

		TArray<AActor*> IgnoreList;

		if (IgnoreExtra)
			IgnoreList.Add(IgnoreExtra);

		if (IgnoreOffset != -1) memcpy(Params.data() + IgnoreOffset, &IgnoreList, sizeof(IgnoreList));

		LibraryDefault->ProcessEvent(TraceFn, Params.data());
		++gTracesFrame;
		++gTracesTotal;

		if (!*(bool*)(Params.data() + ReturnOffset))
			return false;

		UObject* Hit = nullptr;

		if (auto Item = GetItemByIndex(*(int*)(Params.data() + OutHitOffset + HitActorOffset)))
			Hit = (UObject*)Item->Object;

		static auto PlayerPawnClass = AFortPlayerPawnAthena::StaticClass();

		// not other players, and not loot lying on the ground (the sweep touches it, but it stops nobody)
		if (!Hit || Hit->IsA(PlayerPawnClass) || Hit->GetName().find("Pickup") != std::string::npos)
			return false;

		if (OutActor)
			*OutActor = Hit;

		if (OutPoint)
			*OutPoint = HitPointOffset != -1 ? *(FVector*)(Params.data() + OutHitOffset + HitPointOffset) : To;

		return true;
	}

	// the first floor below FromZ + 200 at this spot
	inline bool NavFloorBelow(float X, float Y, float FromZ, float& OutZ)
	{
		FVector Hit{};
		++gNavRays;

		if (!TraceBlocked(gNavTracePawn, nullptr, FVector{ X, Y, FromZ + 200.0f }, FVector{ X, Y, FromZ - 1100.0f }, nullptr, nullptr, nullptr, &Hit))
			return false;

		OutZ = Hit.Z;
		return true;
	}

	// ---- the coarse map ----
	// The detailed map only exists around loot, so a long trip (to the storm, to a far town) had nothing to follow between places and went in a straight
	// line, into mountains and lakes. The coarse map covers the whole island in 400 unit cells with just the essentials (floor height, slope, a capsule
	// sweep), explored the same way and kept in its own cache file. Long routes use it; the detailed map takes over near the destination.
	constexpr float CoarseCell = 400.0f;
	constexpr int CoarseTile = 32;
	constexpr int CoarseTileNodes = CoarseTile * CoarseTile;
	constexpr float CoarseMaxClimb = 260.0f;
	constexpr float CoarseMaxDrop = 500.0f;

	struct CoarseTileData
	{
		std::vector<float> Z;
		std::vector<uint8_t> State;   // 0 empty, 1 floor known, 2 reached, 3 expanded
		std::vector<uint8_t> Edge;    // bit k: the neighbour in direction k can be walked to
	};

	inline std::unordered_map<int64_t, CoarseTileData> gCoarseTiles;
	inline std::deque<uint64_t> gCoarseFrontier;
	inline bool gCoarseDone = false;
	inline int gCoarseLattice = 0;
	inline int gCoarseNodes = 0;
	inline int gCoarseCalls = 0, gCoarseRoutes = 0;

	inline int CoarseCellOf(float V) { return (int)std::floor(V / CoarseCell); }
	inline uint64_t CoarsePack(int Gx, int Gy) { return ((uint64_t)(uint32_t)(Gx + 100000) << 32) | (uint64_t)(uint32_t)(Gy + 100000); }
	inline void CoarseUnpack(uint64_t P, int& Gx, int& Gy) { Gx = (int)(P >> 32) - 100000; Gy = (int)(P & 0xFFFFFFFFu) - 100000; }

	inline CoarseTileData* CoarseGetTile(int Gx, int Gy, bool bCreate)
	{
		const int64_t Key = NavTileKey(NavFloorDiv(Gx, CoarseTile), NavFloorDiv(Gy, CoarseTile));
		auto It = gCoarseTiles.find(Key);

		if (It != gCoarseTiles.end())
			return &It->second;

		if (!bCreate)
			return nullptr;

		CoarseTileData& T = gCoarseTiles[Key];
		T.Z.assign(CoarseTileNodes, 0.0f);
		T.State.assign(CoarseTileNodes, 0);
		T.Edge.assign(CoarseTileNodes, 0);
		return &T;
	}

	inline int CoarseIndex(int Gx, int Gy)
	{
		return (Gy - NavFloorDiv(Gy, CoarseTile) * CoarseTile) * CoarseTile + (Gx - NavFloorDiv(Gx, CoarseTile) * CoarseTile);
	}

	inline bool CoarseAddSeed(const FVector& Floor)
	{
		const int Gx = CoarseCellOf(Floor.X), Gy = CoarseCellOf(Floor.Y);
		auto T = CoarseGetTile(Gx, Gy, true);
		const int I = CoarseIndex(Gx, Gy);

		if (T->State[I] == 1)
		{
			T->State[I] = 2;
			gCoarseFrontier.push_back(CoarsePack(Gx, Gy));
			gCoarseDone = false;
			return true;
		}

		if (T->State[I] != 0)
			return false;

		T->Z[I] = Floor.Z;
		T->State[I] = 2;
		++gCoarseNodes;
		gCoarseFrontier.push_back(CoarsePack(Gx, Gy));
		gCoarseDone = false;
		return true;
	}

	inline void SaveCoarseCache()
	{
		FILE* File = nullptr;
		fopen_s(&File, gCoarseDone ? "bot_nav_coarse7.bin" : "bot_nav_coarse7.partial.bin", "wb");

		if (!File)
			return;

		const uint32_t Magic = 0x434F4152, Count = (uint32_t)gCoarseTiles.size();
		fwrite(&Magic, 4, 1, File);
		fwrite(&Count, 4, 1, File);

		for (auto& Pair : gCoarseTiles)
		{
			fwrite(&Pair.first, 8, 1, File);
			fwrite(Pair.second.Z.data(), 4, CoarseTileNodes, File);
			fwrite(Pair.second.State.data(), 1, CoarseTileNodes, File);
			fwrite(Pair.second.Edge.data(), 1, CoarseTileNodes, File);
		}

		fclose(File);
		LOG_INFO(LogBots, "BotNav: saved the coarse map: {} tiles, {} nodes ({})", (int)Count, gCoarseNodes, gCoarseDone ? "complete" : "unfinished");
	}

	inline void LoadCoarseCache()
	{
		FILE* File = nullptr;
		fopen_s(&File, "bot_nav_coarse7.bin", "rb");

		if (!File)
			return;

		uint32_t Magic = 0, Count = 0;

		if (fread(&Magic, 4, 1, File) != 1 || fread(&Count, 4, 1, File) != 1 || Magic != 0x434F4152)
		{
			fclose(File);
			return;
		}

		for (uint32_t i = 0; i < Count; ++i)
		{
			int64_t Key = 0;
			CoarseTileData T;
			T.Z.resize(CoarseTileNodes);
			T.State.resize(CoarseTileNodes);
			T.Edge.resize(CoarseTileNodes);

			if (fread(&Key, 8, 1, File) != 1 || fread(T.Z.data(), 4, CoarseTileNodes, File) != (size_t)CoarseTileNodes || fread(T.State.data(), 1, CoarseTileNodes, File) != (size_t)CoarseTileNodes || fread(T.Edge.data(), 1, CoarseTileNodes, File) != (size_t)CoarseTileNodes)
				break;

			for (int n = 0; n < CoarseTileNodes; ++n)
				gCoarseNodes += T.State[n] >= 2 ? 1 : 0;

			gCoarseTiles[Key] = std::move(T);
		}

		fclose(File);
		gCoarseDone = true;
		LOG_INFO(LogBots, "BotNav: loaded the coarse map: {} tiles, {} nodes", (int)gCoarseTiles.size(), gCoarseNodes);
	}

	// explores the coarse map; the first thing it does is lay a lattice of seeds over the island so that every piece of land gets some
	inline void StepCoarseMap(int RayBudget)
	{
		if (!gNavTracePawn || (gCoarseDone && gCoarseFrontier.empty() && gCoarseLattice < 0))
			return;

		const long long Start = gNavRays;

		// seeds: a downward ray from high above every 8000 units; whatever it lands on (ground, a roof) is somewhere walkable to grow from
		while (gCoarseLattice >= 0 && (gNavRays - Start) < RayBudget)
		{
			const int Side = 40;
			const int Ix = gCoarseLattice % Side, Iy = gCoarseLattice / Side;

			if (Iy >= Side)
			{
				gCoarseLattice = -1;
				break;
			}

			++gCoarseLattice;
			const float X = -160000.0f + Ix * 8000.0f, Y = -160000.0f + Iy * 8000.0f;
			FVector Hit{};
			++gNavRays;

			if (TraceBlocked(gNavTracePawn, nullptr, FVector{ X, Y, 30000.0f }, FVector{ X, Y, -3000.0f }, nullptr, nullptr, nullptr, &Hit))
				CoarseAddSeed(Hit);
		}

		while ((gNavRays - Start) < RayBudget && !gCoarseFrontier.empty())
		{
			const uint64_t Packed = gCoarseFrontier.front();
			gCoarseFrontier.pop_front();
			int Gx, Gy;
			CoarseUnpack(Packed, Gx, Gy);
			auto T = CoarseGetTile(Gx, Gy, true);
			const int I = CoarseIndex(Gx, Gy);

			if (T->State[I] == 3)
				continue;

			T->State[I] = 3;
			const float Pz = T->Z[I], Px = (Gx + 0.5f) * CoarseCell, Py = (Gy + 0.5f) * CoarseCell;

			for (int k = 0; k < 8; ++k)
			{
				const int Nx = Gx + NavDx[k], Ny = Gy + NavDy[k];

				if (std::abs(Nx) > 520 || std::abs(Ny) > 520)
					continue;

				auto NT = CoarseGetTile(Nx, Ny, true);
				const int J = CoarseIndex(Nx, Ny);

				if (NT->State[J] == 3)
					continue;

				const float Qx = (Nx + 0.5f) * CoarseCell, Qy = (Ny + 0.5f) * CoarseCell;
				float Fz = 0.0f;

				if (NT->State[J] == 0)
				{
					FVector Hit{};
					++gNavRays;

					if (!TraceBlocked(gNavTracePawn, nullptr, FVector{ Qx, Qy, Pz + 300.0f }, FVector{ Qx, Qy, Pz - 900.0f }, nullptr, nullptr, nullptr, &Hit))
						continue;

					Fz = Hit.Z;
				}
				else
				{
					Fz = NT->Z[J];
				}

				const float Step = (NavDx[k] != 0 && NavDy[k] != 0) ? 1.414f : 1.0f;
				const float Rise = Fz - Pz;
				const bool bForward = Rise <= CoarseMaxClimb * Step && Rise >= -CoarseMaxDrop;
				const bool bBack = -Rise <= CoarseMaxClimb * Step && -Rise >= -CoarseMaxDrop;

				if (!bForward && !bBack)
					continue;

				if (NavCapsuleBlocked(FVector{ Px, Py, Pz + 92.0f }, FVector{ Qx, Qy, Fz + 92.0f }, 34.0f, 70.0f))
					continue;

				if (NT->State[J] == 0)
				{
					NT->Z[J] = Fz;
					NT->State[J] = 1;
					++gCoarseNodes;
				}

				if (bForward) T->Edge[I] |= (uint8_t)(1 << k);
				if (bBack) NT->Edge[J] |= (uint8_t)(1 << ((k + 4) & 7));

				if (bForward && NT->State[J] == 1)
				{
					NT->State[J] = 2;
					gCoarseFrontier.push_back(CoarsePack(Nx, Ny));
				}
			}
		}

		if (gCoarseFrontier.empty() && gCoarseLattice < 0 && !gCoarseDone)
		{
			gCoarseDone = true;
			LOG_INFO(LogBots, "BotNav: the coarse map is explored: {} tiles, {} nodes", (int)gCoarseTiles.size(), gCoarseNodes);
			SaveCoarseCache();
		}
	}

	// is the straight walk between two coarse nodes a chain of valid steps?
	inline bool CoarseLineWalkable(int Ax, int Ay, int Bx, int By)
	{
		int X = Ax, Y = Ay;
		const int Dx = Bx - Ax, Dy = By - Ay, Steps = (std::max)(std::abs(Dx), std::abs(Dy));

		for (int s = 1; s <= Steps; ++s)
		{
			const int Tx = Ax + (int)std::lround((float)Dx * s / Steps), Ty = Ay + (int)std::lround((float)Dy * s / Steps);
			int K = -1;

			for (int k = 0; k < 8; ++k)
			{
				if (NavDx[k] == Tx - X && NavDy[k] == Ty - Y)
				{
					K = k;
					break;
				}
			}

			auto T = CoarseGetTile(X, Y, false);

			if (K < 0 || !T || !(T->Edge[CoarseIndex(X, Y)] & (1 << K)))
				return false;

			X = Tx;
			Y = Ty;
		}

		return true;
	}

	// A route over the whole island, as waypoints, cut off after about MaxLength units. If the destination itself cannot be walked to (a lake, a cliff top),
	// the route goes to the reachable spot nearest to it.
	inline bool CoarsePath(const FVector& Start, const FVector& End, std::vector<FVector>& Out, float MaxLength)
	{
		Out.clear();
		++gCoarseCalls;

		if (gCoarseTiles.empty())
			return false;

		auto Nearest = [&](float X, float Y, int Radius, int& Ox, int& Oy) -> bool
		{
			const int Cx = CoarseCellOf(X), Cy = CoarseCellOf(Y);
			float Best = 1e30f;
			bool bFound = false;

			for (int Dy = -Radius; Dy <= Radius; ++Dy)
			{
				for (int Dx = -Radius; Dx <= Radius; ++Dx)
				{
					auto T = CoarseGetTile(Cx + Dx, Cy + Dy, false);

					if (!T || T->State[CoarseIndex(Cx + Dx, Cy + Dy)] < 2)
						continue;

					const float D = (float)(Dx * Dx + Dy * Dy);

					if (D < Best)
					{
						Best = D;
						Ox = Cx + Dx;
						Oy = Cy + Dy;
						bFound = true;
					}
				}
			}

			return bFound;
		};

		int Sx = 0, Sy = 0, Ex = CoarseCellOf(End.X), Ey = CoarseCellOf(End.Y);

		if (!Nearest(Start.X, Start.Y, 3, Sx, Sy))
			return false;

		const int Margin = 24;
		int MinX = (std::min)(Sx, Ex) - Margin, MaxX = (std::max)(Sx, Ex) + Margin, MinY = (std::min)(Sy, Ey) - Margin, MaxY = (std::max)(Sy, Ey) + Margin;
		MinX = (std::max)(MinX, -520); MaxX = (std::min)(MaxX, 520); MinY = (std::max)(MinY, -520); MaxY = (std::min)(MaxY, 520);
		const int W = MaxX - MinX + 1, H = MaxY - MinY + 1;

		if (W <= 0 || H <= 0 || (long long)W * H > 700000)
			return false;

		std::vector<float> G((size_t)W * H, 1e30f);
		std::vector<int32_t> Parent((size_t)W * H, -1);
		std::vector<uint8_t> Closed((size_t)W * H, 0);
		using Item = std::pair<float, uint32_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> Open;

		auto Idx = [&](int Gx, int Gy) { return (size_t)(Gy - MinY) * W + (Gx - MinX); };
		auto Heuristic = [&](int Gx, int Gy)
		{
			const float Dx = (float)std::abs(Gx - Ex), Dy = (float)std::abs(Gy - Ey);
			return CoarseCell * ((std::max)(Dx, Dy) + 0.414f * (std::min)(Dx, Dy));
		};

		G[Idx(Sx, Sy)] = 0.0f;
		Open.push({ Heuristic(Sx, Sy), (uint32_t)Idx(Sx, Sy) });
		int Expanded = 0, BestX = Sx, BestY = Sy;
		float BestH = Heuristic(Sx, Sy);
		bool bReached = false;

		while (!Open.empty() && Expanded < 250000)
		{
			const uint32_t C = Open.top().second;
			Open.pop();

			if (Closed[C])
				continue;

			Closed[C] = 1;
			++Expanded;
			const int Gx = MinX + (int)(C % W), Gy = MinY + (int)(C / W);
			const float Hc = Heuristic(Gx, Gy);

			if (Hc < BestH)
			{
				BestH = Hc;
				BestX = Gx;
				BestY = Gy;
			}

			if (Gx == Ex && Gy == Ey)
			{
				bReached = true;
				break;
			}

			auto T = CoarseGetTile(Gx, Gy, false);

			if (!T)
				continue;

			const int I = CoarseIndex(Gx, Gy);

			for (int k = 0; k < 8; ++k)
			{
				if (!(T->Edge[I] & (1 << k)))
					continue;

				const int Nx = Gx + NavDx[k], Ny = Gy + NavDy[k];

				if (Nx < MinX || Nx > MaxX || Ny < MinY || Ny > MaxY)
					continue;

				auto NT = CoarseGetTile(Nx, Ny, false);

				if (!NT)
					continue;

				const size_t N = Idx(Nx, Ny);

				if (Closed[N])
					continue;

				const float Fall = T->Z[I] - NT->Z[CoarseIndex(Nx, Ny)];
				const float Cost = G[C] + ((NavDx[k] != 0 && NavDy[k] != 0) ? 1.414f : 1.0f) * CoarseCell * (1.0f + std::fabs(NT->Z[CoarseIndex(Nx, Ny)] - T->Z[I]) / 300.0f) + (Fall > 150.0f ? Fall * 0.6f : 0.0f);

				if (Cost < G[N])
				{
					G[N] = Cost;
					Parent[N] = (int32_t)C;
					Open.push({ Cost + Heuristic(Nx, Ny), (uint32_t)N });
				}
			}
		}

		const int TargetX = bReached ? Ex : BestX, TargetY = bReached ? Ey : BestY;
		std::vector<std::pair<int, int>> Chain;

		for (int64_t C = (int64_t)Idx(TargetX, TargetY); C != -1; C = Parent[C])
			Chain.push_back({ MinX + (int)(C % W), MinY + (int)(C / W) });

		std::reverse(Chain.begin(), Chain.end());

		if (Chain.size() < 2)
			return false;

		// shorten with straight walks, then cut it off after MaxLength
		std::vector<std::pair<int, int>> Short;
		size_t At = 0;
		Short.push_back(Chain[0]);

		while (At + 1 < Chain.size())
		{
			size_t Best = At + 1;

			for (size_t j = (std::min)(Chain.size() - 1, At + 24); j > At + 1; --j)
			{
				if (CoarseLineWalkable(Chain[At].first, Chain[At].second, Chain[j].first, Chain[j].second))
				{
					Best = j;
					break;
				}
			}

			At = Best;
			Short.push_back(Chain[At]);
		}

		Out.push_back(Start);
		float Length = 0.0f;
		FVector Last = Start;

		for (size_t i = 1; i < Short.size(); ++i)
		{
			auto T = CoarseGetTile(Short[i].first, Short[i].second, false);
			const FVector P{ (Short[i].first + 0.5f) * CoarseCell, (Short[i].second + 0.5f) * CoarseCell, (T ? T->Z[CoarseIndex(Short[i].first, Short[i].second)] : Start.Z) + 90.0f };
			Length += Dist2D(Last, P);
			Out.push_back(P);
			Last = P;

			if (Length >= MaxLength)
				break;
		}

		++gCoarseRoutes;
		return Out.size() >= 2;
	}

	// a new spot to grow the map from (a container, or ground a bot stands on that the map does not know)
	inline bool NavAddSeed(const FVector& Floor, float InterestRadius = 0.0f)
	{
		if (!gNavGridEnabled)
			return false;

		CoarseAddSeed(Floor);

		if (InterestRadius > 0.0f)
			NavAddInterest(Floor.X, Floor.Y, InterestRadius);

		const int Gx = NavCellOf(Floor.X), Gy = NavCellOf(Floor.Y);
		auto T = NavGetTile(Gx, Gy, true);

		for (int l = 0; l < NavL; ++l)
		{
			const int I = NavIndex(Gx, Gy, l);

			if (T->State[I] != 0)
			{
				if (std::fabs(T->Z[I] - Floor.Z) >= 60.0f)
					continue;

				if (T->State[I] == 1)
				{
					T->State[I] = 2;
					gNavFrontier.push_back(NavPack(Gx, Gy, l));
					return true;
				}

				return false;
			}

			T->Z[I] = Floor.Z;
			T->State[I] = 2;
			++gNavNodes;
			gNavFrontier.push_back(NavPack(Gx, Gy, l));
			return true;
		}

		return false;
	}

	constexpr uint32_t NavCacheMagic = 0x4E415637;

	inline void SaveNavCache()
	{
		// an unfinished map never replaces a finished one
		const char* FileName = gNavScanDone ? "bot_nav_cache7.bin" : "bot_nav_cache7.partial.bin";
		FILE* File = nullptr;
		fopen_s(&File, FileName, "wb");

		if (!File)
			return;

		const uint32_t Chests = (uint32_t)gChests.size(), Count = (uint32_t)gNavTiles.size(), Done = gNavScanDone ? 1 : 0;
		fwrite(&NavCacheMagic, 4, 1, File);
		fwrite(&Chests, 4, 1, File);
		fwrite(&Count, 4, 1, File);
		fwrite(&Done, 4, 1, File);

		for (auto& Pair : gNavTiles)
		{
			const int64_t Key = Pair.first;
			const NavTileData& T = Pair.second;
			fwrite(&Key, 8, 1, File);
			fwrite(T.Z.data(), 4, NavTileNodes, File);
			fwrite(T.State.data(), 1, NavTileNodes, File);
			fwrite(T.Edge.data(), 1, NavTileNodes, File);
			fwrite(T.Layer.data(), 2, NavTileNodes, File);
			fwrite(T.Jump.data(), 1, NavTileNodes, File);
		}

		fclose(File);
		gNavDirty = false;
		LOG_INFO(LogBots, "BotNav: saved {} tiles ({} nodes, scan {}) to {}", (int)Count, gNavNodes, gNavScanDone ? "complete" : "unfinished", FileName);
	}

	inline void LoadNavCache()
	{
		FILE* File = nullptr;
		fopen_s(&File, "bot_nav_cache7.bin", "rb");

		if (!File)
			return;

		uint32_t Magic = 0, Chests = 0, Count = 0, Done = 0;

		if (fread(&Magic, 4, 1, File) != 1 || fread(&Chests, 4, 1, File) != 1 || fread(&Count, 4, 1, File) != 1 || fread(&Done, 4, 1, File) != 1 || Magic != NavCacheMagic)
		{
			fclose(File);
			LOG_INFO(LogBots, "BotNav: ignoring bot_nav_cache.bin (a different map or version)");
			return;
		}

		for (uint32_t i = 0; i < Count; ++i)
		{
			int64_t Key = 0;
			NavTileData T;
			T.Z.resize(NavTileNodes);
			T.State.resize(NavTileNodes);
			T.Edge.resize(NavTileNodes);
			T.Layer.resize(NavTileNodes);
			T.Jump.resize(NavTileNodes);

			if (fread(&Key, 8, 1, File) != 1 || fread(T.Z.data(), 4, NavTileNodes, File) != (size_t)NavTileNodes || fread(T.State.data(), 1, NavTileNodes, File) != (size_t)NavTileNodes
				|| fread(T.Edge.data(), 1, NavTileNodes, File) != (size_t)NavTileNodes || fread(T.Layer.data(), 2, NavTileNodes, File) != (size_t)NavTileNodes || fread(T.Jump.data(), 1, NavTileNodes, File) != (size_t)NavTileNodes)
				break;

			for (int n = 0; n < NavTileNodes; ++n)
				gNavNodes += T.State[n] >= 2 ? 1 : 0;

			gNavTiles[Key] = std::move(T);
		}

		fclose(File);
		gNavScanDone = Done != 0;

		// nodes reached but never expanded go back on the frontier, so an unfinished scan carries on
		for (auto& Pair : gNavTiles)
		{
			const int Tx = (int)(Pair.first >> 32), Ty = (int)(uint32_t)Pair.first;

			for (int n = 0; n < NavTileNodes; ++n)
			{
				if (Pair.second.State[n] != 2)
					continue;

				const int L = n % NavL, Cell = n / NavL;
				gNavFrontier.push_back(NavPack(Tx * NavTile + Cell % NavTile, Ty * NavTile + Cell / NavTile, L));
			}
		}

		LOG_INFO(LogBots, "BotNav: loaded {} tiles ({} nodes) from bot_nav_cache.bin, scan {}, {} nodes still to expand", (int)gNavTiles.size(), gNavNodes, gNavScanDone ? "complete" : "unfinished", (int)gNavFrontier.size());
	}

	// explores the map, within a ray budget per frame
	inline void StepNavMap()
	{
		if (!gNavGridEnabled || !gNavTracePawn)
			return;

		long long Used = 0;
		const long long Start = gNavRays;

		while ((gNavRays - Start) < gNavScanBudget && !gNavFrontier.empty())
		{
			const uint64_t Packed = gNavFrontier.front();
			gNavFrontier.pop_front();
			int Gx, Gy, L;
			NavUnpack(Packed, Gx, Gy, L);

			auto T = NavGetTile(Gx, Gy, true);
			const int I = NavIndex(Gx, Gy, L);

			if (T->State[I] == 3)
				continue;

			T->State[I] = 3;
			const float Pz = T->Z[I];
			const float Px = (Gx + 0.5f) * NavCell, Py = (Gy + 0.5f) * NavCell;

			for (int k = 0; k < 8; ++k)
			{
				const int Nx = Gx + NavDx[k], Ny = Gy + NavDy[k];

				if (!NavInterestAt(Nx, Ny))
					continue;

				const float Qx = (Nx + 0.5f) * NavCell, Qy = (Ny + 0.5f) * NavCell;
				float Fz = 0.0f;

				if (!NavFloorBelow(Qx, Qy, Pz, Fz))
					continue;

				auto NT = NavGetTile(Nx, Ny, true);
				int Slot = -1;

				for (int l = 0; l < NavL; ++l)
				{
					const int J = NavIndex(Nx, Ny, l);

					if (NT->State[J] != 0 && std::fabs(NT->Z[J] - Fz) < 60.0f)
					{
						Slot = l;
						break;
					}
				}

				if (Slot < 0)
				{
					for (int l = 0; l < NavL; ++l)
					{
						if (NT->State[NavIndex(Nx, Ny, l)] == 0)
						{
							Slot = l;
							break;
						}
					}

					if (Slot < 0)
						continue;
				}

				const int J = NavIndex(Nx, Ny, Slot);

				if (NT->State[J] == 3)
					continue; // that pair was already looked at from the other side

				const float Step = (NavDx[k] != 0 && NavDy[k] != 0) ? 1.414f : 1.0f;
				const float Rise = Fz - Pz;
				const bool bForward = Rise <= NavMaxClimb * Step && Rise >= -NavMaxDrop;
				const bool bBack = -Rise <= NavMaxClimb * Step && -Rise >= -NavMaxDrop;

				if (!bForward && !bBack)
					continue;

				// walk: a capsule whose feet clear the ground by a little (stairs, kerbs); hop: one whose feet clear it by a lot (railings, low fences, crates)
				bool bJump = false;
				const float Half = 66.0f, Radius = 28.0f;

				if (NavCapsuleBlocked(FVector{ Px, Py, Pz + Half + 22.0f }, FVector{ Qx, Qy, Fz + Half + 22.0f }, Radius, Half))
				{
					if (NavCapsuleBlocked(FVector{ Px, Py, Pz + Half + 135.0f }, FVector{ Qx, Qy, Fz + Half + 135.0f }, Radius, Half))
						continue;

					bJump = true;
				}

				if (NT->State[J] == 0)
				{
					NT->Z[J] = Fz;
					NT->State[J] = 1;
					++gNavNodes;
				}

				if (bForward)
				{
					T->Edge[I] |= (uint8_t)(1 << k);
					T->Layer[I] = (uint16_t)((T->Layer[I] & ~(3 << (2 * k))) | (Slot << (2 * k)));

					if (bJump) T->Jump[I] |= (uint8_t)(1 << k); else T->Jump[I] &= (uint8_t)~(1 << k);
				}

				if (bBack)
				{
					const int Rk = (k + 4) & 7;
					NT->Edge[J] |= (uint8_t)(1 << Rk);
					NT->Layer[J] = (uint16_t)((NT->Layer[J] & ~(3 << (2 * Rk))) | (L << (2 * Rk)));

					if (bJump) NT->Jump[J] |= (uint8_t)(1 << Rk); else NT->Jump[J] &= (uint8_t)~(1 << Rk);
				}

				if (bForward && NT->State[J] == 1)
				{
					NT->State[J] = 2;
					gNavFrontier.push_back(NavPack(Nx, Ny, Slot));
				}
			}

			gNavDirty = true;
		}

		StepCoarseMap(gNavScanBudget / 2);

		if (gNavFrontier.empty() && !gNavScanDone && !gNavTiles.empty())
		{
			gNavScanDone = true;
			gNavDirty = true;
			LOG_INFO(LogBots, "BotNav: the map is explored: {} tiles, {} nodes, {} rays", (int)gNavTiles.size(), gNavNodes, gNavRays);
			SaveNavCache();
		}
		else if (gNavDirty && ++gNavSaveTick % 3600 == 0)
		{
			SaveNavCache();
		}
	}

	// every container is somewhere a bot is going to stand, so the map grows from each of them (the containers show up over the first minute or so)
	inline void SeedNavMap()
	{
		static bool bLoaded = false;
		static size_t Seeded = 0;

		if (!gNavGridEnabled)
			return;

		if (!bLoaded)
		{
			bLoaded = true;
			LoadNavCache();
			LoadCoarseCache();

			if (gCoarseDone)
				gCoarseLattice = -1;
		}

		if (gChests.size() < Seeded)
			Seeded = 0; // the containers were scanned again

		if (gPois.empty() || gChests.size() <= Seeded)
			return;

		int Added = 0;

		for (; Seeded < gChests.size(); ++Seeded)
		{
			const size_t i = Seeded;

			if (i < gChestPoi.size() && gChestPoi[i] >= 0 && gPois[gChestPoi[i]].Name.find("Lobby") != std::string::npos)
				continue; // the spawn island

			NavAddInterest(gChests[i].X, gChests[i].Y, 4000.0f);

			if (NavAddSeed(gChests[i]))
				++Added;
		}

		LOG_INFO(LogBots, "BotNav: seeded from {} containers, {} of them new to the map ({} frontier nodes)", (int)gChests.size(), Added, (int)gNavFrontier.size());
	}

	// ---- routes ----
	// is the straight walk from one node to another a chain of valid steps (so the route between them can be shortened)?
	inline bool NavLineWalkable(int Ax, int Ay, int Al, int Bx, int By, int Bl, float& MaxZStep)
	{
		int X = Ax, Y = Ay, L = Al;
		const int Dx = Bx - Ax, Dy = By - Ay;
		const int Steps = (std::max)(std::abs(Dx), std::abs(Dy));
		MaxZStep = 0.0f;

		for (int s = 1; s <= Steps; ++s)
		{
			const int Tx = Ax + (int)std::lround((float)Dx * s / Steps), Ty = Ay + (int)std::lround((float)Dy * s / Steps);
			const int Mx = Tx - X, My = Ty - Y;
			int K = -1;

			for (int k = 0; k < 8; ++k)
			{
				if (NavDx[k] == Mx && NavDy[k] == My)
				{
					K = k;
					break;
				}
			}

			if (K < 0)
				return false;

			auto T = NavGetTile(X, Y, false);

			if (!T)
				return false;

			const int I = NavIndex(X, Y, L);

			if (!(T->Edge[I] & (1 << K)) || (T->Jump[I] & (1 << K)))
				return false;

			const int Nl = (T->Layer[I] >> (2 * K)) & 3;
			auto NT = NavGetTile(Tx, Ty, false);

			if (!NT)
				return false;

			MaxZStep = (std::max)(MaxZStep, std::fabs(NT->Z[NavIndex(Tx, Ty, Nl)] - T->Z[I]));
			X = Tx;
			Y = Ty;
			L = Nl;
		}

		return X == Bx && Y == By && L == Bl;
	}

	// A*. 1 = route found, 0 = no opinion (the map does not cover this), -1 = the goal is covered and cannot be walked to.
	// bEndZKnown: End.Z is a real floor height (a chest, a pickup), not a guess. OutJump marks the waypoints that are reached by a hop.
	inline int GridPathImpl(const FVector& Start, const FVector& End, std::vector<FVector>& Out, std::vector<uint8_t>* OutJump, bool bEndZKnown, float Health)
	{
		Out.clear();
		if (OutJump) OutJump->clear();
		gNavGridUnreachable = false;
		gGridNoStartFlag = false;
		gNavEnclosed = false;

		if (!gNavGridEnabled || gNavTiles.empty())
			return 0;

		// the walked node nearest to a point (horizontal distance, with a penalty for another height)
		auto Nearest = [&](float X, float Y, float Z, bool bUseZ, float Radius, int& OutGx, int& OutGy, int& OutL) -> bool
		{
			const int Cx = NavCellOf(X), Cy = NavCellOf(Y), R = (int)std::ceil(Radius / NavCell);
			float BestScore = 1e30f;
			bool bFound = false;

			for (int Dy = -R; Dy <= R; ++Dy)
			{
				for (int Dx = -R; Dx <= R; ++Dx)
				{
					auto T = NavGetTile(Cx + Dx, Cy + Dy, false);

					if (!T)
						continue;

					for (int l = 0; l < NavL; ++l)
					{
						const int I = NavIndex(Cx + Dx, Cy + Dy, l);

						if (T->State[I] < 2)
							continue;

						const float Qx = (Cx + Dx + 0.5f) * NavCell - X, Qy = (Cy + Dy + 0.5f) * NavCell - Y;
						float Score = Qx * Qx + Qy * Qy;

						if (bUseZ)
						{
							const float Dz = std::fabs(T->Z[I] - Z);

							if (Dz > 220.0f)
								continue;

							Score += Dz * Dz * 4.0f;
						}

						if (Score < BestScore && Score <= Radius * Radius + 1.0f)
						{
							BestScore = Score;
							OutGx = Cx + Dx;
							OutGy = Cy + Dy;
							OutL = l;
							bFound = true;
						}
					}
				}
			}

			return bFound;
		};

		int Sx = 0, Sy = 0, Sl = 0;

		if (!Nearest(Start.X, Start.Y, Start.Z - 88.0f, true, 300.0f, Sx, Sy, Sl))
		{
			gGridNoStartFlag = true;
			++gGridNoStart;
			return 0;
		}

		// a goal further than the search allows is approached in stages
		FVector Goal = End;
		bool bStaged = false;
		const float Dist = Dist2D(Start, End);

		if (Dist > 6500.0f)
		{
			// a long trip: the whole-island map gives the way, the detailed map the last stretch near where the bot is
			std::vector<FVector> Rough;

			if (CoarsePath(Start, End, Rough, 7000.0f))
			{
				size_t M = Rough.size() - 1;

				for (size_t i = 1; i < Rough.size(); ++i)
				{
					if (Dist2D(Start, Rough[i]) >= 2500.0f)
					{
						M = i;
						break;
					}
				}

				std::vector<FVector> Fine;
				std::vector<uint8_t> FineJump;

				if (Dist2D(Start, Rough[M]) <= 6500.0f && GridPathImpl(Start, Rough[M], Fine, &FineJump, false, Health) == 1)
				{
					Out = Fine;
					if (OutJump) *OutJump = FineJump;

					for (size_t i = M + 1; i < Rough.size(); ++i)
					{
						Out.push_back(Rough[i]);
						if (OutJump) OutJump->push_back(0);
					}
				}
				else
				{
					Out = Rough;
					if (OutJump) OutJump->assign(Out.size(), 0);
				}

				gNavGridUnreachable = false;
				return Out.size() >= 2 ? 1 : 0;
			}

			const float F = 5500.0f / Dist;
			Goal = FVector{ Start.X + (End.X - Start.X) * F, Start.Y + (End.Y - Start.Y) * F, End.Z };
			bStaged = true;
		}

		int Ex = 0, Ey = 0, El = 0;
		const bool bGoalNode = Nearest(Goal.X, Goal.Y, Goal.Z, bEndZKnown && !bStaged, bStaged ? 700.0f : 320.0f, Ex, Ey, El);

		if (!bGoalNode)
		{
			// covered by a finished map yet nothing walkable there: it cannot be reached on foot
			if (!bStaged && gNavScanDone && NavInterestAt(NavCellOf(End.X), NavCellOf(End.Y)) && NavInterestAt(NavCellOf(End.X) + 6, NavCellOf(End.Y)) && NavInterestAt(NavCellOf(End.X) - 6, NavCellOf(End.Y)))
			{
				int Ax = 0, Ay = 0, Al = 0;
				const bool bAny = Nearest(End.X, End.Y, End.Z, false, 900.0f, Ax, Ay, Al);
				auto AT = bAny ? NavGetTile(Ax, Ay, false) : nullptr;
				snprintf(gNavWhy, sizeof(gNavWhy), "no node near the goal: nearest any-height node %s %.0f away, %.0f higher", bAny ? "found" : "none", bAny ? Dist2D(End, FVector{ (Ax + 0.5f) * NavCell, (Ay + 0.5f) * NavCell, 0.0f }) : 0.0f, (bAny && AT) ? AT->Z[NavIndex(Ax, Ay, Al)] - End.Z : 0.0f);
				gNavGridUnreachable = true;
				return -1;
			}

			++gGridNotDone;
			return 0;
		}

		if (Sx == Ex && Sy == Ey && Sl == El)
		{
			++gGridSame;
			return 0;
		}

		// the part of the map between them
		const int Margin = 2200 / (int)NavCell + 6;
		const int MinX = (std::min)(Sx, Ex) - Margin, MaxX = (std::max)(Sx, Ex) + Margin, MinY = (std::min)(Sy, Ey) - Margin, MaxY = (std::max)(Sy, Ey) + Margin;
		const int W = MaxX - MinX + 1, H = MaxY - MinY + 1;
		const size_t Nodes = (size_t)W * H * NavL;

		if (Nodes > 400000)
			return 0;

		auto Idx = [&](int Gx, int Gy, int L) { return ((size_t)(Gy - MinY) * W + (Gx - MinX)) * NavL + L; };
		std::vector<float> G(Nodes, 1e30f);
		std::vector<int32_t> Parent(Nodes, -1);
		std::vector<uint8_t> Closed(Nodes, 0);
		using Item = std::pair<float, uint32_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> Open;

		auto Heuristic = [&](int Gx, int Gy)
		{
			const float Dx = (float)std::abs(Gx - Ex), Dy = (float)std::abs(Gy - Ey);
			return NavCell * ((std::max)(Dx, Dy) + 0.414f * (std::min)(Dx, Dy));
		};

		// the longest drop worth taking: one whose fall damage is well under what the bot has left
		const float MaxFall = (std::max)(300.0f, 100.0f * (Health * 0.6f / 4.5f + 8.0f));
		const size_t SIdx = Idx(Sx, Sy, Sl), EIdx = Idx(Ex, Ey, El);
		G[SIdx] = 0.0f;
		Open.push({ Heuristic(Sx, Sy), (uint32_t)SIdx });
		bool bFound = false;
		int Expanded = 0;

		while (!Open.empty() && Expanded < 60000)
		{
			const uint32_t C = Open.top().second;
			Open.pop();

			if (Closed[C])
				continue;

			Closed[C] = 1;
			++Expanded;

			if (C == EIdx)
			{
				bFound = true;
				break;
			}

			const int L = (int)(C % NavL), Cell = (int)(C / NavL), Gx = MinX + Cell % W, Gy = MinY + Cell / W;
			auto T = NavGetTile(Gx, Gy, false);

			if (!T)
				continue;

			const int I = NavIndex(Gx, Gy, L);

			for (int k = 0; k < 8; ++k)
			{
				if (!(T->Edge[I] & (1 << k)))
					continue;

				const int Nx = Gx + NavDx[k], Ny = Gy + NavDy[k], Nl = (T->Layer[I] >> (2 * k)) & 3;

				if (Nx < MinX || Nx > MaxX || Ny < MinY || Ny > MaxY)
					continue;

				auto NT = NavGetTile(Nx, Ny, false);

				if (!NT)
					continue;

				const int NI = NavIndex(Nx, Ny, Nl);
				const size_t N = Idx(Nx, Ny, Nl);

				if (Closed[N] || NT->State[NI] < 2)
					continue;

				const float Fall = T->Z[I] - NT->Z[NI];

				if (Fall > MaxFall)
					continue; // would hurt too much at this health

				const float Cost = G[C] + ((NavDx[k] != 0 && NavDy[k] != 0) ? 1.414f : 1.0f) * NavCell * (1.0f + std::fabs(NT->Z[NI] - T->Z[I]) / 250.0f) + ((T->Jump[I] & (1 << k)) ? 120.0f : 0.0f) + (Fall > 150.0f ? Fall * 0.5f : 0.0f);

				if (Cost < G[N])
				{
					G[N] = Cost;
					Parent[N] = (int32_t)C;
					Open.push({ Cost + Heuristic(Nx, Ny), (uint32_t)N });
				}
			}
		}

		if (!bFound)
		{
			// both ends are on the finished map and no chain of steps joins them
			// a small closed-in area (a roof it landed on, a ledge): no way out on foot, which is not the same as an unreachable goal
			if (Expanded < 900)
			{
				gNavEnclosed = true;
				return 0;
			}

			if (!bStaged && gNavScanDone)
			{
				snprintf(gNavWhy, sizeof(gNavWhy), "both ends are on the map but no chain of steps joins them (%d nodes searched, goal %.0f away)", Expanded, Dist);
				gNavGridUnreachable = true;
				return -1;
			}

			return 0;
		}

		// the chain, start to goal
		struct Step { int Gx, Gy, L; float Z; bool bJump; };
		std::vector<Step> Chain;

		for (int64_t C = (int64_t)EIdx; C != -1; C = Parent[C])
		{
			const int L = (int)(C % NavL), Cell = (int)(C / NavL), Gx = MinX + Cell % W, Gy = MinY + Cell / W;
			auto T = NavGetTile(Gx, Gy, false);
			Chain.push_back({ Gx, Gy, L, T ? T->Z[NavIndex(Gx, Gy, L)] : 0.0f, false });
		}

		std::reverse(Chain.begin(), Chain.end());

		// which steps are hops
		for (size_t i = 1; i < Chain.size(); ++i)
		{
			auto T = NavGetTile(Chain[i - 1].Gx, Chain[i - 1].Gy, false);
			const int Mx = Chain[i].Gx - Chain[i - 1].Gx, My = Chain[i].Gy - Chain[i - 1].Gy;

			for (int k = 0; k < 8; ++k)
			{
				if (T && NavDx[k] == Mx && NavDy[k] == My && (T->Jump[NavIndex(Chain[i - 1].Gx, Chain[i - 1].Gy, Chain[i - 1].L)] & (1 << k)))
					Chain[i].bJump = true;
			}
		}

		// shorten: from each waypoint go as far as a straight walk of valid steps allows (never across a hop)
		std::vector<Step> Short;
		size_t At = 0;
		Short.push_back(Chain[0]);

		while (At + 1 < Chain.size())
		{
			size_t Best = At + 1;

			if (!Chain[At + 1].bJump)
			{
				for (size_t j = (std::min)(Chain.size() - 1, At + 14); j > At + 1; --j)
				{
					bool bHop = false;

					for (size_t m = At + 1; m <= j; ++m)
						bHop = bHop || Chain[m].bJump;

					float ZStep = 0.0f;

					if (!bHop && NavLineWalkable(Chain[At].Gx, Chain[At].Gy, Chain[At].L, Chain[j].Gx, Chain[j].Gy, Chain[j].L, ZStep) && ZStep < 40.0f)
					{
						Best = j;
						break;
					}
				}
			}

			At = Best;
			Short.push_back(Chain[At]);
		}

		Out.push_back(Start);
		if (OutJump) OutJump->push_back(0);

		for (size_t i = 1; i < Short.size(); ++i)
		{
			Out.push_back(FVector{ (Short[i].Gx + 0.5f) * NavCell, (Short[i].Gy + 0.5f) * NavCell, Short[i].Z + 90.0f });
			if (OutJump) OutJump->push_back(Short[i].bJump ? 1 : 0);
		}

		if (!bStaged)
		{
			Out.push_back(End);
			if (OutJump) OutJump->push_back(0);
		}

		return Out.size() >= 2 ? 1 : 0;
	}

	inline int GridPath(const FVector& Start, const FVector& End, std::vector<FVector>& Out, bool bEndZKnown = false, std::vector<uint8_t>* OutJump = nullptr, float Health = 100.0f)
	{
		const double Began = PerfNow();
		int Result = GridPathImpl(Start, End, Out, OutJump, bEndZKnown, Health);

		// no opinion from the detailed map (not covered here): the whole-island map can still say which way to go
		if (Result == 0 && Dist2D(Start, End) > 700.0f)
		{
			std::vector<FVector> Rough;

			if (CoarsePath(Start, End, Rough, 7000.0f))
			{
				Out = Rough;
				if (OutJump) OutJump->assign(Out.size(), 0);
				Result = 1;
			}
		}

		gGridTimeUs += (int)((PerfNow() - Began) * 1000.0);
		++gGridCalls;
		if (Result == 1) ++gGridRoutes; else if (Result == -1) ++gGridUnreach; else ++gGridNoOpinion;
		return Result;
	}

	// a walkable spot near a point (the map's node nearest to it), or false when the map knows none
	inline bool NavNodePos(float X, float Y, float Z, float Radius, FVector& Out)
	{
		const int Cx = NavCellOf(X), Cy = NavCellOf(Y), R = (int)std::ceil(Radius / NavCell);
		float BestScore = Radius * Radius + 1.0f;
		bool bFound = false;

		for (int Dy = -R; Dy <= R; ++Dy)
		{
			for (int Dx = -R; Dx <= R; ++Dx)
			{
				auto T = NavGetTile(Cx + Dx, Cy + Dy, false);

				if (!T)
					continue;

				for (int l = 0; l < NavL; ++l)
				{
					const int I = NavIndex(Cx + Dx, Cy + Dy, l);

					if (T->State[I] < 2 || std::fabs(T->Z[I] - Z) > 150.0f)
						continue;

					const float Qx = (Cx + Dx + 0.5f) * NavCell - X, Qy = (Cy + Dy + 0.5f) * NavCell - Y;
					const float Score = Qx * Qx + Qy * Qy;

					if (Score < BestScore)
					{
						BestScore = Score;
						Out = FVector{ (Cx + Dx + 0.5f) * NavCell, (Cy + Dy + 0.5f) * NavCell, T->Z[I] };
						bFound = true;
					}
				}
			}
		}

		return bFound;
	}

	inline bool QueryNavPath(const FVector& Start, const FVector& End, std::vector<FVector>& Out, bool bForceMesh = false)
	{
		Out.clear();

		if (gNavState == -1 && !bForceMesh)
			return GridPath(Start, End, Out) == 1;

		static auto FindPathFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously");
		static auto NavDefault = FindObject(L"/Script/NavigationSystem.Default__NavigationSystemV1");

		if (!FindPathFn || !NavDefault)
			return false;

		static int ParamsSize = FindPathFn->GetPropertiesSize();
		static int WorldOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously", "WorldContextObject", false);
		static int StartOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously", "PathStart", false);
		static int EndOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously", "PathEnd", false);
		static int ReturnOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously", "ReturnValue", false);

		if (WorldOffset == -1 || StartOffset == -1 || EndOffset == -1 || ReturnOffset == -1)
			return false;

		std::vector<uint8_t> Params(ParamsSize + 64, 0);
		*(UObject**)(Params.data() + WorldOffset) = GetWorld();
		*(FVector*)(Params.data() + StartOffset) = Start;
		*(FVector*)(Params.data() + EndOffset) = End;

		NavDefault->ProcessEvent(FindPathFn, Params.data());

		auto Path = *(UObject**)(Params.data() + ReturnOffset);

		if (!Path)
			return false;

		static int PointsOffset = Path->GetOffset("PathPoints", false);

		if (PointsOffset == -1)
			return false;

		auto& Points = Path->Get<TArray<FVector>>(PointsOffset);

		for (int i = 0; i < Points.Num() && i < 200; ++i)
			Out.push_back(Points.at(i));

		return Out.size() >= 2;
	}

	// A route from the navigation mesh (when there is one under the bot), as waypoints plus a flag for each that is a climb up to something higher
	// (a link the mesh offers: a railing, a ledge). Refuses a path that stops short of the goal or wanders far from the straight line.
	inline bool TryMeshRoute(const FVector& From, const FVector& To, std::vector<FVector>& Out, std::vector<uint8_t>& Jumps)
	{
		if (!gMeshReady)
			return false;

		std::vector<FVector> Points;

		if (!QueryNavPath(From, To, Points, true) || Points.size() < 2)
			return false;

		float Length = 0.0f;

		for (size_t i = 1; i < Points.size(); ++i)
			Length += Dist2D(Points[i - 1], Points[i]);

		// a path that stops short of the goal is still good as the first stage of a long trip when it gets well on the way (the mesh only exists around the bots)
		const float Straight = Dist2D(From, To), EndDist = Dist2D(Points.back(), To);
		const bool bFull = EndDist <= 300.0f;
		const float Progress = Straight - EndDist;

		if (!bFull && (Progress < 600.0f || Progress < Straight * 0.3f))
			return false;

		if (Length > (bFull ? Straight : Progress) * 2.5f + 600.0f)
			return false;

		Out = Points;
		Jumps.assign(Points.size(), 0);

		for (size_t i = 1; i < Points.size(); ++i)
		{
			if (Points[i].Z - Points[i - 1].Z > 55.0f && Dist2D(Points[i - 1], Points[i]) < 450.0f)
				Jumps[i] = 1;
		}

		// The mesh does not cut round railings, props and overhangs (a test across 4000 obstacles found it going straight through nine in ten), and a bot trusts a
		// route over what it sees. So the first stretches are checked with rays at knee and chest height: something in the way that cannot be jumped (its top is
		// 150 or more above the feet) throws the mesh route away for the grid's; something low is allowed, with a jump flagged there.
		if (gNavTracePawn)
		{
			for (size_t i = 1; i < Points.size() && i <= 3; ++i)
			{
				const FVector& A = Points[i - 1];
				const FVector& B = Points[i];
				const float Zlow = (std::max)(A.Z, B.Z) + 48.0f, Zhigh = (std::max)(A.Z, B.Z) + 128.0f;
				FVector HitLow{}, HitHigh{};
				UObject* HitActor = nullptr;
				static auto PlayerPawnClass2 = AFortPlayerPawnAthena::StaticClass();
				const bool bLow = TraceBlocked(gNavTracePawn, nullptr, FVector{ A.X, A.Y, Zlow }, FVector{ B.X, B.Y, Zlow }, nullptr, nullptr, &HitActor, &HitLow);

				if (!bLow || (HitActor && HitActor->IsA(PlayerPawnClass2))) // nothing, or only another player (they move)
					continue;

				// blocked at knee height: how high is it?
				const float Dx = B.X - A.X, Dy = B.Y - A.Y, DL = (std::max)(std::sqrt(Dx * Dx + Dy * Dy), 1.0f);
				FVector Top{};
				const float Px = HitLow.X + Dx / DL * 8.0f, Py = HitLow.Y + Dy / DL * 8.0f;
				const float Feet = (std::max)(A.Z, B.Z);

				if (TraceBlocked(gNavTracePawn, nullptr, FVector{ Px, Py, Feet + 320.0f }, FVector{ Px, Py, Feet - 40.0f }, nullptr, nullptr, nullptr, &Top) && Top.Z - Feet >= 50.0f && Top.Z - Feet < 150.0f)
				{
					Jumps[i] = 1; // low enough to jump over, on the way to this point
					continue;
				}

				return false;
			}
		}

		return true;
	}

	inline void RotateDir(float& Dx, float& Dy, float Degrees)
	{
		const float Cos = std::cos(Degrees * DegToRad);
		const float Sin = std::sin(Degrees * DegToRad);
		const float NewX = Dx * Cos - Dy * Sin;
		const float NewY = Dx * Sin + Dy * Cos;
		Dx = NewX;
		Dy = NewY;
	}

	// ---- aim skill ----

	inline float Gauss()
	{
		const float U1 = (std::max)(RandRange(0.0f, 1.0f), 1e-4f);
		const float U2 = RandRange(0.0f, 1.0f);
		return std::sqrt(-2.0f * std::log(U1)) * std::cos(6.2831853f * U2);
	}

	inline float NormalizeAngle(float Degrees)
	{
		while (Degrees > 180.0f) Degrees -= 360.0f;
		while (Degrees < -180.0f) Degrees += 360.0f;
		return Degrees;
	}

	inline float RollSkill()
	{
		if (gSkillFixed >= 0.0f)
			return gSkillFixed;

		const float Low = (std::min)(gSkillMin, gSkillMax);
		const float High = (std::max)(gSkillMin, gSkillMax);
		return Low + (High - Low) * 0.5f * (RandRange(0.0f, 1.0f) + RandRange(0.0f, 1.0f)); // most bots land in the middle of the range
	}

	// standard deviation of where a bullet strays from where the bot points, in degrees
	inline float AimSigma(float Skill)
	{
		return (std::max)(0.2f, 14.0f * std::exp(-Skill / 28.0f)); // skill 10: ~10, 30: ~4.8, 50: ~2.4, 80: ~0.8
	}

	// How fast it can swing its aim, degrees a second. Was 45 + 1.6*skill; the user found the aim "a little locked on" (2026-11-09), so it is slower.
	inline float TurnRate(float Skill) { return 38.0f + Skill * 1.3f; }
	// How quickly the aim closes the remaining gap (per second, exponential). A hand eases onto a target instead of moving at full speed and
	// stopping dead on it, so a strafing target is always trailed by a few degrees (about angular speed / gain) rather than tracked exactly.
	inline float AimSettleRate(float Skill) { return 7.0f + Skill * 0.09f; }      // skill 30: ~9.7 (a 6 m strafe trails ~6 deg), 80: ~14
	// seconds from seeing a target to the first shot; was max(0.35, 1 - 0.008*skill) + 0..0.35, too slow next to a player (2026-11-09)
	inline float ReactionTime(float Skill) { return (std::max)(0.25f, 0.75f - 0.006f * Skill) + RandRange(0.0f, 0.25f); }

	// ---- falls: bots only guess how bad a drop is ----

	// Fortnite-ish fall damage for a drop of Height units (the bot's own numbers; the real thing may differ)
	inline float FallDamageFor(float Height) { return (std::max)(0.0f, (Height / 100.0f - 8.0f) * 4.5f); }

	// What this bot THINKS the drop would cost: the real figure scaled by its personal misjudgement and a little extra for this particular spot.
	inline float GuessFallDamage(BotBrainState& Brain, float Height, float X, float Y)
	{
		if (Brain.FallBias < 0.0f)
			Brain.FallBias = RandRange(0.75f, 1.3f);

		return FallDamageFor(Height) * Brain.FallBias * (1.0f + 0.12f * std::sin(X * 0.013f + Y * 0.017f));
	}

	// ---- vision: what UE4 gives us without a navmesh is ray casts, so the bot "looks" with a fan of them and remembers where it has been ----

	inline constexpr float VisionCell = 400.0f;
	inline constexpr float VisionRange = 900.0f;
	inline constexpr float VisionMinClear = 220.0f; // a direction with less room than this is a wall (the capsule is about 100 wide)

	inline int64_t CellKey(float X, float Y)
	{
		const int32_t Cx = (int32_t)std::floor(X / VisionCell);
		const int32_t Cy = (int32_t)std::floor(Y / VisionCell);
		return ((int64_t)Cx << 32) ^ (int64_t)(uint32_t)Cy;
	}

	inline int VisitsAt(const BotBrainState& Brain, float X, float Y)
	{
		auto Found = Brain.Cells.find(CellKey(X, Y));
		return Found != Brain.Cells.end() ? (int)Found->second : 0;
	}

	// Called while walking: counts a visit to the cell the bot stands in (twice a second at most), and forgets old visits slowly.
	inline void RememberCell(BotBrainState& Brain, const FVector& Pos, float Now)
	{
		if (Now - Brain.CellStamp < 0.5f)
			return;

		Brain.CellStamp = Now;

		if (Brain.CellDecayAt <= 0.0f)
			Brain.CellDecayAt = Now + 40.0f;

		if (Now >= Brain.CellDecayAt)
		{
			Brain.CellDecayAt = Now + 40.0f;

			for (auto It = Brain.Cells.begin(); It != Brain.Cells.end();)
			{
				if (--It->second == 0)
					It = Brain.Cells.erase(It);
				else
					++It;
			}
		}

		if (Brain.Cells.size() > 800)
			Brain.Cells.clear();

		auto& Visits = Brain.Cells[CellKey(Pos.X, Pos.Y)];

		if (Visits < 20)
			++Visits;
	}

	// Casts a fan of 16 rays round the bot (relative to the way it wants to go, Hx/Hy is a unit vector) and picks the best way on:
	// roomy, roughly towards the goal, away from ground it has already walked over, and sticking with its last choice.
	// Returns the angle to steer away from the heading, in degrees (the convention of RotateDir), and how much room there is that way.
	inline bool ScanVision(BotBrainState& Brain, AActor* Pawn, AActor* IgnoreExtra, const FVector& Pos, float Hx, float Hy, float& OutAngle, float& OutFree)
	{
		if (gTracesFrame > gTraceBudget)
			return false; // too many ray casts this frame already: this bot takes the simple sidestep instead

		const float HeadingYaw = std::atan2(Hy, Hx) * RadToDeg;
		float PrevRelative = Brain.VisionYaw - HeadingYaw;

		while (PrevRelative > 180.0f) PrevRelative -= 360.0f;
		while (PrevRelative < -180.0f) PrevRelative += 360.0f;

		float BestScore = 0.0f;
		bool bFound = false;

		for (int k = -8; k < 8; ++k)
		{
			const float Angle = k * 22.5f;
			float Dx = Hx, Dy = Hy;
			RotateDir(Dx, Dy, Angle);

			float Fraction = 1.0f;
			TraceBlocked(Pawn, IgnoreExtra, FVector{ Pos.X, Pos.Y, Pos.Z + 10.0f }, FVector{ Pos.X + Dx * VisionRange, Pos.Y + Dy * VisionRange, Pos.Z + 10.0f }, nullptr, &Fraction);

			const float Free = Fraction * VisionRange;

			if (Free < VisionMinClear)
				continue;

			const float Cosine = std::cos(Angle * DegToRad);
			const float Interest = 0.25f + 0.75f * (0.5f + 0.5f * Cosine);
			const float Room = Free / VisionRange;
			const int Visits = VisitsAt(Brain, Pos.X + Dx * 550.0f, Pos.Y + Dy * 550.0f);
			float Score = Interest * Room / (1.0f + 0.7f * (float)Visits);

			if (std::fabs(Angle - PrevRelative) < 23.0f)
				Score *= 1.25f;

			if (Score > BestScore)
			{
				BestScore = Score;
				OutAngle = Angle;
				OutFree = Free;
				bFound = true;
			}
		}

		if (bFound)
			Brain.VisionYaw = HeadingYaw + OutAngle;

		++Brain.VisionScans;
		return bFound;
	}

	// "Athena_POI_CommercialDowntown_001" -> "CommercialDowntown"
	inline std::string PoiNameFromPath(const std::string& Path)
	{
		const size_t Start = Path.find("Athena_POI_");

		if (Start == std::string::npos)
			return {};

		size_t Pos = Start + 11;
		size_t End = Pos;

		while (End < Path.size() && Path[End] != '.' && Path[End] != ':' && Path[End] != '/')
			++End;

		std::string Name = Path.substr(Pos, End - Pos);

		// drop a trailing "_001" style index
		const size_t Underscore = Name.find_last_of('_');

		if (Underscore != std::string::npos && Underscore + 1 < Name.size() && std::isdigit((unsigned char)Name[Underscore + 1]))
			Name = Name.substr(0, Underscore);

		return Name;
	}

	// ---- settings file, re-read while the match runs ----
	// ---- names ----
	// FString::Set() stores the pointer it is given, it does not copy. Names have to live in memory that stays valid, otherwise the
	// engine ends up reading freed memory and shows random unicode.
	inline const wchar_t* StableWide(const std::wstring& Text)
	{
		static std::deque<std::wstring> Storage; // a deque never moves its elements
		Storage.push_back(Text);
		return Storage.back().c_str();
	}

	inline std::wstring MakeBotName()
	{
		static const wchar_t* First[] = { L"Shadow", L"Rapid", L"Silent", L"Frost", L"Crimson", L"Neon", L"Toxic", L"Lunar", L"Savage", L"Turbo", L"Pixel", L"Ghost", L"Rogue", L"Blaze", L"Cosmic",
			L"Iron", L"Tony", L"Bruce", L"Steve", L"Swift", L"Mystic", L"Hyper", L"Vortex", L"Atomic", L"Dusty", L"Lucky", L"Salty", L"Sneaky", L"Clutch", L"Golden", L"Frozen", L"Rusty", L"Wild" };
		static const wchar_t* Second[] = { L"Wolf", L"Sniper", L"Knight", L"Viper", L"Dragon", L"Ninja", L"Phantom", L"Hawk", L"Raider", L"Storm", L"Tiger", L"Falcon", L"Reaper", L"Hunter", L"Racer",
			L"Titan", L"Stark", L"Banner", L"Rogers", L"Panda", L"Cobra", L"Comet", L"Bandit", L"Fox", L"Shark", L"Pilot", L"Rider", L"Goat", L"Wizard", L"Ranger", L"Biscuit", L"Turtle", L"Bear" };
		static std::set<std::wstring> Used;

		for (int Try = 0; Try < 300; ++Try)
		{
			std::wstring Name = First[rand() % (sizeof(First) / sizeof(First[0]))];
			Name += Second[rand() % (sizeof(Second) / sizeof(Second[0]))];

			switch (rand() % 4)
			{
			case 0: Name += std::to_wstring(rand() % 100); break;
			case 1: Name += std::to_wstring(2005 + rand() % 20); break;
			case 2: Name += L"_" + std::to_wstring(rand() % 1000); break;
			default: break;
			}

			if (Used.insert(Name).second)
				return Name;
		}

		return L"Player" + std::to_wstring(rand() % 100000);
	}

	// ProcessEvent with a zeroed, correctly sized parameter buffer (a null buffer crashes the engine for any function that has parameters).
	inline void CallSafe(UObject* Object, UFunction* Function)
	{
		if (!Object || !Function)
			return;

		std::vector<uint8_t> Params((std::max)((int)Function->GetPropertiesSize(), 16) + 16, 0);
		Object->ProcessEvent(Function, Params.data());
	}

	// The engine holsters / deactivates the held weapon around skydiving and landing and a real client's RPCs undo that. A bot has no
	// client, so without this the weapon animation plays but the weapon itself stays hidden.
	inline void EnsureWeaponShown(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, float Now, int Id)
	{
		if (Brain.NextWeaponShowCheck > Now)
			return;

		Brain.NextWeaponShowCheck = Now + 1.0f;

		auto Weapon = Pawn->GetCurrentWeapon();

		if (!Weapon)
			return;

		static int ActivatedOffset = Pawn->GetOffset("bWeaponActivated", false);
		static auto ActivatedProp = Pawn->GetProperty("bWeaponActivated", false);
		static int HolsteredOffset = Pawn->GetOffset("bWeaponHolstered", false);
		static auto HolsteredProp = Pawn->GetProperty("bWeaponHolstered", false);
		static auto ActivatedRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.OnRep_WeaponActivated");
		static auto HolsteredRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.OnRep_bWeaponHolstered");

		bool bFixed = false;
		bool bWasInactive = false;
		bool bWasHolstered = false;

		// The red gunfire flash on a listener's compass is raised by the weapon's own fire effects on that listener's client (7.30 exe
		// 0x15b6fd0, called from the muzzle-flash/fire-sound routine 0x15afcf0): it reads the weapon's replicated Instigator, wants a pawn
		// there, and does nothing otherwise. A bot's weapon is equipped without a controller, so make sure it names the bot as its instigator.
		{
			static int InstigatorOffset = Weapon->GetOffset("Instigator", false);

			if (InstigatorOffset != -1)
			{
				auto& Instigator = *(UObject**)((uint8_t*)Weapon + InstigatorOffset);

				if (Instigator != (UObject*)Pawn)
				{
					static int Logged = 0;

					if (++Logged <= 6)
						LOG_INFO(LogBots, "BotWeapon #{} weapon instigator was {}: set it to the bot", Id, Instigator ? "another actor" : "empty");

					Instigator = (UObject*)Pawn;
					bFixed = true;
				}
			}
		}

		if (ActivatedProp && ActivatedOffset != -1 && !Pawn->ReadBitfieldValue(ActivatedOffset, GetFieldMask(ActivatedProp)))
		{
			bWasInactive = true;
			Pawn->SetBitfieldValue(ActivatedOffset, GetFieldMask(ActivatedProp), true);
			CallSafe(Pawn, ActivatedRepFn);
			bFixed = true;
		}

		if (HolsteredProp && HolsteredOffset != -1 && Pawn->ReadBitfieldValue(HolsteredOffset, GetFieldMask(HolsteredProp)))
		{
			bWasHolstered = true;
			Pawn->SetBitfieldValue(HolsteredOffset, GetFieldMask(HolsteredProp), false);
			CallSafe(Pawn, HolsteredRepFn);
			bFixed = true;
		}

		if (bFixed)
		{
			Weapon->ForceNetUpdate();
			Pawn->ForceNetUpdate();

			static int Logged = 0;

			if ((bWasInactive || bWasHolstered) && ++Logged <= 6)
				LOG_INFO(LogBots, "BotWeapon #{} weapon was {}{}{}: re-activated it", Id, bWasInactive ? "inactive" : "", bWasInactive && bWasHolstered ? " and " : "", bWasHolstered ? "holstered" : "");
		}
	}

	// The engine's storm only damages players it can find through their controller, which bots don't have. Do it ourselves: roughly
	// the real damage per second for how small the circle has become.
	inline void ApplyStormDamage(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, const FVector& Pos, float Now, int Id)
	{
		if (!gZoneKnown || Brain.NextStormTick > Now)
			return;

		Brain.NextStormTick = Now + 1.0f;

		if (Dist2D(Pos, gZoneCenter) <= gZoneRadius)
			return;

		const float Damage = gZoneRadius > 50000.0f ? 1.0f : (gZoneRadius > 25000.0f ? 2.0f : (gZoneRadius > 12000.0f ? 5.0f : 10.0f));
		Pawn->SetHealth(Pawn->GetHealth() - Damage);

		if (!Brain.bLoggedStorm)
		{
			Brain.bLoggedStorm = true;
			LOG_INFO(LogBots, "BotStorm #{} is outside the circle (radius {:.0f}), taking {:.0f} damage a second", Id, gZoneRadius, Damage);
		}
	}

	inline void ScanContainers()
	{
		auto Containers = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingContainer::StaticClass());

		gChests.clear();
		gChestActors.clear();
		gChestPoi.clear();
		gChestLooted.clear();
		gPois.clear();
		gPoiRemaining.clear();

		std::unordered_map<std::string, int> PoiIndex;
		std::map<std::string, int> Kinds;
		std::vector<double> SumX, SumY;
		int Logged = 0;

		for (int i = 0; i < Containers.Num(); ++i)
		{
			auto Container = (ABuildingContainer*)Containers.at(i);

			if (!Container || Container->IsActorBeingDestroyed())
				continue;

			const FVector Location = Container->GetActorLocation();
			const std::string Path = Container->GetPathName();
			const std::string Poi = PoiNameFromPath(Path);

			if (Poi.empty() && Logged < 4)
			{
				++Logged;
				LOG_INFO(LogBots, "BotBrain: container without a POI sublevel: {}", Path);
			}

			int Index = -1;

			if (!Poi.empty())
			{
				auto Found = PoiIndex.find(Poi);

				if (Found == PoiIndex.end())
				{
					Index = (int)gPois.size();
					PoiIndex[Poi] = Index;
					BotPoi NewPoi;
					NewPoi.Name = Poi;
					gPois.push_back(NewPoi);
					SumX.push_back(0.0);
					SumY.push_back(0.0);
				}
				else
				{
					Index = Found->second;
				}

				++gPois[Index].Count;
				SumX[Index] += Location.X;
				SumY[Index] += Location.Y;
			}

			// Not everything the game calls a container is worth walking to. The floor-loot markers that are still around are spots Reboot did not
			// spawn loot from (it removes the ones it used); they sit inside the floor, so a bot arrives and finds "something between it and the
			// chest" every time. Shrubs are containers too. Both still count for the POI above (they show where the buildings are).
			const std::string ContainerName = Container->GetName();
			++Kinds[ContainerName.substr(0, ContainerName.find_last_not_of("0123456789_") + 1)];

			if (ContainerName.find("FloorLoot") != std::string::npos || ContainerName.find("Plant_") != std::string::npos || ContainerName.find("Shrub") != std::string::npos)
				continue;

			gChests.push_back(Location);
			gChestActors.push_back(Container);
			gChestPoi.push_back(Index);
			gChestLooted.push_back(Container->IsAlreadySearched() ? 1 : 0);
		}

		Containers.Free();

		{
			static int Logged = 0;

			if (++Logged == 3) // the third scan: the far parts of the map are in by then
			{
				std::string Line;

				for (auto& Kind : Kinds)
				{
					if (Kind.second >= 5)
						Line += std::format("{} {}, ", Kind.first, Kind.second);
				}

				LOG_INFO(LogBots, "BotChests: kinds of container in the map: {}({} of them are goals for the bots)", Line, (int)gChests.size());
			}
		}

		gPoiRemaining.assign(gPois.size(), 0);

		for (size_t i = 0; i < gChests.size(); ++i)
		{
			if (!gChestLooted[i] && gChestPoi[i] >= 0)
				++gPoiRemaining[gChestPoi[i]];
		}

		for (size_t i = 0; i < gPois.size(); ++i)
		{
			gPois[i].Center = FVector{ (float)(SumX[i] / gPois[i].Count), (float)(SumY[i] / gPois[i].Count), 0.0f };

			LOG_INFO(LogBots, "BotBrain: POI '{}' {} containers, centre ({:.0f}, {:.0f})", gPois[i].Name, gPois[i].Count, gPois[i].Center.X, gPois[i].Center.Y);
		}

		LOG_INFO(LogBots, "BotBrain: {} containers cached in {} POIs", (int)gChests.size(), (int)gPois.size());

		// where the containers are densest (for picking a place to drop the bots): cells of 5000 units with their container count and mean height
		{
			struct Cell { int N = 0; double X = 0, Y = 0, Z = 0; };
			std::map<std::pair<int, int>, Cell> Cells;

			for (auto& Chest : gChests)
			{
				auto& C = Cells[{ (int)std::floor(Chest.X / 5000.0f), (int)std::floor(Chest.Y / 5000.0f) }];
				++C.N; C.X += Chest.X; C.Y += Chest.Y; C.Z += Chest.Z;
			}

			std::vector<Cell> Dense;

			for (auto& [Key, C] : Cells)
			{
				if (C.N >= 10)
					Dense.push_back(C);
			}

			std::sort(Dense.begin(), Dense.end(), [](const Cell& A, const Cell& B) { return A.N > B.N; });

			for (size_t i = 0; i < Dense.size() && i < 20; ++i)
				LOG_INFO(LogBots, "BotDensity: {} containers around ({:.0f}, {:.0f}), mean height {:.0f}", Dense[i].N, Dense[i].X / Dense[i].N, Dense[i].Y / Dense[i].N, Dense[i].Z / Dense[i].N);
		}
	}


	// ---- the world around the bots: props that can be harvested for wood, and doors ----

	struct BotHarvestable { ABuildingSMActor* Actor; FVector Pos; };
	struct BotDoor { AActor* Actor; FVector Pos; };

	inline std::vector<BotHarvestable> gHarvestables;
	inline std::vector<BotDoor> gDoors;
	inline std::vector<BotHarvestable> gNewHarvestables;
	inline std::vector<BotDoor> gNewDoors;
	inline std::vector<AActor*> gWorldQueue;
	inline size_t gWorldIndex = 0;
	inline bool gWorldScanning = false;
	inline float gNextWorldScan = 0.0f;
	inline std::unordered_map<UClass*, bool> gDoorClass;

	// The prop and door lists live for minutes, long enough for the engine to free an actor and give its memory to something else (a stale pointer that
	// still passes IsValidLowLevel crashed GetActorLocation). Whatever sits at that address now has to still be a building.
	inline bool IsLiveBuilding(AActor* Actor)
	{
		static auto BuildingClass = ABuildingSMActor::StaticClass();
		return Actor && Actor->IsValidLowLevel() && Actor->IsA(BuildingClass) && !Actor->IsActorBeingDestroyed();
	}

	// Walks every building actor of the map a slice per tick (a full pass would stall the server): wooden props and doors are kept.
	inline void StepWorldScan(float Now)
	{
		if (!gWorldScanning)
		{
			if (Now < gNextWorldScan)
				return;

			gWorldScanning = true;
			gWorldIndex = 0;
			gWorldQueue.clear();
			gNewHarvestables.clear();
			gNewDoors.clear();

			auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingSMActor::StaticClass());

			for (int i = 0; i < Actors.Num(); ++i)
				gWorldQueue.push_back(Actors.at(i));

			Actors.Free();
			LOG_INFO(LogBots, "BotWorld: scanning {} building actors", gWorldQueue.size());
		}

		for (int Budget = 1500; Budget > 0 && gWorldIndex < gWorldQueue.size(); --Budget)
		{
			auto Actor = (ABuildingSMActor*)gWorldQueue[gWorldIndex++];

			if (!Actor || Actor->IsActorBeingDestroyed() || Actor->IsPlayerPlaced())
				continue;

			UClass* Class = Actor->ClassPrivate;
			auto Known = gDoorClass.find(Class);

			if (Known == gDoorClass.end())
			{
				const std::string ClassName = ((UObject*)Class)->GetName();
				const bool bDoor = Contains(Lower(ClassName), "door") && Actor->GetOffset("bDoorOpen", false) != -1;
				Known = gDoorClass.emplace(Class, bDoor).first;

				if (bDoor)
					LOG_INFO(LogBots, "BotWorld: door class {}", ClassName);
			}

			if (Known->second)
			{
				gNewDoors.push_back({ Actor, Actor->GetActorLocation() });
				continue;
			}

			// not a truck or a house: nothing a pickaxe is going to take apart
			if (Actor->GetResourceType() == EFortResourceType::Wood && !Actor->IsDestroyed() && Actor->GetMaxHealth() <= 1500.0f && !Contains(Lower(Actor->GetName()), "car_") && !Contains(Lower(Actor->GetName()), "truck") && !Contains(Lower(Actor->GetName()), "bus") && !Contains(Lower(Actor->GetName()), "vehicle"))
				gNewHarvestables.push_back({ Actor, Actor->GetActorLocation() });
		}

		if (gWorldIndex >= gWorldQueue.size())
		{
			gHarvestables.swap(gNewHarvestables);
			gDoors.swap(gNewDoors);
			gWorldQueue.clear();
			gWorldScanning = false;
			gNextWorldScan = Now + 240.0f;
			LOG_INFO(LogBots, "BotWorld: {} harvestable wooden props and {} doors known", gHarvestables.size(), gDoors.size());
		}
	}


	// Lists the fields and functions of an object's class chain whose names contain one of the words (to find what this build calls things).
	inline void ListFieldsMatching(const char* Label, UObject* Object, std::initializer_list<const char*> Words)
	{
		if (!Object)
			return;

		int Listed = 0;

		for (UStruct* Struct = (UStruct*)Object->ClassPrivate; Struct && Listed < 150; Struct = Struct->GetSuperStruct())
		{
			for (void* Child = *(void**)(__int64(Struct) + Offsets::Children); Child && Listed < 150; Child = GetNext(Child))
			{
				const std::string Name = GetFNameOfProp(Child)->ToString();
				const std::string LowerName = Lower(Name);
				bool bMatch = false;

				for (auto Word : Words)
					bMatch = bMatch || Contains(LowerName, Word);

				if (!bMatch)
					continue;

				++Listed;
				LOG_INFO(LogBots, "Fields {}: {}::{}", Label, ((UObject*)Struct)->GetName(), Name);
			}
		}
	}

	// Vehicles: not driven by bots yet. This only reports what the map has and what this build calls its seat/interaction fields,
	// so the mounting code can be written against the real names.
	inline void LogVehicles(int Pass)
	{
		static auto VehicleClass = FindObject<UClass>(L"/Script/FortniteGame.FortAthenaVehicle");

		if (!VehicleClass || Pass > 2)
			return;

		auto Vehicles = UGameplayStatics::GetAllActorsOfClass(GetWorld(), VehicleClass);
		LOG_INFO(LogBots, "BotVehicle: {} vehicles in the world", Vehicles.Num());

		for (int i = 0; i < Vehicles.Num() && i < 12; ++i)
		{
			auto Vehicle = Vehicles.at(i);

			if (!Vehicle)
				continue;

			const FVector At = Vehicle->GetActorLocation();
			LOG_INFO(LogBots, "BotVehicle: {} at ({:.0f}, {:.0f}, {:.0f})", ((UObject*)Vehicle->ClassPrivate)->GetName(), At.X, At.Y, At.Z);
		}

		if (Pass == 1 && Vehicles.Num() > 0 && Vehicles.at(0))
		{
			ListFieldsMatching("Vehicle", Vehicles.at(0), { "seat", "enter", "exit", "driver", "slot", "interact", "throttle", "steer", "occupant", "pawn" });
		}

		Vehicles.Free();
	}

	inline void ProcessShotChecks(float Now);

	inline bool PawnIsSprinting(AFortPlayerPawnAthena* Pawn)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.IsSprinting");

		if (!Fn || !Pawn)
			return false;

		struct { bool bReturn; char Pad[7]; } Params{};
		Pawn->ProcessEvent(Fn, &Params);
		return Params.bReturn;
	}

	// The game's own hearing numbers (the Athena AI settings the husks listen with): how far a footstep carries, from a slow walk to a full sprint, and how far a
	// shot that hurt somebody is heard. Read once from the settings object; the values that cannot be read fall back to a guess.
	inline float gFootMin = 800.0f, gFootMax = 3000.0f, gFootSpeedScale = 550.0f, gHearCrouch = 0.5f, gHearDamaged = 5000.0f, gHearMaxNpc = 6000.0f;
	inline bool gHearingRead = false;

	inline void ReadHearingSettings()
	{
		if (gHearingRead)
			return;

		gHearingRead = true;
		static auto Settings = FindObject(L"/Script/FortniteGame.Default__AthenaAISettings");

		if (!Settings)
		{
			LOG_INFO(LogBots, "BotHearing: no Athena AI settings object, using made-up footstep ranges");
			return;
		}

		auto Read = [&](const char* Name, float& Out)
		{
			const int Offset = Settings->GetOffset(Name, false);

			if (Offset == -1)
				return -1.0f;

			const float Value = Settings->Get<float>(Offset);

			if (Value > 0.0f)
				Out = Value;

			return Value;
		};

		const float A = Read("MinFootstepHearingRange", gFootMin), B = Read("MaxFootstepHearingRange", gFootMax), C = Read("MaxPlayerSpeedScaleFootstepSounds", gFootSpeedScale);
		const float D = Read("CrouchHearingModifier", gHearCrouch), E = Read("DamagedHearingRange", gHearDamaged), F = Read("MaxNPCHearingRange", gHearMaxNpc);
		LOG_INFO(LogBots, "BotHearing: the game's settings say footsteps carry {:.2f} to {:.2f}, speed scale {:.2f}, crouch x{:.2f}, a damaging shot {:.2f}, an NPC hears at most {:.2f}", A, B, C, D, E, F);
	}

	// how far this pawn's footsteps carry at that ground speed
	inline float FootstepRange(float Speed)
	{
		ReadHearingSettings();
		const float Full = gFootSpeedScale > 10.0f ? gFootSpeedScale : gFootSpeedScale * RunSpeed;
		const float T = (std::max)(0.0f, (std::min)(1.0f, Speed / (std::max)(Full, 1.0f)));
		return gFootMin + (gFootMax - gFootMin) * T;
	}

	// How fast a real player really runs and sprints on this build, learned from the real players on the server (a bot adopts it once it has seen enough).
	inline float gHumanSprint = 0.0f, gHumanRun = 0.0f;
	inline int gHumanSprintSamples = 0, gHumanRunSamples = 0;

	inline void LogLearnedSpeeds()
	{
		static float Last = 0.0f;
		static int Logged = 0;

		if (Logged < 6 && std::fabs(SprintSpeed - Last) > 5.0f)
		{
			++Logged;
			Last = SprintSpeed;
			LOG_INFO(LogBots, "BotSpeeds: bots now sprint at {:.0f} and travel at {:.0f}, walk at {:.0f} (what the real players on this server do)", SprintSpeed, RunSpeed, WalkSpeed);
		}
	}

	inline std::vector<AFortPlayerPawnAthena*> gSprinters; // bots that are sprinting right now (see SprintStyleBegin)

	inline void SprintStyleWrite(uint8_t Value)
	{
		for (size_t i = 0; i < gSprinters.size();)
		{
			auto Pawn = gSprinters[i];

			if (!Pawn || !Pawn->IsValidLowLevel() || Pawn->IsActorBeingDestroyed())
			{
				gSprinters.erase(gSprinters.begin() + i);
				continue;
			}

			static int Offset = Pawn->GetOffset("CurrentMovementStyle", false);

			if (Offset != -1)
				Pawn->Get<uint8_t>(Offset) = Value;

			++i;
		}
	}

	// A bot's velocity is written by the brain after the game's movement update, so what goes out to the other players is the speed it asked for, not
	// the speed it got. Held up against something that is a full run on the spot: their screens keep carrying it forward and pulling it back, with
	// the run animation playing (it looks like sliding). While the world is replicated such a bot shows the velocity it really has.
	struct ShownVelocity { FVector* Velocity; float RealX, RealY, AskedX, AskedY; };
	inline std::vector<ShownVelocity> gShownVelocity;

	inline void SprintStyleBegin()
	{
		SprintStyleWrite(3);

		for (auto& Shown : gShownVelocity)
		{
			Shown.AskedX = Shown.Velocity->X;
			Shown.AskedY = Shown.Velocity->Y;
			Shown.Velocity->X = Shown.RealX;
			Shown.Velocity->Y = Shown.RealY;
		}
	}

	inline void SprintStyleEnd()
	{
		SprintStyleWrite(0);

		for (auto& Shown : gShownVelocity)
		{
			Shown.Velocity->X = Shown.AskedX;
			Shown.Velocity->Y = Shown.AskedY;
		}

		gShownVelocity.clear(); // the pointers are only good for the frame they were taken in
	}

	// FortPawn.CurrentMovementStyle (an EFortMovementStyle byte that is also part of the replicated SharedRepMovement): read for the log only
	inline int PawnMovementStyle(AFortPlayerPawnAthena* Pawn)
	{
		static int Offset = Pawn->GetOffset("CurrentMovementStyle", false);
		return Offset != -1 ? (int)Pawn->Get<uint8_t>(Offset) : -1;
	}

	inline void LearnHumanSpeed(AFortPlayerPawnAthena* Pawn, float Speed, float Now)
	{
		static auto ModeOffset = -2;

		if (ModeOffset == -2)
		{
			auto Movement = (UObject*)nullptr;
			static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);

			if (CharacterMovementOffset != -1)
				Movement = Pawn->Get<UObject*>(CharacterMovementOffset);

			ModeOffset = Movement ? Movement->GetOffset("MovementMode", false) : -1;
		}

		static auto CharacterMovementOffset2 = Pawn->GetOffset("CharacterMovement", false);
		UObject* Movement = CharacterMovementOffset2 != -1 ? Pawn->Get<UObject*>(CharacterMovementOffset2) : nullptr;
		const bool bGrounded = Movement && ModeOffset >= 0 && Movement->Get<uint8_t>(ModeOffset) == 1;
		const bool bSprint = PawnIsSprinting(Pawn);

		static float NextLog = 0.0f;

		if (Now >= NextLog)
		{
			NextLog = Now + 8.0f;
			static int MaxWalkOffset = Movement ? Movement->GetOffset("MaxWalkSpeed", false) : -1;
			LOG_INFO(LogBots, "HumanMove: {:.0f} units/s on the ground={} sprinting={} style={} maxWalkSpeed={:.0f}; learned sprint {:.0f} ({} samples), run {:.0f} ({} samples)", Speed, bGrounded, bSprint, PawnMovementStyle(Pawn), (Movement && MaxWalkOffset != -1) ? Movement->Get<float>(MaxWalkOffset) : -1.0f, gHumanSprint, gHumanSprintSamples, gHumanRun, gHumanRunSamples);
		}

		// the engine's own number: while a real player is moving flat out, their movement component's MaxWalkSpeed is what they can do
		static int MaxWalkOffset2 = Movement ? Movement->GetOffset("MaxWalkSpeed", false) : -1;

		if (!bGrounded || MaxWalkOffset2 == -1 || Speed < 300.0f || Speed > 2200.0f)
			return;

		const float MaxWalk = Movement->Get<float>(MaxWalkOffset2);

		if (Speed < MaxWalk * 0.85f)
			return; // not at full speed (turning, starting, a wall in the way)

		if (MaxWalk >= 480.0f && MaxWalk <= 900.0f)
		{
			gHumanSprint = gHumanSprintSamples == 0 ? MaxWalk : gHumanSprint * 0.8f + MaxWalk * 0.2f;

			if (++gHumanSprintSamples >= 3)
			{
				SprintSpeed = RunSpeed = gHumanSprint;
				LogLearnedSpeeds();
			}
		}
		else if (MaxWalk >= 380.0f && MaxWalk < 480.0f)
		{
			gHumanRun = gHumanRunSamples == 0 ? MaxWalk : gHumanRun * 0.8f + MaxWalk * 0.2f;

			if (++gHumanRunSamples >= 3)
			{
				WalkSpeed = gHumanRun;
				LogLearnedSpeeds();
			}
		}
	}

	inline float GetActorYaw(AActor* Actor);

	// ---- session recorder ----
	// Writes what the real players do (and a few bots, for comparison) to session_record.csv next to the log, five times a second, so that how people really
	// move, fight, build and heal can be studied afterwards (see analyze_session.py). One line per pawn per sample: S rows are samples, E rows are events.
	inline void RecordSession(float Now)
	{
		static float Next = 0.0f, FlushAt = 0.0f;
		static FILE* File = nullptr;
		static bool bFailed = false;

		if (bFailed || Now < Next)
			return;

		Next = Now + 0.2f;

		if (!File)
		{
			fopen_s(&File, "session_record.csv", "a");

			if (!File)
			{
				bFailed = true;
				return;
			}

			fprintf(File, "# kind,t,who,human,x,y,z,vx,vy,vz,yaw,pitch,mode,health,shield,weapon,ammo,sinceFired,maxWalk,crouched,nearestEnemy,nearestEnemyBearing,nearestEnemyIsHuman,zoneEdgeDist,zoneRadius\n");
		}

		struct Last { float Health = 0.0f; int Ammo = 0; std::string Weapon; };
		static std::unordered_map<void*, Last> Previous;
		static std::unordered_map<void*, int> Ids;

		for (auto& P : gPawns)
		{
			auto Pawn = P.Pawn;

			if (!Pawn || Pawn->IsActorBeingDestroyed() || !Pawn->IsValidLowLevel())
				continue;

			const bool bHuman = !P.bIsBot;

			if (!bHuman && (((uintptr_t)Pawn >> 4) % 18) != 0)
				continue; // a handful of bots to compare with

			auto Id = Ids.find(Pawn);

			if (Id == Ids.end())
				Id = Ids.emplace(Pawn, (int)Ids.size()).first;

			static int CharacterMovementOffset = Pawn->GetOffset("CharacterMovement", false);
			UObject* Movement = CharacterMovementOffset != -1 ? Pawn->Get<UObject*>(CharacterMovementOffset) : nullptr;
			FVector Velocity{};
			uint8_t Mode = 0;
			float MaxWalk = 0.0f;

			if (Movement)
			{
				static int VelocityOffset = Movement->GetOffset("Velocity", false);
				static int ModeOffset = Movement->GetOffset("MovementMode", false);
				static int MaxWalkOffset = Movement->GetOffset("MaxWalkSpeed", false);

				if (VelocityOffset != -1) Velocity = Movement->Get<FVector>(VelocityOffset);
				if (ModeOffset != -1) Mode = Movement->Get<uint8_t>(ModeOffset);
				if (MaxWalkOffset != -1) MaxWalk = Movement->Get<float>(MaxWalkOffset);
			}

			static int PitchOffset = Pawn->GetOffset("RemoteViewPitch", false);
			const float Pitch = PitchOffset != -1 ? ((float)Pawn->Get<uint8_t>(PitchOffset) * 360.0f / 256.0f) : 0.0f;
			static int CrouchProp = Pawn->GetOffset("bIsCrouched", false);
			bool bCrouched = false;

			if (CrouchProp != -1)
			{
				if (auto Prop = Pawn->GetProperty("bIsCrouched", false))
					bCrouched = Pawn->ReadBitfieldValue(CrouchProp, GetFieldMask(Prop));
			}

			const FVector At = Pawn->GetActorLocation();
			const float Yaw = GetActorYaw(Pawn);
			const float Health = Pawn->GetHealth(), Shield = Pawn->GetShield();
			std::string Weapon = "-";
			int Ammo = -1;

			if (auto Held = Pawn->GetCurrentWeapon())
			{
				if (auto Def = (UObject*)Held->GetWeaponData())
					Weapon = Def->GetName();

				Ammo = Held->GetAmmoCount();
			}

			// the nearest other pawn on another team
			float Nearest = -1.0f, Bearing = 0.0f;
			bool bNearestHuman = false;

			for (auto& O : gPawns)
			{
				if (O.Pawn == Pawn || O.Team == P.Team || !O.Pawn)
					continue;

				const float D = Dist2D(At, O.Pos);

				if (Nearest < 0.0f || D < Nearest)
				{
					Nearest = D;
					Bearing = std::atan2(O.Pos.Y - At.Y, O.Pos.X - At.X) * RadToDeg - Yaw;
					while (Bearing > 180.0f) Bearing -= 360.0f;
					while (Bearing < -180.0f) Bearing += 360.0f;
					bNearestHuman = !O.bIsBot;
				}
			}

			const float SinceFired = P.LastFired > 0.0f ? Now - P.LastFired : 999.0f;
			fprintf(File, "S,%.2f,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.1f,%d,%.0f,%.0f,%s,%d,%.2f,%.0f,%d,%.0f,%.0f,%d,%.0f,%.0f\n", Now, Id->second, bHuman ? 1 : 0, At.X, At.Y, At.Z, Velocity.X, Velocity.Y, Velocity.Z, Yaw, Pitch, (int)Mode,
				Health, Shield, Weapon.c_str(), Ammo, SinceFired, MaxWalk, bCrouched ? 1 : 0, Nearest, Bearing, bNearestHuman ? 1 : 0,
				gZoneKnown ? Dist2D(At, gZoneCenter) - gZoneRadius : -999999.0f, gZoneKnown ? gZoneRadius : 0.0f);

			// events: damage taken, healing, shots, weapon changes
			auto& Prev = Previous[Pawn];

			if (Prev.Health > 0.0f)
			{
				if (Health + Shield < Prev.Health - 2.0f)
					fprintf(File, "E,%.2f,%d,%d,hurt,%.0f,%.0f,%.0f,%d\n", Now, Id->second, bHuman ? 1 : 0, Prev.Health - Health - Shield, Nearest, Bearing, bNearestHuman ? 1 : 0);
				else if (Health + Shield > Prev.Health + 4.0f)
					fprintf(File, "E,%.2f,%d,%d,heal,%.0f,%.0f,%.0f,%d\n", Now, Id->second, bHuman ? 1 : 0, Health + Shield - Prev.Health, Nearest, Bearing, bNearestHuman ? 1 : 0);
			}

			if (Prev.Ammo > 0 && Ammo >= 0 && Ammo < Prev.Ammo && Weapon == Prev.Weapon)
				fprintf(File, "E,%.2f,%d,%d,shot,%d,%.0f,%.0f,%d\n", Now, Id->second, bHuman ? 1 : 0, Prev.Ammo - Ammo, Nearest, Bearing, bNearestHuman ? 1 : 0);

			if (!Prev.Weapon.empty() && Weapon != Prev.Weapon)
				fprintf(File, "E,%.2f,%d,%d,equip,%s,%.0f,%.0f,%d\n", Now, Id->second, bHuman ? 1 : 0, Weapon.c_str(), Nearest, Bearing, bNearestHuman ? 1 : 0);

			Prev.Health = Health + Shield;
			Prev.Ammo = Ammo;
			Prev.Weapon = Weapon;
		}

		if (Now >= FlushAt)
		{
			FlushAt = Now + 5.0f;
			fflush(File);
		}
	}

	inline void ScanProjectiles(float Now)
	{
		if (Now < gNextProjectileScan)
			return;

		gNextProjectileScan = Now + 0.15f;
		static auto ProjectileClass = FindObject<UClass>(L"/Script/FortniteGame.FortProjectileBase");
		static std::unordered_map<void*, std::pair<FVector, float>> Previous;

		std::vector<BotProjectile> Found;
		std::unordered_map<void*, std::pair<FVector, float>> Seen;

		if (ProjectileClass)
		{
			auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ProjectileClass);

			for (int i = 0; i < Actors.Num() && i < 80; ++i)
			{
				auto Actor = Actors.at(i);

				if (!Actor || Actor->IsActorBeingDestroyed())
					continue;

				const FVector At = Actor->GetActorLocation();
				Seen[Actor] = { At, Now };
				auto Before = Previous.find(Actor);

				if (Before == Previous.end() || Now - Before->second.second < 0.03f)
					continue;

				const float Dt = Now - Before->second.second;
				Found.push_back({ At, FVector{ (At.X - Before->second.first.X) / Dt, (At.Y - Before->second.first.Y) / Dt, (At.Z - Before->second.first.Z) / Dt }, Now });
			}

			Actors.Free();
		}

		Previous.swap(Seen);
		gProjectiles.swap(Found);
	}

	inline void RefreshShared(float Now, AFortGameModeAthena* GameMode)
	{
		ScanProjectiles(Now);

		StepWorldScan(Now);
		ProcessShotChecks(Now);

		{
			static int VehiclePass = 0;
			static float NextVehicleLog = 60.0f;

			if (Now >= NextVehicleLog && VehiclePass < 3)
			{
				NextVehicleLog = Now + 120.0f;
				LogVehicles(++VehiclePass);
			}
		}

		if (gNextChestScan <= Now)
		{
			gNextChestScan = Now + 20.0f;
			ScanContainers();
		}

		if (gNextZoneRead <= Now)
		{
			gNextZoneRead = Now + 1.0f;
			gZoneKnown = false;

			auto Indicator = GameMode ? GameMode->GetSafeZoneIndicator() : nullptr;

			if (Indicator)
			{
				static auto CenterFn = FindObject<UFunction>(L"/Script/FortniteGame.FortSafeZoneIndicator.GetSafeZoneCenter");
				static auto RadiusFn = FindObject<UFunction>(L"/Script/FortniteGame.FortSafeZoneIndicator.GetSafeZoneRadius");

				if (CenterFn && RadiusFn)
				{
					struct { FVector ReturnValue; } CenterParams{};
					struct { float ReturnValue; } RadiusParams{};
					Indicator->ProcessEvent(CenterFn, &CenterParams);
					Indicator->ProcessEvent(RadiusFn, &RadiusParams);

					if (RadiusParams.ReturnValue > 100.0f)
					{
						gZoneKnown = true;
						gZoneCenter = CenterParams.ReturnValue;
						gZoneRadius = RadiusParams.ReturnValue;
					}
					else
					{
						// the getter can read zero before the circle is set up; the property holds the same value
						static int RadiusOffset = Indicator->GetOffset("Radius", false);

						if (RadiusOffset != -1 && Indicator->Get<float>(RadiusOffset) > 100.0f)
						{
							gZoneKnown = true;
							gZoneCenter = Indicator->GetActorLocation();
							gZoneRadius = Indicator->Get<float>(RadiusOffset);
						}
					}
				}
			}

			// how fast the edge closes in, from the last reading a second ago (falls back to 0 slowly, so a pause between readings is not taken for a stop)
			{
				static FVector PrevCenter{};
				static float PrevRadius = 0.0f, PrevTime = 0.0f;

				if (gZoneKnown && PrevRadius > 0.0f && Now - PrevTime > 0.5f && Now - PrevTime < 5.0f)
				{
					const float Closing = ((PrevRadius - gZoneRadius) + Dist2D(PrevCenter, gZoneCenter)) / (Now - PrevTime);
					gZoneCloseSpeed = Closing > gZoneCloseSpeed ? Closing : gZoneCloseSpeed * 0.8f + (std::max)(Closing, 0.0f) * 0.2f;
				}

				if (gZoneKnown)
				{
					PrevCenter = gZoneCenter;
					PrevRadius = gZoneRadius;
					PrevTime = Now;
				}
			}

			// The next circle is on the map for everybody; a player heads for it long before the storm arrives. Only believed when it lies inside the current one.
			gNextZoneKnown = false;

			if (Indicator && gZoneKnown)
			{
				static int NextCenterOffset = Indicator->GetOffset("NextCenter", false);
				static int NextRadiusOffset = Indicator->GetOffset("NextRadius", false);

				if (NextCenterOffset != -1 && NextRadiusOffset != -1)
				{
					const FVector NextCenter = *Indicator->GetPtr<FVector>(NextCenterOffset);
					const float NextRadius = *Indicator->GetPtr<float>(NextRadiusOffset);

					if (NextRadius > 500.0f && NextRadius < gZoneRadius - 300.0f && Dist2D(NextCenter, gZoneCenter) + NextRadius <= gZoneRadius + 3000.0f)
					{
						gNextZoneKnown = true;
						gNextZoneCenter = NextCenter;
						gNextZoneRadius = NextRadius;
					}
				}
			}

			static float NextZoneLog = 0.0f;

			if (Now >= NextZoneLog)
			{
				NextZoneLog = Now + 15.0f;
				LOG_INFO(LogBots, "BotZone indicator={} known={} centre=({:.0f}, {:.0f}) radius={:.0f} closing {:.0f}u/s next={} ({:.0f}, {:.0f}) radius={:.0f}", Indicator != nullptr, gZoneKnown, gZoneCenter.X, gZoneCenter.Y, gZoneRadius, gZoneCloseSpeed, gNextZoneKnown, gNextZoneCenter.X, gNextZoneCenter.Y, gNextZoneRadius);
			}

		}

		if (gNextPawnScan <= Now)
		{
			gNextPawnScan = Now + 0.4f;
			gPawns.clear();

			auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFortPlayerPawnAthena::StaticClass());

			for (int i = 0; i < Actors.Num(); ++i)
			{
				auto Pawn = (AFortPlayerPawnAthena*)Actors.at(i);

				if (!Pawn || Pawn->IsActorBeingDestroyed() || Pawn->GetHealth() <= 0.0f || Pawn->IsDBNO())
					continue;

				auto PlayerState = Cast<AFortPlayerStateAthena>((UObject*)Pawn->GetPlayerState());

				BotPawnInfo Info;
				Info.Pawn = Pawn;
				Info.Pos = Pawn->GetActorLocation();
				Info.Team = PlayerState ? (int)PlayerState->GetTeamIndex() : -1;
				Info.bIsBot = PlayerState && IsBotPlayerState(PlayerState);
				Info.Health = Pawn->GetHealth();
				Info.Shield = Pawn->GetShield();

				// where a real player is looking: a bot being aimed at by somebody it chose to ignore thinks again
				if (!Info.bIsBot)
				{
					static auto GetControlRotationFn = FindObject<UFunction>(L"/Script/Engine.Pawn.GetControlRotation");
					FRotator Aim{};

					if (GetControlRotationFn)
					{
						struct { FRotator ReturnValue; } Params{};
						Pawn->ProcessEvent(GetControlRotationFn, &Params);
						Aim = Params.ReturnValue;
					}

					if (Aim.Yaw == 0.0f && Aim.Pitch == 0.0f)
						Aim = Pawn->GetActorRotation();

					Info.AimYaw = Aim.Yaw;
					Info.bAimKnown = true;
				}

				if (auto HeldWeapon = Pawn->GetCurrentWeapon())
				{
					const auto& HeldInfo = GetItemInfo((UObject*)HeldWeapon->GetWeaponData());
					Info.WeaponScore = HeldInfo.Kind == EBotItemKind::Gun ? HeldInfo.Score : 0;

					// a real player's shots make a noise the bots can hear (the bots' own shots are announced where they are fired)
					if (!Info.bIsBot && HeldInfo.Kind == EBotItemKind::Gun)
					{
						static std::unordered_map<void*, int> LastAmmo;
						const int Ammo = HeldWeapon->GetAmmoCount();
						auto Previous = LastAmmo.find(Pawn);

						if (Previous != LastAmmo.end() && Ammo < Previous->second)
						{
							AddSoundNow(Info.Pos, HeldInfo.bShotgun ? 4000.0f : 5000.0f, Pawn, Info.Team);
							gHumanShotTime[Pawn] = UGameplayStatics::GetTimeSeconds(GetWorld());
						}

						LastAmmo[Pawn] = Ammo;
					}
				}

				// how fast it is moving, and when it last fired (both give it away)
				{
					static std::unordered_map<void*, std::pair<FVector, float>> Previous;
					auto Found = Previous.find(Pawn);

					if (Found != Previous.end() && Now - Found->second.second > 0.05f)
						Info.Speed = Dist2D(Info.Pos, Found->second.first) / (Now - Found->second.second);

					Previous[Pawn] = { Info.Pos, Now };

					for (auto& Sound : gSounds)
					{
						if (Sound.Source == (void*)Pawn && Sound.Time > Info.LastFired)
							Info.LastFired = Sound.Time;
					}
				}

				if (!Info.bIsBot)
				{
					static int FiredTimeOffset = Pawn->GetOffset("LastFiredTime", false);

					if (FiredTimeOffset != -1)
					{
						const float Fired = Pawn->Get<float>(FiredTimeOffset);

						if (Fired > Info.LastFired && Fired <= Now + 1.0f && Fired > Now - 30.0f)
							Info.LastFired = Fired;
					}

					LearnHumanSpeed(Pawn, Info.Speed, Now);
				}

				gPawns.push_back(Info);

				static float NextHumanLog = 0.0f;

				if (!Info.bIsBot && Now >= NextHumanLog)
				{
					NextHumanLog = Now + 5.0f;
					LOG_INFO(LogBots, "HumanPos ({:.0f}, {:.0f}, {:.0f})", Info.Pos.X, Info.Pos.Y, Info.Pos.Z);
				}
			}

			Actors.Free();
		}

		if (gNextPickupScan <= Now)
		{
			gNextPickupScan = Now + 0.5f;
			gPickups.clear();

			auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFortPickup::StaticClass());
			static int bPickedUpOffset = -2; // looked up from the first pickup instance, never from the class object

			for (int i = 0; i < Actors.Num(); ++i)
			{
				auto Pickup = (AFortPickup*)Actors.at(i);

				if (!Pickup || Pickup->IsActorBeingDestroyed())
					continue;

				if (bPickedUpOffset == -2)
					bPickedUpOffset = Pickup->GetOffset("bPickedUp", false);

				if (bPickedUpOffset >= 0 && Pickup->Get<bool>(bPickedUpOffset))
					continue;

				auto Entry = Pickup->GetPrimaryPickupItemEntry();
				auto Def = Entry ? (UObject*)Entry->GetItemDefinition() : nullptr;
				const auto& Info = GetItemInfo(Def);

				if (Info.Kind != EBotItemKind::Gun && Info.Kind != EBotItemKind::Heal)
					continue; // ammo and materials are collected by walking over them (Reboot's overlap hook)

				BotPickupInfo PickupInfo;
				PickupInfo.Pickup = Pickup;
				PickupInfo.Pos = Pickup->GetActorLocation();
				PickupInfo.Score = Info.Score;
				PickupInfo.Kind = Info.Kind;
				gPickups.push_back(PickupInfo);
			}

			Actors.Free();
		}
	}

	inline UObject* GetMovement(AFortPlayerPawnAthena* Pawn)
	{
		static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement");
		return Pawn->Get<UObject*>(CharacterMovementOffset);
	}

	// Reads a bitfield property straight from memory (no engine call, so it is safe on a pawn the engine is tearing down).
	inline int GetPawnBitOn(UObject* Object, const char* Name)
	{
		const int Offset = Object->GetOffset(Name, false);
		auto Prop = Object->GetProperty(Name, false);
		return Offset != -1 && Prop ? (int)Object->ReadBitfieldValue(Offset, GetFieldMask(Prop)) : -1;
	}

	// Sets a bitfield property directly (no OnRep call).
	inline void SetPawnBitOn(UObject* Object, const char* Name, bool bValue)
	{
		const int Offset = Object->GetOffset(Name, false);
		auto Prop = Object->GetProperty(Name, false);

		if (Offset != -1 && Prop)
			Object->SetBitfieldValue(Offset, GetFieldMask(Prop), bValue);
	}

	inline void SetPawnBit(AFortPlayerPawnAthena* Pawn, const char* Name, bool bValue)
	{
		const int Offset = Pawn->GetOffset(Name, false);
		auto Prop = Pawn->GetProperty(Name, false);

		if (Offset != -1 && Prop)
			Pawn->SetBitfieldValue(Offset, GetFieldMask(Prop), bValue);
	}

	// The OnRep_ functions take the previous value (bPreviousState). ProcessEvent with no parameter buffer makes the engine
	// memcpy its parameters from null and crash, so always pass one.
	inline void CallOnRep(AFortPlayerPawnAthena* Pawn, const wchar_t* FunctionPath, bool bPreviousState)
	{
		auto Fn = FindObject<UFunction>(FunctionPath);

		if (Fn)
		{
			struct { bool bPreviousState; } Params{ bPreviousState };
			Pawn->ProcessEvent(Fn, &Params);
		}
	}

	inline void SetSkyVisuals(AFortPlayerPawnAthena* Pawn, bool bSkydiving, bool bParachute)
	{
		if (!bBotGliderVisuals)
			return;

		// 1.x: a skydiving pawn's own tick opens the parachute near the ground (custom movement mode 2), and that code plays the glider's
		// OpenSound from Pawn->CustomizationLoadout.Glider (pawn+0x1698 on 1.10). Bots get a glider in PlayerBot::GiveGlider; without one
		// the server crashed at the first bot parachute (1.10, 2026-11-08 23:30 and 23:35), so a bot that has none only shows a plain fall.
		if (Fortnite_Version < 2)
		{
			static int LoadoutOffset = Pawn->GetOffset("CustomizationLoadout", false);
			static int GliderOffset = FindOffsetStruct("/Script/FortniteGame.FortAthenaLoadout", "Glider", false);

			if (LoadoutOffset == -1 || GliderOffset == -1 || !*(UObject**)(__int64(Pawn) + LoadoutOffset + GliderOffset))
				return;
		}

		SetPawnBit(Pawn, "bIsSkydiving", bSkydiving);
		CallOnRep(Pawn, L"/Script/FortniteGame.FortPlayerPawn.OnRep_IsSkydiving", !bSkydiving);
		SetPawnBit(Pawn, "bIsParachuteOpen", bParachute);
		CallOnRep(Pawn, L"/Script/FortniteGame.FortPlayerPawn.OnRep_IsParachuteOpen", !bParachute);
	}

	// ---- observe: the only place that reads the engine ----
	// Can the bot see this spot from its eyes? Loot behind a wall is not noticed, and cannot be picked up or opened through one either.
	inline bool HasSight(AFortPlayerPawnAthena* Pawn, const FVector& From, AActor* Item, const FVector& At)
	{
		return !TraceBlocked(Pawn, Item, FVector{ From.X, From.Y, From.Z + 50.0f }, FVector{ At.X, At.Y, At.Z + 40.0f });
	}

	// a prop in the way that a pickaxe can take down: a building piece that is not vehicle-sized and has not already defeated this bot
	inline bool IsBreakableBlocker(const BotBrainState& Brain, UObject* Blocker)
	{
		static auto BuildingClass = ABuildingSMActor::StaticClass();

		if (!Blocker || !Blocker->IsA(BuildingClass))
			return false;

		const bool bPlayerBuilt = Blocker->GetName().rfind("PBWA_", 0) == 0;
		const bool bTough = !bPlayerBuilt && (((ABuildingSMActor*)Blocker)->GetMaxHealth() > 1500.0f || ((ABuildingSMActor*)Blocker)->GetHealth() > 450.0f || Contains(Lower(Blocker->GetName()), "bus"));

		return !bTough && std::find(Brain.SkipHarvest.begin(), Brain.SkipHarvest.end(), (void*)Blocker) == Brain.SkipHarvest.end();
	}

	inline void StartBreak(BotBrainState& Brain, const BotObservation& Obs, int Id, const char* Why)
	{
		Brain.bBreaking = true;
		Brain.BreakUntil = Obs.Now + 10.0f;
		++Brain.BreakCount;
		Brain.HarvestActor = (AActor*)Obs.BlockerActor;
		Brain.Target = FVector{ Obs.BlockerPoint.X, Obs.BlockerPoint.Y, Obs.Pos.Z };
		Brain.TargetKind = EBotTarget::Harvest;
		Brain.bHasTarget = true;
		Brain.TargetPickedAt = Obs.Now;
		Brain.NavPath.clear();
		Brain.bNavWanted = false;

		static int Smashed = 0;

		if (++Smashed <= 40)
			LOG_INFO(LogBots, "BotBreak #{} is blocked by {} ({}) and goes to break it", Id, Obs.BlockerActor->GetName(), Why);
	}

	inline void BeginBreak(BotBrainState& Brain, const BotObservation& Obs, AActor* Piece, int Id, const char* Why);

	inline BotObservation Observe(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, UObject* Movement, AFortInventory* Inventory, EAthenaGamePhase Phase, float Now, int SelfTeam)
	{
		double ObsLast = PerfNow();
		auto Mark = [&](int Part) { const double Now2 = PerfNow(); gObsSection[Part] += Now2 - ObsLast; ObsLast = Now2; };

		BotObservation Obs;
		Obs.SelfTeam = SelfTeam;
		Obs.Now = Now;
		Obs.Phase = Phase;
		Obs.Governor = FightGovernor(Now, Phase);
		Obs.Pos = Pawn->GetActorLocation();
		Obs.SelfPawn = Pawn;

		static auto MovementModeOffset = Movement->GetOffset("MovementMode");
		Obs.MovementMode = Movement->Get<uint8_t>(MovementModeOffset);

		Obs.bZoneKnown = gZoneKnown;
		Obs.ZoneCenter = gZoneCenter;
		Obs.ZoneRadius = gZoneRadius;
		Obs.bNextZoneKnown = gNextZoneKnown;
		Obs.NextZoneCenter = gNextZoneCenter;
		Obs.NextZoneRadius = gNextZoneRadius;

		// Height above ground, traced a few times a second while in the air.
		if (Brain.State == EBotBrainState::Skydive || Brain.State == EBotBrainState::Glide)
		{
			// How far to the ground. The game's FindGroundLocationAt was the single most expensive thing in the bot code (it took the server from
			// 30 to 12 frames a second while 90 bots were in the air), so this is one plain ray cast down, rarely when far up and often near the ground.
			const float KnownHeight = Brain.bHaveGroundZ ? Obs.Pos.Z - Brain.GroundZ : 1e9f;

			if (Brain.NextGroundTraceTime <= Now)
			{
				Brain.NextGroundTraceTime = Now + (KnownHeight > 15000.0f ? 1.0f : (KnownHeight > 6000.0f ? 0.4f : 0.15f));

				FVector Ground{};

				if (TraceBlocked(Pawn, nullptr, Obs.Pos, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 150000.0f }, nullptr, nullptr, nullptr, &Ground))
				{
					Brain.GroundZ = Ground.Z;
					Brain.bHaveGroundZ = true;
				}
			}

			if (Brain.bHaveGroundZ)
			{
				Obs.bHaveHeight = true;
				Obs.HeightAboveGround = Obs.Pos.Z - Brain.GroundZ;
			}

			return Obs;
		}

		if (Obs.MovementMode != 1)
			return Obs;

		auto HeldWeapon = Pawn->GetCurrentWeapon();
		Obs.SelfWeapon = HeldWeapon;

		Obs.Health = Pawn->GetHealth();
		Obs.Shield = Pawn->GetShield();

		Mark(0);

		// weapons and healing items: what is in the inventory and what is in hand
		if (Inventory)
		{
			auto& Items = Inventory->GetItemList().GetItemInstances();
			int PrimarySlotsUsed = 0;
			float BestHealValue = 0.0f;

			for (int i = 0; i < Items.Num(); ++i)
			{
				auto Item = Items.at(i);
				auto Entry = Item ? Item->GetItemEntry() : nullptr;

				if (!Entry || !Entry->GetItemDefinition())
					continue;

				if (IsPrimaryQuickbar(Entry->GetItemDefinition()))
					++PrimarySlotsUsed;

				const auto& Info = GetItemInfo((UObject*)Entry->GetItemDefinition());

				if (Info.bWood)
					Obs.WoodCount += Entry->GetCount();

				if (Info.Kind == EBotItemKind::Heal)
				{
					Obs.HealCount += Entry->GetCount();

					// what this item would add right now, with a small preference for the quick ones
					const float GainHealth = (std::max)(0.0f, (std::min)(Info.HealHealth, Info.HealCapHealth - Obs.Health));
					const float GainShield = (std::max)(0.0f, (std::min)(Info.HealShield, Info.HealCapShield - Obs.Shield));
					const float Gain = GainHealth + GainShield;
					const float Value = Gain - Info.UseTime * 0.5f;

					if (Gain >= 8.0f && Value > BestHealValue)
					{
						BestHealValue = Value;
						Obs.bCanHeal = true;
						Obs.HealDef = (UObject*)Entry->GetItemDefinition();
						Obs.HealGuid = Entry->GetItemGuid();
						Obs.HealUseTime = Info.UseTime;
						Obs.HealHealth = Info.HealHealth;
						Obs.HealShield = Info.HealShield;
						Obs.HealCapHealth = Info.HealCapHealth;
						Obs.HealCapShield = Info.HealCapShield;
						Obs.bHealIsWeapon = Info.bWeaponDef;
					}

					continue;
				}

				if (Info.Kind != EBotItemKind::Gun)
					continue;

				const int Score = Info.Score + (Entry->GetLoadedAmmo() > 0 ? 25 : 0);

				if (!Obs.bHasGun || Score > Obs.BestGunScore)
				{
					Obs.bHasGun = true;
					Obs.BestGunScore = Score;
					Obs.BestGunDef = (UObject*)Entry->GetItemDefinition();
					Obs.BestGunGuid = Entry->GetItemGuid();
				}
			}

			Obs.FreeSlots = 5 - PrimarySlotsUsed;

			// worth using something: hurt, or with room to spare on the shield
			Obs.bNeedsHeal = Obs.bCanHeal && Obs.Now >= Brain.NoHealUntil && (Obs.Health < 75.0f || Obs.Health + Obs.Shield < 110.0f);
		}

		if (HeldWeapon)
		{
			auto HeldDef = (UObject*)HeldWeapon->GetWeaponData();
			const auto& HeldInfo = GetItemInfo(HeldDef);

			if (HeldInfo.Kind == EBotItemKind::Gun)
			{
				Obs.bHoldingGun = true;
				Obs.SelfWeaponScore = HeldInfo.Score;
				Obs.HeldRange = HeldInfo.Range;
				Obs.HeldInterval = HeldInfo.Interval;
				Obs.bHeldShotgun = HeldInfo.bShotgun;
				Obs.bHoldingBestGun = HeldDef == Obs.BestGunDef;
			}
		}

		Mark(1);

		// route from the navigation system, when a decision asked for one (a few times a second at most)
		if (Brain.bNavWanted && (gNavState != -1 || !gNavTiles.empty()) && Now >= Brain.NavQueryAt)
		{
			Brain.NavQueryAt = Now + 1.5f;
			std::vector<FVector> Points;

			std::vector<uint8_t> MeshJumps;

			if (gNavState == -1 && TryMeshRoute(Obs.Pos, Brain.NavGoal, Points, MeshJumps))
			{
				// the game's navigation mesh knew the way
				++gMeshRoutes;
				Brain.NavPath = Points;
				Brain.NavJump = MeshJumps;
				Brain.NavIndex = 1;
				Brain.NavPathTime = Now;
				Brain.NavQueryAt = Now + 3.0f;
			}
			else if (gNavState == -1)
			{
				if (gMeshReady)
					++gMeshFallbacks;

				// our own grid: a route, or the news that the goal cannot be walked to
				std::vector<uint8_t> Jumps;
				const int GridResult = GridPath(Obs.Pos, Brain.NavGoal, Points, Brain.TargetKind == EBotTarget::Chest || Brain.TargetKind == EBotTarget::Pickup, &Jumps, Obs.Health + Obs.Shield * 0.3f);

				if (gGridNoStartFlag && Obs.MovementMode == 1 && Now >= Brain.NextSeedAt)
				{
					Brain.NextSeedAt = Now + 6.0f;
					NavAddSeed(FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 88.0f }, 2500.0f);
				}

				if (gNavEnclosed && Obs.MovementMode == 1)
				{
					// walled in by drops on every side (a roof): it will have to jump off, so the drop check relaxes
					Brain.DropRelaxUntil = Now + 40.0f;
					Brain.Paces = (std::max)(Brain.Paces, 2);
				}

				if (GridResult == 1)
				{
					Brain.NavPath = Points;
					Brain.NavJump = Jumps;
					Brain.NavIndex = 1;
					Brain.NavPathTime = Now;
					Brain.NavQueryAt = Now + 5.0f;
				}
				else
				{
					Brain.NavPath.clear();

					if (gNavGridUnreachable && Brain.bHasTarget && Brain.TargetKind != EBotTarget::Storm && (Obs.Phase == EAthenaGamePhase::SafeZones || Obs.Phase == EAthenaGamePhase::EndGame))
					{
						bool bBrokeThrough = false;

						// On a roof with the chest somewhere underneath: there is no walking down, but a player who lands on a roof does not walk down
						// either, they pickaxe through it. The piece under the feet is smashed and the chest stays on the list for when the bot has dropped in.
						if (Brain.TargetKind == EBotTarget::Chest && Brain.Target.Z < Obs.Pos.Z - 150.0f && Dist2D(Obs.Pos, Brain.Target) < 2500.0f && Obs.MovementMode == 1
							&& !Brain.bBreaking && Brain.RoofBreaks < 8 && Now >= Brain.NextRoofBreak)
						{
							UObject* Under = nullptr;
							static auto BuildingClassUnder = ABuildingSMActor::StaticClass();

							if (TraceBlocked(Pawn, HeldWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 40.0f }, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 260.0f }, nullptr, nullptr, &Under)
								&& Under && Under->IsA(BuildingClassUnder) && IsLiveBuilding((AActor*)Under) && ((ABuildingSMActor*)Under)->GetHealth() <= 600.0f)
							{
								Brain.NextRoofBreak = Now + 4.0f;
								++Brain.RoofBreaks;
								ChestOutcome("on a roof above it: breaking through", Brain, Obs.Pos, Now, -1);
								BeginBreak(Brain, Obs, (AActor*)Under, -1, "the roof between it and a chest below");
								Brain.BreakUntil = Now + 25.0f; // a roof takes a good many swings
								bBrokeThrough = true;
							}

							Brain.NextRoofBreak = Now + 4.0f;
						}

						if (!bBrokeThrough && Brain.TargetKind == EBotTarget::Chest)
							ChestOutcome("given up: the map says it cannot be walked to", Brain, Obs.Pos, Now, -1);

						static int Unreachable = 0;

						if (++Unreachable <= 25)
							LOG_INFO(LogBots, "BotNavGrid: the goal at ({:.0f}, {:.0f}) cannot be walked to (kind {}, from ({:.0f}, {:.0f}, {:.0f}), z known {}): {}", Brain.NavGoal.X, Brain.NavGoal.Y, (int)Brain.TargetKind, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, Brain.TargetKind == EBotTarget::Chest || Brain.TargetKind == EBotTarget::Pickup, gNavWhy);

						if (!bBrokeThrough && Brain.TargetKind == EBotTarget::Chest)
							Brain.VisitedChests.push_back(Brain.Target);

						if (!bBrokeThrough && Brain.TargetKind == EBotTarget::Pickup)
							Brain.BadPickups.push_back({ Brain.TargetPickup, Now + 300.0f });

						if (!bBrokeThrough)
						{
							Brain.bHasTarget = false;
							Brain.bNavWanted = false;
						}
					}
				}
			}
			else if (QueryNavPath(Obs.Pos, Brain.NavGoal, Points))
			{
				if (gNavState != 1)
				{
					gNavState = 1;
					LOG_INFO(LogBots, "BotNav: the map has a navmesh, routing with it ({} waypoints on the first path)", (int)Points.size());
				}

				gNavFailures = 0;
				Brain.NavPath = Points;
				Brain.NavIndex = 1;
				Brain.NavPathTime = Now;
			}
			else
			{
				Brain.NavPath.clear();

				if (gNavState == 0 && ++gNavFailures >= 8)
				{
					gNavState = -1;
					LOG_INFO(LogBots, "BotNav: no navmesh answers in this map, steering around obstacles instead");
				}
			}
		}

		Mark(2);

		// obstacle probes towards where the bot is heading: the next waypoint / goal, else the way it was last told to walk
		const auto& Last = Brain.LastAction;
		float Hx = 0.0f, Hy = 0.0f;
		bool bHeading = false;

		if (Last.bDriveXY && Last.Speed > 50.0f)
		{
			if (Brain.bSteerGoal)
			{
				Hx = Brain.SteerGoal.X - Obs.Pos.X;
				Hy = Brain.SteerGoal.Y - Obs.Pos.Y;
				const float HLength = std::sqrt(Hx * Hx + Hy * Hy);

				if (HLength > 1.0f)
				{
					Hx /= HLength;
					Hy /= HLength;
					bHeading = true;
				}
			}

			if (!bHeading)
			{
				Hx = Last.DirX;
				Hy = Last.DirY;
				bHeading = true;
			}
		}

		if (Obs.MovementMode == 1)
			RememberCell(Brain, Obs.Pos, Now);

		Mark(8);

		if (bHeading)
		{
			AActor* IgnoreExtra = HeldWeapon;
			std::string HitName;

			auto Probe = [&](float Dz, float Length, float Angle, std::string* Name, bool bRecordBlocker = false) -> bool
			{
				float Px = Hx, Py = Hy;
				RotateDir(Px, Py, Angle);
				UObject* Actor = nullptr;
				FVector Point{};
				bool bHit = TraceBlocked(Pawn, IgnoreExtra, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + Dz }, FVector{ Obs.Pos.X + Px * Length, Obs.Pos.Y + Py * Length, Obs.Pos.Z + Dz }, Name, nullptr, &Actor, &Point);

				// other players are not walls (they move; the bot walks round them, it does not smash or give up its route)
				static auto PlayerPawnClass = AFortPlayerPawnAthena::StaticClass();

				if (bHit && Actor && Actor->IsA(PlayerPawnClass))
					return false;

				if (bHit && bRecordBlocker && Actor)
				{
					Obs.BlockerActor = Actor;
					Obs.BlockerPoint = Point;
				}

				return bHit;
			};

			Obs.bProbed = true;
			Obs.bLowBlocked = Probe(-40.0f, ProbeLength, 0.0f, nullptr);
			Obs.bHighBlocked = Probe(40.0f, ProbeLength, 0.0f, Brain.WallLogs < 12 ? &HitName : nullptr, true);

			Mark(9);

			// how tall is what is in the way? A railing or a low wall can be jumped; a trace from above its edge down finds the top.
			if (gHopOver && Obs.bLowBlocked && Obs.bHighBlocked && Obs.BlockerActor)
			{
				const float FeetZ = Obs.Pos.Z - 88.0f;
				const float Ax = Obs.BlockerPoint.X + Hx * 8.0f, Ay = Obs.BlockerPoint.Y + Hy * 8.0f; // just inside the face that was hit (further on, a thin wall would let the ray fall to the ground behind it)
				FVector Top{};

				const bool bTopFound = TraceBlocked(Pawn, IgnoreExtra, FVector{ Ax, Ay, FeetZ + 320.0f }, FVector{ Ax, Ay, FeetZ - 40.0f }, nullptr, nullptr, nullptr, &Top);
				const float Height = bTopFound ? Top.Z - FeetZ : 0.0f;

				if (bTopFound && Height >= 50.0f && Height < 170.0f) // the knee ray hit it, so it is at least that tall (a jump clears about 170)
				{
					Obs.bHopOver = true;
					Obs.HopHeight = Height;
				}
				else if (!bTopFound || Height < 50.0f)
				{
					// bars, slats and rails thinner than the probe: the knee ray hit something the top ray falls straight past, so its height can't be measured.
					// Jump and pray: most of them are a railing, and a try or two costs nothing.
					Obs.bHopOver = true;
					Obs.HopHeight = 100.0f;
				}
			}

			if (Obs.bLowBlocked && Obs.bHighBlocked)
			{
				if (Brain.WallLogs < 12)
				{
					++Brain.WallLogs;
					LOG_INFO(LogBots, "BotBrain wall probe hit: {}", HitName);
				}

				// which way round is free? look all the way round (16 rays) and take the roomiest way that isn't ground we just covered
				float Angle = 0.0f, Free = 0.0f;

				if (ScanVision(Brain, Pawn, IgnoreExtra, Obs.Pos, Hx, Hy, Angle, Free))
				{
					Obs.bHasDeflection = true;
					Obs.DeflectionAngle = Angle;
				}
			}
			else if (Now >= Brain.NextLookScan && gTracesFrame <= gTraceBudget)
			{
				// nothing in reach yet: look further ahead and start curving round an obstacle before bumping into it
				Brain.NextLookScan = Now + 0.3f;
				const float LookLength = 800.0f;
				float Fraction = 1.0f;

				if (TraceBlocked(Pawn, IgnoreExtra, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + 10.0f }, FVector{ Obs.Pos.X + Hx * LookLength, Obs.Pos.Y + Hy * LookLength, Obs.Pos.Z + 10.0f }, nullptr, &Fraction))
				{
					const float HitDistance = Fraction * LookLength;
					const float GoalDistance = Brain.bSteerGoal ? Dist2D(Obs.Pos, Brain.SteerGoal) : 100000.0f;

					// a goal that sits right against the thing we see (a chest on a wall) is not an obstacle
					if (HitDistance < 500.0f && GoalDistance > HitDistance + 250.0f)
					{
						float Angle = 0.0f, Free = 0.0f;

						if (ScanVision(Brain, Pawn, IgnoreExtra, Obs.Pos, Hx, Hy, Angle, Free) && std::fabs(Angle) >= 20.0f)
						{
							Obs.bHasDeflection = true;
							Obs.bSoftDeflect = true;
							Obs.DeflectionAngle = Angle;
						}
					}
				}
			}
			Mark(10);

			// a drop ahead: the bot guesses whether the fall would kill it and steers away from the ones it thinks would
			if (bHeading && Obs.MovementMode == 1 && !Obs.bHasDeflection && !(Obs.bLowBlocked && Obs.bHighBlocked) && gTracesFrame <= gTraceBudget)
			{
				AActor* IgnoreExtra = HeldWeapon;

				auto DropAt = [&](float Angle, float& OutX, float& OutY) -> float
				{
					float Px = Hx, Py = Hy;
					RotateDir(Px, Py, Angle);
					OutX = Obs.Pos.X + Px * 320.0f;
					OutY = Obs.Pos.Y + Py * 320.0f;
					float Fraction = 1.0f;
					const bool bHit = TraceBlocked(Pawn, IgnoreExtra, FVector{ OutX, OutY, Obs.Pos.Z }, FVector{ OutX, OutY, Obs.Pos.Z - 6000.0f }, nullptr, &Fraction);
					return (bHit ? Fraction * 6000.0f : 6000.0f) - 90.0f;
				};

				float X = 0.0f, Y = 0.0f;
				const float Drop = DropAt(0.0f, X, Y);

				const float Relax = Obs.Now < Brain.DropRelaxUntil ? 1.15f + 0.05f * (float)(std::min)(Brain.Paces, 4) : 1.0f; // after pacing at an edge for a while it risks the jump

				if (Drop > 600.0f && GuessFallDamage(Brain, Drop, X, Y) >= Obs.Health * 0.95f * Relax)
				{
					if (Obs.Now - Brain.DropAvoidWindow > 12.0f)
					{
						Brain.DropAvoidWindow = Obs.Now;
						Brain.DropAvoids = 0;
					}

					if (++Brain.DropAvoids > 30)
					{
						Brain.DropAvoids = 0;
						Brain.DropRelaxUntil = Obs.Now + 8.0f;
					}

					Obs.bLowBlocked = true;
					Obs.bHighBlocked = true;
					Obs.bProbed = true;

					static const float Angles[] = { 55.0f, -55.0f, 100.0f, -100.0f, 150.0f, -150.0f };

					for (float Angle : Angles)
					{
						float Ax = 0.0f, Ay = 0.0f;
						const float Alternative = DropAt(Angle, Ax, Ay);

						if (Alternative < 600.0f || GuessFallDamage(Brain, Alternative, Ax, Ay) < Obs.Health * 0.95f)
						{
							Obs.bHasDeflection = true;
							Obs.DeflectionAngle = Angle;
							break;
						}
					}

					static int Dropped = 0;

					if (++Dropped <= 15)
						LOG_INFO(LogBots, "BotDrop: sees a {:.0f} unit drop it thinks would hurt too much (guess {:.0f} damage, {:.0f} health), turning away: {}", Drop, GuessFallDamage(Brain, Drop, X, Y), Obs.Health, Obs.bHasDeflection);
				}
			}
		}

		Mark(3);

		// cornered: look all the way round for a direction that is actually open
		if (Brain.bWantEscape)
		{
			Brain.bWantEscape = false;

			struct Candidate { float X, Y; int Score; };
			std::vector<Candidate> Candidates;
			AActor* IgnoreExtra = HeldWeapon;
			const float Offset = RandRange(0.0f, 30.0f);

			for (int k = 0; k < 12; ++k)
			{
				const float Angle = (Offset + k * 30.0f) * DegToRad;
				const float Cx = std::cos(Angle);
				const float Cy = std::sin(Angle);

				auto Open = [&](float Dz, float Length) -> bool
				{
					return !TraceBlocked(Pawn, IgnoreExtra, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + Dz }, FVector{ Obs.Pos.X + Cx * Length, Obs.Pos.Y + Cy * Length, Obs.Pos.Z + Dz });
				};

				// free at knee and head height for a few metres; the second probe prefers directions that stay open further out
				if (Open(-40.0f, 450.0f) && Open(40.0f, 450.0f))
					Candidates.push_back({ Cx, Cy, Open(20.0f, 1100.0f) ? 2 : 1 });
			}

			if (!Candidates.empty())
			{
				int BestScore = 0;

				for (auto& C : Candidates)
					BestScore = (std::max)(BestScore, C.Score);

				std::vector<size_t> Best;

				for (size_t i = 0; i < Candidates.size(); ++i)
				{
					if (Candidates[i].Score == BestScore)
						Best.push_back(i);
				}

				auto& Chosen = Candidates[Best[rand() % Best.size()]];
				Obs.bHasEscape = true;
				Obs.EscapeDir = FVector{ Chosen.X, Chosen.Y, 0.0f };
			}
		}

		Mark(4);

		// The nearest enemies (a few of them), and whether this bot has actually noticed one. Seeing is not instant and not perfect: it takes a moment
		// that grows with distance, it needs a clear line to some part of the target (head, body, feet), it works best straight ahead and hardly at
		// all behind the bot's back, and it goes faster when the target runs or has just fired.
		{
			struct Candidate { const BotPawnInfo* Info; float Distance; };
			std::vector<Candidate> Candidates;

			for (auto& Other : gPawns)
			{
				if (Other.Pawn == Pawn || Other.Team == SelfTeam || (Other.bIsBot && !bBotsFightBots))
					continue;

				const float Dz = Other.Pos.Z - Obs.Pos.Z;

				if (Dz > 2500.0f || Dz < -2500.0f)
					continue;

				const float Distance = Dist2D(Obs.Pos, Other.Pos);

				if (Distance < DisengageRange)
					Candidates.push_back({ &Other, Distance });
			}

			std::sort(Candidates.begin(), Candidates.end(), [](const Candidate& A, const Candidate& B) { return A.Distance < B.Distance; });

			const bool bFighting = Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat;
			const float FacingYaw = Brain.bHaveFaceYaw ? Brain.FaceYaw : std::atan2(Brain.LastAction.DirY, Brain.LastAction.DirX) * RadToDeg;
			const float PerceptDt = (std::min)(0.5f, (std::max)(0.0f, Obs.Now - Brain.PerceptLast));
			Brain.PerceptLast = Obs.Now;
			const Candidate* Chosen = nullptr;
			float ChosenVisible = 0.0f;
			int Looked = 0;

			for (auto& C : Candidates)
			{
				if (Looked >= 3)
					break;

				++Looked;
				const float Dx = C.Info->Pos.X - Obs.Pos.X, Dy = C.Info->Pos.Y - Obs.Pos.Y;
				const float Length = (std::max)(C.Distance, 1.0f);
				const float Reach = C.Distance > 150.0f ? C.Distance - 120.0f : 0.0f;
				const float Ux = Dx / Length, Uy = Dy / Length;

				// how much of the target can be seen: head, body and (close up) feet
				int Seen = 0, Tried = 0;

				if (Reach <= 0.0f)
				{
					Seen = Tried = 1;
				}
				else
				{
					const float Heights[3] = { 75.0f, 5.0f, -65.0f };

					for (int h = 0; h < (C.Distance > 2800.0f ? 2 : 3); ++h)
					{
						++Tried;

						if (!TraceBlocked(Pawn, HeldWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + 40.0f }, FVector{ Obs.Pos.X + Ux * Reach, Obs.Pos.Y + Uy * Reach, C.Info->Pos.Z + Heights[h] }))
							++Seen;
					}
				}

				float Visible = Tried > 0 ? (float)Seen / (float)Tried : 0.0f;

				// is it in front of the bot?
				float Off = std::atan2(Uy, Ux) * RadToDeg - FacingYaw;

				while (Off > 180.0f) Off -= 360.0f;
				while (Off < -180.0f) Off += 360.0f;

				const float AbsOff = std::fabs(Off);
				// Sees well within 70 degrees of where it faces, less out to 150 (a player moves the camera about all the time, so the real field of view is far
				// wider than the screen), and everything for a moment whenever it 'looks around'.
				// Alert (an enemy seen or a hit taken in the last AlertMemory seconds, or in a fight) it watches far wider, like the game's own AI
				// switching from its dormant to its alert sight; it still never sees straight behind as well as in front.
				const bool bAlert = bFighting || Obs.Now - Brain.EnemyLastSeen < AlertMemory || Obs.Now - Brain.HurtAt < AlertMemory;
				float Field = AbsOff <= 70.0f ? 1.0f : (AbsOff <= 150.0f ? 1.0f - 0.8f * (AbsOff - 70.0f) / 80.0f : 0.0f);

				if (bAlert)
					Field = AbsOff <= AlertFullFieldDeg ? 1.0f : 1.0f - 0.7f * (AbsOff - AlertFullFieldDeg) / (180.0f - AlertFullFieldDeg);

				if (C.Distance < 350.0f)
					Field = (std::max)(Field, 0.6f);
				else if (C.Distance < 900.0f)
					Field = (std::max)(Field, 0.35f);

				if (Obs.Now >= Brain.NextScan)
				{
					Brain.ScanUntil = Obs.Now + 0.6f;
					Brain.NextScan = Obs.Now + RandRange(1.5f, 4.0f);
				}

				if (Obs.Now < Brain.ScanUntil)
					Field = (std::max)(Field, 0.9f);

				if (bFighting)
					Field = 1.0f; // already looking at the fight

				// footsteps: a player turns round to somebody who can be heard coming
				if (gFootsteps && C.Info->Speed > 150.0f && C.Distance < FootstepRange(C.Info->Speed) * 0.5f)
					Field = (std::max)(Field, 0.85f);

				float Boost = 1.0f;

				if (C.Info->Speed > 450.0f) Boost *= 1.5f;
				if (C.Info->Speed > 700.0f) Boost *= 1.2f;
				if (Obs.Now - C.Info->LastFired < 1.5f && C.Distance < 3500.0f) Boost *= 3.0f;

				// each enemy is noticed on its own; somebody new starts from nothing (unless something is shooting at the bot), and an enemy
				// not seen for 10 s is forgotten
				std::erase_if(Brain.AwareOf, [&](const BotBrainState::FAwareOf& A) { return Obs.Now - A.Updated > 10.0f; });

				auto Found = std::find_if(Brain.AwareOf.begin(), Brain.AwareOf.end(), [&](const BotBrainState::FAwareOf& A) { return A.Pawn == (void*)C.Info->Pawn; });

				if (Found == Brain.AwareOf.end())
				{
					Brain.AwareOf.push_back({ (void*)C.Info->Pawn, Obs.Now - Brain.HurtAt < 3.0f ? 1.2f : 0.0f, Obs.Now });
					Found = Brain.AwareOf.end() - 1;
				}

				float& Awareness = Found->Value;
				Found->Updated = Obs.Now;

				// someone it had noticed and saw a moment ago, still close to where it saw them, stays 'in sight' (the engine's
				// AutoSuccessRangeFromLastSeenLocation): crouching behind a crate no longer wipes them from its mind
				if (Visible > 0.0f)
				{
					Found->LastSeenPos = C.Info->Pos;
					Found->LastSeenAt = Obs.Now;
				}
				else if (Awareness >= 1.0f && Obs.Now - Found->LastSeenAt < AutoSuccessTime && Dist2D(C.Info->Pos, Found->LastSeenPos) < AutoSuccessRange)
				{
					Visible = 0.5f;
				}

				const float NoticeTime = (0.2f + C.Distance / 2400.0f) * (bAlert ? AlertNoticeScale : 1.0f);
				const float Rate = Visible * Field * Boost / NoticeTime * (Brain.Alertness > 0.0f ? Brain.Alertness : 1.0f);

				if (Rate > 0.0f)
					Awareness = (std::min)(1.5f, Awareness + Rate * PerceptDt);
				else
					Awareness = (std::max)(0.0f, Awareness - 0.3f * PerceptDt);

				// hurt by somebody it can see nothing of: it knows where from
				if (Obs.Now - Brain.HurtAt < 3.0f && C.Distance < 4500.0f)
					Awareness = 1.5f;

				const bool bNoticed = Awareness >= 1.0f && Visible > 0.0f;

				if (bNoticed)
				{
					// the nearest one it has noticed is the one it deals with, except that a real player it has noticed comes first unless the
					// other enemy is much closer: a bot brawling with another bot right next to a player otherwise never turned to the player
					if (ChosenVisible <= 0.0f)
					{
						Chosen = &C;
						ChosenVisible = 1.0f;

						if (!C.Info->bIsBot)
							break;

						continue; // look on for a real player among the others
					}

					if (!C.Info->bIsBot && Chosen->Distance > C.Distance * 0.5f)
					{
						Chosen = &C;
						break;
					}

					continue;
				}

				if (!Chosen)
				{
					Chosen = &C;
					ChosenVisible = 0.0f;
				}
			}

			if (Chosen)
			{
				const auto& Other = *Chosen->Info;
				Obs.bHasEnemy = true;
				Obs.EnemyPos = Other.Pos;
				Obs.EnemyDist = Chosen->Distance;
				Obs.EnemyPawn = Other.Pawn;
				Obs.bEnemyIsHuman = !Other.bIsBot;
				Obs.EnemyHealth = Other.Health;
				Obs.EnemyShield = Other.Shield;
				Obs.EnemyWeaponScore = Other.WeaponScore;
				Obs.bEnemyVisible = ChosenVisible > 0.0f;

				// pointing at this bot: within about a body width of it (wider up close), at a distance a shot could matter
				if (Other.bAimKnown && Chosen->Distance < 3500.0f)
				{
					float Bearing = std::atan2(Obs.Pos.Y - Other.Pos.Y, Obs.Pos.X - Other.Pos.X) * RadToDeg;
					float Off = std::fmod(std::fabs(Bearing - Other.AimYaw), 360.0f);
					Off = Off > 180.0f ? 360.0f - Off : Off;
					const float Allowed = std::atan2(60.0f, (std::max)(Chosen->Distance, 1.0f)) * RadToDeg + 4.0f;
					Obs.bEnemyAimingAtMe = Off < Allowed;
				}
			}
		}

		// something flying at the bot (a rocket): when would it arrive, and from which way
		for (auto& P : gProjectiles)
		{
			const float Speed = std::sqrt(P.Vel.X * P.Vel.X + P.Vel.Y * P.Vel.Y + P.Vel.Z * P.Vel.Z);

			if (Speed < 700.0f || Now - P.Time > 0.6f)
				continue;

			const float Ux = P.Vel.X / Speed, Uy = P.Vel.Y / Speed, Uz = P.Vel.Z / Speed;
			const float Rx = Obs.Pos.X - P.Pos.X, Ry = Obs.Pos.Y - P.Pos.Y, Rz = Obs.Pos.Z - P.Pos.Z;
			const float Closing = Rx * Ux + Ry * Uy + Rz * Uz;

			if (Closing <= 0.0f)
				continue;

			const float Time = Closing / Speed;
			const float Mx = Rx - Ux * Closing, My = Ry - Uy * Closing, Mz = Rz - Uz * Closing;
			const float Miss = std::sqrt(Mx * Mx + My * My + Mz * Mz);

			if (Time < 2.5f && Miss < 700.0f && Time < Obs.ProjTime)
			{
				Obs.bProjectile = true;
				Obs.ProjTime = Time;
				Obs.ProjDirX = Ux;
				Obs.ProjDirY = Uy;
			}
		}

		Mark(5);

		// the closest recent noise that came from somebody else
		{
			float BestDistance2 = 1e30f;

			for (auto& Sound : gSounds)
			{
				if (Obs.Now - Sound.Time > 8.0f || Sound.Source == Pawn || (Sound.Team != -1 && Sound.Team == SelfTeam))
					continue;

				const float Distance = Dist2D(Obs.Pos, Sound.Pos);

				if (Distance > Sound.Radius || Distance < 400.0f || Distance >= BestDistance2)
					continue;

				BestDistance2 = Distance;
				Obs.bHeardSound = true;
				Obs.SoundPos = Sound.Pos;
				Obs.SoundTime = Sound.Time;
			}
		}

		// footsteps of somebody else on the move, within the range the game gives them
		if (gFootsteps && Obs.Now >= Brain.NextFootHeard)
		{
			float Nearest = Obs.bHeardSound ? Dist2D(Obs.Pos, Obs.SoundPos) : 1e30f;

			for (auto& Other : gPawns)
			{
				if (Other.Pawn == Pawn || Other.Team == SelfTeam || (Other.bIsBot && !bBotsFightBots) || Other.Speed < 150.0f)
					continue;

				const float Distance = Dist2D(Obs.Pos, Other.Pos);

				if (Distance < 400.0f || Distance >= Nearest || Distance > FootstepRange(Other.Speed) || std::fabs(Other.Pos.Z - Obs.Pos.Z) > 1500.0f)
					continue;

				Nearest = Distance;
				Obs.bHeardSound = true;
				Obs.SoundPos = Other.Pos;
				Obs.SoundTime = Obs.Now;
			}

			if (Nearest < 1e29f && Obs.SoundTime == Obs.Now)
				Brain.NextFootHeard = Obs.Now + 8.0f;
		}

		Mark(6);

		// loot on the ground worth walking to: a gun that beats what we have, or a healing item while there is room for it
		{
			float BestValue = 0.0f;

			for (auto& Pickup : gPickups)
			{
				bool bBad = false;

				for (auto& Bad : Brain.BadPickups)
				{
					if (Bad.first == Pickup.Pickup && Obs.Now < Bad.second)
					{
						bBad = true;
						break;
					}
				}

				if (bBad)
					continue;

				if (Pickup.Kind == EBotItemKind::Gun)
				{
					if (Pickup.Score <= Obs.BestGunScore)
						continue;
				}
				else if (Obs.FreeSlots <= 0 || Obs.HealCount >= 6)
				{
					continue;
				}

				const float Dz = Pickup.Pos.Z - Obs.Pos.Z;

				if (Dz > 600.0f || Dz < -600.0f)
					continue;

				const float Distance = Dist2D(Obs.Pos, Pickup.Pos);

				if (Distance > PickupSearchRadius)
					continue;

				const float Value = (float)Pickup.Score / (1.0f + Distance / 1000.0f);

				if (Value > BestValue)
				{
					// only loot in view counts (answers are kept for a moment)
					bool bSeen = false;
					bool bKnown = false;

					for (auto It = Brain.PickupSight.begin(); It != Brain.PickupSight.end();)
					{
						if (Obs.Now >= It->Until) { It = Brain.PickupSight.erase(It); continue; }
						if (It->Item == (void*)Pickup.Pickup) { bSeen = It->bSeen; bKnown = true; }
						++It;
					}

					if (!bKnown)
					{
						bSeen = HasSight(Pawn, Obs.Pos, Pickup.Pickup, Pickup.Pos);
						Brain.PickupSight.push_back({ (void*)Pickup.Pickup, Obs.Now + 1.5f, bSeen });
					}

					// (Tried 2026-11-11: letting under-equipped bots go for loot they cannot see, to make them search houses. A bots-only match then had
					// about half the weapon pick-ups of the one before, so it is out again; searching buildings needs real room-by-room goals.)
					if (!bSeen)
						continue;

					BestValue = Value;
					Obs.bHasPickup = true;
					Obs.PickupPos = Pickup.Pos;
					Obs.PickupPtr = Pickup.Pickup;
				}
			}
		}

		Mark(7);

		return Obs;
	}

	// ---- decide: pure logic ----
	inline void SetState(BotBrainState& Brain, EBotBrainState NewState, float Now, int Id)
	{
		if (Brain.State == NewState)
			return;

		LOG_INFO(LogBots, "BotBrain #{} {} -> {}", Id, BotBrainStateName(Brain.State), BotBrainStateName(NewState));
		Brain.State = NewState;
		Brain.StateSince = Now;

		if (NewState == EBotBrainState::Ground && Brain.FirstGroundTime <= 0.0f)
			Brain.FirstGroundTime = Now;
		Brain.bHasTarget = false;
		Brain.TargetKind = EBotTarget::None;
		Brain.BlockedAccum = 0.0f;
		Brain.BlockedEvents = 0;
		Brain.HealStart = 0.0f;
	}

	inline bool IsVisited(const BotBrainState& Brain, const FVector& Chest)
	{
		for (auto& Visited : Brain.VisitedChests)
		{
			if (Dist2D(Visited, Chest) < 300.0f)
				return true;
		}

		return false;
	}

	// The circle a bot is working towards: the next one once it is time to rotate (same rule DecideWalking uses for the storm goal), else the current one.
	// False when there is no usable circle (before it shows, or 50v50 where there is none).
	inline bool ZoneGoalOf(const BotBrainState& Brain, const BotObservation& Obs, FVector& OutCenter, float& OutRadius)
	{
		if (!Obs.bZoneKnown || Obs.ZoneRadius < 1500.0f)
			return false;

		OutCenter = Obs.ZoneCenter;
		OutRadius = Obs.ZoneRadius;

		if (Obs.bNextZoneKnown && Obs.Now >= Brain.RotateAt)
		{
			OutCenter = Obs.NextZoneCenter;
			OutRadius = Obs.NextZoneRadius;
		}

		return true;
	}

	inline void MarkChestLooted(ABuildingContainer* Chest)
	{
		for (size_t i = 0; i < gChestActors.size() && i < gChestLooted.size(); ++i)
		{
			if (gChestActors[i] != Chest)
				continue;

			if (!gChestLooted[i])
			{
				gChestLooted[i] = 1;

				if (i < gChestPoi.size() && gChestPoi[i] >= 0 && gChestPoi[i] < (int)gPoiRemaining.size() && gPoiRemaining[gChestPoi[i]] > 0)
					--gPoiRemaining[gChestPoi[i]];
			}

			return;
		}
	}

	// A bot gave up on a chest (idle watchdog, pacing, timed out): remembered for everybody.
	inline void NoteChestFail(const FVector& Chest)
	{
		++gChestFails[ChestKey(Chest)];
	}

	// The nearest chest worth walking to: unopened (by anybody), inside the circle the bot is heading for, not one that already stalled several bots.
	inline int FindNearestChest(const BotBrainState& Brain, const BotObservation& Obs, const FVector& From, float MaxDistance)
	{
		float Best = MaxDistance;
		int Found = -1;

		FVector ZoneC{};
		float ZoneR = 0.0f;
		const bool bZone = ZoneGoalOf(Brain, Obs, ZoneC, ZoneR);
		const float ZoneLimit = ZoneR - (std::min)(800.0f, ZoneR * 0.1f);

		for (size_t i = 0; i < gChests.size(); ++i)
		{
			// a chest on another level costs the way round (stairs, a path down the cliff), so it counts as further away
			const float Height = std::fabs(gChests[i].Z - From.Z);
			const float Distance = Dist2D(From, gChests[i]) + (Height > 250.0f ? (Height - 250.0f) * 4.0f : 0.0f);

			if (Distance >= Best)
				continue;

			if (i < gChestLooted.size() && gChestLooted[i])
				continue;

			if (bZone && Dist2D(gChests[i], ZoneC) > ZoneLimit)
				continue;

			if (IsVisited(Brain, gChests[i]))
				continue;

			if (!gChestFails.empty())
			{
				auto Failed = gChestFails.find(ChestKey(gChests[i]));

				if (Failed != gChestFails.end() && Failed->second >= 3)
					continue;
			}

			// the scan is up to 20 s old: ask the chest itself before sending a bot to it
			if (i < gChestActors.size())
			{
				auto Chest = gChestActors[i];

				if (!IsLiveBuilding(Chest) || Chest->IsAlreadySearched())
				{
					if (i < gChestLooted.size())
						gChestLooted[i] = 1;

					continue;
				}
			}

			Best = Distance;
			Found = (int)i;
		}

		return Found;
	}

	// Where to travel when nothing is left to loot here. Chosen by weight, not at random: places with many unopened chests left, close, and inside the circle
	// the bot is heading for count most, so bots no longer walk to a town the other bots emptied or one the storm already covers. It stays a weighted
	// draw (not "the best one") so bots do not all pile into the same town. -1 if nothing is worth the trip.
	inline int PickPoi(const BotBrainState& Brain, const BotObservation& Obs, const FVector& From)
	{
		FVector ZoneC{};
		float ZoneR = 0.0f;
		const bool bZone = ZoneGoalOf(Brain, Obs, ZoneC, ZoneR);

		for (int MinLeft = 2; MinLeft >= 1; --MinLeft)
		{
			std::vector<std::pair<int, double>> Candidates;
			double Total = 0.0;

			for (size_t i = 0; i < gPois.size(); ++i)
			{
				const int Left = i < gPoiRemaining.size() ? gPoiRemaining[i] : gPois[i].Count;

				if (Left < MinLeft)
					continue;

				if (std::find(Brain.RecentPois.begin(), Brain.RecentPois.end(), (int)i) != Brain.RecentPois.end())
					continue;

				const float Distance = Dist2D(From, gPois[i].Center);

				if (Distance < PoiMinDistance || Distance > PoiMaxDistance)
					continue;

				double Fit = 1.0;

				if (bZone)
				{
					const double Outside = (double)Dist2D(gPois[i].Center, ZoneC) - ((double)ZoneR - 600.0);

					if (Outside > 0.0)
						Fit = (std::max)(0.02, std::exp(-Outside / 1500.0));
				}

				const double Value = std::pow((double)(std::min)(Left, 14), 0.8);
				const double Weight = Fit * Value / std::pow((double)Distance + 2500.0, 1.2);

				Candidates.push_back({ (int)i, Weight });
				Total += Weight;
			}

			if (Candidates.empty())
				continue;

			double Roll = (double)RandRange(0.0f, 1.0f) * Total;

			for (auto& [Index, Weight] : Candidates)
			{
				Roll -= Weight;

				if (Roll <= 0.0)
					return Index;
			}

			return Candidates.back().first;
		}

		return -1;
	}

	inline FVector PickWanderPoint(const FVector& Around, float Radius)
	{
		const float Angle = RandRange(0.0f, 6.2831853f);
		const float Distance = RandRange(Radius * 0.3f, Radius);
		return FVector{ Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, Around.Z };
	}

	inline void SteerDir(BotAction& Action, float DirX, float DirY, float Speed)
	{
		Action.bDriveXY = true;
		Action.DirX = DirX;
		Action.DirY = DirY;
		Action.Speed = Speed;
		Action.bFace = true;
		Action.Yaw = std::atan2(DirY, DirX) * RadToDeg;
	}

	// Walks towards Goal and reacts to obstacles: low-only block = a step (jump), low+high = a wall (go round it),
	// and a progress check (did we actually move along our heading?) as the backstop for everything the probes miss.
	inline void MoveToward(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, const FVector& FinalGoal, float Speed, int Id)
	{
		// long trips follow the navmesh route when the map has one
		FVector Goal = FinalGoal;

		if ((gNavState != -1 || !gNavTiles.empty()) && Dist2D(Obs.Pos, FinalGoal) > 350.0f)
		{
			if (!Brain.bNavWanted || Dist2D(Brain.NavGoal, FinalGoal) > 200.0f)
			{
				Brain.bNavWanted = true;
				Brain.NavGoal = FinalGoal;
				Brain.NavPath.clear();
				Brain.NavQueryAt = 0.0f;
			}

			while (Brain.NavIndex < Brain.NavPath.size() && Dist2D(Obs.Pos, Brain.NavPath[Brain.NavIndex]) < 180.0f)
				++Brain.NavIndex;

			if (Brain.NavIndex < Brain.NavPath.size())
			{
				Goal = Brain.NavPath[Brain.NavIndex];

				// the next stretch is a hop over something low
				if (Brain.NavIndex < Brain.NavJump.size() && Brain.NavJump[Brain.NavIndex] && Dist2D(Obs.Pos, Goal) < 190.0f && Obs.MovementMode == 1 && Obs.Now >= Brain.NextJumpAllowed)
				{
					Action.bJump = true;
					Brain.NextJumpAllowed = Obs.Now + 0.7f;
				}
			}
			else if (!Brain.NavPath.empty())
			{
				Brain.NavQueryAt = 0.0f; // the route is used up: ask for the next stage
			}
		}
		else
		{
			Brain.bNavWanted = false;
		}

		Brain.SteerGoal = Goal;
		Brain.bSteerGoal = true;

		float Dx = Goal.X - Obs.Pos.X;
		float Dy = Goal.Y - Obs.Pos.Y;
		const float Length = std::sqrt(Dx * Dx + Dy * Dy);

		if (Length < 1.0f)
			return;

		Dx /= Length;
		Dy /= Length;

		// progress since the last decision along what we were told to do
		{
			const float Dt = Obs.Now - Brain.LastDecisionTime;
			const auto& Last = Brain.LastAction;

			if (Brain.LastDecisionTime > 0.0f && Dt > 0.05f && Dt < 1.0f && Last.bDriveXY && Last.Speed > 100.0f)
			{
				const float Moved = (Obs.Pos.X - Brain.LastDecisionPos.X) * Last.DirX + (Obs.Pos.Y - Brain.LastDecisionPos.Y) * Last.DirY;
				const float Expected = Last.Speed * Dt;

				if (Moved < Expected * 0.3f)
					Brain.BlockedAccum += Dt;
				else
					Brain.BlockedAccum = Brain.BlockedAccum > Dt * 2.0f ? Brain.BlockedAccum - Dt * 2.0f : 0.0f;
			}
		}

		// Following a route from the map, the route knows better than the probes: stairs, ramps and doorways look like walls to a ray straight ahead.
		// Only not getting anywhere (below) makes the bot doubt the route.
		const bool bRouted = !Brain.NavPath.empty() && Brain.NavIndex < Brain.NavPath.size() && Brain.NavPathTime > 0.0f && Obs.Now - Brain.NavPathTime < 12.0f;

		// an obstacle seen well ahead: start curving round it now, without counting it as being blocked
		if (!bRouted && Obs.bSoftDeflect && Obs.bHasDeflection && Brain.EscapeUntil <= Obs.Now)
		{
			Brain.AvoidAngle = Obs.DeflectionAngle;
			Brain.AvoidUntil = Obs.Now + 0.7f;
		}

		bool bBlockedEvent = false;
		const char* Why = "";

		if (bRouted && Obs.bProbed && Obs.bLowBlocked && Obs.bHighBlocked)
		{
			// trust the route; the progress check below catches a real obstacle
			if (Brain.BlockedAccum > 0.6f)
			{
				bBlockedEvent = true;
				Why = "no progress on a route";
				Brain.NavQueryAt = 0.0f;
			}
		}
		else if (Obs.bProbed && Obs.bLowBlocked && Obs.bHighBlocked && Obs.bHopOver && !Brain.bBreaking && Obs.BlockerActor && Brain.BreakCount < 60 && IsBreakableBlocker(Brain, Obs.BlockerActor)
			&& Dist2D(Obs.Pos, Obs.BlockerPoint) < 260.0f)
		{
			// a railing or fence that can be broken: break it rather than hoping the jump clears it
			StartBreak(Brain, Obs, Id, "low and breakable");
		}
		else if (Obs.bProbed && Obs.bLowBlocked && Obs.bHighBlocked && Obs.bHopOver && (Obs.Now < Brain.HopUntil || Brain.HopTries < 5 || Brain.HopBlocker != (void*)Obs.BlockerActor))
		{
			// a railing or low wall: jump over it, straight on (not round it)
			if (Brain.HopBlocker != (void*)Obs.BlockerActor)
			{
				Brain.HopBlocker = (void*)Obs.BlockerActor;
				Brain.HopTries = 0;
			}

			if (Obs.Now >= Brain.HopUntil && Dist2D(Obs.Pos, Obs.BlockerPoint) < 170.0f && Obs.MovementMode == 1 && Obs.Now >= Brain.NextJumpAllowed)
			{
				Action.bJump = true;
				Brain.NextJumpAllowed = Obs.Now + 0.9f;
				Brain.HopUntil = Obs.Now + 0.8f;
				++Brain.HopTries;

				static int Hopped = 0;

				if (++Hopped <= 25)
					LOG_INFO(LogBots, "BotHop #{} jumps over {} ({:.0f} high, try {})", Id, Obs.BlockerActor->GetName(), Obs.HopHeight, Brain.HopTries);
			}
		}
		else if (Obs.bProbed && Obs.bLowBlocked && Obs.bHighBlocked)
		{
			if (Obs.bHasDeflection)
			{
				// walk round it, and keep going that way for a moment so the bot follows the wall instead of dithering
				Brain.AvoidAngle = Obs.DeflectionAngle;
				Brain.AvoidUntil = Obs.Now + 0.9f;

				if (Obs.Now - Brain.AvoidWindowStart > 6.0f)
				{
					Brain.AvoidWindowStart = Obs.Now;
					Brain.AvoidCount = 0;
				}

				// still walking into walls after many detours: this route is not working
				if (++Brain.AvoidCount > 24)
				{
					bBlockedEvent = true;
					Why = "dead end";
					Brain.AvoidCount = 0;
				}
			}
			else
			{
				bBlockedEvent = true;
				Why = "wall";
			}
		}
		else if (Obs.bProbed && Obs.bLowBlocked && !Obs.bHighBlocked)
		{
			if (Obs.Now >= Brain.NextJumpAllowed)
			{
				Action.bJump = true;
				Brain.NextJumpAllowed = Obs.Now + 0.6f;
			}
		}
		else if (Brain.BlockedAccum > 0.6f)
		{
			bBlockedEvent = true;
			Why = "no progress";
		}

		// Not getting anywhere and no ray shows why: feel for it with the body (see PawnSweep). What it finds is then treated like any other blocker
		// below: smashed if it can be (a railing, a fence), jumped at otherwise.
		if (bBlockedEvent && !Obs.BlockerActor && Obs.SelfPawn && Obs.MovementMode == 1)
		{
			const auto& Last = Brain.LastAction;
			const float Fx = Last.bDriveXY ? Last.DirX : Dx, Fy = Last.bDriveXY ? Last.DirY : Dy;
			UObject* Unseen = nullptr;
			FVector At{};

			// feet 55 above the ground, so a stair or a kerb is not a blocker; 130 ahead
			if (PawnSweep(Obs.SelfPawn, Obs.SelfWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + 27.0f }, FVector{ Obs.Pos.X + Fx * 130.0f, Obs.Pos.Y + Fy * 130.0f, Obs.Pos.Z + 27.0f }, 30.0f, 60.0f, &Unseen, &At) && Unseen)
			{
				if (Dist2D(At, Obs.Pos) > 400.0f) // pressed right against it the sweep starts inside it and has no impact point
					At = FVector{ Obs.Pos.X + Fx * 60.0f, Obs.Pos.Y + Fy * 60.0f, Obs.Pos.Z };

				auto& Seen = const_cast<BotObservation&>(Obs); // the observation is this decision's own copy: fill in what the rays missed
				Seen.BlockerActor = Unseen;
				Seen.BlockerPoint = At;

				const bool bBreakable = IsBreakableBlocker(Brain, Unseen);
				static int Logged = 0;

				if (++Logged <= 60)
					LOG_INFO(LogBots, "BotUnseen #{} is held up by {} at ({:.0f}, {:.0f}, {:.0f}), which no ray saw (breakable={})", Id, Unseen->GetName(), Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, bBreakable);

				if (!bBreakable && Obs.Now >= Brain.NextJumpAllowed)
				{
					Action.bJump = true;
					Brain.NextJumpAllowed = Obs.Now + 0.9f;
				}
			}
		}

		// walking up to a chest or a pickup, the thing itself is in the way: that is arriving, not being stuck
		if (bBlockedEvent && (Brain.TargetKind == EBotTarget::Chest || Brain.TargetKind == EBotTarget::Pickup) && Dist2D(Obs.Pos, FinalGoal) < 420.0f)
			bBlockedEvent = false;

		if (bBlockedEvent && Obs.Now - Brain.LastBlockedEventTime > 0.4f)
		{
			Brain.LastBlockedEventTime = Obs.Now;
			Brain.BlockedAccum = 0.0f;
			++Brain.BlockedEvents;
			if (Obs.Phase == EAthenaGamePhase::SafeZones || Obs.Phase == EAthenaGamePhase::EndGame)
				NavAddSeed(FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 88.0f }, 2500.0f); // learn this neighbourhood

			// the same obstacle again and again (a railing, a fence, a thin wall): smash it with the pickaxe
			if (Brain.BlockedEvents >= 1 && !Brain.bBreaking && Obs.BlockerActor && Brain.BreakCount < 60)
			{
				const bool bPlayerBuilt = Obs.BlockerActor->GetName().rfind("PBWA_", 0) == 0;

				if (IsBreakableBlocker(Brain, Obs.BlockerActor) && (bPlayerBuilt || Obs.Now >= Brain.BreakUntil + 3.0f))
					StartBreak(Brain, Obs, Id, "stuck");
			}

			// boxed in again after a sidestep: look for a real way out instead of trying the same corner a third time
			if (Brain.BlockedEvents >= 2 && Obs.Now - Brain.LastEscapeAt > 2.0f)
				Brain.bWantEscape = true;

			if (Obs.Now >= Brain.NextBlockLog)
			{
				Brain.NextBlockLog = Obs.Now + 3.0f;
				float NearPawn = 1e9f;

				for (auto& Other : gPawns)
				{
					if ((void*)Other.Pawn != (void*)Obs.SelfPawn)
						NearPawn = (std::min)(NearPawn, Dist2D(Obs.Pos, Other.Pos));
				}

				LOG_INFO(LogBots, "BotBrain #{} blocked ({}) nearpawn={:.0f} low={} high={} deflection={} events={} at ({:.0f}, {:.0f}, {:.0f}) route={} kind={} grid={} goal={:.0f}u zgoal={:.0f} blocker={}", Id, Why, NearPawn, Obs.bLowBlocked, Obs.bHighBlocked, Obs.bHasDeflection, Brain.BlockedEvents, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, (int)Brain.NavPath.size(), (int)Brain.TargetKind, NavCovered(Obs.Pos.X, Obs.Pos.Y), Dist2D(Obs.Pos, FinalGoal), FinalGoal.Z - Obs.Pos.Z, Obs.BlockerActor ? Obs.BlockerActor->GetName() : std::string("-"));

				if (Obs.BlockerActor)
				{
					static auto BuildingClass2 = ABuildingSMActor::StaticClass();
					const bool bB = Obs.BlockerActor->IsA(BuildingClass2);
					LOG_INFO(LogBots, "BotBlocker #{} {} building={} health={:.0f}/{:.0f} breakable={} hopOver={} hopHeight={:.0f}", Id, Obs.BlockerActor->GetName(), bB, bB ? ((ABuildingSMActor*)Obs.BlockerActor)->GetHealth() : 0.0f, bB ? ((ABuildingSMActor*)Obs.BlockerActor)->GetMaxHealth() : 0.0f, IsBreakableBlocker(Brain, Obs.BlockerActor), Obs.bHopOver, Obs.HopHeight);
				}
			}

			if (Brain.BlockedEvents % 4 == 3)
			{
				// give up on this goal; a chest that can't be reached is crossed off so it isn't picked again
				if (Brain.TargetKind == EBotTarget::Chest)
					ChestOutcome("given up: blocked three times on the way", Brain, Obs.Pos, Obs.Now, Id);

				if (Brain.TargetKind == EBotTarget::Chest)
					Brain.VisitedChests.push_back(Brain.Target);
					if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });

				Brain.bHasTarget = false;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;
			}
			else
			{
				float Sx = Dx, Sy = Dy;
				RotateDir(Sx, Sy, (rand() & 1) ? 100.0f : -100.0f);
				Brain.SidestepDir = FVector{ Sx, Sy, 0.0f };
				Brain.SidestepUntil = Obs.Now + 0.8f;

				if (Brain.BlockedEvents % 2 == 1 && Obs.Now >= Brain.NextJumpAllowed)
				{
					Action.bJump = true;
					Brain.NextJumpAllowed = Obs.Now + 0.6f;
				}
			}
		}
		else if (!bBlockedEvent && Brain.BlockedAccum == 0.0f && Obs.Now - Brain.LastBlockedEventTime > 4.0f)
		{
			Brain.BlockedEvents = 0;
		}

		if (Obs.bHasEscape)
		{
			Brain.EscapeDir = Obs.EscapeDir;
			Brain.EscapeUntil = Obs.Now + 1.6f;
			Brain.bEscapePlanned = false;
			Brain.NextEscapeJump = 0.0f;
			Brain.LastEscapeAt = Obs.Now;
			Brain.SidestepUntil = 0.0f;
			Brain.AvoidUntil = 0.0f;

			if (Obs.Now - Brain.EscapeWindowStart > 25.0f)
			{
				Brain.EscapeWindowStart = Obs.Now;
				Brain.EscapeCount = 0;
			}

			if (++Brain.EscapeCount >= 4)
			{
				// this goal keeps trapping the bot: drop it for good
				LOG_INFO(LogBots, "BotBrain #{} keeps getting cornered on the way to its goal, dropping it", Id);

				if (Brain.TargetKind == EBotTarget::Chest)
					ChestOutcome("given up: cornered on the way", Brain, Obs.Pos, Obs.Now, Id);

				if (Brain.TargetKind == EBotTarget::Chest)
					Brain.VisitedChests.push_back(Brain.Target);
					if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });

				if (Brain.TargetKind == EBotTarget::Storm)
				{
					Brain.StormDropAt = Obs.Now;
					++Brain.StormTries;
				}

				Brain.bHasTarget = false;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;
				Brain.EscapeCount = 0;
			}
		}

		if (Brain.EscapeUntil > Obs.Now)
		{
			SteerDir(Action, Brain.EscapeDir.X, Brain.EscapeDir.Y, Speed);

			if (Brain.bEscapePlanned)
			{
				// jump where the plan says there is a ledge or a low wall
				const float Travelled = (Obs.Pos.X - Brain.EscapeStart.X) * Brain.EscapeDir.X + (Obs.Pos.Y - Brain.EscapeStart.Y) * Brain.EscapeDir.Y;

				// A jump from a standstill goes straight up and comes straight down. The bot needs a run-up, so one that is at the foot of the obstacle steps back first.
				for (size_t h = 0; h < Brain.EscapeHops.size() && h < Brain.EscapeRises.size(); ++h)
				{
					const float ToFace0 = Brain.EscapeHops[h] - Travelled;

					if (ToFace0 < -150.0f)
						continue;

					const float Need0 = (std::min)(Brain.EscapeRises[h] + 30.0f, 172.0f);
					const float Lead0 = (600.0f - std::sqrt((std::max)(600.0f * 600.0f - 4.0f * 492.0f * Need0, 0.0f))) / (2.0f * 492.0f) * 480.0f;
					const float RunUp = Lead0 + 120.0f;

					if (!Brain.bEscapeBackoff && ToFace0 > -20.0f && ToFace0 < 60.0f && Brain.EscapeBackoffs < 3 && Brain.EscapeJumps >= 0)
					{
						Brain.bEscapeBackoff = true;
						++Brain.EscapeBackoffs;
						Brain.EscapeUntil = (std::max)(Brain.EscapeUntil, Obs.Now + RunUp / 380.0f + 2.5f);
					}
					else if (Brain.bEscapeBackoff && ToFace0 >= RunUp)
					{
						Brain.bEscapeBackoff = false;
					}

					break; // only the nearest obstacle ahead matters
				}

				if (Brain.bEscapeBackoff)
					SteerDir(Action, -Brain.EscapeDir.X, -Brain.EscapeDir.Y, Speed * 0.8f);

				for (size_t h = 0; h < Brain.EscapeHops.size() && h < Brain.EscapeRises.size(); ++h)
				{
					if (Brain.bEscapeBackoff)
						break;

					// a jump rises 600t - 492t^2 (apex ~183): leave the ground early enough that the feet are above the ledge when the face is reached
					const float Need = (std::min)(Brain.EscapeRises[h] + 30.0f, 172.0f);
					const float Disc = 600.0f * 600.0f - 4.0f * 492.0f * Need;
					const float T1 = (600.0f - std::sqrt((std::max)(Disc, 0.0f))) / (2.0f * 492.0f);
					const float Lead = T1 * 480.0f; // the distance covered while rising to that height
					const float ToFace = Brain.EscapeHops[h] - Travelled;

					if (Obs.Now >= Brain.NextEscapeJump && ToFace > -150.0f && ToFace < Lead + 110.0f) // too close already: jump now and again until it is over
					{
						Action.bJump = true;
						++Brain.EscapeJumps;
						Brain.NextEscapeJump = Obs.Now + 0.7f;
						break;
					}
				}
			}
			else if (Obs.Now >= Brain.NextEscapeJump)
			{
				Action.bJump = true; // hop over whatever low thing is in the way while running for the gap
				Brain.NextEscapeJump = Obs.Now + 0.55f;
			}
		}
		else if (Brain.SidestepUntil > Obs.Now)
		{
			SteerDir(Action, Brain.SidestepDir.X, Brain.SidestepDir.Y, Speed);
		}
		else if (Brain.AvoidUntil > Obs.Now)
		{
			float Ax = Dx, Ay = Dy;
			RotateDir(Ax, Ay, Brain.AvoidAngle);
			SteerDir(Action, Ax, Ay, Speed);
		}
		else
		{
			SteerDir(Action, Dx, Dy, Speed);
		}
	}

	// Put the best gun in hand (rate limited; equipping is done in Apply).
	inline void MaybeEquip(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action)
	{
		if (Obs.bHasGun && !Obs.bHoldingBestGun && Obs.Now >= Brain.NextEquipTime)
		{
			Brain.NextEquipTime = Obs.Now + 3.0f;
			Action.bEquip = true;
			Action.EquipDef = Obs.BestGunDef;
			Action.EquipGuid = Obs.BestGunGuid;
		}
	}

	inline bool ShouldFlee(BotBrainState& Brain, const BotObservation& Obs);
	// ---- getting ready, then hunting ----

	// "good enough to go out": armed, with something to heal with and materials to build (or nothing left to harvest), or simply long enough on the ground.
	inline bool IsReadyToHunt(const BotBrainState& Brain, const BotObservation& Obs)
	{
		const float OnGround = Brain.FirstGroundTime > 0.0f ? Obs.Now - Brain.FirstGroundTime : 0.0f;

		// only the fighters go looking for a fight, and only once they are kitted out; the rest meet people by chance or when the storm pushes everybody together
		if (Obs.Governor < 0.5f && !Brain.bStrong)
			return false;

		if (Brain.Aggression < 0.3f)
			return OnGround > 420.0f && Obs.bHasGun;

		if (OnGround > 360.0f)
			return true;

		return Obs.bHasGun && Obs.HealCount >= 2 && OnGround > 120.0f && (Obs.WoodCount >= 150 || gHarvestables.empty());
	}

	// Somebody to look for: mostly the nearest, real players preferred. The spot is a bit off so the bot searches an area instead of homing in.
	inline bool PickHuntTarget(const BotObservation& Obs, FVector& OutPos)
	{
		float BestScore = 1e30f;
		bool bFound = false;

		for (auto& Other : gPawns)
		{
			if (!Other.Pawn || Other.Team == Obs.SelfTeam || (Other.bIsBot && !bBotsFightBots) || Other.Health <= 0.0f)
				continue;

			const float Distance = Dist2D(Obs.Pos, Other.Pos);

			if (Distance < 800.0f)
				continue;

			const float Score = Distance * (Other.bIsBot ? 1.0f : 0.5f) * RandRange(0.8f, 1.25f);

			if (Score < BestScore)
			{
				BestScore = Score;
				OutPos = FVector{ Other.Pos.X + RandRange(-600.0f, 600.0f), Other.Pos.Y + RandRange(-600.0f, 600.0f), Obs.Pos.Z };
				bFound = true;
			}
		}

		return bFound;
	}

	inline bool PieceAlive(AActor* Piece)
	{
		return Piece && Piece->IsValidLowLevel() && !Piece->IsActorBeingDestroyed() && !((ABuildingSMActor*)Piece)->IsDestroyed();
	}

	// Heads for a building piece to smash it with the pickaxe (the Harvest target handles walking up and swinging).
	inline void BeginBreak(BotBrainState& Brain, const BotObservation& Obs, AActor* Piece, int Id, const char* Why)
	{
		const FVector At = Piece->GetActorLocation();
		const float Dist = (std::max)(Dist2D(Obs.Pos, At), 1.0f);
		const float Reach = (std::min)(Dist, 170.0f);

		Brain.bBreaking = true;
		Brain.BreakUntil = Obs.Now + 10.0f;
		++Brain.BreakCount;
		Brain.HarvestActor = Piece;
		Brain.Target = FVector{ Obs.Pos.X + (At.X - Obs.Pos.X) / Dist * Reach, Obs.Pos.Y + (At.Y - Obs.Pos.Y) / Dist * Reach, Obs.Pos.Z };
		Brain.TargetKind = EBotTarget::Harvest;
		Brain.bHasTarget = true;
		Brain.TargetPickedAt = Obs.Now;
		Brain.NavPath.clear();
		Brain.bNavWanted = false;
		Brain.PauseUntil = 0.0f;

		static int Logged = 0;

		if (++Logged <= 25)
			LOG_INFO(LogBots, "BotBreak #{} smashes {} ({})", Id, Piece->GetName(), Why);
	}

	// Boxed in by its own walls: once the danger has passed (or the storm is closing in) cut a way out, towards the zone if outside it, else towards the enemy.
	inline bool MaybeBreakOut(BotBrainState& Brain, const BotObservation& Obs, int Id)
	{
		if (Brain.BoxedAt <= 0.0f)
			return false;

		int Near = 0;
		AActor* Best = nullptr;
		float BestScore = 1e9f;
		const bool bInStorm = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius;
		FVector Want = Obs.Pos;
		bool bWant = false;

		if (bInStorm) { Want = Obs.ZoneCenter; bWant = true; }
		else if (Obs.bHasEnemy) { Want = Obs.EnemyPos; bWant = true; }

		for (auto Piece : Brain.BuiltPieces)
		{
			if (!PieceAlive(Piece))
				continue;

			const FVector At = Piece->GetActorLocation();
			const float Dist = Dist2D(Obs.Pos, At);

			if (Dist > 420.0f)
				continue;

			++Near;
			float Score = Dist;

			if (bWant)
			{
				const float Wx = Want.X - Obs.Pos.X, Wy = Want.Y - Obs.Pos.Y;
				const float Wl = (std::max)(std::sqrt(Wx * Wx + Wy * Wy), 1.0f);
				Score -= 300.0f * (((At.X - Obs.Pos.X) * Wx + (At.Y - Obs.Pos.Y) * Wy) / (Wl * (std::max)(Dist, 1.0f)));
			}

			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Piece;
			}
		}

		if (!Near)
		{
			Brain.BoxedAt = 0.0f;
			Brain.BuiltPieces.clear();
			return false;
		}

		if (Brain.bBreaking || !Best || Obs.Now < Brain.PauseUntil)
			return false;

		const float Waited = Obs.Now - Brain.BoxedAt;
		const bool bSeen = Obs.bHasEnemy && Obs.bEnemyVisible;

		if (Waited < (bInStorm ? 1.0f : 4.0f) || (bSeen && !bInStorm && Waited < 12.0f))
			return false;

		BeginBreak(Brain, Obs, Best, Id, bInStorm ? "boxed in, storm" : "boxed in");
		SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
		return true;
	}

	inline ABuildingSMActor* FindHarvestable(const BotBrainState& Brain, const FVector& From, float MaxDistance, FVector* OutPos = nullptr)
	{
		ABuildingSMActor* Best = nullptr;
		float BestDistance = MaxDistance;

		for (auto& Candidate : gHarvestables)
		{
			const float Distance = Dist2D(From, Candidate.Pos);

			if (Distance >= BestDistance || std::fabs(Candidate.Pos.Z - From.Z) > 500.0f)
				continue;

			if (std::find(Brain.SkipHarvest.begin(), Brain.SkipHarvest.end(), (void*)Candidate.Actor) != Brain.SkipHarvest.end())
				continue;

			if (!IsLiveBuilding(Candidate.Actor) || Candidate.Actor->IsDestroyed())
				continue;

			BestDistance = Distance;
			Best = Candidate.Actor;

			if (OutPos)
				*OutPos = Candidate.Pos;
		}

		return Best;
	}

	inline void DecideEngage(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id);
	inline void DecideRetreat(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id);
	inline void DecideHeal(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id);
	inline bool TryClimbPlan(BotBrainState& Brain, const BotObservation& Obs, const FVector& Goal, const char* Why, int Id);

	// Walking logic shared by Wander (warmup) and Ground.

	// A bot that has been going nowhere for a long time (shut in on a roof by railings and overhangs, say) climbs down the ladder: first a run and jump off the
	// nearest edge that is a survivable drop, again from another side, and, if nothing has worked and no player is near enough to see it, it is put down on the ground.
	// Where can a trapped bot actually go? Walk outwards along 24 directions in steps, the way a player would size up a pen: at every step measure the floor
	// (a rise up to about 150 is a ledge that can be jumped onto, a fall up to about 420 can be jumped down), and look for walls at knee and head height
	// (a low one is hopped, a high one ends that direction). The direction that gets furthest towards the goal wins; the jump spots are returned so the
	// bot jumps where the ledge is instead of bouncing along the wall.
	struct EscapePlan
	{
		bool bFound = false;
		float Dx = 0.0f, Dy = 0.0f;
		float Reach = 0.0f;
		std::vector<float> Hops, Rises;
		float MaxRise = 0.0f, MaxDrop = 0.0f;
		int Tried = 0, Walled = 0, Short = 0;
	};

	inline EscapePlan SolveEscape(const FVector& Pos, float Gx, float Gy, AActor* Self, AActor* Weapon, const std::vector<float>& BadHeadings)
	{
		EscapePlan Best;
		float BestScore = -1e9f;
		const float StepLen = 80.0f;
		const int MaxSteps = 12;
		const float StartFloor = Pos.Z - 88.0f;
		const float Spin = RandRange(0.0f, 15.0f);

		for (int k = 0; k < 24; ++k)
		{
			const float Angle = (Spin + k * 15.0f) * DegToRad;
			const float Dx = std::cos(Angle), Dy = std::sin(Angle);
			++Best.Tried;

			bool bBad = false;

			for (float Bad : BadHeadings)
			{
				float D = std::fabs(Bad - (Spin + k * 15.0f));

				while (D > 360.0f) D -= 360.0f;
				if (D > 180.0f) D = 360.0f - D;
				if (D < 25.0f) bBad = true;
			}

			if (bBad)
				continue;

			float Floor = StartFloor, PrevX = Pos.X, PrevY = Pos.Y;
			int Steps = 0;
			std::vector<float> Hops, HopRises;
			float Rise = 0.0f, Drop = 0.0f;
			bool bWall = false;

			for (int i = 1; i <= MaxSteps; ++i)
			{
				const float X = Pos.X + Dx * StepLen * i, Y = Pos.Y + Dy * StepLen * i;
				FVector Ground{};

				if (!TraceBlocked(Self ? Self : gNavTracePawn, Weapon, FVector{ X, Y, Floor + 170.0f }, FVector{ X, Y, Floor - 430.0f }, nullptr, nullptr, nullptr, &Ground))
					break; // no floor within reach: a hole

				const float Step = Ground.Z - Floor;

				if (Step > 150.0f || Step < -420.0f)
				{
					bWall = Step > 150.0f;
					break;
				}

				// a wall the floor trace could not see (thin, or taller than the step)
				const float Top = Floor + (std::max)(Step, 0.0f);
				float KneeFraction = 0.5f;
				const bool bKneeBlocked = TraceBlocked(Self ? Self : gNavTracePawn, Weapon, FVector{ PrevX, PrevY, Floor + 60.0f }, FVector{ X, Y, Floor + 60.0f }, nullptr, &KneeFraction);

				if (TraceBlocked(Self ? Self : gNavTracePawn, Weapon, FVector{ PrevX, PrevY, Top + 140.0f }, FVector{ X, Y, Top + 140.0f }))
				{
					bWall = true;
					break;
				}

				if (bKneeBlocked || Step > 45.0f)
				{
					Hops.push_back(StepLen * (i - 1) + (bKneeBlocked ? KneeFraction : 0.5f) * StepLen); // where the knee ray met it (else somewhere in this step)
					HopRises.push_back(Step > 45.0f ? Step : 130.0f); // a thin wall's height is not known: assume a railing's
				}

				Rise = (std::max)(Rise, Ground.Z - StartFloor);
				Drop = (std::max)(Drop, StartFloor - Ground.Z);
				Floor = Ground.Z;
				PrevX = X;
				PrevY = Y;
				Steps = i;
			}

			if (bWall && Steps < 3)
				++Best.Walled;

			static int Diag = 0;

			if (Steps == 0 && Diag < 12)
			{
				++Diag;
				LOG_INFO(LogBots, "BotStruggleDiag: heading {:.0f} stops at step 1 (wall={} floor {:.0f} start {:.0f})", k * 15.0f, bWall, Floor, StartFloor);
			}

			const float Reach = Steps * StepLen;

			if (Reach < 560.0f)
			{
				++Best.Short;
				continue;
			}

			const float Score = Reach + (Dx * Gx + Dy * Gy) * 500.0f - (float)Hops.size() * 60.0f - Drop * 0.1f;

			if (Score > BestScore)
			{
				BestScore = Score;
				Best.bFound = true;
				Best.Dx = Dx;
				Best.Dy = Dy;
				Best.Reach = Reach;
				Best.Hops = Hops;
				Best.Rises = HopRises;
				Best.MaxRise = Rise;
				Best.MaxDrop = Drop;
			}
		}

		return Best;
	}

	// A bot that is boxed in reports it and works out a way out. Triggers: blocked again and again, or no real progress for 10 seconds although it has somewhere to go.
	inline void StruggleCheck(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, bool bWarmup, int Id)
	{
		if (bWarmup || !gTraceBudget)
			return;

		// did the last plan work?
		if (Brain.StruggleAt > 0.0f && Obs.Now - Brain.StruggleAt > 6.0f)
		{
			const bool bFreed = Dist2D(Obs.Pos, Brain.StrugglePos) > 450.0f;
			++(bFreed ? gStruggleFreed : gStruggleNotFreed);
			const float Along = (Obs.Pos.X - Brain.StrugglePos.X) * Brain.EscapeDir.X + (Obs.Pos.Y - Brain.StrugglePos.Y) * Brain.EscapeDir.Y;
			LOG_INFO(LogBots, "BotStruggleResult #{} {} 6s after the plan: moved {:.0f} from ({:.0f}, {:.0f}, {:.0f}); {:.0f} along the plan's heading of {:.0f}u reach, first obstacle at {:.0f}, {} jump(s) made, height now {:+.0f}", Id, bFreed ? "got free" : "is STILL STUCK", Dist2D(Obs.Pos, Brain.StrugglePos), Brain.StrugglePos.X, Brain.StrugglePos.Y, Brain.StrugglePos.Z, Along, Brain.EscapeReach, Brain.EscapeFace0, Brain.EscapeJumps, Obs.Pos.Z - Brain.StrugglePos.Z);
			if (!bFreed)
				Brain.BadEscapes.push_back({ Brain.StrugglePos, std::atan2(Brain.EscapeDir.Y, Brain.EscapeDir.X) * RadToDeg, Obs.Now });

			Brain.StruggleAt = 0.0f;
		}

		if (Brain.StruggleRefTime <= 0.0f || Dist2D(Obs.Pos, Brain.StruggleRefPos) > 350.0f)
		{
			Brain.StruggleRefTime = Obs.Now;
			Brain.StruggleRefPos = Obs.Pos;
		}

		const bool bGoing = Brain.bHasTarget || (Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius - 600.0f);
		const bool bNoProgress = bGoing && Obs.Now - Brain.StruggleRefTime >= 10.0f;
		const bool bBlocked = Brain.BlockedEvents >= 3;

		if ((!bNoProgress && !bBlocked) || Obs.Now < Brain.NextSolveAt || Brain.EscapeUntil > Obs.Now)
			return;

		Brain.NextSolveAt = Obs.Now + 5.0f;
		Brain.StruggleRefTime = Obs.Now;

		FVector Goal = Brain.bHasTarget ? Brain.Target : (Obs.bZoneKnown ? Obs.ZoneCenter : Obs.Pos);
		float Gx = Goal.X - Obs.Pos.X, Gy = Goal.Y - Obs.Pos.Y;
		const float GLen = (std::max)(std::sqrt(Gx * Gx + Gy * Gy), 1.0f);
		Gx /= GLen; Gy /= GLen;

		std::vector<float> Bad;

		for (size_t i = 0; i < Brain.BadEscapes.size();)
		{
			if (Obs.Now - Brain.BadEscapes[i].Time > 150.0f)
			{
				Brain.BadEscapes.erase(Brain.BadEscapes.begin() + i);
				continue;
			}

			if (Dist2D(Brain.BadEscapes[i].Pos, Obs.Pos) < 500.0f)
				Bad.push_back(Brain.BadEscapes[i].Heading);

			++i;
		}

		const auto Plan = SolveEscape(Obs.Pos, Gx, Gy, Obs.SelfPawn, Obs.SelfWeapon, Bad);
		++Brain.Struggles;
		++gStruggles;

		if (!Plan.bFound)
		{
			++gStruggleNone;
			LOG_INFO(LogBots, "BotStruggle #{} ({}) at ({:.0f}, {:.0f}, {:.0f}) goal {:.0f}u away kind {} blockedEvents {}: NO way out found ({} directions tried, {} walled in within 3 steps, {} too short)", Id, bBlocked ? "blocked" : "no progress", Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, GLen, (int)Brain.TargetKind, Brain.BlockedEvents, Plan.Tried, Plan.Walled, Plan.Short);
			Brain.DropRelaxUntil = Obs.Now + 20.0f;

			// walled in on foot: build a way up and out towards the goal
			if (Obs.WoodCount >= 20)
				TryClimbPlan(Brain, Obs, Goal, "builds its way out of a pocket", Id);

			return;
		}

		++gStruggleExits;
		Brain.EscapeDir = FVector{ Plan.Dx, Plan.Dy, 0.0f };
		Brain.EscapeStart = Obs.Pos;
		Brain.EscapeHops = Plan.Hops;
		Brain.EscapeRises = Plan.Rises;
		Brain.EscapeJumps = 0;
		Brain.bEscapeBackoff = false;
		Brain.EscapeBackoffs = 0;
		Brain.EscapeReach = Plan.Reach;
		Brain.EscapeFace0 = Plan.Hops.empty() ? -1.0f : Plan.Hops[0];
		Brain.bEscapePlanned = true;
		Brain.EscapeUntil = Obs.Now + Plan.Reach / 450.0f + 1.5f;
		Brain.NextEscapeJump = 0.0f;
		Brain.SidestepUntil = 0.0f;
		Brain.AvoidUntil = 0.0f;
		Brain.NavPath.clear();
		Brain.bNavWanted = false;
		Brain.BlockedEvents = 0;
		Brain.StruggleAt = Obs.Now;
		Brain.StrugglePos = Obs.Pos;

		LOG_INFO(LogBots, "BotStruggle #{} ({}) at ({:.0f}, {:.0f}, {:.0f}) goal {:.0f}u away kind {}: way out heading {:.0f} deg, {:.0f}u clear, {} jump(s), highest step up {:.0f}, deepest drop {:.0f}", Id, bBlocked ? "blocked" : "no progress", Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, GLen, (int)Brain.TargetKind, std::atan2(Plan.Dy, Plan.Dx) / DegToRad, Plan.Reach, (int)Plan.Hops.size(), Plan.MaxRise, Plan.MaxDrop);
	}

	inline void MaybeUnstick(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, bool bWarmup, int Id)
	{
		if (bWarmup || Obs.MovementMode != 1)
			return;

		if (Brain.StuckSince <= 0.0f || Dist2D(Obs.Pos, Brain.StuckPos) > 700.0f)
		{
			Brain.StuckPos = Obs.Pos;
			Brain.StuckSince = Obs.Now;
			Brain.StuckStage = 0;
			return;
		}

		const float Stuck = Obs.Now - Brain.StuckSince;
		// in the storm every second of waiting costs health, so the ladder runs much faster there
		const bool bStorm = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius;
		const float Pace = bStorm ? 0.4f : 1.0f;
		const int Wanted = Stuck >= 100.0f * Pace ? 3 : (Stuck >= 65.0f * Pace ? 2 : (Stuck >= 35.0f * Pace ? 1 : 0));

		if (Wanted <= Brain.StuckStage || (Wanted >= 3 && Obs.Now < Brain.NextWarpTry))
			return;

		FVector Goal = Brain.bHasTarget ? Brain.Target : (Obs.bZoneKnown ? Obs.ZoneCenter : Obs.Pos);
		float Gx = Goal.X - Obs.Pos.X, Gy = Goal.Y - Obs.Pos.Y;
		const float GLen = (std::max)(std::sqrt(Gx * Gx + Gy * Gy), 1.0f);
		Gx /= GLen; Gy /= GLen;
		const float FeetZ = Obs.Pos.Z - 88.0f;

		struct Spot { float Dx, Dy, Drop, Score; };
		Spot Best{ 0, 0, 0, -1e9f };
		const float Spin = RandRange(0.0f, 22.0f);

		for (int k = 0; k < 16; ++k)
		{
			const float Angle = (Spin + k * 22.5f) * DegToRad;
			const float Dx = std::cos(Angle), Dy = std::sin(Angle);

			// the way out has to be walkable at knee and chest height
			if (TraceBlocked(gNavTracePawn, nullptr, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 40.0f }, FVector{ Obs.Pos.X + Dx * 450.0f, Obs.Pos.Y + Dy * 450.0f, Obs.Pos.Z - 40.0f })
				|| TraceBlocked(gNavTracePawn, nullptr, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + 40.0f }, FVector{ Obs.Pos.X + Dx * 450.0f, Obs.Pos.Y + Dy * 450.0f, Obs.Pos.Z + 40.0f }))
				continue;

			for (float Reach : { 450.0f, 700.0f })
			{
				FVector Ground{};

				if (!TraceBlocked(gNavTracePawn, nullptr, FVector{ Obs.Pos.X + Dx * Reach, Obs.Pos.Y + Dy * Reach, Obs.Pos.Z + 50.0f }, FVector{ Obs.Pos.X + Dx * Reach, Obs.Pos.Y + Dy * Reach, FeetZ - 2600.0f }, nullptr, nullptr, nullptr, &Ground))
					continue;

				const float Drop = FeetZ - Ground.Z;

				if (Drop < 100.0f || Drop > 1500.0f)
					continue;

				const float Score = (Dx * Gx + Dy * Gy) * 300.0f - Drop * 0.2f + (Stuck >= 65.0f ? RandRange(0.0f, 200.0f) : 0.0f);

				if (Score > Best.Score)
					Best = { Dx, Dy, Drop, Score };
			}
		}

		if (Wanted <= 2)
		{
			Brain.StuckStage = Wanted;

			if (Best.Score > -1e8f)
			{
				Brain.EscapeDir = FVector{ Best.Dx, Best.Dy, 0.0f };
				Brain.EscapeUntil = Obs.Now + 3.0f;
				Brain.bEscapePlanned = false;
				Brain.NextEscapeJump = 0.0f;
				Brain.SidestepUntil = 0.0f;
				Brain.AvoidUntil = 0.0f;
				Brain.DropRelaxUntil = Obs.Now + 30.0f;
				Brain.bHasTarget = false;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;
				Action.bJump = true;

				static int Logged = 0;

				if (++Logged <= 25)
					LOG_INFO(LogBots, "BotUnstick #{} has been going nowhere for {:.0f}s: runs and jumps off a {:.0f} unit drop at ({:.0f}, {:.0f}, {:.0f})", Id, Stuck, Best.Drop, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z);
			}

			return;
		}

		// last resort: onto the ground, but only where no player can watch it happen
		for (auto& Other : gPawns)
		{
			if (Other.Pawn && !Other.bIsBot && Other.Health > 0.0f && Dist2D(Other.Pos, Obs.Pos) < 6000.0f)
			{
				Brain.NextWarpTry = Obs.Now + 5.0f;
				return; // try again a little later
			}

			}

		// boxed in with no clear run-up in any direction (a walled rooftop, a pocket in the rocks): look for open ground from above instead
		bool bFromAbove = false;
		FVector Spot{};

		if (Best.Score <= -1e8f)
		{
			float SpotScore = -1e9f;

			// first look for ground well below (the same roof again is no way out), then for any open ground
			for (int Pass = 0; Pass < 2 && !bFromAbove; ++Pass)
			for (float Reach : { 500.0f, 800.0f, 1200.0f, 1800.0f })
			{
				for (int k = 0; k < 16; ++k)
				{
					const float Angle = (k * 22.5f) * DegToRad;
					const float Dx = std::cos(Angle), Dy = std::sin(Angle);
					const float X = Obs.Pos.X + Dx * Reach, Y = Obs.Pos.Y + Dy * Reach;
					FVector Ground{};

					if (!TraceBlocked(gNavTracePawn, nullptr, FVector{ X, Y, Obs.Pos.Z + 400.0f }, FVector{ X, Y, FeetZ - 2600.0f }, nullptr, nullptr, nullptr, &Ground))
						continue;

					const float Drop = FeetZ - Ground.Z;

					if (Drop < (Pass == 0 ? 150.0f : -100.0f) || Drop > 2400.0f || Obs.Pos.Z + 400.0f - Ground.Z < 20.0f)
						continue; // above where the bot is, or the trace started inside solid rock

					if (TraceBlocked(gNavTracePawn, nullptr, FVector{ X, Y, Ground.Z + 20.0f }, FVector{ X, Y, Ground.Z + 220.0f }))
						continue; // no room to stand

					const float Score = (Dx * Gx + Dy * Gy) * 300.0f - Reach * 0.2f - std::fabs(Drop) * 0.05f + RandRange(0.0f, 100.0f);

					if (Score > SpotScore)
					{
						SpotScore = Score;
						Spot = FVector{ X, Y, Ground.Z };
						bFromAbove = true;
					}
				}

				if (bFromAbove)
					break;
			}
		}

		if (Best.Score <= -1e8f && !bFromAbove)
		{
			Brain.NextWarpTry = Obs.Now + 10.0f; // nowhere found: do not give up for good
			return;
		}

		Brain.StuckStage = 3;

		if (bFromAbove)
		{
			Action.bWarp = true;
			Action.WarpPos = FVector{ Spot.X, Spot.Y, Spot.Z + 95.0f };
			Brain.bHasTarget = false;
			Brain.NavPath.clear();
			Brain.bNavWanted = false;
			Brain.StuckSince = 0.0f;
			Brain.BadEscapes.clear();

			static int WarpedAbove = 0;

			if (++WarpedAbove <= 40)
				LOG_INFO(LogBots, "BotUnstick #{} has been boxed in for {:.0f}s at ({:.0f}, {:.0f}, {:.0f}){} and is put down on open ground at ({:.0f}, {:.0f}, {:.0f})", Id, Stuck, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, bStorm ? " in the storm" : "", Spot.X, Spot.Y, Spot.Z);
		}
		else if (Best.Score > -1e8f)
		{
			Action.bWarp = true;
			Action.WarpPos = FVector{ Obs.Pos.X + Best.Dx * 700.0f, Obs.Pos.Y + Best.Dy * 700.0f, FeetZ - Best.Drop + 95.0f };
			Brain.bHasTarget = false;
			Brain.NavPath.clear();
			Brain.bNavWanted = false;
			Brain.StuckSince = 0.0f;

			static int Warped = 0;

			if (++Warped <= 25)
				LOG_INFO(LogBots, "BotUnstick #{} has been trapped for {:.0f}s at ({:.0f}, {:.0f}, {:.0f}) and is put down {:.0f} lower", Id, Stuck, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, Best.Drop);
		}
	}

	// ---- shut in on a roof ----
	// The diner at Dusty Divot (around 47000, -1500, roof at z 2389) has a flat roof inside a rim (Diner_Roof_OverHang_*) that stands 131 above the
	// feet. Bots gliding to the chest inside land on the roof and then walk in circles for minutes: every way off goes over the rim, the struggle
	// solver keeps finding 900u of open roof with no jump in it and calls a walk across the roof "got free", and the stuck ladder restarts
	// every time the bot wanders more than 700u (the roof is bigger than that). Seen 2026-11-09: #44 spent over 3 minutes up there.
	// What a player does is walk to the edge and jump the rim. So: notice a bot that stays on one raised floor while it wants to be elsewhere,
	// find the nearest edge with a survivable drop behind it, run there and jump at the rim. If two jumps fail, smash the rim piece and walk off.
	inline constexpr float RoofTrapTime = 15.0f;     // seconds on one floor, wanting to leave, before it counts as trapped
	inline constexpr float RoofAreaRadius = 1500.0f; // "one floor": stays within this of where it started...
	inline constexpr float RoofAreaHeight = 150.0f;  // ...and this far up or down

	struct RoofEdge { bool bFound = false; float Yaw = 0.0f, Dx = 0.0f, Dy = 0.0f, EdgeDist = 0.0f, Drop = 0.0f, LipDist = -1.0f, LipRise = 0.0f; UObject* LipActor = nullptr; FVector LipPoint{}; };

	inline RoofEdge FindRoofEdge(const BotBrainState& Brain, const BotObservation& Obs, float Gx, float Gy)
	{
		RoofEdge Best;
		float BestScore = -1e9f;
		const float FeetZ = Obs.Pos.Z - 88.0f;
		const float StepLen = 80.0f;
		const float Spin = RandRange(0.0f, 22.5f);
		// a drop it would survive with room to spare (fall damage only starts above 800)
		const float MaxDrop = (std::min)(2200.0f, 800.0f + (std::max)(Obs.Health - 30.0f, 0.0f) / 4.5f * 100.0f);

		for (int k = 0; k < 16; ++k)
		{
			const float Yaw = NormalizeAngle(Spin + k * 22.5f);
			bool bTried = false;

			for (float Bad : Brain.RoofBadYaws)
				if (std::fabs(NormalizeAngle(Bad - Yaw)) < 20.0f)
					bTried = true;

			if (bTried)
				continue;

			const float Dx = std::cos(Yaw * DegToRad), Dy = std::sin(Yaw * DegToRad);
			float Floor = FeetZ, PrevX = Obs.Pos.X, PrevY = Obs.Pos.Y;
			RoofEdge E;
			E.Yaw = Yaw; E.Dx = Dx; E.Dy = Dy;
			bool bWall = false;

			for (int i = 1; i <= 20 && !E.bFound && !bWall; ++i)
			{
				const float X = Obs.Pos.X + Dx * StepLen * i, Y = Obs.Pos.Y + Dy * StepLen * i;
				FVector Ground{};

				if (!TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ X, Y, Floor + 260.0f }, FVector{ X, Y, FeetZ - MaxDrop - 50.0f }, nullptr, nullptr, nullptr, &Ground))
					break; // nothing below within a survivable fall

				const float Step = Ground.Z - Floor;

				if (Step > 175.0f)
				{
					bWall = true; // taller than a jump
					break;
				}

				// head room over whatever it has to jump (a rim under another wall is no way out)
				const float Top = Floor + (std::max)(Step, 0.0f);

				if (TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ PrevX, PrevY, Top + 150.0f }, FVector{ X, Y, Top + 150.0f }))
				{
					bWall = true;
					break;
				}

				// the rim: a step up, or a thin rail the floor ray slipped past
				float KneeFraction = 0.5f;
				UObject* KneeActor = nullptr;
				FVector KneePoint{};
				const bool bKnee = Step <= 40.0f && TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ PrevX, PrevY, Floor + 60.0f }, FVector{ X, Y, Floor + 60.0f }, nullptr, &KneeFraction, &KneeActor, &KneePoint);

				if ((Step > 40.0f || bKnee) && E.LipDist < 0.0f)
				{
					E.LipDist = StepLen * (i - 1) + (bKnee ? KneeFraction : 0.5f) * StepLen;
					E.LipRise = Step > 40.0f ? Step : 130.0f;
					E.LipActor = KneeActor;
					E.LipPoint = bKnee ? KneePoint : FVector{ X, Y, Ground.Z };

					if (!E.LipActor) // a step up: find what it is made of for a possible smash
						TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ PrevX, PrevY, Floor + 40.0f }, FVector{ X, Y, Floor + 40.0f }, nullptr, nullptr, &E.LipActor, nullptr);
				}

				if (Floor - Ground.Z > 150.0f && Ground.Z < FeetZ - 220.0f)
				{
					E.bFound = true; // over the edge: a real ledge (not a hillside going gently down), ground well below
					E.EdgeDist = StepLen * i;
					E.Drop = FeetZ - Ground.Z;
				}

				Floor = Ground.Z;
				PrevX = X;
				PrevY = Y;
			}

			if (!E.bFound)
				continue;

			const float Score = -E.EdgeDist - E.Drop * 0.3f + (Dx * Gx + Dy * Gy) * 400.0f - (E.LipDist >= 0.0f ? 100.0f : 0.0f);

			if (Score > BestScore)
			{
				BestScore = Score;
				Best = E;
			}
		}

		return Best;
	}

	inline void RoofCheck(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, bool bWarmup, int Id)
	{
		if (bWarmup || Obs.MovementMode != 1 || !gTraceBudget)
			return;

		const float Dt = (std::min)(Obs.Now - Brain.RoofLastTick, 1.0f);
		Brain.RoofLastTick = Obs.Now;

		if (Brain.RoofSince <= 0.0f || Dist2D(Obs.Pos, Brain.RoofAnchor) > RoofAreaRadius || std::fabs(Obs.Pos.Z - Brain.RoofAnchor.Z) > RoofAreaHeight)
		{
			if (Brain.bRoofPending)
			{
				static int Logged = 0;

				if (++Logged <= 25)
					LOG_INFO(LogBots, "BotRoof #{} got off: now at ({:.0f}, {:.0f}, {:.0f}), {:+.0f} high from the roof, after {} tr{}", Id, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, Obs.Pos.Z - Brain.RoofAnchor.Z, Brain.RoofTries, Brain.RoofTries == 1 ? "y" : "ies");
			}

			Brain.RoofAnchor = Obs.Pos;
			Brain.RoofSince = Obs.Now;
			Brain.RoofWantAway = 0.0f;
			Brain.RoofTries = 0;
			Brain.bRoofPending = false;
			Brain.RoofBadYaws.clear();
			return;
		}

		// only a bot that wants to be somewhere else is trapped; one looting a house or fighting there is not
		if (Brain.State != EBotBrainState::Ground && Brain.State != EBotBrainState::Wander)
			return;

		const bool bFarGoal = Brain.bHasTarget && Brain.TargetKind != EBotTarget::Harvest && Dist2D(Brain.Target, Brain.RoofAnchor) > 2000.0f;
		const bool bStormCalls = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius - StormMargin();

		if (bFarGoal || bStormCalls || !Brain.bHasTarget)
			Brain.RoofWantAway += Dt;

		if (Brain.RoofWantAway < RoofTrapTime || Obs.Now < Brain.NextRoofTry || Brain.EscapeUntil > Obs.Now || Brain.bBreaking)
			return;

		Brain.NextRoofTry = Obs.Now + 8.0f;

		FVector Goal = Brain.bHasTarget ? Brain.Target : (Obs.bZoneKnown ? Obs.ZoneCenter : Obs.Pos);
		float Gx = Goal.X - Obs.Pos.X, Gy = Goal.Y - Obs.Pos.Y;
		const float GLen = (std::max)(std::sqrt(Gx * Gx + Gy * Gy), 1.0f);
		Gx /= GLen; Gy /= GLen;

		const RoofEdge Edge = FindRoofEdge(Brain, Obs, Gx, Gy);

		if (!Edge.bFound)
		{
			// not a roof (no edge with ground below in reach): this is for the other unstick layers
			static int Logged = 0;

			if (++Logged <= 15)
				LOG_INFO(LogBots, "BotRoof #{} has kept to one floor for {:.0f}s at ({:.0f}, {:.0f}, {:.0f}) but finds no edge to jump from", Id, Obs.Now - Brain.RoofSince, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z);

			Brain.NextRoofTry = Obs.Now + 20.0f;
			Brain.RoofBadYaws.clear();
			return;
		}

		++Brain.RoofTries;
		Brain.bRoofPending = true;

		// two jumps over this rim did not work: smash the rim piece (a player would pickaxe through it too), then walk off
		static auto BuildingClass = ABuildingSMActor::StaticClass();

		if (Brain.RoofTries >= 3 && Edge.LipActor && Edge.LipActor->IsA(BuildingClass) && PieceAlive((AActor*)Edge.LipActor)
			&& ((ABuildingSMActor*)Edge.LipActor)->GetMaxHealth() <= 1500.0f && Dist2D(Obs.Pos, Edge.LipPoint) < 1500.0f)
		{
			auto& Skip = Brain.SkipHarvest;
			Skip.erase(std::remove(Skip.begin(), Skip.end(), (void*)Edge.LipActor), Skip.end());

			BeginBreak(Brain, Obs, (AActor*)Edge.LipActor, Id, "the rim that keeps it on a roof");
			// BeginBreak walks up to the piece along the line to its centre, which on a long rim can be far off: go to the part found instead
			Brain.Target = FVector{ Edge.LipPoint.X - Edge.Dx * 90.0f, Edge.LipPoint.Y - Edge.Dy * 90.0f, Obs.Pos.Z };
			Brain.NextRoofTry = Obs.Now + 12.0f;
			return;
		}

		// run to the edge and jump at the rim (or at the edge itself, to clear anything the rays missed)
		const float HopAt = Edge.LipDist >= 0.0f ? Edge.LipDist : (std::max)(Edge.EdgeDist - 60.0f, 40.0f);
		const float Rise = Edge.LipDist >= 0.0f ? Edge.LipRise : 60.0f;

		Brain.RoofBadYaws.push_back(Edge.Yaw); // if this one fails the next try goes another way
		Brain.EscapeDir = FVector{ Edge.Dx, Edge.Dy, 0.0f };
		Brain.EscapeStart = Obs.Pos;
		Brain.EscapeHops = { HopAt };
		Brain.EscapeRises = { Rise };
		Brain.EscapeJumps = 0;
		Brain.bEscapeBackoff = false;
		Brain.EscapeBackoffs = 0;
		Brain.EscapeReach = Edge.EdgeDist + 250.0f;
		Brain.EscapeFace0 = HopAt;
		Brain.bEscapePlanned = true;
		Brain.EscapeUntil = Obs.Now + Brain.EscapeReach / 400.0f + 2.5f;
		Brain.NextEscapeJump = 0.0f;
		Brain.SidestepUntil = 0.0f;
		Brain.AvoidUntil = 0.0f;
		Brain.DropRelaxUntil = Obs.Now + 20.0f; // it means to go over the edge
		Brain.NavPath.clear();
		Brain.bNavWanted = false;
		Brain.BlockedEvents = 0;
		Brain.StruggleAt = 0.0f;
		Brain.NextSolveAt = Brain.EscapeUntil + 2.0f;

		static int Logged = 0;

		if (++Logged <= 40)
			LOG_INFO(LogBots, "BotRoof #{} is shut in on a raised floor at ({:.0f}, {:.0f}, {:.0f}) for {:.0f}s (try {}): runs {:.0f}u heading {:.0f} to an edge with a {:.0f}u drop, jumps at {:.0f} ({:.0f} high, {})", Id, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z,
				Obs.Now - Brain.RoofSince, Brain.RoofTries, Edge.EdgeDist, Edge.Yaw, Edge.Drop, HopAt, Rise, Edge.LipActor ? ((UObject*)Edge.LipActor)->GetName() : std::string("no rim"));
	}

	inline void DecideWalking(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, bool bWarmup, int Id)
	{
		Action.MaxWalkSpeed = SprintSpeed;

		if (Obs.MovementMode != 1)
			return; // not on the ground yet, let physics settle

		const bool bHarvesting = Brain.bHasTarget && Brain.TargetKind == EBotTarget::Harvest && Dist2D(Obs.Pos, Brain.Target) < 320.0f;

		if (!bWarmup && !bHarvesting)
			MaybeEquip(Brain, Obs, Action);

		// Got a kill: now and then the winner dances on the spot, the way players did (the showier bots more often, and only when nothing else is going on:
		// DecideWalking only runs when the bot is not in a fight, and seeing somebody takes it out of here)
		if (Brain.Kills > Brain.KillsSeen)
		{
			Brain.KillsSeen = Brain.Kills;

			if (!bWarmup && RandRange(0.0f, 1.0f) < 0.15f + 0.4f * (std::max)(Brain.Curiosity, 0.0f))
				Brain.DanceAt = Obs.Now + RandRange(0.6f, 1.8f);
		}

		if (Brain.DanceAt > 0.0f && Obs.Now >= Brain.DanceAt)
		{
			const bool bCalm = Obs.Now - Brain.DanceAt < 4.0f && !Obs.bHasEnemy && Obs.Now - Brain.HurtAt > 4.0f && Brain.TargetKind != EBotTarget::Storm && !bHarvesting;
			Brain.DanceAt = 0.0f;

			if (bCalm)
			{
				Action.bEmote = true;
				Action.EmoteIndex = rand();
				Action.bStop = true;
				Brain.PauseUntil = Obs.Now + RandRange(2.5f, 4.5f);
				Brain.DanceUntil = Brain.PauseUntil;

				static int Danced = 0;

				if (++Danced <= 25)
					LOG_INFO(LogBots, "BotDance #{} dances over kill number {}", Id, Brain.Kills);

				return;
			}
		}

		if (Brain.PauseUntil > Obs.Now)
		{
			Action.bStop = true;
			return;
		}

		// Held up for a while on the way to somewhere higher (a cliff, a steep hill, a ledge): ramp up it before the struggle solver starts
		// looking for ways round. TryClimbPlan only builds when it sees ground well above the feet just ahead.
		if (!bWarmup && !bHarvesting && Brain.bHasTarget && !Brain.bRampPlan && Obs.WoodCount >= 20 && Obs.Now >= Brain.NextRampPlan
			&& (Brain.Target.Z - Obs.Pos.Z > 250.0f || Brain.TargetKind == EBotTarget::Storm) && Dist2D(Brain.Target, Obs.Pos) < 9000.0f
			&& ((Brain.StruggleRefTime > 0.0f && Obs.Now - Brain.StruggleRefTime >= 6.0f) || Brain.BlockedEvents >= 2))
		{
			if (TryClimbPlan(Brain, Obs, Brain.Target, Brain.TargetKind == EBotTarget::Storm ? "climbs out of the storm's way" : "climbs towards its goal", Id))
				return;
		}

		if (!bHarvesting && gUnstick)
		{
			StruggleCheck(Brain, Obs, Action, bWarmup, Id);
			MaybeUnstick(Brain, Obs, Action, bWarmup, Id);
			RoofCheck(Brain, Obs, Action, bWarmup, Id);
		}

		// Watchdog: a bot that has not moved for ~6 seconds although nothing is holding it is stuck, whatever the reason.
		// Throw away its goal (and mark a chest goal as done so it isn't picked again), shove it sideways and jump.
		if (Obs.Now - Brain.WatchTime >= 3.0f)
		{
			if (Brain.WatchTime > 0.0f && Dist2D(Obs.Pos, Brain.WatchPos) < 120.0f)
				++Brain.IdleChecks;
			else
				Brain.IdleChecks = 0;

			Brain.WatchPos = Obs.Pos;
			Brain.WatchTime = Obs.Now;

			if (Brain.IdleChecks >= 2 && !bHarvesting)
			{
				++Brain.WatchdogResets;

				if (Brain.WatchdogResets <= 3 || Brain.WatchdogResets % 10 == 0)
				{
					LOG_INFO(LogBots, "BotBrain #{} watchdog: idle {:.0f}s at ({:.0f}, {:.0f}, {:.0f}); hasTarget={} kind={} targetDist={:.0f} blockedEvents={} enemy={} pickup={} gun={} (reset #{})",
						Id, 3.0f * Brain.IdleChecks, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, Brain.bHasTarget, (int)Brain.TargetKind, Brain.bHasTarget ? Dist2D(Obs.Pos, Brain.Target) : -1.0f,
						Brain.BlockedEvents, Obs.bHasEnemy, Obs.bHasPickup, Obs.bHasGun, Brain.WatchdogResets);
				}

				if (Brain.bHasTarget && Brain.TargetKind == EBotTarget::Chest)
				{
					ChestOutcome("given up: stood still for six seconds", Brain, Obs.Pos, Obs.Now, Id);
					Brain.VisitedChests.push_back(Brain.Target);
					NoteChestFail(Brain.Target);
				}
					if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });

				Brain.bHasTarget = false;
				Brain.TargetKind = EBotTarget::None;
				Brain.BlockedAccum = 0.0f;
				Brain.IdleChecks = 0;

				const float Angle = RandRange(0.0f, 6.2831853f);
				Brain.SidestepDir = FVector{ std::cos(Angle), std::sin(Angle), 0.0f };
				Brain.SidestepUntil = Obs.Now + 1.5f;
				Action.bJump = true;
				Brain.bWantEscape = true;
			}
		}

		// Pacing: lots of walking, no progress. The idle watchdog above only catches a bot that stands still; this one catches the bot that
		// goes back and forth (a slope it cannot climb, a railing, a loop around the same obstacle).
		if (!bWarmup && !bHarvesting && Brain.PauseUntil <= Obs.Now)
		{
			if (Brain.PaceRefTime <= 0.0f)
			{
				Brain.PaceRefPos = Obs.Pos;
				Brain.PaceLastPos = Obs.Pos;
				Brain.PaceRefTime = Obs.Now;
				Brain.PacePath = 0.0f;
			}

			Brain.PacePath += Dist2D(Obs.Pos, Brain.PaceLastPos);
			Brain.PaceLastPos = Obs.Pos;

			if (Obs.Now - Brain.PaceRefTime >= 8.0f)
			{
				const float Net = Dist2D(Obs.Pos, Brain.PaceRefPos);

				if (Net < 350.0f && Brain.PacePath > 1200.0f)
				{
					++Brain.Paces;
					if (Obs.Phase == EAthenaGamePhase::SafeZones || Obs.Phase == EAthenaGamePhase::EndGame)
						NavAddSeed(FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z - 88.0f }, 2500.0f);
					Brain.DropRelaxUntil = Obs.Now + 20.0f; // stuck on a roof or a ledge: a drop is better than walking in circles

					static int Logged = 0;

					if (++Logged <= 25)
						LOG_INFO(LogBots, "BotPace #{} walked {:.0f} units in 8s and got {:.0f} anywhere at ({:.0f}, {:.0f}, {:.0f}); dropping its goal (kind {})", Id, Brain.PacePath, Net, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, (int)Brain.TargetKind);

					if (Brain.bHasTarget && Brain.TargetKind == EBotTarget::Chest)
					{
						ChestOutcome("given up: walked about without getting nearer", Brain, Obs.Pos, Obs.Now, Id);
						Brain.VisitedChests.push_back(Brain.Target);
						NoteChestFail(Brain.Target);
					}
						if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });

					Brain.bHasTarget = false;
					Brain.TargetKind = EBotTarget::None;
					Brain.NavPath.clear();
					Brain.bNavWanted = false;
					Brain.BlockedEvents = 0;
					Brain.BlockedAccum = 0.0f;

					// head off somewhere else for a few seconds, jumping as it goes
					const float Angle = RandRange(0.0f, 6.2831853f);
					Brain.EscapeDir = FVector{ std::cos(Angle), std::sin(Angle), 0.0f };
					Brain.EscapeUntil = Obs.Now + 3.0f;
					Brain.bEscapePlanned = false;
					Brain.NextEscapeJump = 0.0f;
					Brain.SidestepUntil = 0.0f;
					Brain.AvoidUntil = 0.0f;
					Action.bJump = true;

					if (Brain.Paces % 3 == 0)
					{
						Brain.Target = PickWanderPoint(Obs.Pos, 7000.0f);
						Brain.TargetKind = EBotTarget::Wander;
						Brain.bHasTarget = true;
						Brain.TargetPickedAt = Obs.Now;
					}
				}

				Brain.PaceRefPos = Obs.Pos;
				Brain.PaceLastPos = Obs.Pos;
				Brain.PaceRefTime = Obs.Now;
				Brain.PacePath = 0.0f;
			}
		}

		// storm beats everything outside of warmup
		// Once the next circle is on the map each bot sets off for it after its own delay (the further out it is, the sooner), instead of waiting for the
		// storm edge to reach it: the early circles close faster than a bot can run.
		FVector GoalCenter = Obs.ZoneCenter;
		float GoalRadius = Obs.ZoneRadius;

		if (Obs.bNextZoneKnown)
		{
			if (std::fabs(Brain.SeenNextRadius - Obs.NextZoneRadius) > 1.0f)
			{
				const float Outside = Dist2D(Obs.Pos, Obs.NextZoneCenter) - Obs.NextZoneRadius;
				Brain.SeenNextRadius = Obs.NextZoneRadius;
				// late circles last a minute or so: no long dawdling there, however close the bot is
				const bool bLate = Obs.NextZoneRadius < 15000.0f;
				Brain.RotateAt = Obs.Now + (Outside > 40000.0f ? RandRange(0.0f, 12.0f) : (Outside > 15000.0f ? RandRange(5.0f, 30.0f) : (bLate ? RandRange(0.0f, 8.0f) : RandRange(15.0f, 60.0f))));
			}

			// the circle is moving in right now: go
			if (gZoneCloseSpeed > 30.0f && Dist2D(Obs.Pos, Obs.NextZoneCenter) > Obs.NextZoneRadius - 600.0f)
				Brain.RotateAt = (std::min)(Brain.RotateAt, Obs.Now);

			if (Obs.Now >= Brain.RotateAt || Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius - StormMargin())
			{
				GoalCenter = Obs.NextZoneCenter;
				GoalRadius = Obs.NextZoneRadius;
			}
		}

		if (!bWarmup && Obs.bZoneKnown && Dist2D(Obs.Pos, GoalCenter) > GoalRadius - StormMargin())
		{
			// The way out is the nearest part of the circle, just inside its edge (not a random spot half across it, which sent bots over mountains), and the same
			// goal is kept until it is reached or the circle has shrunk past it. If a goal trapped the bot, the next one is tried from another side.
			const bool bStale = !Brain.bHasTarget || Brain.TargetKind != EBotTarget::Storm || Dist2D(Brain.Target, GoalCenter) > GoalRadius - 1500.0f || Dist2D(Obs.Pos, Brain.Target) < 400.0f;

			if (bStale)
			{
				float Bias = 0.0f;

				if (Obs.Now - Brain.StormDropAt < 25.0f && Brain.StormTries > 0)
					Bias = ((Brain.StormTries & 1) ? 1.0f : -1.0f) * (std::min)(150.0f, 35.0f * (float)((Brain.StormTries + 1) / 2));
				else
					Brain.StormTries = 0;

				const float Bearing = std::atan2(Obs.Pos.Y - GoalCenter.Y, Obs.Pos.X - GoalCenter.X) + Bias * DegToRad;
				// The goal must lie deeper in than the "still has to move in" band above (StormMargin, up to 4600 while the circle closes fast). At a flat 3500
				// a bot that reached its goal still counted as outside, picked the same spot again and stood on that ring for the whole closing.
				const float Inset = (std::max)(3500.0f, StormMargin() + 1500.0f);
				const float Inside = (std::min)((std::max)(GoalRadius * 0.55f, GoalRadius - Inset), (std::max)(GoalRadius - StormMargin() - 500.0f, 0.0f));
				Brain.Target = FVector{ GoalCenter.X + std::cos(Bearing) * Inside, GoalCenter.Y + std::sin(Bearing) * Inside, Obs.Pos.Z };
				Brain.bHasTarget = true;
				Brain.TargetKind = EBotTarget::Storm;
				Brain.TargetPickedAt = Obs.Now;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;
			}
		}
		else if (Brain.TargetKind == EBotTarget::Storm)
		{
			Brain.bHasTarget = false;
			Brain.TargetKind = EBotTarget::None;
		}

		// Priorities: a fight (or the search for whoever just shot it) comes before looting. Within a few seconds of seeing an enemy or being hurt a bot does
		// not wander off to pick things up, unless it has no gun at all and the item is close.
		const bool bThreatened = Obs.Now - Brain.EnemyLastSeen < 8.0f || Obs.Now - Brain.HurtAt < 8.0f;
		const bool bAllowPickup = !bThreatened || (!Obs.bHasGun && Obs.bHasPickup && Dist2D(Obs.Pos, Obs.PickupPos) < 1000.0f);

		// a better gun lying around outranks a chest or wandering
		if (!bWarmup && Obs.bHasPickup && bAllowPickup && Brain.TargetKind != EBotTarget::Storm && !(Brain.TargetKind == EBotTarget::Pickup && Brain.TargetPickup == Obs.PickupPtr))
		{
			Brain.Target = Obs.PickupPos;
			Brain.bHasTarget = true;
			Brain.TargetKind = EBotTarget::Pickup;
			Brain.TargetPickup = Obs.PickupPtr;
			Brain.TargetPickedAt = Obs.Now;
		}

		// a chest somebody else opened while this bot was on its way is not worth the rest of the walk
		if (Brain.bHasTarget && Brain.TargetKind == EBotTarget::Chest && Brain.TargetChest)
		{
			auto Chest = (ABuildingContainer*)Brain.TargetChest;

			if (!IsLiveBuilding(Chest) || Chest->IsAlreadySearched())
			{
				ChestOutcome("dropped: somebody else opened it first", Brain, Obs.Pos, Obs.Now, Id);
				MarkChestLooted(Chest);
				Brain.bHasTarget = false;
				Brain.TargetKind = EBotTarget::None;
				Brain.TargetChest = nullptr;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;
			}
		}

		// a pickup target whose item is gone (taken by someone else) is dropped
		if (Brain.bHasTarget && Brain.TargetKind == EBotTarget::Pickup && (!Obs.bHasPickup || !bAllowPickup))
			Brain.bHasTarget = false;

		// a gunshot or a chest opening nearby: some bots go and have a look (how many depends on the bot's curiosity)
		if (!bWarmup && Obs.bHeardSound && Obs.SoundTime > Brain.LastSoundHandled && Obs.Now >= Brain.NextInvestigate
			&& Brain.TargetKind != EBotTarget::Storm && Brain.TargetKind != EBotTarget::Pickup)
		{
			Brain.LastSoundHandled = Obs.SoundTime;

			if (RandRange(0.0f, 1.0f) < Brain.Curiosity)
			{
				Brain.Target = FVector{ Obs.SoundPos.X + RandRange(-300.0f, 300.0f), Obs.SoundPos.Y + RandRange(-300.0f, 300.0f), Obs.Pos.Z };
				Brain.bHasTarget = true;
				Brain.TargetKind = EBotTarget::Investigate;
				Brain.TargetPickedAt = Obs.Now;
				Brain.NextInvestigate = Obs.Now + 10.0f;

				static int Heard = 0;

				if (++Heard <= 25)
					LOG_INFO(LogBots, "BotHear #{} heard something {:.0f} units away and goes to look", Id, Dist2D(Obs.Pos, Obs.SoundPos));
			}
		}

		// swinging the pickaxe at a prop for wood
		if (Brain.bHasTarget && Brain.TargetKind == EBotTarget::Harvest)
		{
			auto Prop = (ABuildingSMActor*)Brain.HarvestActor;
			const bool bValid = IsLiveBuilding(Prop) && !Prop->IsDestroyed();

			const bool bDone = Brain.bBreaking ? (Obs.Now > Brain.BreakUntil) : (Obs.WoodCount >= 250 || Obs.Now - Brain.TargetPickedAt > 40.0f);

			if (!bValid || bDone)
			{
				if (bValid)
					Brain.SkipHarvest.push_back(Prop); // took too long: leave this one

				if (Brain.bBreaking && !bValid)
					Brain.BlockedEvents = 0; // it is gone: the way is open

				Brain.bBreaking = false;
				Brain.bHasTarget = false;
				Brain.HarvestActor = nullptr;
			}
			else if (Dist2D(Obs.Pos, Brain.Target) < 240.0f)
			{
				SteerDir(Action, Brain.Target.X - Obs.Pos.X, Brain.Target.Y - Obs.Pos.Y, 0.0f);
				Action.bDriveXY = false;
				Action.bStop = true;
				Action.bHarvest = true;
				return;
			}
		}

		// arrived or stale target -> act on it, then pick a new one
		if (Brain.bHasTarget)
		{
			const float Reach = Brain.TargetKind == EBotTarget::Chest ? 250.0f : (Brain.TargetKind == EBotTarget::Pickup ? 130.0f : (Brain.TargetKind == EBotTarget::Poi ? PoiArriveRadius : (Brain.TargetKind == EBotTarget::Investigate ? 700.0f : (Brain.TargetKind == EBotTarget::Hunt ? 1200.0f : (Brain.TargetKind == EBotTarget::Harvest ? 1.0f : 220.0f)))));
			// getting close to a chest or pickup but not any closer (the thing is in a nook, behind a railing): after a moment that counts as there
			bool bNearEnough = false;

			if (Brain.TargetKind == EBotTarget::Chest || Brain.TargetKind == EBotTarget::Pickup)
			{
				const float Here = Dist2D(Obs.Pos, Brain.Target);

				if (Here > 650.0f || Brain.NearTarget != Brain.Target.X + Brain.Target.Y)
				{
					Brain.NearTarget = Brain.Target.X + Brain.Target.Y;
					Brain.NearBest = 1e9f;
					Brain.NearSince = Obs.Now;
				}
				else if (Here < Brain.NearBest - 60.0f)
				{
					Brain.NearBest = Here;
					Brain.NearSince = Obs.Now;
				}
				else if (Obs.Now - Brain.NearSince > 2.5f && Here < 520.0f)
				{
					bNearEnough = true;
				}
			}

			const bool bArrived = Dist2D(Obs.Pos, Brain.Target) < Reach || bNearEnough;
			const bool bStale = Obs.Now - Brain.TargetPickedAt > (Brain.TargetKind == EBotTarget::Poi ? 170.0f : (Brain.TargetKind == EBotTarget::Investigate ? 25.0f : (Brain.TargetKind == EBotTarget::Hunt ? 30.0f : 30.0f)));

			if (bArrived)
			{
				if (Brain.TargetKind == EBotTarget::Chest)
				{
					ChestOutcome(bNearEnough ? "arrived (as near as it could get)" : "arrived", Brain, Obs.Pos, Obs.Now, Id);
					Brain.VisitedChests.push_back(Brain.Target);
					if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });
					Action.bSearchChest = true;
					Brain.PauseUntil = Obs.Now + 1.6f; // stand still while the loot drops
				}
				else if (Brain.TargetKind == EBotTarget::Pickup)
				{
					Action.bPickup = true;
					Brain.PauseUntil = Obs.Now + 1.0f;
				}
				else if (Brain.TargetKind == EBotTarget::Investigate)
				{
					Brain.PauseUntil = Obs.Now + RandRange(1.5f, 3.0f); // stands and listens for a moment
				}
				else if (Brain.TargetKind == EBotTarget::Poi)
				{
					static int Arrived = 0;

					if (++Arrived <= 12 && Brain.PoiIndex >= 0 && Brain.PoiIndex < (int)gPois.size())
						LOG_INFO(LogBots, "BotPoi #{} arrived at '{}', looking for chests", Id, gPois[Brain.PoiIndex].Name);
				}
				else
				{
					Brain.PauseUntil = Obs.Now + RandRange(0.0f, 1.5f);
				}

				Action.bStop = true;
			}

			if (bArrived || bStale)
			{
				if (bStale && Brain.TargetKind == EBotTarget::Chest)
				{
					ChestOutcome("given up: thirty seconds and not there", Brain, Obs.Pos, Obs.Now, Id);
					Brain.VisitedChests.push_back(Brain.Target); // took too long to get there: forget it
					NoteChestFail(Brain.Target);
				}
					if (Brain.TargetKind == EBotTarget::Pickup) Brain.BadPickups.push_back({ Brain.TargetPickup, Obs.Now + 300.0f });

				Brain.bHasTarget = false;
				Brain.NavPath.clear();

				if (bArrived)
					return; // Apply performs the action using Brain.TargetChest / TargetPickup
			}
		}

		if (!Brain.bHasTarget)
		{
			int ChestIndex = bWarmup ? -1 : FindNearestChest(Brain, Obs, Obs.Pos, ChestSearchRadius);

			// nothing to go for: say why (how many chests are around, and what rules each of them out)
			if (!bWarmup && ChestIndex == -1)
			{
				int Around = 0, Opened = 0, GaveUp = 0, CrossedOff = 0;

				for (size_t i = 0; i < gChests.size(); ++i)
				{
					if (Dist2D(Obs.Pos, gChests[i]) > ChestSearchRadius)
						continue;

					++Around;
					auto Failed = gChestFails.find(ChestKey(gChests[i]));

					if (i < gChestLooted.size() && gChestLooted[i]) ++Opened;
					else if (IsVisited(Brain, gChests[i])) ++GaveUp;
					else if (Failed != gChestFails.end() && Failed->second >= 3) ++CrossedOff;
				}

				const int Count = ++gChestOutcomes[Around == 0 ? "looked for a chest: none within 6000" : (Around == Opened ? "looked for a chest: all around are opened" : "looked for a chest: unopened ones around, all ruled out")];

				if (Around > Opened && Count <= 20)
					LOG_INFO(LogBots, "BotChest #{} has no chest to go for at ({:.0f}, {:.0f}, {:.0f}): {} within 6000, {} opened, {} it gave up on itself, {} crossed off for everyone, {} ruled out otherwise (too far up or down, outside the circle)",
						Id, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z, Around, Opened, GaveUp, CrossedOff, Around - Opened - GaveUp - CrossedOff);
			}
			const bool bReady = !bWarmup && IsReadyToHunt(Brain, Obs);

			if (bReady && !Brain.bLoggedReady)
			{
				Brain.bLoggedReady = true;
				LOG_INFO(LogBots, "BotHunt #{} feels ready: gun={} heals={} wood={} after {:.0f}s on the ground", Id, Obs.bHasGun, Obs.HealCount, Obs.WoodCount, Obs.Now - Brain.FirstGroundTime);
			}

			// ready bots only take a chest that is right there
			if (bReady && ChestIndex != -1 && Dist2D(Obs.Pos, gChests[ChestIndex]) > 2500.0f)
				ChestIndex = -1;

			FVector PropPos{};
			ABuildingSMActor* Prop = (!bWarmup && !bReady && Obs.WoodCount < 150) ? FindHarvestable(Brain, Obs.Pos, 6000.0f, &PropPos) : nullptr;
			FVector HuntPos{};
			int PoiPick = -1;

			if (ChestIndex != -1)
			{
				Brain.Target = gChests[ChestIndex];
				Brain.TargetChest = gChestActors[ChestIndex];
				Brain.TargetKind = EBotTarget::Chest;
				Brain.TargetStartDist = Dist2D(Obs.Pos, Brain.Target) + 4.0f * std::fabs(Brain.Target.Z - Obs.Pos.Z);
				++gChestOutcomes["set off for a chest"];
			}
			else if (Prop)
			{
				Brain.HarvestActor = Prop;
				Brain.Target = FVector{ PropPos.X, PropPos.Y, Obs.Pos.Z };
				Brain.TargetKind = EBotTarget::Harvest;

				static int LoggedHarvest = 0;

				if (++LoggedHarvest <= 12)
					LOG_INFO(LogBots, "BotHarvest #{} heads for a prop {:.0f} units away (wood {})", Id, Dist2D(Obs.Pos, Brain.Target), Obs.WoodCount);
			}
			else if (bReady && PickHuntTarget(Obs, HuntPos))
			{
				Brain.Target = HuntPos;
				Brain.TargetKind = EBotTarget::Hunt;

				static int LoggedHunt = 0;

				if (++LoggedHunt <= 25)
					LOG_INFO(LogBots, "BotHunt #{} goes looking for somebody {:.0f} units away", Id, Dist2D(Obs.Pos, HuntPos));
			}
			else if (!bWarmup && (PoiPick = PickPoi(Brain, Obs, Obs.Pos)) != -1)
			{
				// nothing left to loot here: head for another point of interest
				const int Poi = PoiPick;
				Brain.PoiIndex = Poi;
				Brain.RecentPois.push_back(Poi);

				if (Brain.RecentPois.size() > 5)
					Brain.RecentPois.erase(Brain.RecentPois.begin());

				Brain.Target = FVector{ gPois[Poi].Center.X, gPois[Poi].Center.Y, Obs.Pos.Z };
				Brain.TargetKind = EBotTarget::Poi;

				static int Logged = 0;

				if (++Logged <= 25)
					LOG_INFO(LogBots, "BotPoi #{} travelling to '{}' ({:.0f} units away)", Id, gPois[Poi].Name, Dist2D(Obs.Pos, Brain.Target));
			}
			else
			{
				Brain.Target = bWarmup ? PickWanderPoint(Brain.Anchor, 900.0f) : PickWanderPoint(Obs.Pos, 2500.0f);
				Brain.TargetKind = EBotTarget::Wander;
			}

			Brain.bHasTarget = true;
			Brain.TargetPickedAt = Obs.Now;
			Brain.BlockedAccum = 0.0f;
			return;
		}

		// Players run at their normal speed most of the time and sprint in bursts (about 4 in 10 of the time they move); the storm and a chase are always flat out
		if (Obs.Now >= Brain.NextSprintToggle)
		{
			Brain.NextSprintToggle = Obs.Now + RandRange(4.0f, 12.0f);
			Brain.bTravelSprint = RandRange(0.0f, 1.0f) < (Brain.SprintShare > 0.0f ? Brain.SprintShare : 0.4f);
		}

		const float Speed = bWarmup ? WalkSpeed : ((Brain.TargetKind == EBotTarget::Storm || Brain.bTravelSprint) ? SprintSpeed : WalkSpeed);
		MoveToward(Brain, Obs, Action, Brain.Target, Speed, Id);
	}

	inline void DecideEngage(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		Action.MaxWalkSpeed = SprintSpeed;

		if (Obs.MovementMode != 1)
			return;

		// the game's own AI gives up a target that has run more than ReducedDeAggroRange away once it has chased for ReducedDeAggroAfter
		// seconds (AthenaAISettings); nothing hurting it meanwhile
		if (Obs.bHasEnemy && Obs.EnemyDist > ReducedDeAggroRange && Obs.Now - Brain.StateSince > ReducedDeAggroAfter && Obs.Now - Brain.HurtAt > 6.0f)
		{
			static int Logged = 0;

			if (++Logged <= 20)
				LOG_INFO(LogBots, "BotAware #{} gives up a chase after {:.0f}s ({:.0f}u away)", Id, Obs.Now - Brain.StateSince, Obs.EnemyDist);

			Brain.IgnoreUntil = Obs.Now + RandRange(15.0f, 30.0f);
			Brain.IgnoredAtDist = Obs.EnemyDist;
			Brain.IgnoredAt = Obs.Now;
			Brain.bIgnoredLostSight = false;
			SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
			return;
		}

		// a wall between it and the enemy: cut through it with the pickaxe instead of standing there
		if (Obs.Now < Brain.FightBreakUntil)
		{
			auto Piece = Brain.HarvestActor;

			if ((Obs.bHasEnemy && Obs.bEnemyVisible) || !PieceAlive(Piece))
			{
				Brain.FightBreakUntil = 0.0f;
				Brain.HarvestActor = nullptr;
			}
			else
			{
				const FVector At = Piece->GetActorLocation();

				if (Dist2D(Obs.Pos, At) < 240.0f)
				{
					SteerDir(Action, At.X - Obs.Pos.X, At.Y - Obs.Pos.Y, 0.0f);
					Action.bDriveXY = false;
					Action.bStop = true;
					Action.bHarvest = true;
				}
				else
				{
					MoveToward(Brain, Obs, Action, At, WalkSpeed, Id);
				}

				return;
			}
		}

		MaybeEquip(Brain, Obs, Action);

		if (Obs.bHasEnemy && Obs.bEnemyVisible)
		{
			// only what it can see updates where it believes the enemy is, and which way it was going
			const float Since = Obs.Now - Brain.EnemyLastSeen;

			if (Since > 0.05f && Since < 1.0f)
			{
				Brain.EnemyVel.X = Brain.EnemyVel.X * 0.5f + 0.5f * (Obs.EnemyPos.X - Brain.EnemyLastPos.X) / Since;
				Brain.EnemyVel.Y = Brain.EnemyVel.Y * 0.5f + 0.5f * (Obs.EnemyPos.Y - Brain.EnemyLastPos.Y) / Since;
			}
			else if (Since >= 1.0f)
			{
				Brain.EnemyVel = FVector{};
			}

			Brain.EnemyLastPos = Obs.EnemyPos;
			Brain.EnemyLastSeen = Obs.Now;
		}

		// where the enemy probably is now: where it was seen, carried on the way it was going for a couple of seconds
		FVector Guess = Brain.EnemyLastPos;

		if (!(Obs.bHasEnemy && Obs.bEnemyVisible))
		{
			const float Gone = (std::min)(Obs.Now - Brain.EnemyLastSeen, 2.5f);
			Guess = FVector{ Brain.EnemyLastPos.X + Brain.EnemyVel.X * Gone, Brain.EnemyLastPos.Y + Brain.EnemyVel.Y * Gone, Brain.EnemyLastPos.Z };
		}

		if (!(Obs.bHasEnemy && Obs.bEnemyVisible) && Obs.Now - Brain.EnemyLastSeen > 0.3f && Obs.Now - Brain.EnemyLastSeen < 4.0f && Obs.Now >= Brain.NextWallCheck && Dist2D(Obs.Pos, Guess) < 900.0f)
		{
			Brain.NextWallCheck = Obs.Now + 0.5f;
			UObject* Blocker = nullptr;

			if (TraceBlocked(gNavTracePawn, nullptr, FVector{ Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z + 50.0f }, FVector{ Guess.X, Guess.Y, Guess.Z + 40.0f }, nullptr, nullptr, &Blocker) && Blocker && Blocker->IsA(ABuildingSMActor::StaticClass()))
			{
				auto Piece = (ABuildingSMActor*)Blocker;
				const bool bPlayerBuilt = Piece->GetName().rfind("PBWA_", 0) == 0;

				if ((bPlayerBuilt || Piece->GetMaxHealth() <= 1500.0f) && Dist2D(Obs.Pos, Piece->GetActorLocation()) < 450.0f && PieceAlive(Piece))
				{
					Brain.HarvestActor = Piece;
					Brain.FightBreakUntil = Obs.Now + 8.0f;
					++Brain.BreakCount;

					static int Logged = 0;

					if (++Logged <= 25)
						LOG_INFO(LogBots, "BotBreak #{} smashes {} that is between it and its enemy", Id, Piece->GetName());
				}
			}
		}

		// losing the fight: get away before it is too late
		if (ShouldFlee(Brain, Obs))
		{
			if (Obs.Now >= Brain.FleeLogAt)
			{
				Brain.FleeLogAt = Obs.Now + 10.0f;
				static int Fled = 0;

				if (++Fled <= 25)
					LOG_INFO(LogBots, "BotFlee #{} runs: health {:.0f}+{:.0f} shield vs the enemy's {:.0f}+{:.0f}", Id, Obs.Health, Obs.Shield, Obs.EnemyHealth, Obs.EnemyShield);
			}

			SetState(Brain, EBotBrainState::Retreat, Obs.Now, Id);
			DecideRetreat(Brain, Obs, Action, Id);
			return;
		}

		// the lobby is thinning out too fast and nobody has hurt this bot: it lets the other one go
		// (the governor paces bot-against-bot fights; a real player is never let go because of it)
		if (!Obs.bEnemyIsHuman && Obs.Governor < 0.3f && Obs.Now - Brain.HurtAt > 6.0f && Obs.Now - Brain.StateSince > 2.0f && Obs.Health + Obs.Shield > 50.0f)
		{
			Brain.IgnoreUntil = Obs.Now + RandRange(25.0f, 50.0f);
			Brain.IgnoredAtDist = Obs.EnemyDist;
			Brain.IgnoredAt = Obs.Now;
			Brain.bIgnoredLostSight = false;
			SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
			Action.bStop = true;
			return;
		}

		// behind a wall it has just built: get right behind it and stay there a few seconds (heal if it can), then come out
		if (Brain.HideUntil > Obs.Now)
		{
			if (Dist2D(Obs.Pos, Brain.HidePos) > 90.0f)
			{
				MoveToward(Brain, Obs, Action, Brain.HidePos, SprintSpeed, Id);
				return;
			}

			Action.bStop = true;
			Action.bFace = true;
			Action.Yaw = Brain.HideYaw;

			if (Obs.bCanHeal && Obs.Health + Obs.Shield < 90.0f)
			{
				Brain.HideUntil = 0.0f;
				SetState(Brain, EBotBrainState::Heal, Obs.Now, Id);
				return;
			}

			// The window play: the better players do not just sit behind the wall, they edit a window into it, shoot through and close it again
			// (with a shotgun up close above all). The edit takes them a moment; the unskilled hardly ever try.
			{
				const float SkillNow = Brain.Skill < 0.0f ? 30.0f : Brain.Skill;

				if (Obs.bHoldingGun && Brain.HideWall && !Brain.bWindowOpen && Brain.WindowAt <= 0.0f && Obs.Now >= Brain.NextWindowPlay && Obs.Now - Brain.EnemyLastSeen < 6.0f
					&& Dist2D(Obs.Pos, Brain.EnemyLastPos) < (Obs.bHeldShotgun ? 1300.0f : 2500.0f) && Obs.Health + Obs.Shield > 40.0f)
				{
					Brain.NextWindowPlay = Obs.Now + RandRange(6.0f, 12.0f);

					if (RandRange(0.0f, 1.0f) < SkillNow / 100.0f)
					{
						Brain.WindowAt = Obs.Now + 0.9f - 0.006f * SkillNow;
						Brain.HideUntil = (std::max)(Brain.HideUntil, Brain.WindowAt + 0.2f);
					}
				}
			}

			// somebody came round the side: fight
			if (Obs.bEnemyVisible && Obs.EnemyDist < 900.0f)
				Brain.HideUntil = 0.0f;
			else
				return;
		}

		// shot at from a distance in the open: run to something that hides the bot, then come back to the fight
		if (Obs.Now - Brain.HurtAt < 2.5f && Obs.bHasEnemy && Obs.bEnemyVisible && Obs.EnemyDist > 600.0f && Obs.Health + Obs.Shield < 85.0f && Obs.Now >= Brain.NextCoverAt
			&& Brain.CoverUntil <= Obs.Now && !gNavTiles.empty() && Obs.MovementMode == 1)
		{
			Brain.NextCoverAt = Obs.Now + 10.0f;
			FVector Best{};
			float BestDistance = 1e9f;
			const FVector EnemyEye{ Obs.EnemyPos.X, Obs.EnemyPos.Y, Obs.EnemyPos.Z + 60.0f };
			int Tries = 0;

			for (int Bearing = 0; Bearing < 12 && Tries < 10; ++Bearing)
			{
				const float Angle = (Bearing * 30.0f + RandRange(-10.0f, 10.0f)) * DegToRad;

				for (float Reach : { 450.0f, 850.0f, 1300.0f })
				{
					FVector Spot{};

					if (!NavNodePos(Obs.Pos.X + std::cos(Angle) * Reach, Obs.Pos.Y + std::sin(Angle) * Reach, Obs.Pos.Z - 88.0f, 160.0f, Spot))
						continue;

					// not towards the enemy
					if (Dist2D(Spot, Obs.EnemyPos) < Obs.EnemyDist - 150.0f)
						continue;

					++Tries;

					if (TraceBlocked(gNavTracePawn ? gNavTracePawn : nullptr, nullptr, EnemyEye, FVector{ Spot.X, Spot.Y, Spot.Z + 80.0f }))
					{
						const float D = Dist2D(Obs.Pos, Spot);

						if (D < BestDistance)
						{
							BestDistance = D;
							Best = Spot;
						}

						break; // the nearest hidden spot on this bearing is enough
					}
				}
			}

			if (BestDistance < 1e8f)
			{
				Brain.CoverPos = Best;
				Brain.CoverUntil = Obs.Now + 6.0f;

				static int Covered = 0;

				if (++Covered <= 25)
					LOG_INFO(LogBots, "BotCover #{} runs {:.0f} units to a spot the enemy ({:.0f} away) cannot see", Id, BestDistance, Obs.EnemyDist);
			}
		}

		if (Brain.CoverUntil > Obs.Now)
		{
			if (Dist2D(Obs.Pos, Brain.CoverPos) > 140.0f)
			{
				MoveToward(Brain, Obs, Action, Brain.CoverPos, SprintSpeed, Id);
				return;
			}

			// behind cover: catch its breath (heal if it can), then peek again
			Action.bStop = true;
			Brain.CoverUntil = (std::min)(Brain.CoverUntil, Obs.Now + 1.5f + RandRange(0.0f, 1.5f));

			if (!Obs.bEnemyVisible && Obs.bCanHeal && Obs.Health + Obs.Shield < 90.0f)
			{
				Brain.CoverUntil = 0.0f;
				SetState(Brain, EBotBrainState::Heal, Obs.Now, Id);
			}

			return;
		}

		// hurt badly with an enemy in the open: a real player boxes up and heals instead of trading shots
		if (Obs.Health + Obs.Shield < 55.0f && Obs.WoodCount >= 80 && !(Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius) && Obs.Now >= Brain.NextBoxUp && Obs.bHasEnemy && Obs.EnemyDist > 150.0f && (Obs.bCanHeal || Obs.Health + Obs.Shield < 40.0f))
		{
			Brain.NextBoxUp = Obs.Now + 25.0f;
			Action.bBoxUp = true;
			Action.bBoxRoof = Obs.EnemyDist < 3000.0f && Obs.WoodCount >= 90; // close enough to shoot down into an open box
			Action.bStop = true;
			Brain.PauseUntil = Obs.Now + 4.0f;

			if (Obs.bCanHeal)
				SetState(Brain, EBotBrainState::Heal, Obs.Now, Id);

			static int Boxed = 0;

			if (++Boxed <= 25)
				LOG_INFO(LogBots, "BotDefend #{} boxes up to heal ({:.0f} health, {:.0f} shield, {} wood)", Id, Obs.Health, Obs.Shield, Obs.WoodCount);

			return;
		}

		const bool bOutsideStorm = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius + 1500.0f;
		const bool bLost = Obs.Now - Brain.EnemyLastSeen > 4.0f || (!Obs.bHasEnemy && Obs.Now - Brain.EnemyLastSeen > 1.0f);

		if (bLost || (bOutsideStorm && (!Obs.bHasEnemy || Obs.EnemyDist > 800.0f)))
		{
			SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
			Action.bStop = true;

			// it remembers where the enemy was last seen and goes to look there
			if (bLost && !bOutsideStorm)
			{
				Brain.Target = Guess;
				Brain.bHasTarget = true;
				Brain.TargetKind = EBotTarget::Investigate;
				Brain.TargetPickedAt = Obs.Now;
			}

			return;
		}

		const float Distance = Dist2D(Obs.Pos, Guess);
		const float Dx = Guess.X - Obs.Pos.X;
		const float Dy = Guess.Y - Obs.Pos.Y;
		const float YawToEnemy = std::atan2(Dy, Dx) * RadToDeg;

		if (Obs.bHoldingGun)
		{
			// gun: get to a sensible range, strafe, keep facing them and shoot
			float Preferred = Obs.bHeldShotgun ? 500.0f : (std::min)(1500.0f, Obs.HeldRange * 0.5f);

			// The other one is nearly dead and this bot is clearly ahead: it pushes in to finish it instead of trading shots from range (the bold ones
			// sooner). This is what makes a fight end with somebody running at you rather than both sides plinking away.
			const bool bPush = Obs.bHasEnemy && Obs.EnemyHealth + Obs.EnemyShield < 45.0f + 60.0f * Brain.Aggression && Obs.Health + Obs.Shield > Obs.EnemyHealth + Obs.EnemyShield + 30.0f && Distance < 3500.0f;

			if (bPush)
				Preferred = (std::min)(Preferred, 350.0f);

			if (Brain.bWindowOpen && Dist2D(Obs.Pos, Brain.HidePos) < 220.0f)
			{
				Action.bStop = true; // at its window: stays put and shoots through it until it closes it again
			}
			else if (Distance > Preferred + 300.0f)
			{
				MoveToward(Brain, Obs, Action, Guess, SprintSpeed, Id);
			}
			else if (!Obs.bEnemyVisible && Obs.Now - Brain.EnemyLastSeen > 0.4f && Distance > 250.0f)
			{
				// lost them behind something: go and have a look at where they probably went, gun up, not too fast
				MoveToward(Brain, Obs, Action, Guess, WalkSpeed, Id);
			}
			else
			{
				// A player in a fight stands and shoots, steps sideways, or pushes in and backs off: it changes every second or so and is never a clean circle
				if (Obs.Now >= Brain.StrafeUntil)
				{
					const float Roll = RandRange(0.0f, 1.0f);
					Brain.StrafeSign = (rand() & 1) ? 1.0f : -1.0f;

					if (Roll < 0.22f)
					{
						Brain.StrafeMode = 0; // plant the feet
						Brain.StrafeUntil = Obs.Now + RandRange(0.4f, 1.1f);
					}
					else if (Roll < 0.72f)
					{
						Brain.StrafeMode = 1; // sideways
						Brain.StrafeUntil = Obs.Now + RandRange(0.7f, 1.8f);
					}
					else
					{
						Brain.StrafeMode = 2; // towards or away from them
						Brain.StrafeUntil = Obs.Now + RandRange(0.6f, 1.4f);
					}
				}

				float Px = Dx, Py = Dy;
				const float Len = std::sqrt(Px * Px + Py * Py);

				if (Brain.StrafeMode == 0)
				{
					Action.bStop = true;
				}
				else if (Len > 1.0f)
				{
					Px /= Len;
					Py /= Len;

					if (Brain.StrafeMode == 1)
					{
						RotateDir(Px, Py, 90.0f * Brain.StrafeSign + RandRange(-15.0f, 15.0f));
					}
					else if (Distance < Preferred * 0.8f)
					{
						Px = -Px; // too close: back off
						Py = -Py;
					}

					SteerDir(Action, Px, Py, WalkSpeed);
				}
			}

			// under fire a person jumps around instead of strafing in a straight line
			if (Obs.Now - Brain.HurtAt < 4.0f && Obs.Now >= Brain.NextFightJump && Distance < 2500.0f)
			{
				Action.bJump = true;
				Brain.NextFightJump = Obs.Now + RandRange(0.9f, 2.2f);
			}
			// a shotgun fight up close is fought in the air: jump, shoot, land, jump again (the better the player, the more of it)
			else if (Obs.bHeldShotgun && Obs.bEnemyVisible && Distance < 750.0f && Obs.Now >= Brain.NextFightJump)
			{
				Action.bJump = true;
				Brain.NextFightJump = Obs.Now + RandRange(0.8f, 1.6f) + (100.0f - (Brain.Skill < 0.0f ? 30.0f : Brain.Skill)) * 0.03f;
			}

			// face the target (overrides the heading SteerDir set) and aim the pitch with a little human error
			Action.bFace = true;
			Action.Yaw = YawToEnemy + RandRange(-2.5f, 2.5f);
			Action.bSetPitch = true;
			const float Dz = Guess.Z - Obs.Pos.Z;
			Action.Pitch = std::atan2(Dz, (std::max)(Distance, 1.0f)) * RadToDeg + RandRange(-1.5f, 1.5f);

			if (Obs.bHasEnemy && Obs.bEnemyVisible)
			{
				Brain.AimTarget = Obs.EnemyPawn;
				Brain.AimTargetTime = Obs.Now;
				Brain.FoughtPawn = Obs.EnemyPawn;
				Brain.FoughtUntil = Obs.Now + 90.0f;

				if (!Brain.bSawEnemy)
				{
					Brain.bSawEnemy = true;
					Brain.AcquireTime = Obs.Now;
					Brain.Reaction = ReactionTime(Brain.Skill < 0.0f ? 30.0f : Brain.Skill);
				}
			}
			else if (Obs.Now - Brain.EnemyLastSeen > 1.0f)
			{
				Brain.bSawEnemy = false;
			}

			// the trigger is pulled once it has reacted and its aim is roughly on the target (the unskilled shoot while still turning)
			const float AimOff = std::fabs(NormalizeAngle(YawToEnemy - Brain.AimYaw));
			const float AimTolerance = 6.0f + (100.0f - (Brain.Skill < 0.0f ? 30.0f : Brain.Skill)) * 0.25f;
			const bool bReacted = Brain.bSawEnemy && Obs.Now - Brain.AcquireTime >= Brain.Reaction;

			// nobody hits a stranger across the map: past a certain range even the keen ones hold their fire or close in
			const float SkillNow = Brain.Skill < 0.0f ? 30.0f : Brain.Skill;
			const float FireRange = Obs.HeldRange > 9000.0f ? Obs.HeldRange : (std::min)(Obs.HeldRange, (Obs.bHeldShotgun ? 1100.0f : 3000.0f * (0.6f + 0.4f * SkillNow / 100.0f)));

			if (Obs.bHasEnemy && Obs.bEnemyVisible && bReacted && AimOff < AimTolerance && Distance < FireRange && Obs.Now >= Brain.NextAttackTime)
			{
				Action.bAttackRanged = true;
				Brain.TargetPawn = Obs.EnemyPawn;
				Brain.TargetDist = Distance;
				Brain.NextAttackTime = Obs.Now + Obs.HeldInterval + RandRange(0.0f, 0.1f);
			}

			return;
		}

		// no gun: pickaxe
		if (Distance < MeleeRange * 0.75f && Obs.bHasEnemy)
			Action.bStop = true;
		else
			MoveToward(Brain, Obs, Action, Guess, SprintSpeed * 0.92f * (Obs.Now < Brain.MeleeSlowUntil ? gMeleeSlowMult : 1.0f), Id); // a swing slows the swinger, so a runner can get away

		if (Obs.bHasEnemy && Distance < 900.0f)
		{
			Action.bFace = true;
			Action.Yaw = YawToEnemy;
		}

		if (Obs.bHasEnemy && Distance < MeleeRange && Obs.Now >= Brain.NextAttackTime)
		{
			Action.bAttackMelee = true;
			Brain.TargetPawn = Obs.EnemyPawn;
			Brain.TargetDist = Distance;
			Brain.NextAttackTime = Obs.Now + RandRange(0.8f, 1.05f); // a pickaxe swing takes its time
		}
	}

	// Who is winning? Health, shield and a little for the weapon, compared. Braver bots stay in fights longer.
	inline bool ShouldFlee(BotBrainState& Brain, const BotObservation& Obs)
	{
		if (!Obs.bHasEnemy || Obs.Now < Brain.NoFleeUntil)
			return false;

		// the lobby is thinning out too fast: whoever is losing a fight now gets out of it
		if (Obs.Governor < 0.3f && Obs.Health + Obs.Shield < 75.0f)
			return true;

		// a pickaxe against a gun is a fight nobody sane takes in the open
		// (not when it has just been shot: then it turns on the shooter with the pickaxe, the way a person with no gun would)
		if (!Obs.bHasGun && Obs.EnemyWeaponScore > 30.0f && Obs.EnemyDist > 350.0f && Obs.Now - Brain.HurtAt > 12.0f)
			return true;

		const float Self = Obs.Health + Obs.Shield + Obs.SelfWeaponScore * 0.3f;
		const float Enemy = Obs.EnemyHealth + Obs.EnemyShield + Obs.EnemyWeaponScore * 0.3f;
		const float Ratio = Self / (std::max)(Enemy, 1.0f);
		const float Bravery = Brain.Bravery > 0.0f ? Brain.Bravery : 1.0f;
		const bool bBadlyHurt = Obs.Health + Obs.Shield < 40.0f;

		return Ratio < 0.3f / Bravery || (bBadlyHurt && Enemy > 40.0f && Ratio < 0.7f);
	}

	// Run directly away from the enemy until it is out of sight, then heal if there is something to heal with.
	inline void DecideRetreat(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		Action.MaxWalkSpeed = SprintSpeed;

		if (Obs.MovementMode != 1)
			return;

		if (Obs.bHasEnemy && Obs.bEnemyVisible)
		{
			// only what it can see updates where it believes the enemy is
			Brain.EnemyLastPos = Obs.EnemyPos;
			Brain.EnemyLastSeen = Obs.Now;
		}

		const float SinceSeen = Obs.Now - Brain.EnemyLastSeen;
		const float Distance = Dist2D(Obs.Pos, Brain.EnemyLastPos);

		// out of sight and some way off: patch up, or just carry on
		if (SinceSeen > 1.5f && Distance > 1400.0f)
		{
			if (Obs.bNeedsHeal)
			{
				SetState(Brain, EBotBrainState::Heal, Obs.Now, Id);
				DecideHeal(Brain, Obs, Action, Id);
				return;
			}

			if (SinceSeen > 4.0f)
			{
				SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
				Action.bStop = true;
				return;
			}
		}

		// nowhere left to run: turn and fight
		if (Obs.Now - Brain.StateSince > 7.0f && Obs.bHasEnemy && Obs.bEnemyVisible && Distance < 900.0f)
		{
			Brain.NoFleeUntil = Obs.Now + 10.0f;
			SetState(Brain, EBotBrainState::Engage, Obs.Now, Id);
			DecideEngage(Brain, Obs, Action, Id);
			return;
		}

		float Ax = Obs.Pos.X - Brain.EnemyLastPos.X;
		float Ay = Obs.Pos.Y - Brain.EnemyLastPos.Y;
		float Length = std::sqrt(Ax * Ax + Ay * Ay);

		if (Length < 1.0f)
		{
			Ax = 1.0f;
			Ay = 0.0f;
			Length = 1.0f;
		}

		Ax /= Length;
		Ay /= Length;

		// running into the storm is no escape: lean towards the safe zone when far from its centre
		if (Obs.bZoneKnown)
		{
			const float Cx = Obs.ZoneCenter.X - Obs.Pos.X;
			const float Cy = Obs.ZoneCenter.Y - Obs.Pos.Y;
			const float CLength = std::sqrt(Cx * Cx + Cy * Cy);

			if (CLength > Obs.ZoneRadius * 0.7f && CLength > 1.0f)
			{
				Ax = Ax * 0.5f + Cx / CLength * 0.5f;
				Ay = Ay * 0.5f + Cy / CLength * 0.5f;
				const float NewLength = std::sqrt(Ax * Ax + Ay * Ay);

				if (NewLength > 0.01f)
				{
					Ax /= NewLength;
					Ay /= NewLength;
				}
			}
		}

		MoveToward(Brain, Obs, Action, FVector{ Obs.Pos.X + Ax * 2500.0f, Obs.Pos.Y + Ay * 2500.0f, Obs.Pos.Z }, SprintSpeed, Id);
	}

	// Stand still and use a healing item; a threat, damage, or the storm interrupts it (the item is only spent when the use finishes).
	inline void DecideHeal(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		Action.MaxWalkSpeed = SprintSpeed;

		if (Obs.MovementMode != 1)
			return;

		Action.bStop = true;

		if (Brain.HealStart <= 0.0f)
		{
			if (!Obs.bCanHeal)
			{
				SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
				return;
			}

			Brain.HealStart = Obs.Now;
			Brain.HealUse = Obs.HealUseTime;
			Brain.HealGuid = Obs.HealGuid;
			Brain.HealHealth = Obs.HealHealth;
			Brain.HealShield = Obs.HealShield;
			Brain.HealCapHealth = Obs.HealCapHealth;
			Brain.HealCapShield = Obs.HealCapShield;
			Brain.HealStartHealth = Obs.Health;
			Brain.HealStartShield = Obs.Shield;
			Brain.HealDefPtr = Obs.HealDef;
			Brain.bHealAbilityFired = false;
			Brain.bHealAbilityOk = false;

			if (Obs.bHealIsWeapon)
			{
				Action.bEquip = true; // holds the item for the animation
				Action.EquipDef = Obs.HealDef;
				Action.EquipGuid = Obs.HealGuid;
			}

			static int Started = 0;

			if (++Started <= 25)
				LOG_INFO(LogBots, "BotHeal #{} starts healing ({:.0f} health, {:.0f} shield, {:.1f}s)", Id, Obs.Health, Obs.Shield, Brain.HealUse);
		}

		const bool bThreat = Obs.bHasEnemy && Obs.bEnemyVisible && Obs.EnemyDist < 2500.0f;
		const bool bHurt = Obs.Health + Obs.Shield < Brain.HealStartHealth + Brain.HealStartShield - 1.0f;
		const bool bStorm = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius - 300.0f;

		// a real player stops healing when somebody can be heard nearby (a shot, footsteps) or an enemy is close even if out of sight
		const bool bHeard = Obs.bHeardSound && Obs.SoundTime >= Brain.HealStart && Dist2D(Obs.Pos, Obs.SoundPos) < 3500.0f;
		const bool bClose = Obs.bHasEnemy && Obs.EnemyDist < 1200.0f;

		if (bThreat || bHurt || bStorm || bHeard || bClose)
		{
			Brain.NoHealUntil = Obs.Now + 4.0f;

			static int Interrupted = 0;

			if (++Interrupted <= 30)
				LOG_INFO(LogBots, "BotHeal #{} stops healing: threat={} hurt={} storm={} heard={} close={}", Id, bThreat, bHurt, bStorm, bHeard, bClose);

			Brain.HealStart = 0.0f;
			Brain.NextEquipTime = 0.0f; // get the gun back in hand

			if (bThreat)
			{
				Brain.EnemyLastPos = Obs.EnemyPos;
				Brain.EnemyLastSeen = Obs.Now;

				if (ShouldFlee(Brain, Obs))
				{
					SetState(Brain, EBotBrainState::Retreat, Obs.Now, Id);
					DecideRetreat(Brain, Obs, Action, Id);
				}
				else
				{
					SetState(Brain, EBotBrainState::Engage, Obs.Now, Id);
					DecideEngage(Brain, Obs, Action, Id);
				}
			}
			else
			{
				SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
			}

			return;
		}

		// the item's own ability is running: it plays the animation, heals and spends the item. Done when the health has gone up; if nothing happens
		// two seconds after it should have finished, fall back to doing it ourselves.
		if (Brain.bHealAbilityOk)
		{
			if (Obs.Health + Obs.Shield >= Brain.HealStartHealth + Brain.HealStartShield + 2.0f)
			{
				static int Natural = 0;

				if (++Natural <= 10)
					LOG_INFO(LogBots, "BotHeal #{} healed by the item's own ability: health {:.0f} -> {:.0f}, shield {:.0f} -> {:.0f}", Id, Brain.HealStartHealth, Obs.Health, Brain.HealStartShield, Obs.Shield);

				++Brain.Heals;
				Brain.HealStart = 0.0f;
				Brain.NextEquipTime = 0.0f;
				SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
				return;
			}

			if (Obs.Now - Brain.HealStart < Brain.HealUse + 2.0f)
				return;

			LOG_INFO(LogBots, "BotHeal #{} the item's ability did nothing in {:.1f}s, healing by hand", Id, Obs.Now - Brain.HealStart);
			Brain.bHealAbilityOk = false;
		}

		if (Obs.Now - Brain.HealStart >= Brain.HealUse)
		{
			Action.bUseHeal = true;
			Action.HealGuid = Brain.HealGuid;
			Action.HealHealth = Brain.HealHealth;
			Action.HealShield = Brain.HealShield;
			Action.HealCapHealth = Brain.HealCapHealth;
			Action.HealCapShield = Brain.HealCapShield;
			Brain.NextEquipTime = 0.0f; // gun back in hand next
			SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
		}
	}


	// ---- spawn island: while the bus is not here yet, bots do the things players do rather than only wander ----

	inline bool DecideIsland(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		if (Obs.MovementMode != 1)
			return false;

		Action.MaxWalkSpeed = SprintSpeed;

		if (Obs.Now >= Brain.IslandUntil)
		{
			Brain.IslandMode = 0;
			Brain.bHasTarget = false;
			Brain.PauseUntil = 0.0f;
			const float Roll = RandRange(0.0f, 100.0f);
			Brain.IslandUntil = Obs.Now + RandRange(5.0f, 12.0f);
			Brain.IslandYaw = RandRange(-180.0f, 180.0f);
			Brain.IslandNextTurn = Obs.Now + 1.2f;

			if (Roll < 28.0f)
			{
				Brain.IslandMode = 0;
			}
			else if (Roll < 55.0f)
			{
				Brain.IslandMode = 1;
				Brain.IslandUntil = Obs.Now + RandRange(4.0f, 8.0f);
				Action.bEmote = true;
				Action.EmoteIndex = rand();
			}
			else if (Roll < 66.0f)
			{
				Brain.IslandMode = 2;
				Brain.IslandUntil = Obs.Now + RandRange(3.0f, 6.0f);
			}
			else if (Roll < 80.0f)
			{
				Brain.IslandMode = 3;
				Brain.IslandUntil = Obs.Now + RandRange(2.5f, 5.0f);
			}
			else if (Roll < 90.0f && Obs.bHasPickup)
			{
				Brain.IslandMode = 4;
				Brain.IslandUntil = Obs.Now + 25.0f;
			}
			else if (Obs.bHasGun)
			{
				Brain.IslandMode = 5;
				Brain.IslandUntil = Obs.Now + RandRange(3.0f, 6.0f);
			}

			static int Logged = 0;

			if (++Logged <= 40)
				LOG_INFO(LogBots, "BotIsland #{} picks activity {} for {:.1f}s", Id, Brain.IslandMode, Brain.IslandUntil - Obs.Now);

			if (Brain.IslandMode == 1)
			{
				Action.bStop = true;
				return true;
			}
		}

		switch (Brain.IslandMode)
		{
		case 1: // emote: stand still while it plays
			Action.bStop = true;
			return true;

		case 2: // look around
			Action.bStop = true;

			if (Obs.Now >= Brain.IslandNextTurn)
			{
				Brain.IslandNextTurn = Obs.Now + RandRange(0.8f, 1.8f);
				Brain.IslandYaw += RandRange(-110.0f, 110.0f);
			}

			Action.bFace = true;
			Action.Yaw = Brain.IslandYaw;
			return true;

		case 3: // hop about while wandering
			if (Obs.Now >= Brain.NextJumpAllowed)
			{
				Action.bJump = true;
				Brain.NextJumpAllowed = Obs.Now + RandRange(0.6f, 1.1f);
			}

			return false;

		case 4: // walk to a weapon lying on the ground and take it
			if (!Obs.bHasPickup)
			{
				Brain.IslandUntil = Obs.Now;
				return false;
			}

			if (Dist2D(Obs.Pos, Obs.PickupPos) < 130.0f)
			{
				Brain.TargetPickup = (AFortPickup*)Obs.PickupPtr;
				Action.bPickup = true;
				Action.bStop = true;
				Brain.IslandUntil = Obs.Now + 0.6f;
				return true;
			}

			MoveToward(Brain, Obs, Action, Obs.PickupPos, RunSpeed, Id);
			return true;

		case 5: // fire into the air
		{
			MaybeEquip(Brain, Obs, Action);
			Action.bStop = true;
			Action.bFace = true;
			Action.Yaw = Brain.IslandYaw;
			Action.bSetPitch = true;
			Action.Pitch = 12.0f;

			if (Obs.bHoldingGun && Obs.Now >= Brain.NextAttackTime)
			{
				Brain.TargetPawn = nullptr;
				Brain.TargetDist = 3000.0f;
				Action.bAttackRanged = true;
				Brain.NextAttackTime = Obs.Now + (std::max)(Obs.HeldInterval, 0.2f);
			}

			return true;
		}

		default:
			return false;
		}
	}

	// Hurt by somebody it cannot see: it stops and looks around, confused, until its sight finds the attacker; if that takes more than a moment
	// it boxes itself in with four walls (or runs, with no wood).
	inline bool DecideHitReaction(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		const float Total = Obs.Health + Obs.Shield;
		// the storm's 1 a second does not count; nor do the late storm's 5-10 when nobody is about
		const bool bInStorm = Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius;
		const bool bHurt = Brain.PrevTotal > 0.0f && Total < Brain.PrevTotal - 4.0f && !(bInStorm && !(Obs.bHasEnemy && Obs.EnemyDist < 4500.0f));
		Brain.HurtFrom = Brain.PrevTotal;
		Brain.PrevTotal = Total;

		// Whatever it is doing, being hit is remembered (this used to be set only while it walked about with nobody in sight, so a bot shot
		// by somebody it could see, or in the middle of a fight or a heal, did not count as provoked and could still roll to ignore them).
		if (bHurt)
		{
			Brain.HurtAt = Obs.Now;

			if (Obs.bHasEnemy && Obs.EnemyDist < 4500.0f)
			{
				Brain.FoughtPawn = Obs.EnemyPawn;
				Brain.FoughtUntil = Obs.Now + 90.0f;
			}
		}

		if (Brain.State != EBotBrainState::Ground || Obs.MovementMode != 1)
		{
			Brain.ConfusedUntil = 0.0f;
			Brain.bBoxPending = false;
			return false;
		}

		if (Obs.bHasEnemy && Obs.bEnemyVisible)
		{
			// found them: the normal fight logic takes over
			Brain.ConfusedUntil = 0.0f;
			Brain.bBoxPending = false;
			return false;
		}

		if (bHurt && Obs.Now > Brain.ConfusedUntil + 4.0f && !Brain.bBoxPending)
		{
			Brain.ConfusedUntil = Obs.Now + 1.5f;
			Brain.bBoxPending = true;
			Brain.LookDir = (rand() & 1) ? 1.0f : -1.0f;
			Brain.LookYaw = Brain.bHaveAim ? Brain.AimYaw : Brain.FaceYaw;

			// somebody it knows is near: that is who is shooting, so turn to them (a person feels which way the shots came from)
			if (Obs.bHasEnemy && Obs.EnemyDist < 4500.0f)
			{
				Brain.LookTarget = std::atan2(Obs.EnemyPos.Y - Obs.Pos.Y, Obs.EnemyPos.X - Obs.Pos.X) * RadToDeg + RandRange(-20.0f, 20.0f);
				Brain.bLookAtAttacker = true;
				Brain.EnemyLastSeen = Obs.Now; // no longer a stranger behind its back: the field of view no longer hides them
				Brain.IgnoreUntil = 0.0f;
			}
			else
			{
				Brain.bLookAtAttacker = false;
			}

			static int Hurt = 0;

			if (++Hurt <= 20)
				LOG_INFO(LogBots, "BotReact #{} was hit by somebody it cannot see ({:.0f} -> {:.0f}), looking around", Id, Brain.HurtFrom, Total);
		}

		if (Obs.Now < Brain.ConfusedUntil)
		{
			Action.bStop = true;
			if (Brain.bLookAtAttacker)
			{
				const float Diff = NormalizeAngle(Brain.LookTarget - Brain.LookYaw);
				Brain.LookYaw += (std::max)(-40.0f, (std::min)(40.0f, Diff));
			}
			else
			{
				Brain.LookYaw += Brain.LookDir * 230.0f * 0.16f;
			}

			Action.bFace = true;
			Action.Yaw = Brain.LookYaw;
			return true;
		}

		if (Brain.bBoxPending)
		{
			Brain.bBoxPending = false;

			if (Obs.WoodCount >= 10 && !(Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius))
			{
				Action.bBoxUp = true;
				Action.bStop = true;
				Brain.PauseUntil = Obs.Now + 5.0f;

				static int Boxed = 0;

				if (++Boxed <= 20)
					LOG_INFO(LogBots, "BotReact #{} could not see its attacker, boxing up ({} wood)", Id, Obs.WoodCount);
			}
			else
			{
				// nothing to build with: get away from here
				// prefer a direction the probes say is open (not into a railing or wall); ask for a fresh look
				Brain.bWantEscape = true;

				if (Obs.bHasEscape)
				{
					Brain.EscapeDir = Obs.EscapeDir;
				}
				else
				{
					const float Angle = RandRange(0.0f, 6.2831853f);
					Brain.EscapeDir = FVector{ std::cos(Angle), std::sin(Angle), 0.0f };
				}

				Brain.EscapeUntil = Obs.Now + 2.0f;

				Brain.bEscapePlanned = false;
				SteerDir(Action, Brain.EscapeDir.X, Brain.EscapeDir.Y, SprintSpeed);
			}

			return true;
		}

		return false;
	}

	// A wall between the bot and its enemy when it is hurt in a fight or hiding to heal.
	// height of whatever is under (X, Y) between two heights, or -1e9 when nothing
	inline float SurfaceZ(float X, float Y, float From, float To)
	{
		FVector Hit{};

		if (!TraceBlocked(gNavTracePawn, nullptr, FVector{ X, Y, From }, FVector{ X, Y, To }, nullptr, nullptr, nullptr, &Hit))
			return -1e9f;

		return Hit.Z;
	}

	enum EBotPiece { PieceWall = 0, PieceStair = 1, PieceFloor = 2, PieceRoof = 3 };

	// ---- building ramps ----
	// What players build most is ramps: to get up a cliff or onto a hill, to take the high ground on somebody above them, and with a wall in
	// front of each one when they are being shot at. A ramp fills a 512x512 building tile and rises 384 over it (a slope a pawn walks up).
	// A plan is a run of ramps along one grid axis: the bot stands at the foot, the next ramp goes down just ahead of it, it walks up, and so on.
	inline constexpr float TileSize = 512.0f;
	inline constexpr float TileRise = 384.0f;

	// Begin a run of Count ramps rising along the grid axis nearest (DirX, DirY). The first ramp sits StartTiles tiles ahead of the bot's tile
	// (0 or less: it first backs off to make room), its foot at BaseZ.
	inline void StartRampPlan(BotBrainState& Brain, const BotObservation& Obs, float DirX, float DirY, int Count, int StartTiles, float BaseZ, bool bWalls, const char* Why, int Id)
	{
		const bool bX = std::fabs(DirX) >= std::fabs(DirY);
		const float Dx = bX ? (DirX >= 0.0f ? 1.0f : -1.0f) : 0.0f;
		const float Dy = bX ? 0.0f : (DirY >= 0.0f ? 1.0f : -1.0f);
		const float Cx = std::round(Obs.Pos.X / TileSize) * TileSize;
		const float Cy = std::round(Obs.Pos.Y / TileSize) * TileSize;

		Brain.bRampPlan = true;
		Brain.RampDx = Dx;
		Brain.RampDy = Dy;
		Brain.RampTile0 = FVector{ Cx + Dx * TileSize * StartTiles, Cy + Dy * TileSize * StartTiles, BaseZ };
		Brain.RampStand = FVector{ Brain.RampTile0.X - Dx * (TileSize * 0.5f + 110.0f), Brain.RampTile0.Y - Dy * (TileSize * 0.5f + 110.0f), Obs.Pos.Z };
		Brain.RampPlaced = 0;
		Brain.RampWanted = Count;
		Brain.RampFails = 0;
		Brain.bRampWalls = bWalls;
		Brain.RampUntil = Obs.Now + 6.0f + Count * 2.5f;
		Brain.RampNextPiece = 0.0f;
		Brain.RampBest = -1e9f;
		Brain.RampProgressAt = Obs.Now;
		Brain.RampJumpAt = 0.0f;
		Brain.RampWhy = Why;

		// already past the stand point (the first ramp is ahead of it): straight to building
		const float Along = (Obs.Pos.X - Brain.RampStand.X) * Dx + (Obs.Pos.Y - Brain.RampStand.Y) * Dy;
		Brain.RampPhase = (Along >= -60.0f && std::fabs((Obs.Pos.X - Brain.RampStand.X) * Dy - (Obs.Pos.Y - Brain.RampStand.Y) * Dx) < 200.0f) ? 1 : 0;

		Brain.NavPath.clear();
		Brain.bNavWanted = false;
		Brain.EscapeUntil = 0.0f;
		Brain.SidestepUntil = 0.0f;
		Brain.AvoidUntil = 0.0f;

		static int Logged = 0;

		if (++Logged <= 60)
			LOG_INFO(LogBots, "BotRamp #{} {}: {} ramp(s){} rising towards ({:.0f}, {:.0f}) from ({:.0f}, {:.0f}, {:.0f}), first one {} tile(s) ahead, wood {}", Id, Why, Count, bWalls ? " with walls" : "",
				Dx, Dy, Brain.RampTile0.X, Brain.RampTile0.Y, BaseZ, StartTiles, Obs.WoodCount);
	}

	inline void EndRampPlan(BotBrainState& Brain, const BotObservation& Obs, const char* How, int Id)
	{
		static int Logged = 0;

		if (++Logged <= 60)
			LOG_INFO(LogBots, "BotRamp #{} ({}) ends: {} after {} of {} ramp(s), {:+.0f} high from the foot", Id, Brain.RampWhy, How, Brain.RampPlaced, Brain.RampWanted, Obs.Pos.Z - 88.0f - Brain.RampTile0.Z);

		Brain.bRampPlan = false;
		Brain.NextRampPlan = Obs.Now + RandRange(4.0f, 8.0f);
		Brain.StruggleRefTime = Obs.Now; // give the normal movement a fresh start
		Brain.StruggleRefPos = Obs.Pos;
	}

	// Something too steep to walk is between the bot and where it wants to go, and that place is higher up: ramp over it, the way a player
	// gets up a cliff. Looks along the grid axis nearest the goal for the first tile whose ground is well above the feet, measures how high
	// it goes, and backs off enough tiles that a straight run of ramps arrives at the top (each rises 384).
	inline bool TryClimbPlan(BotBrainState& Brain, const BotObservation& Obs, const FVector& Goal, const char* Why, int Id)
	{
		if (Obs.WoodCount < 20 || Obs.Now < Brain.NextRampPlan || Brain.bRampPlan || !gTraceBudget || gTracesFrame > gTraceBudget)
			return false;

		Brain.NextRampPlan = Obs.Now + 8.0f;

		const float Gx = Goal.X - Obs.Pos.X, Gy = Goal.Y - Obs.Pos.Y;
		const bool bX = std::fabs(Gx) >= std::fabs(Gy);
		const float Dx = bX ? (Gx >= 0.0f ? 1.0f : -1.0f) : 0.0f;
		const float Dy = bX ? 0.0f : (Gy >= 0.0f ? 1.0f : -1.0f);
		const float Cx = std::round(Obs.Pos.X / TileSize) * TileSize;
		const float Cy = std::round(Obs.Pos.Y / TileSize) * TileSize;
		const float FeetZ = Obs.Pos.Z - 88.0f;

		int CliffTile = -1;
		float TopZ = FeetZ;

		for (int k = 1; k <= 3; ++k)
		{
			// the far half of the tile: what the ramp has to arrive on
			const float Z = SurfaceZ(Cx + Dx * TileSize * k, Cy + Dy * TileSize * k, FeetZ + 1700.0f, FeetZ - 300.0f);

			if (Z < -1e8f)
				continue;

			if (CliffTile < 0 && Z > FeetZ + 150.0f)
				CliffTile = k;

			if (CliffTile > 0)
				TopZ = (std::max)(TopZ, Z);
		}

		const float Height = TopZ - FeetZ;

		if (CliffTile < 0 || Height > 1500.0f)
		{
			static int Logged = 0;

			if (++Logged <= 20)
				LOG_INFO(LogBots, "BotRamp #{} wants to climb ({}) but {} at ({:.0f}, {:.0f}, {:.0f})", Id, Why, CliffTile < 0 ? "sees nothing high to ramp onto ahead" : "the top is too high", Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z);

			return false;
		}

		const int Count = (std::max)(1, (std::min)(4, (int)std::ceil((Height + 30.0f) / TileRise)));

		if (Obs.WoodCount < Count * 10)
			return false;

		// the first ramp starts Count tiles before the high tile; if that is behind the bot it backs off first, along open ground
		const int Start = CliffTile - Count;

		if (Start < 1)
		{
			const float BackX = Cx + Dx * (TileSize * Start - TileSize * 0.5f - 110.0f), BackY = Cy + Dy * (TileSize * Start - TileSize * 0.5f - 110.0f);
			const float BackZ = SurfaceZ(BackX, BackY, FeetZ + 300.0f, FeetZ - 300.0f);

			if (BackZ < -1e8f || std::fabs(BackZ - FeetZ) > 150.0f
				|| TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, FeetZ + 60.0f }, FVector{ BackX, BackY, FeetZ + 60.0f }))
			{
				static int Logged = 0;

				if (++Logged <= 20)
					LOG_INFO(LogBots, "BotRamp #{} wants to climb {:.0f} ({}) but has no room to back off at ({:.0f}, {:.0f}, {:.0f})", Id, Height, Why, Obs.Pos.X, Obs.Pos.Y, Obs.Pos.Z);

				return false;
			}
		}

		StartRampPlan(Brain, Obs, Dx, Dy, Count, Start, FeetZ, false, Why, Id);
		return true;
	}

	// One tick of a ramp plan: walk to the start, then put each ramp down just ahead and walk up it. Overrides the movement Decide chose.
	inline void StepRampPlan(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		if (!Brain.bRampPlan)
			return;

		const bool bFight = Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat;

		if (Obs.Now > Brain.RampUntil)
			return EndRampPlan(Brain, Obs, "ran out of time", Id);

		if (Brain.RampFails >= 2)
			return EndRampPlan(Brain, Obs, "the game refused the pieces", Id);

		if (Brain.State == EBotBrainState::Skydive || Brain.State == EBotBrainState::Glide || Brain.State == EBotBrainState::Heal)
			return EndRampPlan(Brain, Obs, "something else came first", Id);

		if (bFight && Obs.bHasEnemy && Obs.EnemyDist < 350.0f)
			return EndRampPlan(Brain, Obs, "the enemy is on top of it", Id);

		const float Dx = Brain.RampDx, Dy = Brain.RampDy;
		const float Yaw = std::atan2(Dy, Dx) * RadToDeg;

		if (Brain.RampPhase == 0)
		{
			const float Gx = Brain.RampStand.X - Obs.Pos.X, Gy = Brain.RampStand.Y - Obs.Pos.Y;
			const float Dist = std::sqrt(Gx * Gx + Gy * Gy);

			if (Dist < 90.0f || Obs.Now - Brain.RampProgressAt > 5.0f)
			{
				Brain.RampPhase = 1;
				Brain.RampProgressAt = Obs.Now;
				Brain.RampBest = -1e9f;
			}
			else
			{
				SteerDir(Action, Gx / Dist, Gy / Dist, RunSpeed);
				Action.bStop = false;
				Action.MaxWalkSpeed = RunSpeed;
				return;
			}
		}

		// how far along the run it is: 0 at the foot of the first ramp, 512 at its top
		const float Along = (Obs.Pos.X - Brain.RampTile0.X) * Dx + (Obs.Pos.Y - Brain.RampTile0.Y) * Dy + TileSize * 0.5f;

		if (Along > Brain.RampBest + 40.0f)
		{
			Brain.RampBest = Along;
			Brain.RampProgressAt = Obs.Now;
		}

		if (Along > TileSize * Brain.RampWanted + 150.0f)
			return EndRampPlan(Brain, Obs, "walked off the top", Id);

		if (Obs.Now - Brain.RampProgressAt > 3.0f)
			return EndRampPlan(Brain, Obs, "got no further", Id);

		// next ramp: as soon as the bot is close to where it starts
		// one ramp at a time: the piece asked for last is put up when the bot has turned to it (StepBuildQueue), and only then is the next one due
		const bool bRampPending = std::any_of(Brain.BuildQueue.begin(), Brain.BuildQueue.end(), [](const BotBrainState::BuildOrder& Order) { return Order.bPlanRamp; });

		if (!bRampPending && Brain.RampPlaced < Brain.RampWanted && Along >= TileSize * Brain.RampPlaced - 420.0f && Obs.Now >= Brain.RampNextPiece)
		{
			if (Obs.WoodCount < 10)
				return EndRampPlan(Brain, Obs, "out of wood", Id);

			const int i = Brain.RampPlaced;
			const FVector At{ Brain.RampTile0.X + Dx * TileSize * i, Brain.RampTile0.Y + Dy * TileSize * i, Brain.RampTile0.Z + TileRise * i };
			Action.Pieces.push_back({ PieceStair, At, Yaw, true });

			// a wall at the top of the ramp, between the bot and whoever is shooting from up ahead
			if (Brain.bRampWalls && Obs.WoodCount >= 20)
				Action.Pieces.push_back({ PieceWall, FVector{ At.X + Dx * TileSize * 0.5f, At.Y + Dy * TileSize * 0.5f, At.Z }, Yaw, false }); // standing on the ramp's own level, the whole tile high

			Brain.RampNextPiece = Obs.Now + 0.3f;
		}

		// walk up; wait at the top of the last ramp built until the next one is down
		const bool bWait = Brain.RampPlaced < Brain.RampWanted && Along > TileSize * Brain.RampPlaced - 40.0f;

		if (bWait)
		{
			Action.bStop = true;
			Action.bDriveXY = false;
			Brain.RampProgressAt = (std::max)(Brain.RampProgressAt, Obs.Now - 1.5f); // waiting for a piece is not being stuck
		}
		else
		{
			SteerDir(Action, Dx, Dy, RunSpeed);
			Action.bStop = false;

			// the ramp's foot can stand a little proud of uneven ground: hop onto it when it stops making headway there
			if (Obs.MovementMode == 1 && Obs.Now - Brain.RampProgressAt > 0.8f && Obs.Now >= Brain.RampJumpAt)
			{
				Action.bJump = true;
				Brain.RampJumpAt = Obs.Now + 0.9f;
			}
		}

		Action.MaxWalkSpeed = RunSpeed;

		if (!bFight)
		{
			Action.bFace = true;
			Action.Yaw = Yaw;
		}
	}

	// Is the first couple of tiles towards (DirX, DirY) open enough to build ramps into (no wall at knee or head height)?
	inline bool RampLaneClear(const BotObservation& Obs, float DirX, float DirY)
	{
		const bool bX = std::fabs(DirX) >= std::fabs(DirY);
		const float Dx = bX ? (DirX >= 0.0f ? 1.0f : -1.0f) : 0.0f, Dy = bX ? 0.0f : (DirY >= 0.0f ? 1.0f : -1.0f);
		const float FeetZ = Obs.Pos.Z - 88.0f;
		const FVector End{ Obs.Pos.X + Dx * 900.0f, Obs.Pos.Y + Dy * 900.0f, 0.0f };

		return !TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, FeetZ + 60.0f }, FVector{ End.X, End.Y, FeetZ + 60.0f })
			&& !TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ Obs.Pos.X, Obs.Pos.Y, FeetZ + 420.0f }, FVector{ End.X, End.Y, FeetZ + 420.0f });
	}

	// What to build, from the situation. A run of ramps is its own plan (StepRampPlan); single walls and boxes are one-off actions.
	//  - an enemy well above it: ramp up towards them to take the height away
	//  - a straight fight at range: skilled, aggressive bots ramp-rush (ramps with a wall at the top of each)
	//  - being shot: a wall in the line of fire, or (the better ones) a ramp with a wall on it, which is cover and height at once
	//  - hiding or healing with the enemy about: a wall towards where they were
	inline void MaybeBuild(BotBrainState& Brain, const BotObservation& Obs, BotAction& Action, int Id)
	{
		if (Brain.bRampPlan || Obs.MovementMode != 1 || Obs.WoodCount < 30)
			return;

		const float Skill = Brain.Skill < 0.0f ? 30.0f : Brain.Skill;
		const float Aggression = Brain.Aggression < 0.0f ? 0.3f : Brain.Aggression;
		const bool bFighting = Brain.State == EBotBrainState::Engage && Obs.bHasEnemy && Obs.bEnemyVisible && Obs.EnemyDist > 350.0f && Obs.EnemyDist < 4000.0f;
		const bool bUnderFire = Obs.Now - Brain.HurtAt < 3.0f && Obs.bHasEnemy && Obs.EnemyDist > 250.0f && Obs.EnemyDist < 4500.0f && Obs.Now - Brain.StateSince > 0.5f
			&& (Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat);
		const float Above = Obs.bHasEnemy ? Obs.EnemyPos.Z - Obs.Pos.Z : 0.0f;
		const float ToEnemyX = Obs.EnemyPos.X - Obs.Pos.X, ToEnemyY = Obs.EnemyPos.Y - Obs.Pos.Y;

		if (bFighting && Obs.bHasGun && Obs.Now >= Brain.NextRampPlan && gTracesFrame <= gTraceBudget)
		{
			// the enemy has the height: take it back (most players do, the better ones nearly always)
			if (Above > 250.0f && Obs.EnemyDist < 2200.0f && Obs.WoodCount >= 30)
			{
				Brain.NextRampPlan = Obs.Now + RandRange(3.0f, 6.0f);

				if (RandRange(0.0f, 1.0f) < 0.35f + Skill / 160.0f && RampLaneClear(Obs, ToEnemyX, ToEnemyY))
				{
					const int Count = (std::max)(2, (std::min)(4, (int)std::ceil((Above + 150.0f) / TileRise)));
					StartRampPlan(Brain, Obs, ToEnemyX, ToEnemyY, (std::min)(Count, Obs.WoodCount / 10), 1, Obs.Pos.Z - 88.0f, bUnderFire && Obs.WoodCount >= Count * 20, "takes the height from an enemy above", Id);
					return;
				}
			}
			// level ground, mid range: a ramp rush now and then, mostly from the skilled and the aggressive
			else if (std::fabs(Above) < 250.0f && Obs.EnemyDist > 600.0f && Obs.EnemyDist < 1800.0f && Obs.WoodCount >= 60)
			{
				Brain.NextRampPlan = Obs.Now + RandRange(6.0f, 12.0f);

				if (RandRange(0.0f, 1.0f) < (Skill / 100.0f) * (0.25f + Aggression) * 0.5f && RampLaneClear(Obs, ToEnemyX, ToEnemyY))
				{
					StartRampPlan(Brain, Obs, ToEnemyX, ToEnemyY, RandRange(0.0f, 1.0f) < 0.5f ? 2 : 3, 1, Obs.Pos.Z - 88.0f, true, "ramp-rushes its enemy", Id);
					return;
				}
			}
		}

		if (Obs.Now < Brain.NextBuild)
			return;

		const bool bHurtFighting = (bFighting && (Obs.Health + Obs.Shield) < 85.0f) || bUnderFire;
		const bool bHiding = (Brain.State == EBotBrainState::Heal || Brain.State == EBotBrainState::Retreat) && Obs.Now - Brain.EnemyLastSeen < 10.0f;

		// shot at from range with wood to spare: wall + ramp (cover and height) instead of a bare wall, more often the better the bot
		if (bUnderFire && Brain.State == EBotBrainState::Engage && Obs.bHasGun && Obs.EnemyDist > 500.0f && Obs.WoodCount >= 40 && Obs.Now >= Brain.NextRampPlan
			&& RandRange(0.0f, 1.0f) < 0.15f + Skill / 250.0f && gTracesFrame <= gTraceBudget && RampLaneClear(Obs, ToEnemyX, ToEnemyY))
		{
			Brain.NextBuild = Obs.Now + RandRange(3.0f, 5.0f);
			StartRampPlan(Brain, Obs, ToEnemyX, ToEnemyY, 1, 1, Obs.Pos.Z - 88.0f, true, "builds a wall and ramp under fire", Id);
			return;
		}

		if (bHurtFighting || bHiding || (bFighting && RandRange(0.0f, 1.0f) < 0.05f))
		{
			Action.bBuildWall = true;
			Action.BuildToward = (bFighting || bUnderFire) ? Obs.EnemyPos : Brain.EnemyLastPos;
			Brain.NextBuild = Obs.Now + (bUnderFire ? RandRange(2.5f, 4.5f) : RandRange(5.0f, 9.0f));
		}
	}

	inline BotAction Decide(BotBrainState& Brain, const BotObservation& Obs, int Id)
	{
		BotAction Action;

		if (DecideHitReaction(Brain, Obs, Action, Id))
		{
			Brain.LastDecisionPos = Obs.Pos;
			Brain.LastDecisionTime = Obs.Now;
			return Action;
		}

		MaybeBreakOut(Brain, Obs, Id);

		// A rocket is coming: get out of its path. Sideways, fast, with a jump, whatever else the bot was doing.
		if (Obs.bProjectile && Obs.MovementMode == 1 && Brain.State != EBotBrainState::Skydive && Brain.State != EBotBrainState::Glide && Brain.State != EBotBrainState::Wander)
		{
			if (Obs.Now >= Brain.DodgeUntil)
			{
				float Px = -Obs.ProjDirY, Py = Obs.ProjDirX;

				if (RandRange(0.0f, 1.0f) < 0.5f)
				{
					Px = -Px;
					Py = -Py;
				}

				Brain.DodgeDirX = Px;
				Brain.DodgeDirY = Py;

				static int Dodged = 0;

				if (++Dodged <= 25)
					LOG_INFO(LogBots, "BotDodge #{} sees something flying at it, {:.1f}s away, and runs sideways", Id, Obs.ProjTime);
			}

			Brain.DodgeUntil = Obs.Now + 1.3f;
		}

		if (Brain.DodgeUntil > Obs.Now)
		{
			SteerDir(Action, Brain.DodgeDirX, Brain.DodgeDirY, SprintSpeed);
			Action.MaxWalkSpeed = SprintSpeed;

			if (Obs.Now >= Brain.NextFightJump)
			{
				Action.bJump = true;
				Brain.NextFightJump = Obs.Now + 0.8f;
			}

			Brain.LastDecisionPos = Obs.Pos;
			Brain.LastDecisionTime = Obs.Now;
			return Action;
		}

		switch (Brain.State)
		{
		case EBotBrainState::Idle:
			Action.bStop = Obs.MovementMode == 1;
			break;

		case EBotBrainState::Wander:
			if (!Brain.bHasAnchor)
			{
				Brain.Anchor = Obs.Pos;
				Brain.bHasAnchor = true;
			}

			if (!DecideIsland(Brain, Obs, Action, Id))
				DecideWalking(Brain, Obs, Action, true, Id);

			break;

		case EBotBrainState::Skydive:
		case EBotBrainState::Glide:
		{
			const bool bGlide = Brain.State == EBotBrainState::Glide;
			const float Distance = Dist2D(Obs.Pos, Brain.Target);

			if (Obs.MovementMode == 1 && Obs.Now - Brain.StateSince > 1.0f)
			{
				SetState(Brain, EBotBrainState::Ground, Obs.Now, Id);
				Action.bStop = true;
				break;
			}

			if (!bGlide && Obs.bHaveHeight && Obs.HeightAboveGround < GlideDeployHeight)
			{
				SetState(Brain, EBotBrainState::Glide, Obs.Now, Id);
				Brain.bHasTarget = true; // SetState clears it; keep the landing target
			}

			// A chest indoors has a roof over it, and gliding straight at it puts the bot on that roof (the Dusty Divot diner kept bots for
			// minutes, see RoofCheck). Once, on the way in: if something stands well above the target, land on open ground beside the building
			// at the target's own height instead, and walk in through the door like everybody else.
			if (!Brain.bLandingChecked && Distance < 3000.0f && Brain.bHasTarget && gTracesFrame <= gTraceBudget)
			{
				Brain.bLandingChecked = true;
				FVector Over{};

				if (TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ Brain.Target.X, Brain.Target.Y, Brain.Target.Z + 3000.0f }, FVector{ Brain.Target.X, Brain.Target.Y, Brain.Target.Z - 200.0f }, nullptr, nullptr, nullptr, &Over)
					&& Over.Z > Brain.Target.Z + 250.0f)
				{
					FVector BestSpot{};
					float BestDist = 1e9f;
					const float Spin = RandRange(0.0f, 30.0f);

					for (float Reach : { 600.0f, 1000.0f, 1500.0f })
					{
						for (int k = 0; k < 12 && BestDist > 1e8f; ++k)
						{
							const float A = (Spin + k * 30.0f) * DegToRad;
							const float X = Brain.Target.X + std::cos(A) * Reach, Y = Brain.Target.Y + std::sin(A) * Reach;
							FVector Ground{};

							if (!TraceBlocked(Obs.SelfPawn, Obs.SelfWeapon, FVector{ X, Y, Brain.Target.Z + 3000.0f }, FVector{ X, Y, Brain.Target.Z - 600.0f }, nullptr, nullptr, nullptr, &Ground))
								continue;

							if (std::fabs(Ground.Z - Brain.Target.Z) > 200.0f)
								continue; // another roof, or a slope well above or below the door

							const float D = Dist2D(FVector{ X, Y, 0.0f }, Obs.Pos);

							if (D < BestDist)
							{
								BestDist = D;
								BestSpot = FVector{ X, Y, Ground.Z + 90.0f };
							}
						}

						if (BestDist < 1e8f)
							break;
					}

					static int Logged = 0;

					if (BestDist < 1e8f)
					{
						if (++Logged <= 30)
							LOG_INFO(LogBots, "BotLand #{} target ({:.0f}, {:.0f}, {:.0f}) has a roof over it at z {:.0f}: lands on open ground at ({:.0f}, {:.0f}, {:.0f}) instead", Id, Brain.Target.X, Brain.Target.Y, Brain.Target.Z, Over.Z, BestSpot.X, BestSpot.Y, BestSpot.Z);

						Brain.Target = BestSpot;
						Brain.LandingTarget = BestSpot;
					}
					else if (++Logged <= 30)
					{
						LOG_INFO(LogBots, "BotLand #{} target ({:.0f}, {:.0f}, {:.0f}) has a roof over it at z {:.0f} and no open ground near it", Id, Brain.Target.X, Brain.Target.Y, Brain.Target.Z, Over.Z);
					}
				}
			}

			float Speed = bGlide ? GlideHorizontal : SkydiveHorizontal;

			if (Distance < 2500.0f)
				Speed *= 0.4f;

			if (Distance > 200.0f)
			{
				const float Dx = Brain.Target.X - Obs.Pos.X;
				const float Dy = Brain.Target.Y - Obs.Pos.Y;
				const float Length = std::sqrt(Dx * Dx + Dy * Dy);

				if (Length > 1.0f)
					SteerDir(Action, Dx / Length, Dy / Length, Speed);
			}

			Action.bDriveZ = true;
			Action.VerticalSpeed = (Brain.State == EBotBrainState::Glide) ? GlideVertical : SkydiveVertical;
			Action.MaxWalkSpeed = SkydiveHorizontal + 400.0f;
			break;
		}

		case EBotBrainState::Ground:
		{
			// somebody with no gun only picks a fight with somebody close; at a distance it carries on looting
			// Seeing somebody is not the same as wanting a fight. Once per sighting the bot decides whether this one is worth it: a fighter with a
			// gun usually is, somebody still looting or holding a pickaxe mostly walks the other way. Being shot, or somebody right on top of
			// it, settles the matter.
			// A decision to leave somebody alone does not survive the situation changing: they close in, point a gun at it, fire close by,
			// or turn up again after being out of sight. Then it thinks again straight away. (A bot in the same room as a real player kept
			// exploring because of a roll made earlier, 2026-11-09.)
			if (Brain.IgnoreUntil > Obs.Now && Obs.bHasEnemy)
			{
				const char* Why = nullptr;

				if (!Obs.bEnemyVisible)
					Brain.bIgnoredLostSight = true;
				else if (Brain.bIgnoredLostSight)
					Why = "came back into view";

				if (!Why && Obs.bEnemyVisible && Brain.IgnoredAtDist > 0.0f && Obs.EnemyDist < Brain.IgnoredAtDist * 0.5f)
					Why = "closed in";

				if (!Why && Obs.bEnemyVisible && Obs.bEnemyAimingAtMe)
					Why = "aims at it";

				if (!Why && Obs.bHeardSound && Obs.SoundTime > Brain.IgnoredAt && Dist2D(Obs.SoundPos, Obs.Pos) < 1500.0f)
					Why = "fired close by";

				// let go while it had nothing to fight with: now it has
				if (!Why && Obs.bEnemyVisible && Obs.bHasGun && Brain.bIgnoredUnarmed)
					Why = "has a gun now";

				// somebody it was fighting a moment ago
				if (!Why && Obs.bEnemyVisible && Obs.EnemyPawn && Obs.EnemyPawn == Brain.FoughtPawn && Obs.Now < Brain.FoughtUntil && Obs.bHasGun)
					Why = "was fighting them";

				if (Why)
				{
					static int Logged = 0;

					if (++Logged <= 40)
						LOG_INFO(LogBots, "BotAware #{} thinks again about the enemy it ignored ({}, {:.0f}u, human={})", Id, Why, Obs.EnemyDist, Obs.bEnemyIsHuman);

					Brain.IgnoreUntil = 0.0f;
					Brain.DecidedAt = -100.0f;
				}
			}

			// a real player in plain view this close is never ignored: an armed bot fights, an unarmed one goes at them with the pickaxe or
			// runs, by its own nerve (ShouldFlee)
			const bool bCloseHuman = Obs.bHasEnemy && Obs.bEnemyIsHuman && Obs.bEnemyVisible && Obs.EnemyDist < 800.0f;

			// the governor paces bot-against-bot fights only: towards a real player a bot keeps its own temperament
			const float Governor = Obs.bEnemyIsHuman ? 1.0f : Obs.Governor;

			if (Obs.bHasEnemy && Obs.bEnemyVisible && Obs.MovementMode == 1 && Obs.Now >= Brain.IgnoreUntil && Obs.Now - Brain.DecidedAt > 25.0f
				&& Obs.Now - Brain.HurtAt > 10.0f && Obs.EnemyDist > 450.0f && !bCloseHuman)
			{
				Brain.DecidedAt = Obs.Now;
				float Appetite = Brain.Aggression * (Obs.bHasGun ? 1.0f : 0.2f) * (Obs.EnemyDist < 1500.0f ? 1.4f : 1.0f) * (Brain.bStrong ? (std::max)(Governor, 0.6f) : Governor);

				// a real player is the interesting target: bots notice them and go for them far more readily than for each other
				if (Obs.bEnemyIsHuman && Obs.bHasGun)
					Appetite = (std::max)(Appetite * 2.5f, 0.7f);

				if (RandRange(0.0f, 1.0f) > Appetite)
				{
					Brain.IgnoreUntil = Obs.Now + RandRange(20.0f, 45.0f);
					Brain.IgnoredAtDist = Obs.EnemyDist;
					Brain.IgnoredAt = Obs.Now;
					Brain.bIgnoredLostSight = false;
					Brain.bIgnoredUnarmed = !Obs.bHasGun;
				}
			}

			// the one it was fighting: back at them, armed, without a fresh roll (ShouldFlee below still decides fight or run)
			const bool bGrudge = Obs.bHasEnemy && Obs.bEnemyVisible && Obs.EnemyPawn && Obs.EnemyPawn == Brain.FoughtPawn && Obs.Now < Brain.FoughtUntil && Obs.bHasGun;

			// out of sight: once patched up and armed, it goes looking where it last saw them instead of back to its looting
			if (!bGrudge && Brain.FoughtPawn && Obs.Now < Brain.FoughtUntil && Obs.bHasGun && Obs.Health + Obs.Shield >= 70.0f && !(Obs.bHasEnemy && Obs.bEnemyVisible)
				&& Obs.Now - Brain.EnemyLastSeen < 30.0f && Obs.Now >= Brain.NextGrudgeHunt && Brain.TargetKind != EBotTarget::Storm && Brain.TargetKind != EBotTarget::Investigate
				&& Dist2D(Obs.Pos, Brain.EnemyLastPos) > 400.0f && Dist2D(Obs.Pos, Brain.EnemyLastPos) < 5000.0f
				&& !(Obs.bZoneKnown && Dist2D(Brain.EnemyLastPos, Obs.ZoneCenter) > Obs.ZoneRadius - 300.0f))
			{
				Brain.NextGrudgeHunt = Obs.Now + 15.0f;
				Brain.Target = Brain.EnemyLastPos;
				Brain.TargetKind = EBotTarget::Investigate;
				Brain.bHasTarget = true;
				Brain.TargetPickedAt = Obs.Now;
				Brain.NavPath.clear();
				Brain.bNavWanted = false;

				static int Logged = 0;

				if (++Logged <= 30)
					LOG_INFO(LogBots, "BotAware #{} goes back for the enemy it was fighting ({:.0f}u to where it last saw them, {:.0f}s ago)", Id, Dist2D(Obs.Pos, Brain.EnemyLastPos), Obs.Now - Brain.EnemyLastSeen);
			}

			const bool bProvoked = Obs.Now - Brain.HurtAt < 10.0f || bCloseHuman || bGrudge
				|| (Obs.bHasEnemy && Obs.EnemyDist < 450.0f && (Obs.bHasGun || Obs.EnemyWeaponScore > 30.0f) && (Brain.Aggression > 0.25f || Governor > 0.5f));

			if (bCloseHuman && !(Obs.bHasEnemy && Obs.Now - Brain.HurtAt < 10.0f))
			{
				static int Logged = 0;
				static void* LastPawn = nullptr;

				if (LastPawn != Brain.Controller && ++Logged <= 40)
				{
					LastPawn = Brain.Controller;
					LOG_INFO(LogBots, "BotAware #{} reacts to a real player {:.0f}u away (armed={})", Id, Obs.EnemyDist, Obs.bHasGun);
				}
			}
			const float Range = Obs.bHasGun ? EngageRange * 0.75f : 700.0f;

			if (Obs.bHasEnemy && Obs.bEnemyVisible && Obs.EnemyDist < (bProvoked ? EngageRange : Range) && Obs.MovementMode == 1 && (bProvoked || Obs.Now >= Brain.IgnoreUntil))
			{
				Brain.EnemyLastPos = Obs.EnemyPos;
				Brain.EnemyLastSeen = Obs.Now;

				if (ShouldFlee(Brain, Obs))
				{
					SetState(Brain, EBotBrainState::Retreat, Obs.Now, Id);
					DecideRetreat(Brain, Obs, Action, Id);
				}
				else
				{
					SetState(Brain, EBotBrainState::Engage, Obs.Now, Id);
					DecideEngage(Brain, Obs, Action, Id);
				}
			}
			else if (Obs.bNeedsHeal && Obs.MovementMode == 1 && !(Obs.bHasEnemy && Obs.EnemyDist < 1200.0f)
				&& !(Obs.bZoneKnown && Dist2D(Obs.Pos, Obs.ZoneCenter) > Obs.ZoneRadius - StormMargin()))
			{
				SetState(Brain, EBotBrainState::Heal, Obs.Now, Id);
				DecideHeal(Brain, Obs, Action, Id);
			}
			else
			{
				DecideWalking(Brain, Obs, Action, false, Id);
			}

			break;
		}

		case EBotBrainState::Engage:
			DecideEngage(Brain, Obs, Action, Id);
			break;

		case EBotBrainState::Retreat:
			DecideRetreat(Brain, Obs, Action, Id);
			break;

		case EBotBrainState::Heal:
			DecideHeal(Brain, Obs, Action, Id);
			break;
		}

		MaybeBuild(Brain, Obs, Action, Id);
		StepRampPlan(Brain, Obs, Action, Id);

		Brain.LastDecisionPos = Obs.Pos;
		Brain.LastDecisionTime = Obs.Now;

		return Action;
	}

	// ---- landing ----
	// Pick where this bot wants to land: a POI near the bus route, spread out by the per-POI caps below.
	inline void ChooseLanding(BotBrainState& Brain, const FVector& BusLocation, int Id, float BusYaw = 1e9f)
	{
		// how far a spot is from the line the bus flies along (a spot behind the bus counts as far away)
		auto OffRoute = [&](const FVector& Spot) -> float
		{
			if (BusYaw > 1e8f)
				return 0.0f;

			const float Dx = std::cos(BusYaw * DegToRad);
			const float Dy = std::sin(BusYaw * DegToRad);
			const float Rx = Spot.X - BusLocation.X;
			const float Ry = Spot.Y - BusLocation.Y;
			const float Along = Rx * Dx + Ry * Dy;
			const float Across = std::fabs(-Rx * Dy + Ry * Dx);
			return Along < -3000.0f ? 1e6f - Along : Across;
		};

		Brain.bLandingChosen = true;
		Brain.PrevBusDist = 1e9f;
		Brain.JumpAt = -1.0f;

		std::vector<int> PoiChoices;

		for (size_t i = 0; i < gPois.size(); ++i)
		{
			if (gPois[i].Count >= 6)
				PoiChoices.push_back((int)i);
		}

		// bots land close to the bus's route so they are easy to follow: POIs within 30000 units of it, else the nearest few
		if (BusYaw < 1e8f && !PoiChoices.empty())
		{
			std::vector<int> Near;
			int NearestPoi = PoiChoices[0];

			for (int Poi : PoiChoices)
			{
				if (OffRoute(gPois[Poi].Center) < OffRoute(gPois[NearestPoi].Center))
					NearestPoi = Poi;

				if (OffRoute(gPois[Poi].Center) <= 45000.0f)
					Near.push_back(Poi);
			}

			PoiChoices = Near.empty() ? std::vector<int>{ NearestPoi } : Near;
		}

		int Poi = -1;

		if (!PoiChoices.empty())
		{
			// big POIs take more bots than small ones (about one bot per three chests, at least four), and a full POI is skipped while others have room
			std::vector<int> Open;

			for (int Candidate : PoiChoices)
			{
				const int Room = (std::max)(4, gPois[Candidate].Count / 3);

				if (gLandingCount[Candidate] < Room)
					Open.push_back(Candidate);
			}

			if (Open.empty())
			{
				// every POI along the route is full: take any POI on the map that still has room, else the least crowded one
				int Least = -1;

				for (size_t i = 0; i < gPois.size(); ++i)
				{
					if (gPois[i].Count < 6)
						continue;

					if (gLandingCount[(int)i] < (std::max)(4, gPois[i].Count / 3))
						Open.push_back((int)i);

					if (Least == -1 || gLandingCount[(int)i] < gLandingCount[Least])
						Least = (int)i;
				}

				if (Open.empty() && Least != -1)
					Open.push_back(Least);
			}

			Poi = Open.empty() ? PoiChoices[rand() % PoiChoices.size()] : Open[rand() % Open.size()];
			++gLandingCount[Poi];
		}

		if (Poi != -1)
		{
			std::vector<size_t> InPoi;

			for (size_t i = 0; i < gChests.size(); ++i)
			{
				if (gChestPoi[i] == Poi)
					InPoi.push_back(i);
			}

			if (!InPoi.empty())
			{
				Brain.LandingTarget = gChests[InPoi[rand() % InPoi.size()]];
				LOG_INFO(LogBots, "BotBrain #{} will land at POI '{}' ({:.0f}, {:.0f})", Id, gPois[Poi].Name, Brain.LandingTarget.X, Brain.LandingTarget.Y);
				return;
			}
		}

		if (!gChests.empty())
			Brain.LandingTarget = gChests[rand() % gChests.size()];
		else
			Brain.LandingTarget = PickWanderPoint(BusLocation, 15000.0f);

		LOG_INFO(LogBots, "BotBrain #{} will land at ({:.0f}, {:.0f}) (no POI data)", Id, Brain.LandingTarget.X, Brain.LandingTarget.Y);
	}

	// Called every tick while the bus flies. True when the bot should leave now: close enough to its landing target to reach it
	// (a free-falling bot covers about 1500 units a second horizontally, the bus about 7800), or the bus has just passed it.
	inline bool ShouldJump(BotBrainState& Brain, const FVector& BusLocation, float Now, int Id, float BusYaw = 1e9f)
	{
		if (!Brain.bLandingChosen)
			ChooseLanding(Brain, BusLocation, Id, BusYaw);

		const float Distance = Dist2D(BusLocation, Brain.LandingTarget);

		if (Brain.JumpAt < 0.0f)
		{
			const bool bInReach = Distance <= JumpReach;
			const bool bReceding = Brain.PrevBusDist < 1e8f && Distance > Brain.PrevBusDist + 500.0f; // closest approach is behind us

			if (bInReach || bReceding)
			{
				// the bus never gets near enough for this target: land at the nearest chest to where we are instead
				if (bReceding && !bInReach && Brain.PrevBusDist > MaxReach && !gChests.empty())
				{
					float Best = 1e30f;

					for (auto& Chest : gChests)
					{
						const float ChestDistance = Dist2D(BusLocation, Chest);

						if (ChestDistance < Best)
						{
							Best = ChestDistance;
							Brain.LandingTarget = Chest;
						}
					}

					LOG_INFO(LogBots, "BotBrain #{} target was out of reach ({:.0f}); landing at the nearest chest ({:.0f}, {:.0f})", Id, Brain.PrevBusDist, Brain.LandingTarget.X, Brain.LandingTarget.Y);
				}

				// Bots leave the bus one after another: 62 jumped within 4 seconds in a test and the client then had to build that many
				// players at once (130 ms hitches, fifteen in a row). About three a second keeps the client smooth.
				static float NextJumpSlot = 0.0f;
				const float Slot = (std::max)(Now, NextJumpSlot);
				NextJumpSlot = Slot + 0.35f;
				Brain.JumpAt = Slot + RandRange(0.0f, 0.15f);
				LOG_INFO(LogBots, "BotBrain #{} jumping in {:.1f}s (bus {:.0f} from target{})", Id, Brain.JumpAt - Now, Distance, bReceding && !bInReach ? ", closest approach" : "");
			}
		}

		Brain.PrevBusDist = Distance;

		return Brain.JumpAt >= 0.0f && Now >= Brain.JumpAt;
	}

	// ---- combat ----
	inline bool PathContains(UObject* Object, const char* Needle)
	{
		return Object && Object->GetPathName().find(Needle) != std::string::npos;
	}

	// Players jump and sprint by activating abilities (the bots have them granted): FortGameplayAbility_Jump gives the real jump (the velocity, the animation,
	// the jump counting) and FortGameplayAbility_Sprint the sprint state other players' screens animate from. Calling Character.Jump instead mostly did nothing.
	inline bool ActivateAbilityNamed(AFortPlayerStateAthena* PlayerState, const char* Needle, UObject** OutInstance = nullptr, bool bResetActiveCount = false)
	{
		if (!PlayerState)
			return false;

		auto ASC = PlayerState->GetAbilitySystemComponent();

		if (!ASC)
			return false;

		auto Container = ASC->GetActivatableAbilities();

		if (!Container)
			return false;

		auto& Items = Container->GetItems();
		int Chosen = -1;

		for (int i = 0; i < Items.Num(); ++i)
		{
			auto& Spec = Items.At(i, FGameplayAbilitySpec::GetStructSize());
			auto Ability = Spec.GetAbility();

			if (Ability && Ability->GetPathName().find(Needle) != std::string::npos)
			{
				Chosen = i;
				break;
			}
		}

		if (Chosen == -1)
			return false;

		auto& Spec = Items.At(Chosen, FGameplayAbilitySpec::GetStructSize());
		FGameplayAbilitySpecHandle Handle = Spec.GetHandle();

		static auto PredictionStruct = FindObject<UStruct>(L"/Script/GameplayAbilities.PredictionKey");
		static auto InputPressedOffset = FindOffsetStruct("/Script/GameplayAbilities.GameplayAbilitySpec", "InputPressed");
		static auto CurrentOffset = FindOffsetStruct("/Script/GameplayAbilities.PredictionKey", "Current");
		static auto BaseOffset = FindOffsetStruct("/Script/GameplayAbilities.PredictionKey", "Base", false);
		static int16_t KeyCounter = 20000;

		if (!PredictionStruct)
			return false;

		const int PredictionSize = PredictionStruct->GetPropertiesSize();
		std::vector<uint8_t> KeyBytes(PredictionSize + 16, 0);

		if (++KeyCounter > 32000)
			KeyCounter = 20000;

		*(int16_t*)(KeyBytes.data() + CurrentOffset) = KeyCounter;

		if (BaseOffset != -1)
			*(int16_t*)(KeyBytes.data() + BaseOffset) = KeyCounter;

		SetBitfield((void*)((__int64)&Spec + InputPressedOffset), 1, false);
		SetBitfield((void*)((__int64)&Spec + InputPressedOffset), 1, true);

		// an ability that is not instanced has nothing to end: its count on the spec only goes up, and at 255 the game refuses (and warns about) every activation
		static auto ActiveCountOffset = FindOffsetStruct("/Script/GameplayAbilities.GameplayAbilitySpec", "ActiveCount", false);

		if (bResetActiveCount && ActiveCountOffset != -1)
			*(uint8_t*)((__int64)&Spec + ActiveCountOffset) = 0;

		UObject* Instance = nullptr;
		bool bActivated = false;

		if (PredictionSize == 0x10 && UAbilitySystemComponent::InternalTryActivateAbilityOriginal)
		{
			PadHex10 PredictionKey{};
			memcpy(&PredictionKey, KeyBytes.data(), sizeof(PredictionKey));
			bActivated = UAbilitySystemComponent::InternalTryActivateAbilityOriginal(ASC, Handle, PredictionKey, &Instance, nullptr, nullptr);
		}
		else if (PredictionSize == 0x18 && UAbilitySystemComponent::InternalTryActivateAbilityOriginal2)
		{
			PadHex18 PredictionKey{};
			memcpy(&PredictionKey, KeyBytes.data(), sizeof(PredictionKey));
			bActivated = UAbilitySystemComponent::InternalTryActivateAbilityOriginal2(ASC, Handle, PredictionKey, &Instance, nullptr, nullptr);
		}

		if (OutInstance)
			*OutInstance = Instance;

		return bActivated;
	}

	inline void BotJump(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, BotBrainState* Brain = nullptr)
	{
		if (Brain)
			Brain->JumpWatch = 4;
		static int Tried = 0, Worked = 0;
		static auto EndFn = FindObject<UFunction>(L"/Script/GameplayAbilities.GameplayAbility.K2_EndAbility");

		if (Brain && Brain->JumpInstance)
		{
			if (EndFn && Brain->JumpInstance->IsValidLowLevel())
				Brain->JumpInstance->ProcessEvent(EndFn);

			Brain->JumpInstance = nullptr;
		}

		UObject* Instance = nullptr;
		const bool bWorked = ActivateAbilityNamed(PlayerState, "FortGameplayAbility_Jump", &Instance, true);
		++Tried;
		Worked += bWorked;

		if (Brain)
		{
			Brain->JumpInstance = Instance;
			Brain->JumpEndAt = -1.0f;
		}

		if (Tried == 20 || Tried == 200)
			LOG_INFO(LogBots, "BotJump: {} of {} jump ability activations worked (instance {})", Worked, Tried, (void*)Instance);

		if (!bWorked)
		{
			static auto JumpFn = FindObject<UFunction>(L"/Script/Engine.Character.Jump");

			if (JumpFn)
				Pawn->ProcessEvent(JumpFn);
		}
	}

	// Activates a weapon ability the way Reboot's server RPC handler does for a client: InputPressed set on the spec and a
	// valid prediction key. A zeroed key is rejected for LocalPredicted abilities when the caller isn't "local".
	inline bool Attack(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, bool bRanged, int Id, int* OutHandle = nullptr, std::vector<uint8_t>* OutKey = nullptr, bool bConsumable = false)
	{
		if (!PlayerState)
			return false;

		if (!bRanged && !bConsumable && !Pawn->GetCurrentWeapon() && Brain.PickaxeDef)
		{
			Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Brain.PickaxeDef, Brain.PickaxeGuid);
			LOG_INFO(LogBots, "BotCombat #{} equipped its pickaxe", Id);
		}

		// a client lets go of the trigger between shots; a held ability cannot be activated again (most activations were failing for that reason)
		if (bRanged && Brain.ReleaseAt > 0.0f)
		{
			Brain.ReleaseAt = 0.0f;

			if (auto HeldWeapon = Pawn->GetCurrentWeapon())
			{
				CallSafe(HeldWeapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.ServerReleaseWeaponPrimaryAbility"));
				CallSafe(HeldWeapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.CancelActiveAbility"));
			}
		}

		auto ASC = PlayerState->GetAbilitySystemComponent();

		if (!ASC)
			return false;

		auto Container = ASC->GetActivatableAbilities();

		if (!Container)
			return false;

		auto& Items = Container->GetItems();

		static bool bLoggedMeleeSpecs = false;
		static bool bLoggedRangedSpecs = false;
		bool& bLogged = bRanged ? bLoggedRangedSpecs : bLoggedMeleeSpecs;

		if (!bLogged)
		{
			bLogged = true;
			auto Weapon = Pawn->GetCurrentWeapon();
			LOG_INFO(LogBots, "BotCombat #{} first {} attack: weapon={} specs={}", Id, bRanged ? "ranged" : "melee", Weapon ? Weapon->GetPathName() : std::string("NONE"), Items.Num());

			for (int i = 0; i < Items.Num() && i < 120; ++i)
			{
				auto& Spec = Items.At(i, FGameplayAbilitySpec::GetStructSize());
				LOG_INFO(LogBots, "BotCombat   spec[{}] handle={} ability={}", i, Spec.GetHandle().Handle, Spec.GetAbility() ? Spec.GetAbility()->GetPathName() : std::string("NONE"));
			}
		}

		if (bConsumable)
		{
			static int LoggedUse = 0;

			if (++LoggedUse <= 6)
			{
				LOG_INFO(LogBots, "BotHeal #{} looks for the item's use ability among {} specs; the newest:", Id, Items.Num());

				for (int i = Items.Num() - 1; i >= 0 && i >= Items.Num() - 8; --i)
				{
					auto& Spec = Items.At(i, FGameplayAbilitySpec::GetStructSize());
					LOG_INFO(LogBots, "BotHeal   spec[{}] {}", i, Spec.GetAbility() ? Spec.GetAbility()->GetPathName() : std::string("NONE"));
				}
			}
		}

		// Pick the weapon's ability by name, searching from the newest spec backwards (a weapon's abilities are added when it is
		// equipped, so they sit at the end). Never "whatever is first": that could be sprint or an emote.
		int Chosen = -1;
		static const char* MeleeNeedles[] = { "Melee", "Pickaxe", "Harvest" };
		static const char* ConsumableNeedles[] = { "Consumable", "Slurp", "Shield", "Medkit", "Bandage", "Heal", "Chug", "Mini", "Cola", "Apple", "Mushroom", "Pepper", "Fish", "Coconut" };
		static const char* RangedNeedles[] = { "Ranged", "Weapon", "Fire", "Shoot", "Gun", "Shotgun", "Rifle", "Pistol", "Smg", "Sniper", "Projectile" };
		static const char* Excluded[] = { "Melee", "DefaultPlayer", "Jump", "Sprint", "Interact", "DBNO", "Death", "Stunned", "Knockback", "Vehicle", "Rift", "Slurp", "Grenade", "SurfaceChange", "Conversation", "BuildingCreated", "BuildingRepaired", "KilledEnemy", "Consumable", "Dance" };

		for (int i = Items.Num() - 1; i >= 0 && Chosen == -1; --i)
		{
			auto& Spec = Items.At(i, FGameplayAbilitySpec::GetStructSize());
			auto Ability = Spec.GetAbility();

			if (!Ability)
				continue;

			bool bExcluded = false;

			if (bRanged)
			{
				for (auto Bad : Excluded)
				{
					if (PathContains(Ability, Bad))
					{
						bExcluded = true;
						break;
					}
				}
			}

			if (bExcluded)
				continue;

			if (bRanged)
			{
				for (auto Needle : RangedNeedles)
				{
					if (PathContains(Ability, Needle))
					{
						Chosen = i;
						break;
					}
				}
			}
			else if (bConsumable)
			{
				// the use ability of the item in hand sits at the end of the list; the generic DefaultPlayer one is only a fallback
				for (auto Needle : ConsumableNeedles)
				{
					if (PathContains(Ability, Needle) && !PathContains(Ability, "DefaultPlayer"))
					{
						Chosen = i;
						break;
					}
				}
			}
			else
			{
				for (auto Needle : MeleeNeedles)
				{
					if (PathContains(Ability, Needle))
					{
						Chosen = i;
						break;
					}
				}
			}
		}

		if (Chosen == -1)
		{
			static bool bLoggedNone[2] = { false, false };

			if (!bLoggedNone[bRanged])
			{
				bLoggedNone[bRanged] = true;
				LOG_WARN(LogBots, "BotCombat #{} found no {} ability among the {} specs", Id, bRanged ? "ranged" : "melee", Items.Num());
			}

			return false;
		}

		auto& Spec = Items.At(Chosen, FGameplayAbilitySpec::GetStructSize());
		FGameplayAbilitySpecHandle Handle = Spec.GetHandle();

		static auto PredictionStruct = FindObject<UStruct>(L"/Script/GameplayAbilities.PredictionKey");
		static auto InputPressedOffset = FindOffsetStruct("/Script/GameplayAbilities.GameplayAbilitySpec", "InputPressed");
		static auto CurrentOffset = FindOffsetStruct("/Script/GameplayAbilities.PredictionKey", "Current");
		static auto BaseOffset = FindOffsetStruct("/Script/GameplayAbilities.PredictionKey", "Base", false);
		static int16_t KeyCounter = 100;

		if (!PredictionStruct)
			return false;

		const int PredictionSize = PredictionStruct->GetPropertiesSize();
		std::vector<uint8_t> KeyBytes(PredictionSize + 16, 0);

		if (++KeyCounter > 30000)
			KeyCounter = 100;

		*(int16_t*)(KeyBytes.data() + CurrentOffset) = KeyCounter;

		if (BaseOffset != -1)
			*(int16_t*)(KeyBytes.data() + BaseOffset) = KeyCounter;

		SetBitfield((void*)((__int64)&Spec + InputPressedOffset), 1, false);
		SetBitfield((void*)((__int64)&Spec + InputPressedOffset), 1, true);

		UObject* OutInstancedAbility = nullptr;
		bool bActivated = false;

		if (PredictionSize == 0x10 && UAbilitySystemComponent::InternalTryActivateAbilityOriginal)
		{
			PadHex10 PredictionKey{};
			memcpy(&PredictionKey, KeyBytes.data(), sizeof(PredictionKey));
			bActivated = UAbilitySystemComponent::InternalTryActivateAbilityOriginal(ASC, Handle, PredictionKey, &OutInstancedAbility, nullptr, nullptr);
		}
		else if (PredictionSize == 0x18 && UAbilitySystemComponent::InternalTryActivateAbilityOriginal2)
		{
			PadHex18 PredictionKey{};
			memcpy(&PredictionKey, KeyBytes.data(), sizeof(PredictionKey));
			bActivated = UAbilitySystemComponent::InternalTryActivateAbilityOriginal2(ASC, Handle, PredictionKey, &OutInstancedAbility, nullptr, nullptr);
		}

		if (bActivated && bRanged)
		{
			static bool bListedAbility = false;

			if (!bListedAbility && OutInstancedAbility)
			{
				bListedAbility = true;
				LOG_INFO(LogBots, "BotShot: ranged ability instance {}", OutInstancedAbility->GetPathName());
				ListFieldsMatching("RangedAbility", OutInstancedAbility, { "target", "hit", "trace", "server", "wait", "commit", "damage", "fire" });
			}
			else if (!bListedAbility)
			{
				bListedAbility = true;
				LOG_INFO(LogBots, "BotShot: the ranged ability left no instance (it is not instanced per activation)");
			}
		}

		if (bActivated && OutHandle)
			*OutHandle = Handle.Handle;

		if (bActivated && OutKey)
			*OutKey = KeyBytes;

		++Brain.Attacks;

		if (Brain.Attacks <= 6 || Brain.Attacks % 25 == 0)
			LOG_INFO(LogBots, "BotCombat #{} {} attack {} spec[{}] activated={}", Id, bRanged ? "ranged" : "melee", Brain.Attacks, Chosen, bActivated);

		const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

		if (bActivated)
		{
			// a client releases the fire button shortly after pressing it; without that the ability stays held and can't fire again
			Brain.FailedActivations = 0;

			if (!bConsumable) // an item being used must not be let go of
				Brain.ReleaseAt = Now + (bRanged ? 0.12f : 0.2f);
		}
		else if (bConsumable)
		{
			// nothing to cancel
		}
		else if (++Brain.FailedActivations >= 2)
		{
			Brain.FailedActivations = 0;

			if (auto Weapon = Pawn->GetCurrentWeapon())
			{
				CallSafe(Weapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.CancelActiveAbility"));
				CallSafe(Weapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.ServerReleaseWeaponPrimaryAbility"));
			}
		}

		return bActivated;
	}

	// Chest search, as Reboot's ServerAttemptInteract does it for a player (no controller needed).
	inline bool SearchChest(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, int Id)
	{
		auto Chest = Brain.TargetChest;
		Brain.TargetChest = nullptr;

		if (!Chest || !Chest->IsValidLowLevel() || Chest->IsActorBeingDestroyed() || Chest->IsAlreadySearched())
		{
			++gChestOutcomes["at the chest: already opened or gone"];
			return false;
		}

		if (!HasSight(Pawn, Pawn->GetActorLocation(), Chest, Chest->GetActorLocation()))
		{
			const FVector Here = Pawn->GetActorLocation(), There = Chest->GetActorLocation();

			if (++gChestOutcomes["at the chest: something between it and the bot"] <= 12)
				LOG_INFO(LogBots, "BotChest #{} cannot open {}: no clear line from ({:.0f}, {:.0f}, {:.0f}) to it at ({:.0f}, {:.0f}, {:.0f})", Id, Chest->GetName(), Here.X, Here.Y, Here.Z, There.X, There.Y, There.Z);

			return false; // a wall between: the chest is on the other side
		}

		++gChestOutcomes["opened"];

		static auto SearchBounceDataOffset = Chest->GetOffset("SearchBounceData", false);
		static auto SearchAnimationCountOffset = FindOffsetStruct("/Script/FortniteGame.FortSearchBounceData", "SearchAnimationCount", false);

		Chest->SpawnLoot(Pawn);
		++Brain.ChestsOpened;
		Chest->SetAlreadySearched(true, false);
		MarkChestLooted(Chest);

		if (SearchBounceDataOffset != -1 && SearchAnimationCountOffset != -1)
		{
			auto SearchBounceData = Chest->GetPtr<void>(SearchBounceDataOffset);
			(*(int*)(__int64(SearchBounceData) + SearchAnimationCountOffset))++;
		}

		Chest->BounceContainer();
		Chest->ForceNetUpdate();
		AddSoundNow(Chest->GetActorLocation(), 1500.0f, Pawn, -1); // a chest popping open carries

		static int Searched = 0;

		if (++Searched <= 8)
			LOG_INFO(LogBots, "BotLoot #{} opened a chest at ({:.0f}, {:.0f})", Id, Chest->GetActorLocation().X, Chest->GetActorLocation().Y);

		return true;
	}

	inline bool PickUp(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, float Now, int Id)
	{
		auto Pickup = Brain.TargetPickup;
		Brain.TargetPickup = nullptr;

		if (!Pickup || !Pickup->IsValidLowLevel() || Pickup->IsActorBeingDestroyed())
			return false;

		if (!HasSight(Pawn, Pawn->GetActorLocation(), Pickup, Pickup->GetActorLocation()))
		{
			Brain.BadPickups.push_back({ Pickup, Now + 30.0f });
			return false; // behind a wall
		}

		static int Picked = 0;

		if (++Picked <= 12)
		{
			auto Entry = Pickup->GetPrimaryPickupItemEntry();
			auto Def = Entry ? (UObject*)Entry->GetItemDefinition() : nullptr;
			LOG_INFO(LogBots, "BotLoot #{} picking up {}", Id, Def ? Def->GetPathName() : std::string("?"));
		}

		AFortPlayerPawn::ServerHandlePickupHook((AFortPlayerPawn*)Pawn, Pickup, 0.4f, FVector(), true);
		++Brain.ItemsPicked;
		return true;
	}

	// Refill the magazine a moment after it runs dry. The bots don't track reserve ammo, so a "reload" just tops up the clip.
	inline void ManageAmmo(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortInventory* Inventory, float Now)
	{
		auto Weapon = Pawn->GetCurrentWeapon();

		if (!Weapon)
			return;

		auto Def = (UObject*)Weapon->GetWeaponData();
		const auto& Info = GetItemInfo(Def);

		if (Info.Kind != EBotItemKind::Gun)
			return;

		if (Brain.ClipDef != Def)
		{
			Brain.ClipDef = Def;
			Brain.ClipGuess = 0;
			Brain.ReloadAt = 0.0f;
		}

		int& Ammo = Weapon->GetAmmoCount();

		if (Ammo > Brain.ClipGuess)
			Brain.ClipGuess = Ammo;

		if (Ammo > 0)
		{
			Brain.ReloadAt = 0.0f;
			return;
		}

		if (Brain.ReloadAt == 0.0f)
		{
			Brain.ReloadAt = Now + 1.6f;
			return;
		}

		if (Now < Brain.ReloadAt)
			return;

		Brain.ReloadAt = 0.0f;
		Ammo = Brain.ClipGuess > 0 ? Brain.ClipGuess : (Info.bShotgun ? 5 : 25);

		if (Inventory)
		{
			if (auto Item = Inventory->FindItemInstance(Weapon->GetItemEntryGuid()))
			{
				if (auto Entry = Item->GetItemEntry())
					Entry->GetLoadedAmmo() = Ammo;
			}
		}
	}

	// ---- damage ----
	// In this version the hit of a gun is worked out by the shooter's client and sent to the server, and a bot has no client, so a bot's
	// shot would never hurt anyone. Bots therefore roll the hit themselves and apply the damage on the server.
	inline float GetActorYaw(AActor* Actor)
	{
		static auto GetRotationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_GetActorRotation");

		if (!GetRotationFn)
			return 0.0f;

		struct { FRotator ReturnValue; } Params{};
		Actor->ProcessEvent(GetRotationFn, &Params);
		return Params.ReturnValue.Yaw;
	}

	inline bool DamagePawnAs(AController* KillerController, AFortPlayerPawnAthena* Attacker, AFortPlayerPawnAthena* Target, float Damage, float& HealthBefore, float& HealthAfter)
	{
		HealthBefore = HealthAfter = 0.0f;

		if (!Target || !Target->IsValidLowLevel() || Target->IsActorBeingDestroyed())
			return false;

		// remember who hurt it last: the engine never runs the death flow for a bot, so this is the only way to credit a bot kill
		if (auto AttackerState = Cast<AFortPlayerStateAthena>((UObject*)Attacker->GetPlayerState()))
			gLastDamager[Target] = AttackerState;
		gLastDamageTime[Target] = UGameplayStatics::GetTimeSeconds(GetWorld());

		if (auto HeldWeapon = Attacker->GetCurrentWeapon())
		{
			if (auto HeldDef = (UObject*)HeldWeapon->GetWeaponData())
				gLastWeaponPath[Target] = Lower(HeldDef->GetPathName());
		}

		const float Health = Target->GetHealth();
		const float Shield = Target->GetShield();
		HealthBefore = Health + Shield;

		if (Health <= 0.0f)
			return false;

		float Remaining = Damage;

		if (Shield > 0.0f)
		{
			const float Absorbed = (std::min)(Shield, Remaining);
			Target->SetShield(Shield - Absorbed);
			Remaining -= Absorbed;
		}

		const float NewHealth = Health - Remaining;
		HealthAfter = (std::max)(NewHealth, 0.0f) + (Shield > Damage ? Shield - Damage : 0.0f);

		if (NewHealth <= 0.0f)
		{
			// kill it the way the game does so the death flow runs with this bot as the killer
			auto PawnState = (void*)Attacker->GetPlayerState();
			auto ControllerState = KillerController ? (void*)KillerController->GetPlayerState() : nullptr;
			LOG_INFO(LogBots, "BotKill: {} kills a pawn. player state via pawn {:#x} ({}), via controller {:#x} ({})", BotRegistry::LabelFor(ControllerState), (uint64_t)PawnState, BotRegistry::LabelFor(PawnState), (uint64_t)ControllerState, BotRegistry::LabelFor(ControllerState));

			static auto ForceKillFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.ForceKill");

			if (ForceKillFn)
			{
				struct { FGameplayTag DeathReason; AController* KillerController; AActor* KillerActor; } Params{ FGameplayTag{}, KillerController, Attacker };
				Target->ProcessEvent(ForceKillFn, &Params);
			}
			else
			{
				Target->SetHealth(0.0f);
			}
		}
		else
		{
			Target->SetHealth(NewHealth);
		}

		return true;
	}

	inline bool DamagePawn(BotBrainState& Brain, AFortPlayerPawnAthena* Attacker, AFortPlayerPawnAthena* Target, float Damage, float& HealthBefore, float& HealthAfter)
	{
		return DamagePawnAs(Brain.Controller, Attacker, Target, Damage, HealthBefore, HealthAfter);
	}

	// Where does one bullet go? From where the bot is actually pointing (not where it wants to), with the bot's inaccuracy added,
	// and it hits only if that line passes through the target's body. Returns true on a hit.
	inline bool ResolveShot(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerPawnAthena* Target, const BotItemInfo& Info, bool& bHead, float* OutShotYaw = nullptr, float* OutShotPitch = nullptr)
	{
		bHead = false;

		const FVector Shooter = Pawn->GetActorLocation();
		const FVector Victim = Target->GetActorLocation();
		const float Dx = Victim.X - Shooter.X;
		const float Dy = Victim.Y - Shooter.Y;
		const float Horizontal = (std::max)(std::sqrt(Dx * Dx + Dy * Dy), 1.0f);
		const float Dz = (Victim.Z + 10.0f) - (Shooter.Z + 50.0f);

		const float TrueYaw = std::atan2(Dy, Dx) * RadToDeg;
		const float TruePitch = std::atan2(Dz, Horizontal) * RadToDeg;

		float Sigma = AimSigma(Brain.Skill < 0.0f ? 30.0f : Brain.Skill);

		const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

		if (Now - Brain.LastShotTime > 0.6f)
			Brain.Spray = 0;

		Brain.LastShotTime = Now;
		Sigma *= 1.0f + 0.1f * (float)(std::min)(Brain.Spray, 8); // recoil builds up while spraying
		++Brain.Spray;

		if (Brain.LastAction.bDriveXY && Brain.LastAction.Speed > 200.0f)
			Sigma *= 1.3f; // shooting on the move

		if (Info.bSniper)
			Sigma *= 0.6f;

		// the line the bullet really takes
		const float ShotYaw = Brain.AimYaw + Brain.WobbleYaw + Gauss() * Sigma;
		const float ShotPitch = Brain.AimPitch + Brain.WobblePitch + Gauss() * Sigma * 0.8f;

		if (OutShotYaw) *OutShotYaw = ShotYaw;
		if (OutShotPitch) *OutShotPitch = ShotPitch;

		// where that line passes the target, relative to the target's centre
		const float Lateral = std::tan(NormalizeAngle(ShotYaw - TrueYaw) * DegToRad) * Horizontal;
		const float Vertical = std::tan((ShotPitch - TruePitch) * DegToRad) * std::sqrt(Horizontal * Horizontal + Dz * Dz);

		const float BodyHalfWidth = Info.bShotgun ? 90.0f : 42.0f; // pellets spread out
		const bool bBody = std::fabs(Lateral) < BodyHalfWidth && Vertical > -80.0f && Vertical < 85.0f && std::fabs(NormalizeAngle(ShotYaw - TrueYaw)) < 60.0f;

		// A head hit takes a smaller part of the head than before and some luck on top: an average player lands about one hit in six on the head, not one in three.
		bHead = bBody && std::fabs(Lateral) < 16.0f && Vertical > 58.0f && RandRange(0.0f, 1.0f) < 0.5f;
		return bBody;
	}

	// Tells every client that this pawn was hit, the way the game does it: a "GameplayCue.Damage" cue on the victim's ability system. Clients run
	// the pawn's GameplayCue_Damage -> OnDamagePlayEffects: the hit particles, the sound, the camera shake and the damage direction arrow.
	inline void WeakObjectAt(uint8_t* At, UObject* Object)
	{
		auto Item = Object ? GetItemByIndex(Object->InternalIndex) : nullptr;
		*(int*)(At + 0) = Object ? Object->InternalIndex : -1;
		*(int*)(At + 4) = Item ? Item->SerialNumber : 0;
	}

	inline void SendHitCue(AFortPlayerPawnAthena* Target, AFortPlayerPawnAthena* Attacker, float Magnitude, bool bHeadshot, bool bFatal, bool bShield)
	{
		static auto CueFn = FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCueExecuted_WithParams");
		static const char* FnPath = "/Script/GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCueExecuted_WithParams";
		static const char* StructPath = "/Script/GameplayAbilities.GameplayCueParameters";
		static int TagOffset = FindOffsetStruct(FnPath, "GameplayCueTag", false);
		static int ParamsOffset = FindOffsetStruct(FnPath, "GameplayCueParameters", false);
		static int LocationOffset = FindOffsetStruct(StructPath, "Location", false);
		static int NormalOffset = FindOffsetStruct(StructPath, "Normal", false);
		static int RawOffset = FindOffsetStruct(StructPath, "RawMagnitude", false);
		static int NormalizedOffset = FindOffsetStruct(StructPath, "NormalizedMagnitude", false);
		static int InstigatorOffset = FindOffsetStruct(StructPath, "Instigator", false);
		static int CauserOffset = FindOffsetStruct(StructPath, "EffectCauser", false);
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "HitCue: fn {} tag@{} params@{} location@{} normal@{} raw@{} instigator@{} causer@{}", CueFn != nullptr, TagOffset, ParamsOffset, LocationOffset, NormalOffset, RawOffset, InstigatorOffset, CauserOffset);
		}

		if (!CueFn || TagOffset == -1 || ParamsOffset == -1 || LocationOffset == -1 || !Target)
			return;

		auto VictimState = Cast<AFortPlayerStateAthena>((UObject*)Target->GetPlayerState());
		auto ASC = VictimState ? VictimState->GetAbilitySystemComponent() : nullptr;

		if (!ASC)
			return;

		const FVector At = Target->GetActorLocation();
		const FVector From = Attacker ? Attacker->GetActorLocation() : At;
		float Nx = From.X - At.X, Ny = From.Y - At.Y;
		const float Length = (std::max)(std::sqrt(Nx * Nx + Ny * Ny), 1.0f);

		std::vector<uint8_t> Params(CueFn->GetPropertiesSize() + 64, 0);
		FGameplayTag Tag{};
		Tag.TagName = UKismetStringLibrary::Conv_StringToName(L"GameplayCue.Damage");
		*(FGameplayTag*)(Params.data() + TagOffset) = Tag;

		uint8_t* Cue = Params.data() + ParamsOffset;
		*(FVector*)(Cue + LocationOffset) = FVector{ At.X, At.Y, At.Z + (bHeadshot ? 62.0f : 10.0f) };

		if (NormalOffset != -1) *(FVector*)(Cue + NormalOffset) = FVector{ Nx / Length, Ny / Length, 0.0f };
		if (RawOffset != -1) *(float*)(Cue + RawOffset) = Magnitude;
		if (NormalizedOffset != -1) *(float*)(Cue + NormalizedOffset) = (std::min)(Magnitude / 100.0f, 1.0f);
		if (InstigatorOffset != -1) WeakObjectAt(Cue + InstigatorOffset, Attacker);
		if (CauserOffset != -1) WeakObjectAt(Cue + CauserOffset, Attacker);

		ASC->ProcessEvent(CueFn, Params.data());
	}


	// The way the game tells everyone about a shot: the shooter's pawn multicasts AthenaBatchedDamageGameplayCues. bWeaponActivate is the muzzle
	// flash / tracer / fire sound on the shooter, the rest is the hit impact, damage arrow and number on whoever was hit.
	inline void SetStructBool(uint8_t* Base, const char* StructPath, const char* Name, bool bValue)
	{
		const int Offset = FindOffsetStruct(StructPath, Name, false);

		if (Offset == -1)
			return;

		auto Prop = FindPropertyStruct(StructPath, Name, false);
		const uint8_t Mask = Prop ? GetFieldMask(Prop) : 0;
		uint8_t& Byte = *(Base + Offset);

		if (Mask == 0 || Mask == 0xFF)
			Byte = bValue ? 1 : 0;
		else
			Byte = bValue ? (Byte | Mask) : (Byte & ~Mask);
	}

	inline void SendShotCue(AFortPlayerPawnAthena* Shooter, AFortPlayerPawnAthena* Target, const FVector& Location, const FVector& Normal, float Magnitude, bool bHit, bool bHeadshot, bool bFatal, bool bShield)
	{
		static const char* FnPath = "/Script/FortniteGame.FortPawn.NetMulticast_Athena_BatchedDamageCues";
		static auto Fn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.NetMulticast_Athena_BatchedDamageCues");
		static const char* Shared = "/Script/FortniteGame.AthenaBatchedDamageGameplayCues_Shared";
		static const char* NonShared = "/Script/FortniteGame.AthenaBatchedDamageGameplayCues_NonShared";
		static int SharedOffset = FindOffsetStruct(FnPath, "SharedData", false);
		static int NonSharedOffset = FindOffsetStruct(FnPath, "NonSharedData", false);
		static int HitActorOffset = FindOffsetStruct(NonShared, "HitActor", false);
		static int MagnitudeOffset = FindOffsetStruct(Shared, "Magnitude", false);
		static int NormalOffset = FindOffsetStruct(Shared, "Normal", false);
		static int LocationOffset = FindOffsetStruct(Shared, "Location", false);
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "ShotCue: fn {} shared@{} nonshared@{} hitactor@{} magnitude@{} normal@{} location@{}", Fn != nullptr, SharedOffset, NonSharedOffset, HitActorOffset, MagnitudeOffset, NormalOffset, LocationOffset);
		}

		if (!Fn || !Shooter || SharedOffset == -1 || NonSharedOffset == -1 || LocationOffset == -1)
			return;

		std::vector<uint8_t> Params(Fn->GetPropertiesSize() + 64, 0);
		uint8_t* S = Params.data() + SharedOffset;
		uint8_t* N = Params.data() + NonSharedOffset;

		SetStructBool(S, Shared, "bIsValid", true);
		SetStructBool(S, Shared, "bWeaponActivate", true);
		SetStructBool(S, Shared, "bIsBallistic", true);
		SetStructBool(S, Shared, "bIsCritical", bHit && bHeadshot);
		SetStructBool(S, Shared, "bIsFatal", bHit && bFatal);
		SetStructBool(S, Shared, "bIsShield", bHit && bShield);

		if (MagnitudeOffset != -1) *(float*)(S + MagnitudeOffset) = bHit ? Magnitude : 0.0f;
		if (NormalOffset != -1) *(FVector*)(S + NormalOffset) = Normal;
		*(FVector*)(S + LocationOffset) = Location;

		if (HitActorOffset != -1 && bHit && Target)
			*(UObject**)(N + HitActorOffset) = (UObject*)Target; // an ObjectProperty: a plain pointer, not a weak pointer

		Shooter->ProcessEvent(Fn, Params.data());
	}

	// ---- shooting the way a client does ----
	// A player's client runs the weapon ability, works out what the bullet hit and sends the server that as "target data" (ServerSetReplicatedTargetData);
	// the server's copy of the ability then applies the damage and plays the effects. The bots do the same, so a hit is processed by the game itself:
	// real damage, damage cues, tracers, the victim's indicator. If the game does not react (target data not understood), the old direct damage takes over.

	struct BotShotCheck { AFortPlayerPawnAthena* Victim; AFortPlayerPawnAthena* Attacker; AController* Controller; float Before; float Due; float Damage; };
	inline std::vector<BotShotCheck> gShotChecks;
	inline int gClientShotsWorked = 0;
	inline int gClientShotsIgnored = 0;

	inline void WriteWeakObject(uint8_t* At, UObject* Object)
	{
		// TWeakObjectPtr: object index and serial number
		auto Item = Object ? GetItemByIndex(Object->InternalIndex) : nullptr;
		*(int*)(At + 0) = Object ? Object->InternalIndex : -1;
		*(int*)(At + 4) = Item ? Item->SerialNumber : 0;
	}

	// The hit result a client would have produced: either on the victim's body, or on whatever the shot ran into.
	inline bool MakeHitResult(AFortPlayerPawnAthena* Pawn, AFortPlayerPawnAthena* Victim, bool bHead, float ShotYaw, float ShotPitch, std::vector<uint8_t>& Out)
	{
		static auto HitStruct = FindObject<UStruct>(L"/Script/Engine.HitResult");
		static const char* Path = "/Script/Engine.HitResult";
		static int ActorOffset = FindOffsetStruct(Path, "Actor", false);
		static int ComponentOffset = FindOffsetStruct(Path, "Component", false);
		static int LocationOffset = FindOffsetStruct(Path, "Location", false);
		static int ImpactPointOffset = FindOffsetStruct(Path, "ImpactPoint", false);
		static int NormalOffset = FindOffsetStruct(Path, "Normal", false);
		static int ImpactNormalOffset = FindOffsetStruct(Path, "ImpactNormal", false);
		static int StartOffset = FindOffsetStruct(Path, "TraceStart", false);
		static int EndOffset = FindOffsetStruct(Path, "TraceEnd", false);
		static int DistanceOffset = FindOffsetStruct(Path, "Distance", false);
		static int TimeOffset = FindOffsetStruct(Path, "Time", false);
		static int BlockingOffset = FindOffsetStruct(Path, "bBlockingHit", false);
		static int RootOffset = Pawn->GetOffset("RootComponent", false);

		if (!HitStruct)
			return false;

		const FVector Muzzle = FVector{ Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y, Pawn->GetActorLocation().Z + 50.0f };
		const float CosP = std::cos(ShotPitch * DegToRad);
		const FVector Direction{ std::cos(ShotYaw * DegToRad) * CosP, std::sin(ShotYaw * DegToRad) * CosP, std::sin(ShotPitch * DegToRad) };
		const FVector End{ Muzzle.X + Direction.X * 12000.0f, Muzzle.Y + Direction.Y * 12000.0f, Muzzle.Z + Direction.Z * 12000.0f };

		if (Victim)
		{
			const FVector At = Victim->GetActorLocation();
			const FVector Point{ At.X, At.Y, At.Z + (bHead ? 62.0f : 10.0f) };
			const float Dx = Point.X - Muzzle.X, Dy = Point.Y - Muzzle.Y, Dz = Point.Z - Muzzle.Z;
			const float Length = (std::max)(std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz), 1.0f);

			Out.assign(HitStruct->GetPropertiesSize() + 16, 0);

			if (BlockingOffset != -1) Out[BlockingOffset] = 1;
			if (ActorOffset != -1) WriteWeakObject(Out.data() + ActorOffset, Victim);

			if (ComponentOffset != -1 && RootOffset != -1)
				WriteWeakObject(Out.data() + ComponentOffset, Victim->Get<UObject*>(RootOffset));

			if (LocationOffset != -1) *(FVector*)(Out.data() + LocationOffset) = Point;
			if (ImpactPointOffset != -1) *(FVector*)(Out.data() + ImpactPointOffset) = Point;
			if (NormalOffset != -1) *(FVector*)(Out.data() + NormalOffset) = FVector{ -Dx / Length, -Dy / Length, -Dz / Length };
			if (ImpactNormalOffset != -1) *(FVector*)(Out.data() + ImpactNormalOffset) = FVector{ -Dx / Length, -Dy / Length, -Dz / Length };
			if (StartOffset != -1) *(FVector*)(Out.data() + StartOffset) = Muzzle;
			if (EndOffset != -1) *(FVector*)(Out.data() + EndOffset) = End;
			if (DistanceOffset != -1) *(float*)(Out.data() + DistanceOffset) = Length;
			if (TimeOffset != -1) *(float*)(Out.data() + TimeOffset) = Length / 12000.0f;
			return true;
		}

		// a miss: whatever the bullet ran into (a wall, the ground), or nothing at all
		float Fraction = 1.0f;
		std::vector<uint8_t> Traced;
		const bool bHit = TraceBlocked(Pawn, Pawn->GetCurrentWeapon(), Muzzle, End, nullptr, &Fraction, nullptr, nullptr, &Traced);

		if (Traced.empty())
			return false;

		Out = Traced;

		if (!bHit)
		{
			if (BlockingOffset != -1) Out[BlockingOffset] = 0;
			if (StartOffset != -1) *(FVector*)(Out.data() + StartOffset) = Muzzle;
			if (EndOffset != -1) *(FVector*)(Out.data() + EndOffset) = End;
		}

		return true;
	}

	// Hands the hit result to the game as the shooter's target data for the ability that was just activated.
	inline bool SendClientShot(AFortPlayerStateAthena* PlayerState, int AbilityHandle, const std::vector<uint8_t>& PredictionKey, const std::vector<uint8_t>& HitResult)
	{
		static const char* TargetDataPath = "/Script/GameplayAbilities.AbilitySystemBlueprintLibrary.AbilityTargetDataFromHitResult";
		static const char* SendPath = "/Script/GameplayAbilities.AbilitySystemComponent.ServerSetReplicatedTargetData";
		static auto TargetDataFn = FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary.AbilityTargetDataFromHitResult");
		static auto LibraryDefault = FindObject(L"/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");
		static auto SendFn = FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.ServerSetReplicatedTargetData");
		static auto HandleStruct = FindObject<UStruct>(L"/Script/GameplayAbilities.GameplayAbilityTargetDataHandle");
		static int HitParam = FindOffsetStruct(TargetDataPath, "HitResult", false);
		static int HandleReturn = FindOffsetStruct(TargetDataPath, "ReturnValue", false);
		static int AbilityHandleParam = FindOffsetStruct(SendPath, "AbilityHandle", false);
		static int OriginalKeyParam = FindOffsetStruct(SendPath, "AbilityOriginalPredictionKey", false);
		static int DataParam = FindOffsetStruct(SendPath, "ReplicatedTargetDataHandle", false);
		static int CurrentKeyParam = FindOffsetStruct(SendPath, "CurrentPredictionKey", false);
		static bool bLogged = false;

		const bool bReady = TargetDataFn && LibraryDefault && SendFn && HandleStruct && HitParam != -1 && HandleReturn != -1 && AbilityHandleParam != -1 && OriginalKeyParam != -1 && DataParam != -1 && CurrentKeyParam != -1;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "BotShot: client path ready={} (target data fn {}, send fn {}, handle struct {}, offsets hit {} ret {} handle {} origkey {} data {} curkey {})", bReady,
				TargetDataFn != nullptr, SendFn != nullptr, HandleStruct != nullptr, HitParam, HandleReturn, AbilityHandleParam, OriginalKeyParam, DataParam, CurrentKeyParam);
		}

		if (!bReady || !PlayerState)
			return false;

		auto ASC = PlayerState->GetAbilitySystemComponent();

		if (!ASC)
			return false;

		const int HandleSize = HandleStruct->GetPropertiesSize();
		std::vector<uint8_t> MakeParams(TargetDataFn->GetPropertiesSize() + 64, 0);
		memcpy(MakeParams.data() + HitParam, HitResult.data(), HitResult.size());
		LibraryDefault->ProcessEvent(TargetDataFn, MakeParams.data());

		// the handle holds a reference-counted pointer; the copy below is never destroyed, which only leaks a few bytes per shot
		std::vector<uint8_t> SendParams(SendFn->GetPropertiesSize() + 64, 0);
		*(int*)(SendParams.data() + AbilityHandleParam) = AbilityHandle;
		memcpy(SendParams.data() + OriginalKeyParam, PredictionKey.data(), PredictionKey.size());
		memcpy(SendParams.data() + CurrentKeyParam, PredictionKey.data(), PredictionKey.size());
		memcpy(SendParams.data() + DataParam, MakeParams.data() + HandleReturn, HandleSize);
		ASC->ProcessEvent(SendFn, SendParams.data());
		return true;
	}

	// A shot sent the client way should hurt its target within a moment; if nothing happened the bot does the damage itself.
	inline void ProcessShotChecks(float Now)
	{
		for (size_t i = 0; i < gShotChecks.size();)
		{
			auto Check = gShotChecks[i];

			if (Check.Due > Now)
			{
				++i;
				continue;
			}

			gShotChecks.erase(gShotChecks.begin() + i);

			if (!Check.Victim || !Check.Victim->IsValidLowLevel() || Check.Victim->IsActorBeingDestroyed())
			{
				++gClientShotsWorked;
				continue;
			}

			const float Total = Check.Victim->GetHealth() + Check.Victim->GetShield();

			if (Check.Victim->GetHealth() <= 0.0f || Total <= Check.Before - 1.0f)
			{
				++gClientShotsWorked;
				continue;
			}

			++gClientShotsIgnored;
			float Before = 0.0f, After = 0.0f;
			DamagePawnAs(Check.Controller, Check.Attacker, Check.Victim, Check.Damage, Before, After);

			static int Logged = 0;

			if (++Logged <= 6)
				LOG_INFO(LogBots, "BotShot: the game did not react to a client-style hit (worked {}, ignored {}); applied {:.0f} damage directly", gClientShotsWorked, gClientShotsIgnored, Check.Damage);

			if (gClientShotsWorked == 0 && gClientShotsIgnored >= 6 && gClientShots)
			{
				gClientShots = false;
				LOG_WARN(LogBots, "BotShot: client-style shots never did anything, switching to direct damage");
			}
		}
	}

	inline void FireShot(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, int Id)
	{
		auto Weapon = Pawn->GetCurrentWeapon();

		if (!Weapon)
			return;

		const auto& Info = GetItemInfo((UObject*)Weapon->GetWeaponData());

		if (Info.Kind != EBotItemKind::Gun)
			return;

		static bool bListedFields = false;

		if (!bListedFields)
		{
			bListedFields = true;
			ListFieldsMatching("Pawn", Pawn, { "damage", "hit", "indicator", "direction", "cue", "impact" });
			ListFieldsMatching("Weapon", Weapon, { "fire", "impact", "tracer", "bullet", "shot", "muzzle" });

			if (Brain.Controller)
				ListFieldsMatching("Controller", Brain.Controller, { "damage", "hit", "indicator", "direction", "marker" });
		}

		int& Ammo = Weapon->GetAmmoCount();

		if (Ammo <= 0)
		{
			Attack(Brain, Pawn, PlayerState, true, Id); // the click of an empty gun; ManageAmmo reloads it shortly
			return;
		}

		--Ammo;
		++Brain.Shots;

		AddSoundNow(Pawn->GetActorLocation(), Info.bShotgun ? 4000.0f : 5000.0f, Pawn, PlayerState ? (int)PlayerState->GetTeamIndex() : -1);

		const float Distance = Brain.TargetDist;
		bool bHead = false;
		bool bHit = false;
		auto ShotTarget = Brain.TargetPawn;
		float ShotYaw = Brain.AimYaw, ShotPitch = Brain.AimPitch;

		if (ShotTarget && ShotTarget->IsValidLowLevel() && !ShotTarget->IsActorBeingDestroyed())
			bHit = ResolveShot(Brain, Pawn, ShotTarget, Info, bHead, &ShotYaw, &ShotPitch);

		// the bullet has to get there: a wall (built or not) between the two stops it, whatever the aim says
		if (bHit)
		{
			const FVector From = Pawn->GetActorLocation();
			const FVector To = ShotTarget->GetActorLocation();
			UObject* Blocker = nullptr;
			const float Heights[3] = { 50.0f, 10.0f, -20.0f };
			bool bClear = false;

			for (int h = 0; h < 3 && !bClear; ++h)
			{
				Blocker = nullptr;

				if (!TraceBlocked(Pawn, Weapon, FVector{ From.X, From.Y, From.Z + 50.0f }, FVector{ To.X, To.Y, To.Z + Heights[h] }, nullptr, nullptr, &Blocker) || Blocker == (UObject*)ShotTarget)
					bClear = true;
			}

			if (!bClear)
				bHit = false;
		}

		float Damage = Info.Damage * (1.0f + 0.04f * (Info.Tier - 1));

		if (Info.bShotgun)
			Damage *= Distance < 500.0f ? 1.0f : (Distance < 1000.0f ? 0.6f : 0.25f);
		else if (bHead)
			Damage *= Info.bSniper ? 2.5f : 1.75f; // a headshot: a rifle's is about 1.5 to 2 times, a sniper's 2.5

		// what the game keeps on a pawn that has just fired (other players' animation uses it to hold the shooting pose)
		{
			static int FiredTimeOffset = Pawn->GetOffset("LastFiredTime", false);
			static int FiredDirectionOffset = Pawn->GetOffset("LastFiredDirection", false);
			static int FiredLocationOffset = Pawn->GetOffset("LastFiredLocation", false);
			const FVector Here = Pawn->GetActorLocation();
			const float CosPitch = std::cos(ShotPitch * DegToRad);

			if (FiredTimeOffset != -1) Pawn->Get<float>(FiredTimeOffset) = UGameplayStatics::GetTimeSeconds(GetWorld());
			if (FiredLocationOffset != -1) Pawn->Get<FVector>(FiredLocationOffset) = FVector{ Here.X, Here.Y, Here.Z + 50.0f };
			if (FiredDirectionOffset != -1) Pawn->Get<FVector>(FiredDirectionOffset) = FVector{ std::cos(ShotYaw * DegToRad) * CosPitch, std::sin(ShotYaw * DegToRad) * CosPitch, std::sin(ShotPitch * DegToRad) };
		}

		// the ability gives the bot its fire animation and effects
		int AbilityHandle = -1;
		std::vector<uint8_t> PredictionKey;
		const bool bActivated = Attack(Brain, Pawn, PlayerState, true, Id, &AbilityHandle, &PredictionKey);

		bool bSentLikeAClient = false;

		if (gClientShots && bActivated)
		{
			std::vector<uint8_t> HitResult;

			if (MakeHitResult(Pawn, bHit ? ShotTarget : nullptr, bHead, ShotYaw, ShotPitch, HitResult))
				bSentLikeAClient = SendClientShot(PlayerState, AbilityHandle, PredictionKey, HitResult);
		}

		float Before = 0.0f, After = 0.0f;
		bool bApplied = false;

		if (bHit)
		{
			if (bSentLikeAClient)
			{
				// the game applies the damage; remember who did it (kill credit for bots) and check on it shortly
				if (auto AttackerState = Cast<AFortPlayerStateAthena>((UObject*)Pawn->GetPlayerState()))
					gLastDamager[ShotTarget] = AttackerState;

				gLastDamageTime[ShotTarget] = UGameplayStatics::GetTimeSeconds(GetWorld());

				if (auto HeldDef = (UObject*)Weapon->GetWeaponData())
					gLastWeaponPath[ShotTarget] = Lower(HeldDef->GetPathName());

				SendHitCue(ShotTarget, Pawn, Damage, bHead, false, false);
				Before = ShotTarget->GetHealth() + ShotTarget->GetShield();
				gShotChecks.push_back({ ShotTarget, Pawn, Brain.Controller, Before, UGameplayStatics::GetTimeSeconds(GetWorld()) + 0.4f, Damage });
				++Brain.ShotsHit;
				After = Before;
			}
			else
			{
				bApplied = DamagePawn(Brain, Pawn, ShotTarget, Damage, Before, After);

				if (bApplied)
				{
					++Brain.ShotsHit;
					SendHitCue(ShotTarget, Pawn, Damage, bHead, After <= 0.0f, false);
				}
			}
		}

		{
			const FVector Here = Pawn->GetActorLocation();
			const FVector Eye{ Here.X, Here.Y, Here.Z + 60.0f };
			const float CosP = std::cos(ShotPitch * DegToRad);
			const FVector Dir{ std::cos(ShotYaw * DegToRad) * CosP, std::sin(ShotYaw * DegToRad) * CosP, std::sin(ShotPitch * DegToRad) };
			FVector Where{ Eye.X + Dir.X * 2500.0f, Eye.Y + Dir.Y * 2500.0f, Eye.Z + Dir.Z * 2500.0f };

			if (bHit && ShotTarget)
			{
				const FVector There = ShotTarget->GetActorLocation();
				Where = FVector{ There.X, There.Y, There.Z + (bHead ? 62.0f : 10.0f) };
			}

			SendShotCue(Pawn, ShotTarget, Where, FVector{ -Dir.X, -Dir.Y, -Dir.Z }, Damage, bHit && bApplied, bHead, After <= 0.0f, false);
		}

		if (Brain.Shots <= 20 || Brain.Shots % 20 == 0)
			LOG_INFO(LogBots, "BotCombat #{} shot {} dist={:.0f} hit={} dmg={:.0f} target {:.0f}->{:.0f} ammo={} yaw actual={:.0f} commanded={:.0f}", Id, Brain.Shots, Distance, bHit, bHit ? Damage : 0.0f, Before, After, Ammo, GetActorYaw(Pawn), Brain.FaceYaw);
	}

	inline void MeleeHit(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, int Id)
	{
		Attack(Brain, Pawn, PlayerState, false, Id);
		AddSoundNow(Pawn->GetActorLocation(), 900.0f, Pawn, PlayerState ? (int)PlayerState->GetTeamIndex() : -1);

		const float SwingAt = UGameplayStatics::GetTimeSeconds(GetWorld());

		// the swing takes a moment to land and the swinger stands for it, then is slowed a little (the game's own figures)
		Brain.PlantUntil = SwingAt + 0.3f;
		Brain.MeleeSlowUntil = SwingAt + gMeleeSlowTime;

		if (Brain.TargetDist > MeleeRange + 60.0f)
			return;

		Brain.MeleeTarget = Brain.TargetPawn;
		Brain.MeleeDamageAt = SwingAt + 0.22f;
	}

	// the blow of a pickaxe swing arriving: only if the target is still within reach
	inline void ResolveMelee(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, float Now, int Id)
	{
		if (Brain.MeleeDamageAt <= 0.0f || Now < Brain.MeleeDamageAt)
			return;

		Brain.MeleeDamageAt = 0.0f;
		auto Target = Brain.MeleeTarget;
		Brain.MeleeTarget = nullptr;

		if (!Target || !Target->IsValidLowLevel() || Target->IsActorBeingDestroyed() || Target->GetHealth() <= 0.0f)
			return;

		// A direct hit only, the way a player's swing connects: the target within the pickaxe's reach, in front of the bot, and with its body
		// at the height the swing passes through. Only the flat distance was checked before (with 90 to spare), so a bot hit a player who
		// was jumping over it, or standing behind it (2026-11-09).
		{
			const FVector From = Pawn->GetActorLocation();
			const FVector To = Target->GetActorLocation();
			const float Flat = Dist2D(From, To);

			if (Flat > MeleeRange)
				return;

			// both are capsules 88 high either side of their middle; the blade sweeps from below the waist (-40) to head height (+60): a player
			// near the top of a jump (feet more than ~150 up) is over it
			const float Dz = To.Z - From.Z;

			if (Dz - 88.0f > 60.0f || Dz + 88.0f < -40.0f)
				return;

			// within 45 degrees of where it is actually pointing (close in, a body is wide enough to be hit a little off-centre anyway)
			const float Facing = Brain.bHaveAim ? Brain.AimYaw : Brain.FaceYaw;
			const float Off = std::fabs(NormalizeAngle(std::atan2(To.Y - From.Y, To.X - From.X) * RadToDeg - Facing));

			if (Off > (Flat < 90.0f ? 70.0f : 45.0f))
				return;
		}

		if (RandRange(0.0f, 1.0f) > 0.92f)
			return;

		{
			// a wall between the two takes the swing
			const FVector From = Pawn->GetActorLocation();
			const FVector To = Target->GetActorLocation();
			UObject* Blocker = nullptr;

			if (TraceBlocked(Pawn, Pawn->GetCurrentWeapon(), FVector{ From.X, From.Y, From.Z + 40.0f }, FVector{ To.X, To.Y, To.Z + 20.0f }, nullptr, nullptr, &Blocker) && Blocker != (UObject*)Target)
				return;
		}

		float Before = 0.0f, After = 0.0f;

		if (DamagePawn(Brain, Pawn, Target, 20.0f, Before, After))
		{
			++Brain.ShotsHit;

			if (Brain.ShotsHit <= 10)
				LOG_INFO(LogBots, "BotCombat #{} pickaxe hit target {:.0f}->{:.0f}", Id, Before, After);
		}
	}

	// ---- apply: the only place that writes the engine ----
	inline void SetYaw(AFortPlayerPawnAthena* Pawn, float Yaw)
	{
		static auto SetRotationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_SetActorRotation");

		if (!SetRotationFn)
			return;

		struct { FRotator NewRotation; bool bTeleportPhysics; bool ReturnValue; } Params{};
		Params.NewRotation.Pitch = 0.0f;
		Params.NewRotation.Yaw = Yaw;
		Params.NewRotation.Roll = 0.0f;
		Params.bTeleportPhysics = false;
		Pawn->ProcessEvent(SetRotationFn, &Params);
	}

	// A pawn without a controller aims with its actor yaw plus the replicated RemoteViewPitch byte.
	inline void SetAimPitch(AFortPlayerPawnAthena* Pawn, float PitchDegrees)
	{
		static int RemoteViewPitchOffset = Pawn->GetOffset("RemoteViewPitch", false);

		if (RemoteViewPitchOffset == -1)
			return;

		int Compressed = (int)(PitchDegrees * 256.0f / 360.0f) & 0xFF;
		Pawn->Get<uint8_t>(RemoteViewPitchOffset) = (uint8_t)Compressed;
	}

	// ---- doors, harvesting and building ----

	// Opens a door the way the game's own door state does: the replicated flags, then the OnReps that move the door and drop its collision.
	inline bool OpenDoor(AActor* Door)
	{
		static int OpenOffset = Door->GetOffset("bDoorOpen", false);
		static int CollisionOffset = Door->GetOffset("bDoorCollisionDisabled", false);
		static auto OpenProp = Door->GetProperty("bDoorOpen", false);
		static auto CollisionProp = Door->GetProperty("bDoorCollisionDisabled", false);
		static auto OpenRep = FindObject<UFunction>(L"/Script/FortniteGame.BuildingWall.OnRep_bDoorOpen");
		static auto CollisionRep = FindObject<UFunction>(L"/Script/FortniteGame.BuildingWall.OnRep_bDoorCollisionDisabled");

		if (OpenOffset == -1 || !OpenProp)
			return false;

		static auto OpenMask = GetFieldMask(OpenProp);

		if (Door->ReadBitfieldValue(OpenOffset, OpenMask))
			return false;

		Door->SetBitfieldValue(OpenOffset, OpenMask, true);

		struct { bool bPreviousState; char Padding[32]; } Params{};

		if (OpenRep)
			Door->ProcessEvent(OpenRep, &Params);

		if (CollisionOffset != -1 && CollisionProp)
		{
			static auto CollisionMask = GetFieldMask(CollisionProp);
			Door->SetBitfieldValue(CollisionOffset, CollisionMask, true);

			if (CollisionRep)
				Door->ProcessEvent(CollisionRep, &Params);
		}

		Door->ForceNetUpdate();
		return true;
	}

	// Doors open for any bot that walks up to one.
	inline void AutoOpenDoors(BotBrainState& Brain, const FVector& Pos, float Now)
	{
		if (Now < Brain.NextDoorCheck || gDoors.empty())
			return;

		Brain.NextDoorCheck = Now + 0.3f;

		for (auto& Door : gDoors)
		{
			if (std::fabs(Door.Pos.X - Pos.X) > 260.0f || std::fabs(Door.Pos.Y - Pos.Y) > 260.0f || std::fabs(Door.Pos.Z - Pos.Z) > 320.0f)
				continue;

			if (!IsLiveBuilding(Door.Actor))
				continue;

			if (OpenDoor(Door.Actor))
			{
				static int Opened = 0;

				if (++Opened <= 12)
					LOG_INFO(LogBots, "BotDoor: opened a door at ({:.0f}, {:.0f}, {:.0f})", Door.Pos.X, Door.Pos.Y, Door.Pos.Z);
			}
		}
	}

	// One swing at a prop: the pickaxe animation, and the same damage call the game makes when a player's pickaxe lands (which also pays out the wood).
	inline bool HarvestHit(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, int Id)
	{
		auto Prop = (ABuildingSMActor*)Brain.HarvestActor;

		if (!IsLiveBuilding(Prop) || Prop->IsDestroyed())
		{
			Brain.HarvestActor = nullptr;
			return false;
		}

		auto Held = Pawn->GetCurrentWeapon();
		const auto& HeldInfo = GetItemInfo(Held ? (UObject*)Held->GetWeaponData() : nullptr);

		if (HeldInfo.Kind != EBotItemKind::Pickaxe)
		{
			if (Brain.PickaxeDef)
			{
				Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Brain.PickaxeDef, Brain.PickaxeGuid);
				Brain.NextWeaponShowCheck = 0.0f;
			}

			return false;
		}

		if (Brain.NextHarvestHit > Brain.LastDecisionTime)
			return false;

		// swings come in runs of a few at a steady beat, then the player stops to look at what is left / around
		if (++Brain.HarvestBurst >= Brain.HarvestBurstLen)
		{
			Brain.HarvestBurst = 0;
			Brain.HarvestBurstLen = (int)RandRange(2.0f, 5.99f);
			Brain.NextHarvestHit = Brain.LastDecisionTime + RandRange(1.1f, 2.4f);
		}
		else
		{
			Brain.NextHarvestHit = Brain.LastDecisionTime + RandRange(0.8f, 1.05f);
		}

		Attack(Brain, Pawn, PlayerState, false, Id);

		static std::vector<uint8_t> HitBuffer(512, 0), ContextBuffer(128, 0);
		std::fill(HitBuffer.begin(), HitBuffer.end(), 0);
		std::fill(ContextBuffer.begin(), ContextBuffer.end(), 0);

		const float Damage = RandRange(0.0f, 1.0f) < 0.18f ? 100.0f : 20.0f; // now and then a weak spot hit
		auto Controller = Cast<AFortPlayerControllerAthena>(Brain.Controller);

		if (!Controller)
			return false;

		// A swing from a bot: the building's damage event hands out the wood, but nothing in it takes health off (nor does the engine's ApplyDamage: tried, the
		// health stays put), so the health is lowered here, and a building at zero is killed the way the game kills one.
		{
			ABuildingActor::OnDamageServerHook(Prop, Damage, FGameplayTagContainer(), FVector(), (__int64)HitBuffer.data(), (APlayerController*)Controller, Held, (__int64)ContextBuffer.data());

			static auto SetHealthFn = FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.SetHealth");
			static int NewHealthParam = FindOffsetStruct("/Script/FortniteGame.BuildingActor.SetHealth", "NewHealth", false);
			static auto DeathFn = FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.OnDeathServer");
			static int DDamage = FindOffsetStruct("/Script/FortniteGame.BuildingActor.OnDeathServer", "Damage", false);
			static int DInstigator = FindOffsetStruct("/Script/FortniteGame.BuildingActor.OnDeathServer", "InstigatedBy", false);
			static int DCauser = FindOffsetStruct("/Script/FortniteGame.BuildingActor.OnDeathServer", "DamageCauser", false);

			const float HealthBefore = Prop->GetHealth();
			const float NewHealth = (std::max)(0.0f, HealthBefore - Damage);

			if (SetHealthFn && NewHealthParam != -1)
			{
				std::vector<uint8_t> Params(SetHealthFn->GetPropertiesSize() + 16, 0);
				*(float*)(Params.data() + NewHealthParam) = NewHealth;
				Prop->ProcessEvent(SetHealthFn, Params.data());
			}

			bool bDied = false;

			if (NewHealth <= 0.0f && !Prop->IsDestroyed() && DeathFn)
			{
				std::vector<uint8_t> Params(DeathFn->GetPropertiesSize() + 64, 0);
				if (DDamage != -1) *(float*)(Params.data() + DDamage) = Damage;
				if (DInstigator != -1) *(UObject**)(Params.data() + DInstigator) = Controller;
				if (DCauser != -1) *(UObject**)(Params.data() + DCauser) = Held;
				Prop->ProcessEvent(DeathFn, Params.data());
				bDied = true;

				if (!Prop->IsDestroyed() && !Prop->IsActorBeingDestroyed())
					Prop->SilentDie(); // the death event left it standing
			}

			static int LoggedHealth = 0;

			if (++LoggedHealth <= 10)
				LOG_INFO(LogBots, "BotHarvest #{} hit {} for {:.0f}: health {:.0f} -> {:.0f}{}", Id, Prop->GetName(), Damage, HealthBefore, Prop->GetHealth(), bDied ? " (killed it)" : "");
		}

		AddSoundNow(Prop->GetActorLocation(), 1400.0f, Pawn, -1);

		++Brain.HarvestHits;

		{
			const float After = Prop->GetHealth();

			if (Brain.HarvestTracked != (void*)Prop)
			{
				Brain.HarvestTracked = Prop;
				Brain.HarvestNoProgress = 0;
			}
			else if (After >= Brain.HarvestLastHealth - 0.5f)
			{
				++Brain.HarvestNoProgress;
			}
			else
			{
				Brain.HarvestNoProgress = 0;
			}

			Brain.HarvestLastHealth = After;

			if (Brain.HarvestNoProgress >= 4)
			{
				// four swings and nothing came off it: leave it be
				Brain.SkipHarvest.push_back(Prop);
				Brain.bBreaking = false;
				Brain.bHasTarget = false;
				Brain.HarvestActor = nullptr;
				Brain.FightBreakUntil = 0.0f;
				Brain.HarvestNoProgress = 0;

				static int Gave = 0;

				if (++Gave <= 15)
					LOG_INFO(LogBots, "BotHarvest #{} gives up on {} (4 hits did nothing)", Id, Prop->GetName());

				return true;
			}
		}

		if (Brain.HarvestHits <= 6)
			LOG_INFO(LogBots, "BotHarvest #{} hit a prop for {:.0f} (hit {})", Id, Damage, Brain.HarvestHits);

		return true;
	}

	// Puts one building piece in the world the way Reboot's ServerCreateBuildingActor does for a player, paid for from the bot's wood.
	inline UClass* WallClass();
	inline UClass* StairClass();
	inline UClass* FloorClass();

	// ---- where a piece really ends up ----
	// The bots ask for a piece by where its body should be: the bottom centre of its bounds (a wall: the middle of a tile edge; a ramp, floor or
	// roof: the middle of the tile), turned so a wall faces along the yaw and a ramp rises along it. Where a piece's pivot sits inside its mesh,
	// and which way its mesh faces at yaw 0, was never checked for these pieces (the wall yaw was a guess). So the first piece of each kind is
	// measured once with the engine's own bounds (and, for a ramp, a ray on its surface) and every later one is placed with that correction.
	struct PieceFit { bool bKnown = false; float OffX = 0.0f, OffY = 0.0f, OffZ = 0.0f; float YawFix = 0.0f; };
	inline std::unordered_map<UClass*, PieceFit> gPieceFit;

	inline bool ActorBounds(AActor* Actor, FVector& Origin, FVector& Extent)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorBounds");
		static const char* Path = "/Script/Engine.Actor.GetActorBounds";
		static int CollidingOffset = FindOffsetStruct(Path, "bOnlyCollidingComponents", false);
		static int OriginOffset = FindOffsetStruct(Path, "Origin", false);
		static int ExtentOffset = FindOffsetStruct(Path, "BoxExtent", false);

		if (!Fn || !Actor || OriginOffset == -1 || ExtentOffset == -1)
			return false;

		std::vector<uint8_t> Params(Fn->GetPropertiesSize() + 32, 0);

		if (CollidingOffset != -1)
			Params[CollidingOffset] = 1;

		Actor->ProcessEvent(Fn, Params.data());
		Origin = *(FVector*)(Params.data() + OriginOffset);
		Extent = *(FVector*)(Params.data() + ExtentOffset);
		return Extent.X + Extent.Y + Extent.Z > 5.0f;
	}

	// ---- the space a piece would fill must be free ----
	// The game's own check (CantBuild) works on the building grid, and the bots' pieces are placed by where their body should go, not snapped to
	// that grid, so it never saw a clash: a bot built a ramp straight through a player's stairs in a build fight (2026-11-09). So the bots
	// check the space themselves the way a player is stopped: no piece where a wall, floor, ramp or roof (anybody's, or the map's) already
	// stands. Trees, props and the ground do not stop a player's build either, so they do not count. Rays are cast through the piece's box
	// (shrunk a little, so the neighbours a piece is meant to touch do not count) along all three axes.
	inline bool IsStructure(UObject* Actor)
	{
		static UClass* Classes[] = {
			FindObject<UClass>(L"/Script/FortniteGame.BuildingWall"), FindObject<UClass>(L"/Script/FortniteGame.BuildingFloor"),
			FindObject<UClass>(L"/Script/FortniteGame.BuildingStairs"), FindObject<UClass>(L"/Script/FortniteGame.BuildingRoof") };

		if (!Actor)
			return false;

		for (auto Class : Classes)
			if (Class && Actor->IsA(Class))
				return true;

		return false;
	}

	// Wanted: bottom centre of the piece's body; Yaw: the way a wall faces / a ramp rises. Returns what is in the way (or nullptr).
	inline UObject* StructureInTheWay(AActor* Self, UClass* PieceClass, const FVector& Wanted, float Yaw)
	{
		const bool bWall = PieceClass == WallClass();
		// half sizes in the piece's own frame (X along the yaw), and its height
		const float HalfX = bWall ? 12.0f : 256.0f, HalfY = 256.0f;
		const float Height = PieceClass == FloorClass() ? 20.0f : 384.0f;
		const float Margin = 50.0f;
		const float C = std::cos(Yaw * DegToRad), S = std::sin(Yaw * DegToRad);

		auto World = [&](float Lx, float Ly, float Z) { return FVector{ Wanted.X + Lx * C - Ly * S, Wanted.Y + Lx * S + Ly * C, Wanted.Z + Z }; };
		auto Hit = [&](const FVector& A, const FVector& B) -> UObject*
		{
			UObject* Actor = nullptr;

			if (TraceBlocked(Self, nullptr, A, B, nullptr, nullptr, &Actor) && IsStructure(Actor))
				return Actor;

			return nullptr;
		};

		const float Ix = (std::max)(HalfX - Margin, 0.0f), Iy = HalfY - Margin;
		const float Z0 = Height > 100.0f ? Margin : Height * 0.5f, Z1 = Height > 100.0f ? Height - Margin : Height * 0.5f;
		const float Fractions[] = { 0.2f, 0.5f, 0.8f };

		for (float A : Fractions)
		{
			for (float B : Fractions)
			{
				const float Z = Z0 + (Z1 - Z0) * B;
				const float Ly = -Iy + 2.0f * Iy * A;
				const float Lx = -Ix + 2.0f * Ix * A;

				// across the width (Y), at several depths and heights
				if (auto In = Hit(World(-Ix + 2.0f * Ix * B, -Iy, Z), World(-Ix + 2.0f * Ix * B, Iy, Z)))
					return In;

				// along the depth (X), only for pieces with depth (a wall is 24 thick)
				if (!bWall)
				{
					if (auto In = Hit(World(-Ix, Ly, Z), World(Ix, Ly, Z)))
						return In;
				}

				// top to bottom
				if (Height > 100.0f)
				{
					if (auto In = Hit(World(Lx, -Iy + 2.0f * Iy * B, Z1), World(Lx, -Iy + 2.0f * Iy * B, Z0)))
						return In;
				}
			}
		}

		return nullptr;
	}

	// The game's building grid: cells 512 wide and 384 high (UBuildingStructuralSupportSystem::GetWorldLocFromGridIndices is origin + index * 512, 512, 384,
	// and the other way round rounds to the nearest). A player's piece always stands on a grid level; the level for a builder whose feet are at
	// FeetZ is the one at or just below them (up to 40 above still counts: that is a step, not a gap).
	inline float GridBaseZ(float FeetZ)
	{
		return std::floor((FeetZ + 40.0f) / TileRise) * TileRise;
	}

	inline bool PlaceBuilding(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, UClass* PieceClass, const FVector& WantedIn, const FRotator& WantedRotation, int Id, bool bRefit = false)
	{
		// every piece goes on the grid in height too (sideways the callers already work in whole tiles); a refit call passes on what was snapped
		FVector Wanted = WantedIn;

		if (!bRefit)
		{
			Wanted.Z = GridBaseZ(Wanted.Z);

			static bool bGridLogged = false;

			if (!bGridLogged)
			{
				bGridLogged = true;
				auto GameStateForGrid = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
				auto Support = GameStateForGrid ? (UObject*)GameStateForGrid->GetStructuralSupportSystem() : nullptr;
				// where the grid starts, for the log only (the member is at 0x378 in the Xbox build of this version; unchecked on PC, so it is not used)
				const FVector Origin = Support ? *(FVector*)(__int64(Support) + 0x378) : FVector{};
				LOG_INFO(LogBots, "BotBuild: pieces go on the 512 x 512 x 384 grid; the game's grid origin reads ({:.0f}, {:.0f}, {:.0f}) (a multiple of the cell size means the two agree)", Origin.X, Origin.Y, Origin.Z);
			}
		}

		if (!bRefit)
		{
			if (auto In = StructureInTheWay(Pawn, PieceClass, Wanted, WantedRotation.Yaw))
			{
				static int Logged = 0;

				if (++Logged <= 30)
					LOG_INFO(LogBots, "BotBuild #{} cannot put a {} at ({:.0f}, {:.0f}, {:.0f}): {} is already there", Id, PieceClass ? PieceClass->GetName() : std::string("?"), Wanted.X, Wanted.Y, Wanted.Z, In->GetName());

				return false;
			}
		}

		// apply what is known about this piece: turn it the right way and move its pivot so the body lands where it was asked for
		auto& Fit = gPieceFit[PieceClass];
		FRotator Rotation = WantedRotation;
		FVector Location = Wanted;

		if (Fit.bKnown)
		{
			Rotation.Yaw = NormalizeAngle(Rotation.Yaw + Fit.YawFix);
			const float C = std::cos(Rotation.Yaw * DegToRad), S = std::sin(Rotation.Yaw * DegToRad);
			Location.X -= Fit.OffX * C - Fit.OffY * S;
			Location.Y -= Fit.OffX * S + Fit.OffY * C;
			Location.Z -= Fit.OffZ;
		}

		auto Controller = Cast<AFortPlayerControllerAthena>(Brain.Controller);
		auto Inventory = Controller ? Controller->GetWorldInventory() : nullptr;
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

		if (!Controller || !Inventory || !GameState || !PlayerState || !PieceClass || !Addresses::CantBuild)
			return false;

		if (auto Support = GameState->GetStructuralSupportSystem())
		{
			if (!Support->IsWorldLocValid(Location))
				return false;
		}

		TArray<ABuildingSMActor*> Existing;
		char Unused;
		static __int64 (*CantBuild)(UObject*, UObject*, FVector, FRotator, char, TArray<ABuildingSMActor*>*, char*) = decltype(CantBuild)(Addresses::CantBuild);

		if (CantBuild(GetWorld(), PieceClass, Location, Rotation, false, &Existing, &Unused))
		{
			Existing.Free();
			return false;
		}

		FTransform Transform{};
		Transform.Translation = Location;
		Transform.Rotation = Rotation.Quaternion();
		Transform.Scale3D = { 1, 1, 1 };

		auto Piece = GetWorld()->SpawnActor<ABuildingSMActor>(PieceClass, Transform);

		if (!Piece)
		{
			Existing.Free();
			return false;
		}

		// first piece of this kind: measure it (see PieceFit)
		if (!Fit.bKnown && !bRefit)
		{
			Fit.bKnown = true;
			FVector Origin{}, Extent{};

			if (ActorBounds(Piece, Origin, Extent))
			{
				const float Wx = Origin.X - Location.X, Wy = Origin.Y - Location.Y;
				const float C = std::cos(Rotation.Yaw * DegToRad), S = std::sin(Rotation.Yaw * DegToRad);
				Fit.OffX = Wx * C + Wy * S;
				Fit.OffY = -Wx * S + Wy * C;
				Fit.OffZ = (Origin.Z - Extent.Z) - Location.Z;

				// the bounds are world-aligned; at a yaw of 0 or 180 the piece's own X is the world X, at 90 or 270 it is the world Y
				const bool bAlongX = std::fabs(std::sin(Rotation.Yaw * DegToRad)) < 0.5f;
				const float OwnX = bAlongX ? Extent.X : Extent.Y, OwnY = bAlongX ? Extent.Y : Extent.X;

				if (PieceClass == WallClass() && OwnX > OwnY * 2.0f)
					Fit.YawFix = 90.0f; // the wall stood along the yaw instead of across it

				float Ahead = -1e9f, Behind = -1e9f;

				if (PieceClass == StairClass())
				{
					// which end is up: a ray on the ramp's surface a little in front of and behind its middle, along the yaw
					Ahead = SurfaceZ(Origin.X + C * 160.0f, Origin.Y + S * 160.0f, Origin.Z + Extent.Z + 50.0f, Origin.Z - Extent.Z - 50.0f);
					Behind = SurfaceZ(Origin.X - C * 160.0f, Origin.Y - S * 160.0f, Origin.Z + Extent.Z + 50.0f, Origin.Z - Extent.Z - 50.0f);

					if (Ahead > -1e8f && Behind > -1e8f && Behind > Ahead + 60.0f)
						Fit.YawFix = 180.0f;
					else if (Ahead > -1e8f && Behind > -1e8f && std::fabs(Ahead - Behind) < 30.0f)
					{
						// level along the yaw: it rises sideways
						const float Left = SurfaceZ(Origin.X - S * 160.0f, Origin.Y + C * 160.0f, Origin.Z + Extent.Z + 50.0f, Origin.Z - Extent.Z - 50.0f);
						const float Right = SurfaceZ(Origin.X + S * 160.0f, Origin.Y - C * 160.0f, Origin.Z + Extent.Z + 50.0f, Origin.Z - Extent.Z - 50.0f);

						if (Left > -1e8f && Right > -1e8f && std::fabs(Left - Right) > 60.0f)
							Fit.YawFix = Left > Right ? -90.0f : 90.0f; // rising towards its own +Y: turn it back a quarter so that side faces the yaw
					}
				}

				LOG_INFO(LogBots, "BotBuild: measured {}: body is ({:.0f}, {:.0f}, {:.0f}) from its pivot in its own frame, extent ({:.0f}, {:.0f}, {:.0f}), ramp surface ahead {:.0f} behind {:.0f}, turn fix {:.0f}",
					PieceClass->GetName(), Fit.OffX, Fit.OffY, Fit.OffZ, Extent.X, Extent.Y, Extent.Z, Ahead > -1e8f ? Ahead - (Origin.Z - Extent.Z) : -1.0f, Behind > -1e8f ? Behind - (Origin.Z - Extent.Z) : -1.0f, Fit.YawFix);

				if (std::fabs(Fit.OffX) > 30.0f || std::fabs(Fit.OffY) > 30.0f || std::fabs(Fit.OffZ) > 30.0f || Fit.YawFix != 0.0f)
				{
					Existing.Free();
					Piece->SilentDie();
					return PlaceBuilding(Brain, Pawn, PlayerState, PieceClass, Wanted, WantedRotation, Id, true);
				}
			}
			else
			{
				LOG_INFO(LogBots, "BotBuild: could not measure {} (no bounds on the server); placing it as asked", PieceClass->GetName());
			}
		}

		auto Material = UFortKismetLibrary::K2_GetResourceItemDefinition(Piece->GetResourceType());
		UFortItem* Instance = Material ? Inventory->FindItemInstance(Material) : nullptr;

		if (!Instance || Instance->GetItemEntry()->GetCount() < 10)
		{
			Existing.Free();
			Piece->SilentDie();
			return false;
		}

		bool bShouldUpdate = false;
		Inventory->RemoveItem(Instance->GetItemEntry()->GetItemGuid(), &bShouldUpdate, 10);

		if (bShouldUpdate)
			Inventory->Update();

		for (int i = 0; i < Existing.Num(); ++i)
			Existing.At(i)->K2_DestroyActor();

		Existing.Free();

		Piece->SetPlayerPlaced(true);
		Piece->InitializeBuildingActor(Controller, Piece, true);
		Piece->SetTeam(PlayerState->GetTeamIndex());

		++Brain.Builds;
		Brain.BuiltPieces.push_back(Piece);

		if (Brain.BuiltPieces.size() > 16)
			Brain.BuiltPieces.erase(Brain.BuiltPieces.begin());

		if (Brain.Builds <= 8)
			LOG_INFO(LogBots, "BotBuild #{} built a piece at ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}", Id, Location.X, Location.Y, Location.Z, Rotation.Yaw);

		return true;
	}

	inline UClass* WallClass()
	{
		static UClass* Class = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C");
		return Class;
	}

	// the ramp a player builds (the wide stair)
	inline UClass* StairClass()
	{
		static UClass* Class = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_StairW.PBWA_W1_StairW_C");
		return Class;
	}

	inline UClass* FloorClass()
	{
		static UClass* Class = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Floor.PBWA_W1_Floor_C");
		return Class;
	}

	// the cone-shaped roof
	inline UClass* RoofClass()
	{
		static UClass* Class = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_RoofC.PBWA_W1_RoofC_C");
		return Class;
	}

	inline UClass* PieceClassOf(int Kind)
	{
		switch (Kind)
		{
		case PieceStair: return StairClass();
		case PieceFloor: return FloorClass();
		case PieceRoof: return RoofClass();
		default: return WallClass();
		}
	}

	inline UClass* WindowWallClass()
	{
		static UClass* Class = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_WindowC.PBWA_W1_WindowC_C");
		return Class;
	}

	// A wooden wall on the nearest tile edge towards Toward. Building tiles are 512 wide with their centres on multiples of 512; a wall sits on an edge, 256 from the centre.
	// The wall is only asked for here: StepBuildQueue puts it up once the bot faces that way.
	inline void QueueWallToward(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, const FVector& Toward, float Now)
	{
		const FVector Pos = Pawn->GetActorLocation();
		const float CentreX = std::round(Pos.X / 512.0f) * 512.0f;
		const float CentreY = std::round(Pos.Y / 512.0f) * 512.0f;
		const float Dx = Toward.X - CentreX;
		const float Dy = Toward.Y - CentreY;
		const bool bAlongX = std::fabs(Dx) >= std::fabs(Dy);
		const float SignX = Dx >= 0.0f ? 1.0f : -1.0f, SignY = Dy >= 0.0f ? 1.0f : -1.0f;
		const float Feet = Pos.Z - 88.0f, Base = GridBaseZ(Feet);

		BotBrainState::BuildOrder Order;
		Order.Kind = PieceWall;
		Order.Location = bAlongX ? FVector{ CentreX + SignX * 256.0f, CentreY, Base } : FVector{ CentreX, CentreY + SignY * 256.0f, Base };
		Order.Yaw = bAlongX ? 0.0f : 90.0f;
		Order.FaceYaw = bAlongX ? (SignX > 0.0f ? 0.0f : 180.0f) : (SignY > 0.0f ? 90.0f : -90.0f);
		Order.bHide = true;
		Order.HidePos = FVector{ CentreX + (bAlongX ? SignX * 175.0f : 0.0f), CentreY + (bAlongX ? 0.0f : SignY * 175.0f), Pos.Z };
		Order.Toward = Toward;
		Order.QueuedAt = Now;
		Brain.BuildQueue.push_back(Order);

		// the grid level is well below the feet here, so that wall only comes up to the waist: the one above it as well, as a player would
		if (Base + TileRise - Feet < 230.0f)
		{
			Order.Location.Z = Base + TileRise;
			Order.bHide = false;
			Brain.BuildQueue.push_back(Order);
		}
	}

	// Four walls round the tile the bot stands in: starting with the side it is looking at and going round, like a player spinning on the spot.
	inline void QueueBox(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, float Now, bool bRoof)
	{
		const FVector Pos = Pawn->GetActorLocation();
		const float CentreX = std::round(Pos.X / 512.0f) * 512.0f;
		const float CentreY = std::round(Pos.Y / 512.0f) * 512.0f;
		const float Feet = Pos.Z - 88.0f, Base = GridBaseZ(Feet);
		const int First = ((int)std::lround(NormalizeAngle(Brain.bHaveAim ? Brain.AimYaw : 0.0f) / 90.0f) % 4 + 4) % 4;
		const int Turn = (rand() & 1) ? 1 : 3;
		const int Layers = Base + TileRise - Feet < 230.0f ? 2 : 1;

		Brain.BoxPlaced = 0;

		for (int Layer = 0; Layer < Layers; ++Layer)
		{
			for (int i = 0; i < 4; ++i)
			{
				const int Side = (First + Turn * i) % 4; // 0: +X, 1: +Y, 2: -X, 3: -Y
				const float Sx = Side == 0 ? 1.0f : (Side == 2 ? -1.0f : 0.0f), Sy = Side == 1 ? 1.0f : (Side == 3 ? -1.0f : 0.0f);

				BotBrainState::BuildOrder Order;
				Order.Kind = PieceWall;
				Order.Location = FVector{ CentreX + Sx * 256.0f, CentreY + Sy * 256.0f, Base + TileRise * Layer };
				Order.Yaw = Sx != 0.0f ? 0.0f : 90.0f;
				Order.FaceYaw = Side * 90.0f;
				Order.bBox = true;
				Order.QueuedAt = Now;
				Brain.BuildQueue.push_back(Order);
			}
		}

		// a cone over the box: somebody close by on higher ground can otherwise shoot straight down into it
		if (bRoof)
		{
			BotBrainState::BuildOrder Order;
			Order.Kind = PieceRoof;
			Order.Location = FVector{ CentreX, CentreY, Base + TileRise * Layers };
			Order.bFace = false;
			Order.QueuedAt = Now;
			Brain.BuildQueue.push_back(Order);
		}
	}

	// An edit: the piece is swapped for another shape of the same piece, which is what the game does for a player's edit (Reboot's ServerEditBuildingActorHook).
	inline AActor* EditPiece(BotBrainState& Brain, AActor* Piece, UClass* NewClass, int Id)
	{
		auto Controller = Cast<AFortPlayerController>(Brain.Controller);

		if (!Addresses::ReplaceBuildingActor || !NewClass || !Controller || !IsLiveBuilding(Piece))
			return nullptr;

		auto Old = (ABuildingSMActor*)Piece;

		if (Old->IsDestroyed() || Old->ClassPrivate == NewClass)
			return nullptr;

		static ABuildingSMActor* (*Replace)(ABuildingSMActor*, __int64, UClass*, int, int, uint8_t, AFortPlayerController*) = decltype(Replace)(Addresses::ReplaceBuildingActor);
		auto New = Replace(Old, 1, NewClass, Old->GetCurrentBuildingLevel(), 0, 0, Controller);

		if (!New)
			return nullptr;

		New->SetPlayerPlaced(true);

		for (auto& Built : Brain.BuiltPieces)
		{
			if (Built == Piece)
				Built = New;
		}

		return New;
	}

	// Puts up the next piece that was asked for, once the bot faces it. Returns true while there is building to do: the bot's facing then belongs to
	// the building (it cannot aim at somebody and build behind its back at the same time, and it does not shoot while it builds).
	inline bool StepBuildQueue(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, float Now, float Dt, int Id)
	{
		while (!Brain.BuildQueue.empty() && Now - Brain.BuildQueue.front().QueuedAt > 3.5f)
		{
			if (Brain.BuildQueue.front().bPlanRamp && Brain.bRampPlan)
			{
				++Brain.RampFails;
				Brain.RampNextPiece = Now + 0.4f;
			}

			Brain.BuildQueue.erase(Brain.BuildQueue.begin()); // too late to matter
		}

		if (Brain.BuildQueue.empty())
			return false;

		const auto Order = Brain.BuildQueue.front();

		if (Order.bFace)
		{
			// a builder flicks round quickly, but it is still a turn
			const float Step = 720.0f * Dt;
			Brain.AimYaw = NormalizeAngle(Brain.AimYaw + (std::max)(-Step, (std::min)(Step, NormalizeAngle(Order.FaceYaw - Brain.AimYaw))));
			Brain.bHaveAim = true;
		}

		if (Now < Brain.NextPieceAt || (Order.bFace && std::fabs(NormalizeAngle(Order.FaceYaw - Brain.AimYaw)) > 50.0f))
			return true;

		Brain.BuildQueue.erase(Brain.BuildQueue.begin());
		const float Skill = Brain.Skill < 0.0f ? 30.0f : Brain.Skill;
		Brain.NextPieceAt = Now + 0.30f - 0.002f * Skill; // the practised place pieces faster

		const bool bPlaced = PlaceBuilding(Brain, Pawn, PlayerState, PieceClassOf(Order.Kind), Order.Location, FRotator{ 0.0f, Order.Yaw, 0.0f }, Id);

		if (Order.bPlanRamp && Brain.bRampPlan)
		{
			if (bPlaced)
			{
				++Brain.RampPlaced;
				Brain.RampFails = 0;
			}
			else
			{
				++Brain.RampFails;
				Brain.RampNextPiece = Now + 0.4f;
			}
		}

		if (bPlaced && Order.bBox && ++Brain.BoxPlaced == 2)
		{
			Brain.BoxedAt = Now;
			LOG_INFO(LogBots, "BotBuild #{} boxes up, turning to place each wall", Id);
		}

		// A wall is something to stand behind: the spot just on this side of it, out of the line to whoever is shooting
		if (bPlaced && Order.bHide)
		{
			const FVector Pos = Pawn->GetActorLocation();

			if (Dist2D(Pos, Order.HidePos) < 700.0f)
			{
				Brain.HidePos = Order.HidePos;
				Brain.HideUntil = Now + RandRange(3.0f, 6.0f);
				Brain.HideYaw = std::atan2(Order.Toward.Y - Pos.Y, Order.Toward.X - Pos.X) * RadToDeg;
				Brain.HideWall = Brain.BuiltPieces.empty() ? nullptr : Brain.BuiltPieces.back();

				static int Hid = 0;

				if (++Hid <= 25)
					LOG_INFO(LogBots, "BotHide #{} built a wall and goes {:.0f} units to stand behind it for {:.1f}s", Id, Dist2D(Pos, Order.HidePos), Brain.HideUntil - Now);
			}
		}

		return !Brain.BuildQueue.empty();
	}

	// The window play: the edit takes a moment (WindowAt), then the wall has a window to shoot through, and after a few seconds it is closed again.
	inline void StepWindow(BotBrainState& Brain, float Now, int Id)
	{
		if (Brain.WindowAt > 0.0f && Now >= Brain.WindowAt)
		{
			Brain.WindowAt = 0.0f;

			if (auto Window = EditPiece(Brain, Brain.HideWall, WindowWallClass(), Id))
			{
				Brain.HideWall = Window;
				Brain.bWindowOpen = true;
				Brain.WindowUntil = Now + RandRange(1.8f, 3.5f);
				Brain.HideUntil = 0.0f; // it is at the window now, not hiding

				static int Logged = 0;

				if (++Logged <= 30)
					LOG_INFO(LogBots, "BotEdit #{} edits a window into its wall", Id);
			}
			else
			{
				static int Logged = 0;

				if (++Logged <= 10)
					LOG_INFO(LogBots, "BotEdit #{} could not edit its wall (gone, or the game refused)", Id);
			}
		}

		if (Brain.bWindowOpen && Now >= Brain.WindowUntil)
		{
			Brain.bWindowOpen = false;

			if (auto Wall = EditPiece(Brain, Brain.HideWall, WallClass(), Id))
			{
				Brain.HideWall = Wall;
				Brain.HideUntil = Now + RandRange(1.0f, 2.5f); // closed up again: a breath behind it

				static int Logged = 0;

				if (++Logged <= 30)
					LOG_INFO(LogBots, "BotEdit #{} closes the window again", Id);
			}
			else
			{
				Brain.HideWall = nullptr;
			}
		}
	}

	// Emotes that exist in this build are found once; the ones that don't just fail to load.
	inline const std::vector<UObject*>& GetEmotes()
	{
		static std::vector<UObject*> Emotes;
		static bool bLoaded = false;

		if (!bLoaded)
		{
			bLoaded = true;

			static const wchar_t* Paths[] = {
				L"/Game/Athena/Items/Cosmetics/Dances/EID_DanceMoves.EID_DanceMoves", L"/Game/Athena/Items/Cosmetics/Dances/EID_Floss.EID_Floss",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Fresh.EID_Fresh", L"/Game/Athena/Items/Cosmetics/Dances/EID_BoogieDown.EID_BoogieDown",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_TakeTheL.EID_TakeTheL", L"/Game/Athena/Items/Cosmetics/Dances/EID_Hype.EID_Hype",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Salute.EID_Salute", L"/Game/Athena/Items/Cosmetics/Dances/EID_Wave.EID_Wave",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Clapping.EID_Clapping", L"/Game/Athena/Items/Cosmetics/Dances/EID_ElectroShuffle.EID_ElectroShuffle",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Shimmer.EID_Shimmer", L"/Game/Athena/Items/Cosmetics/Dances/EID_Dab.EID_Dab",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Spooky.EID_Spooky", L"/Game/Athena/Items/Cosmetics/Dances/EID_BestMates.EID_BestMates",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Disco.EID_Disco", L"/Game/Athena/Items/Cosmetics/Dances/EID_Capoeira.EID_Capoeira",
				L"/Game/Athena/Items/Cosmetics/Dances/EID_Bow.EID_Bow", L"/Game/Athena/Items/Cosmetics/Dances/EID_Celebrate.EID_Celebrate",
			};

			static auto ObjectClass = FindObject<UClass>(L"/Script/CoreUObject.Object");

			for (auto Path : Paths)
			{
				UObject* Asset = StaticLoadObjectOriginal ? StaticLoadObjectOriginal(ObjectClass, nullptr, Path, nullptr, 0, nullptr, false) : FindObject(Path);

				if (Asset)
					Emotes.push_back(Asset);
			}

			LOG_INFO(LogBots, "BotEmote: {} of {} emotes exist in this build", Emotes.size(), sizeof(Paths) / sizeof(Paths[0]));
		}

		return Emotes;
	}

	// Same as the game does for a player's emote (Reboot's ServerPlayEmoteItemHook), but the pawn is the bot's own since it has no controller pawn.
	inline void SetHeldWeaponVisible(AFortPlayerPawnAthena* Pawn, bool bVisible)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.SetWeaponVisibility");

		if (!Fn)
			return;

		auto Weapon = Pawn->GetCurrentWeapon();

		if (!Weapon)
			return;

		struct { bool bForceUpdate; bool bDesiredVisibility; } Params{ true, bVisible };
		Weapon->ProcessEvent(Fn, &Params);
	}

	inline void PlayEmote(AFortPlayerStateAthena* PlayerState, int Index, int Id)
	{
		const auto& Emotes = GetEmotes();

		if (Emotes.empty() || !PlayerState || !Addresses::GiveAbilityAndActivateOnce)
			return;

		auto Asset = Emotes[(size_t)Index % Emotes.size()];
		auto AbilitySystemComponent = PlayerState->GetAbilitySystemComponent();
		static auto EmoteAbility = FindObject(L"/Game/Abilities/Emotes/GAB_Emote_Generic.Default__GAB_Emote_Generic_C");

		if (!AbilitySystemComponent || !EmoteAbility)
			return;

		auto Spec = MakeNewSpec((UClass*)EmoteAbility, Asset, true);

		if (!Spec)
			return;

		static unsigned int* (*GiveAbilityAndActivateOnce)(UAbilitySystemComponent*, int*, __int64, void*) = decltype(GiveAbilityAndActivateOnce)(Addresses::GiveAbilityAndActivateOnce);

		int Handle = 0;
		GiveAbilityAndActivateOnce(AbilitySystemComponent, &Handle, __int64(Spec), nullptr);

		static int Logged = 0;

		if (++Logged <= 10)
			LOG_INFO(LogBots, "BotEmote #{} plays {}", Id, Asset->GetPathName());
	}

	// Walks the pawn with the game's own movement input (what a player's stick does), so the character movement code does the acceleration,
	// the floor following, steps and slopes. Returns false if the function is not there.
	inline bool AddMoveInput(AFortPlayerPawnAthena* Pawn, float DirX, float DirY, float Scale = 1.0f)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/Engine.Pawn.AddMovementInput");
		static int DirectionOffset = FindOffsetStruct("/Script/Engine.Pawn.AddMovementInput", "WorldDirection", false);
		static int ScaleOffset = FindOffsetStruct("/Script/Engine.Pawn.AddMovementInput", "ScaleValue", false);
		static int ForceOffset = FindOffsetStruct("/Script/Engine.Pawn.AddMovementInput", "bForce", false);
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LOG_INFO(LogBots, "BotMove: AddMovementInput function {} (direction@{} scale@{} force@{}); movement_input={}", Fn != nullptr, DirectionOffset, ScaleOffset, ForceOffset, gMovementInput);
		}

		if (!Fn || DirectionOffset == -1 || ScaleOffset == -1)
			return false;

		std::vector<uint8_t> Params(Fn->GetPropertiesSize() + 16, 0);
		*(FVector*)(Params.data() + DirectionOffset) = FVector{ DirX, DirY, 0.0f };
		*(float*)(Params.data() + ScaleOffset) = Scale;

		if (ForceOffset != -1)
			Params[ForceOffset] = 1;

		Pawn->ProcessEvent(Fn, Params.data());
		return true;
	}

	// A person speeds up and slows down: the velocity moves towards the wanted one at a limited rate instead of jumping to it, so a turn or a stop takes
	// a fraction of a second. Other players' screens draw a pawn from its velocity, and a velocity that flips in a single frame looks like a teleport.
	inline void RampVelocity(FVector& Velocity, FVector& Acceleration, float TargetX, float TargetY, float Dt)
	{
		if (Dt <= 0.0f)
			return;

		const float TargetSpeed = std::sqrt(TargetX * TargetX + TargetY * TargetY);
		const float Speed = std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y);
		const float Rate = TargetSpeed + 20.0f < Speed ? 3600.0f : 2600.0f; // slowing down is a bit quicker than speeding up
		float Ex = TargetX - Velocity.X, Ey = TargetY - Velocity.Y;
		const float Error = std::sqrt(Ex * Ex + Ey * Ey);
		const float Step = Rate * Dt;

		if (Error > Step && Error > 0.0f)
		{
			Ex *= Step / Error;
			Ey *= Step / Error;
		}

		Velocity.X += Ex;
		Velocity.Y += Ey;
		Acceleration.X = Error > 1.0f ? Ex / Dt : 0.0f;
		Acceleration.Y = Error > 1.0f ? Ey / Dt : 0.0f;
	}

	inline void Apply(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, AFortInventory* Inventory, UObject* Movement, const BotAction& Action, float Now, int Id)
	{
		if (Brain.ReleaseAt > 0.0f && Now >= Brain.ReleaseAt)
		{
			Brain.ReleaseAt = 0.0f;

			// the fire ability waits for the shooter's hit data, which a bot never sends, so it would stay active and block the next shot (two thirds of
			// the activations failed for that reason): end it as soon as the shot has gone off
			if (auto HeldWeapon = Pawn->GetCurrentWeapon())
			{
				CallSafe(HeldWeapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.ServerReleaseWeaponPrimaryAbility"));
				CallSafe(HeldWeapon, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.CancelActiveAbility"));
			}
		}

		static auto VelocityOffset = Movement->GetOffset("Velocity");
		static auto AccelerationOffset = Movement->GetOffset("Acceleration");
		static auto MaxWalkSpeedOffset = Movement->GetOffset("MaxWalkSpeed", false);

		// side effects of entering a state
		if (Brain.AppliedState != Brain.State)
		{
			Brain.AppliedState = Brain.State;

			if (Brain.State == EBotBrainState::Skydive)
				SetSkyVisuals(Pawn, true, false);
			else if (Brain.State == EBotBrainState::Glide)
				SetSkyVisuals(Pawn, false, true);
			else if (Brain.State == EBotBrainState::Ground || Brain.State == EBotBrainState::Wander)
				SetSkyVisuals(Pawn, false, false);
		}

		static auto ModeOffset = Movement->GetOffset("MovementMode");
		const bool bOnGround = Movement->Get<uint8_t>(ModeOffset) == 1;
		const bool bUseInput = gMovementInput && bOnGround && Action.bDriveXY && !Action.bDriveZ && Action.Speed > 50.0f;

		if (MaxWalkSpeedOffset != -1)
			Movement->Get<float>(MaxWalkSpeedOffset) = bUseInput ? Action.Speed : Action.MaxWalkSpeed; // with input the speed limit is the speed asked for
		if (MaxWalkSpeedOffset != -1 && Action.bDriveXY && Action.Speed >= SprintSpeed * 0.95f)
			Movement->Get<float>(MaxWalkSpeedOffset) *= gSprintMaxWalk;

		// The movement component brakes (friction) a bot every frame because nothing "presses a key"; that took more than half of the speed we asked for.
		// With the braking off, the velocity written stays until the next one is written.
		{
			static int GroundFrictionOffset = gMovementInput ? -1 : Movement->GetOffset("GroundFriction", false);
			static int BrakingDecelOffset = gMovementInput ? -1 : Movement->GetOffset("BrakingDecelerationWalking", false);
			static int BrakingFactorOffset = gMovementInput ? -1 : Movement->GetOffset("BrakingFrictionFactor", false);
			static int BrakingFrictionOffset = gMovementInput ? -1 : Movement->GetOffset("BrakingFriction", false);

			if (GroundFrictionOffset != -1) Movement->Get<float>(GroundFrictionOffset) = 0.0f;
			if (BrakingDecelOffset != -1) Movement->Get<float>(BrakingDecelOffset) = 0.0f;
			if (BrakingFactorOffset != -1) Movement->Get<float>(BrakingFactorOffset) = 0.0f;
			if (BrakingFrictionOffset != -1) Movement->Get<float>(BrakingFrictionOffset) = 0.0f;
		}

		// why does it keep turning round? (diagnostic: a flip of more than 100 degrees in the direction it is told to walk)
		{
			static int Flips = 0;

			if (Action.bDriveXY && bOnGround && Action.Speed > 300.0f && Id < 12 && Flips < 60)
			{
				const float Heading = std::atan2(Action.DirY, Action.DirX) * RadToDeg;

				if (Brain.HavePrevHeading && std::fabs(NormalizeAngle(Heading - Brain.PrevHeading)) > 100.0f && Now - Brain.PrevHeadingTime < 0.5f && Brain.State == EBotBrainState::Ground)
				{
					++Flips;
					LOG_INFO(LogBots, "BotFlip #{}: heading {:.0f} -> {:.0f} state={} kind={} hasTarget={} routeLen={} routeIdx={} avoid={} escape={} sidestep={} breaking={} pause={} blockedEvents={} targetDist={:.0f}",
						Id, Brain.PrevHeading, Heading, BotBrainStateName(Brain.State), (int)Brain.TargetKind, Brain.bHasTarget, (int)Brain.NavPath.size(), (int)Brain.NavIndex, Brain.AvoidUntil > Now, Brain.EscapeUntil > Now, Brain.SidestepUntil > Now, Brain.bBreaking, Brain.PauseUntil > Now, Brain.BlockedEvents, Brain.bHasTarget ? Dist2D(Pawn->GetActorLocation(), Brain.Target) : -1.0f);
				}

				Brain.PrevHeading = Heading;
				Brain.PrevHeadingTime = Now;
				Brain.HavePrevHeading = true;
			}
		}

		if (Brain.JumpInstance)
		{
			static auto JumpEndFn = FindObject<UFunction>(L"/Script/GameplayAbilities.GameplayAbility.K2_EndAbility");

			if (Brain.JumpEndAt < 0.0f)
				Brain.JumpEndAt = Now + 0.9f;
			else if (Now >= Brain.JumpEndAt)
			{
				if (JumpEndFn && Brain.JumpInstance->IsValidLowLevel())
					Brain.JumpInstance->ProcessEvent(JumpEndFn);

				Brain.JumpInstance = nullptr;
			}
		}

		if (Brain.JumpWatch > 0)
		{
			static int Watched = 0;
			static int JumpZOffset = Movement->GetOffset("JumpZVelocity", false);
			static int PressedProp = Pawn->GetOffset("bPressedJump", false);
			static int ModeOffset2 = Movement->GetOffset("MovementMode", false);

			if (Watched < 40)
			{
				bool bPressed = false;

				if (PressedProp != -1)
				{
					if (auto Prop = Pawn->GetProperty("bPressedJump", false))
						bPressed = Pawn->ReadBitfieldValue(PressedProp, GetFieldMask(Prop));
				}

				LOG_INFO(LogBots, "BotJumpWatch #{} frame {}: mode={} vz={:.0f} jumpZVelocity={:.0f} bPressedJump={}", Id, 4 - Brain.JumpWatch, ModeOffset2 != -1 ? (int)Movement->Get<uint8_t>(ModeOffset2) : -1, Movement->Get<FVector>(VelocityOffset).Z, JumpZOffset != -1 ? Movement->Get<float>(JumpZOffset) : -1.0f, bPressed);
				++Watched;
			}

			--Brain.JumpWatch;
		}

		// A player sprints by holding the sprint key: the controller's flags are what the game looks at (and what other players' screens animate from)
		// Nobody sprints sideways or backwards: the game only lets a player sprint the way they face, and the sprint animation other players see is a
		// forward run. A bot that keeps its body on an enemy (or is still turning away from one) while it moves another way goes at the normal run
		// speed with the strafe/backpedal animation instead of gliding along at sprint speed.
		float MoveSpeed = Action.Speed;

		if (bOnGround && Action.bDriveXY && Brain.bHaveAim && (Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat)
			&& std::fabs(NormalizeAngle(std::atan2(Action.DirY, Action.DirX) * RadToDeg - Brain.AimYaw)) > 55.0f)
			MoveSpeed = (std::min)(MoveSpeed, WalkSpeed);

		{
			const bool bSprinting = bOnGround && Action.bDriveXY && MoveSpeed >= SprintSpeed * 0.95f;

			if (Brain.Controller && bSprinting != Brain.bSprintFlag)
			{
				Brain.bSprintFlag = bSprinting;
				SetPawnBitOn(Brain.Controller, "bWantsToSprint", bSprinting);
				SetPawnBitOn(Brain.Controller, "bHoldingSprint", bSprinting);

				// the sprint ability is what makes the game (and other players' animation) treat the pawn as sprinting
				static const wchar_t* EndFnPath = L"/Script/GameplayAbilities.GameplayAbility.K2_EndAbility";
				static auto EndFn = FindObject<UFunction>(EndFnPath);

				if (bSprinting)
				{
					UObject* Instance = nullptr;
					const bool bOk = gSprintAbility && ActivateAbilityNamed(PlayerState, "FortGameplayAbility_Sprint", &Instance);
					Brain.SprintInstance = Instance;

					static int Logged = 0;

					if (++Logged <= 6)
						LOG_INFO(LogBots, "BotSprint #{}: sprint ability activated={} instance={}", Id, bOk, (void*)Instance);
				}
				else if (Brain.SprintInstance && EndFn && Brain.SprintInstance->IsValidLowLevel())
				{
					Brain.SprintInstance->ProcessEvent(EndFn);
					Brain.SprintInstance = nullptr;
				}
			}

			// what other players' animation looks at: the pawn's replicated movement style (a sprinting player reads 3, running 0; measured on a real player)
			{
				static int StyleOffset = Pawn->GetOffset("CurrentMovementStyle", false);

				// The game slows a pawn whose movement style reads "sprinting" to a crawl when it is driven the way a bot is (about 105 units/s whatever its speed
				// is set to), so the pawn never holds the style during the game's own movement update: it is only set while the world is replicated
				// (SprintStyleBegin / SprintStyleEnd, called around ServerReplicateActors), which is all other players' animation reads.
				if (gMoveStyle && StyleOffset != -1 && bOnGround && bSprinting != Brain.bStyleSprint)
				{
					Brain.bStyleSprint = bSprinting;

					auto It = std::find(gSprinters.begin(), gSprinters.end(), Pawn);

					if (bSprinting && It == gSprinters.end())
						gSprinters.push_back(Pawn);
					else if (!bSprinting && It != gSprinters.end())
						gSprinters.erase(It);

					if (!bSprinting)
						Pawn->Get<uint8_t>(StyleOffset) = 0;
				}
			}

			// how fast it really got somewhere while it was being told to walk (position change, not the velocity field)
			{
				const FVector Here = Pawn->GetActorLocation();

				if (Brain.SpeedPrevTime > 0.0f && Now - Brain.SpeedPrevTime > 0.02f && Now - Brain.SpeedPrevTime < 0.6f && Action.bDriveXY && bOnGround && Action.Speed > 300.0f)
				{
					Brain.SpeedSum += Dist2D(Here, Brain.SpeedPrevPos) / (Now - Brain.SpeedPrevTime);
					Brain.AskedSum += Action.Speed;
					++Brain.SpeedSamples;
				}

				Brain.SpeedPrevPos = Here;
				Brain.SpeedPrevTime = Now;
			}

			if (Id < 4 && Now >= Brain.NextMoveLog)
			{
				Brain.NextMoveLog = Now + 10.0f;
				LOG_INFO(LogBots, "BotSpeed #{}: over {} walking updates it was asked for {:.0f} units/s on average and really moved at {:.0f}", Id, Brain.SpeedSamples, Brain.SpeedSamples ? Brain.AskedSum / Brain.SpeedSamples : 0.0f, Brain.SpeedSamples ? Brain.SpeedSum / Brain.SpeedSamples : 0.0f);
				Brain.SpeedSum = Brain.AskedSum = 0.0f;
				Brain.SpeedSamples = 0;
				{
					static auto MaxSpeedFn = FindObject<UFunction>(L"/Script/Engine.MovementComponent.GetMaxSpeed");
					static auto MaxAccelFn = FindObject<UFunction>(L"/Script/Engine.CharacterMovementComponent.GetMaxAcceleration");
					float MaxSpeedNow = -1.0f, MaxAccelNow = -1.0f;
					struct { float R; char Pad[12]; } P1{}, P2{};
					if (MaxSpeedFn) { Movement->ProcessEvent(MaxSpeedFn, &P1); MaxSpeedNow = P1.R; }
					if (MaxAccelFn) { Movement->ProcessEvent(MaxAccelFn, &P2); MaxAccelNow = P2.R; }
					LOG_INFO(LogBots, "BotMoveDbg #{}: sprintflag={} GetMaxSpeed={:.0f} GetMaxAcceleration={:.0f}", Id, Brain.bSprintFlag, MaxSpeedNow, MaxAccelNow);
				}
				LOG_INFO(LogBots, "BotMove #{}: asked {:.0f} units/s sprinting flag={} pawn says sprinting={} style={} accel={:.0f} maxWalkSpeed={:.0f} velocity={:.0f}", Id, Action.bDriveXY ? Action.Speed : 0.0f, Brain.bSprintFlag, PawnIsSprinting(Pawn), PawnMovementStyle(Pawn), std::sqrt(Movement->Get<FVector>(AccelerationOffset).X * Movement->Get<FVector>(AccelerationOffset).X + Movement->Get<FVector>(AccelerationOffset).Y * Movement->Get<FVector>(AccelerationOffset).Y),
					MaxWalkSpeedOffset != -1 ? Movement->Get<float>(MaxWalkSpeedOffset) : -1.0f, std::sqrt(Movement->Get<FVector>(Movement->GetOffset("Velocity")).X * Movement->Get<FVector>(Movement->GetOffset("Velocity")).X + Movement->Get<FVector>(Movement->GetOffset("Velocity")).Y * Movement->Get<FVector>(Movement->GetOffset("Velocity")).Y));
			}
		}

		FVector& Velocity = Movement->Get<FVector>(VelocityOffset);
		FVector& Acceleration = Movement->Get<FVector>(AccelerationOffset);
		const float MoveDt = (std::min)((std::max)(Now - Brain.MoveLastTime, 0.0f), 0.1f);
		Brain.MoveLastTime = Now;

		ResolveMelee(Brain, Pawn, Now, Id);

		{
			static bool bReadSlow = false;

			if (!bReadSlow)
			{
				bReadSlow = true;
				static int MultOffset = Pawn->GetOffset("MeleeCombatSlowSpeedMultiplier", false);
				static int TimeOffset = Pawn->GetOffset("MeleeCombatSlowDuration", false);
				const float Mult = MultOffset != -1 ? Pawn->Get<float>(MultOffset) : -1.0f, Time = TimeOffset != -1 ? Pawn->Get<float>(TimeOffset) : -1.0f;

				if (Mult > 0.2f && Mult <= 1.0f) gMeleeSlowMult = Mult;
				if (Time > 0.0f && Time < 5.0f) gMeleeSlowTime = Time;

				LOG_INFO(LogBots, "BotMelee: the game's own swing slow is x{:.2f} for {:.2f}s (read {} / {})", gMeleeSlowMult, gMeleeSlowTime, Mult, Time);
			}
		}

		if (Action.bFace)
		{
			Brain.FaceYaw = Action.Yaw;
			Brain.bHaveFaceYaw = true;
		}

		if (Action.bSetPitch)
			Brain.AimPitch = Action.Pitch;

		// In a fight the bot really turns to its target: it swings at a limited speed towards where the target is *now*, so a fast
		// mover can get away from it, and the bullets go where it is actually pointing (see ResolveShot). Outside a fight it just faces its heading.
		const bool bCombat = Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat;
		float DesiredYaw = Brain.FaceYaw;
		float DesiredPitch = Brain.AimPitch;

		if (bCombat && Brain.AimTarget && Now - Brain.AimTargetTime < 0.6f && Brain.AimTarget->IsValidLowLevel() && !Brain.AimTarget->IsActorBeingDestroyed())
		{
			const FVector From = Pawn->GetActorLocation();
			FVector To = Brain.AimTarget->GetActorLocation();

			// It aims at where the target was a moment ago, not where it is: the eye sees, the brain decides, the hand follows.
			// Somebody running across its view is therefore ahead of its gun, and it has to keep correcting.
			{
				auto& History = Brain.AimHistory;

				if (Brain.AimHistoryTarget != (void*)Brain.AimTarget)
				{
					History.clear();
					Brain.AimHistoryTarget = (void*)Brain.AimTarget;
				}

				History.push_back({ Now, To });

				while (History.size() > 1 && Now - History.front().first > 1.5f)
					History.pop_front();

				const float Lag = (std::max)(0.12f, 0.5f - 0.004f * (Brain.Skill < 0.0f ? 30.0f : Brain.Skill));
				To = History.front().second;

				for (auto& Entry : History)
				{
					if (Entry.first <= Now - Lag)
						To = Entry.second;
					else
						break;
				}
			}

			DesiredYaw = std::atan2(To.Y - From.Y, To.X - From.X) * RadToDeg;
			DesiredPitch = std::atan2((To.Z + 10.0f) - (From.Z + 50.0f), (std::max)(Dist2D(From, To), 1.0f)) * RadToDeg;
		}

		if (!Brain.bHaveAim || !bCombat)
		{
			// Walking about: the body turns to the heading at a human rate instead of snapping to it. (It used to glance sideways now and then as well, which
			// turned the whole body away from the way it was moving: from the side that looks like being pulled backwards.)
			if (!bCombat && Brain.bHaveAim && bOnGround)
			{
				Brain.LookOffset = 0.0f;
				const float Dt = (std::min)(Now - Brain.AimLastTime, 0.1f);
				const float Step = 600.0f * Dt;
				Brain.AimYaw = NormalizeAngle(Brain.AimYaw + (std::max)(-Step, (std::min)(Step, NormalizeAngle(DesiredYaw - Brain.AimYaw))));
			}
			else
			{
				Brain.AimYaw = DesiredYaw;
			}

			Brain.AimPitch = DesiredPitch;
			Brain.bHaveAim = Brain.bHaveFaceYaw;
		}
		else
		{
			const float Dt = (std::min)(Now - Brain.AimLastTime, 0.1f);
			const float Skill = Brain.Skill < 0.0f ? 30.0f : Brain.Skill;
			// the slow, skill-bound turn is the gun coming round onto a target; a bot with nothing in its sights (running away, running to cover)
			// turns its body to where it runs as fast as anyone does, or it would run off backwards for a second or two
			const bool bAimingAtSomeone = Brain.AimTarget && Now - Brain.AimTargetTime < 0.6f;
			const float Step = (bAimingAtSomeone ? TurnRate(Skill) : 600.0f) * Dt;
			const float Ease = bAimingAtSomeone ? 1.0f - std::exp(-AimSettleRate(Skill) * Dt) : 1.0f;   // share of the remaining gap closed this tick
			const float YawGap = NormalizeAngle(DesiredYaw - Brain.AimYaw);
			const float PitchGap = DesiredPitch - Brain.AimPitch;
			Brain.AimYaw += (std::max)(-Step, (std::min)(Step, YawGap * Ease));
			Brain.AimYaw = NormalizeAngle(Brain.AimYaw);
			Brain.AimPitch += (std::max)(-Step, (std::min)(Step, PitchGap * Ease));
		}

		Brain.AimLastTime = Now;

		// a hand that is never quite steady: a slow drift, bigger for the unskilled
		if (Now >= Brain.NextWobble)
		{
			Brain.NextWobble = Now + 0.25f;
			const float Wobble = AimSigma(Brain.Skill < 0.0f ? 30.0f : Brain.Skill) * 0.35f;
			Brain.WobbleYaw = Gauss() * Wobble;
			Brain.WobblePitch = Gauss() * Wobble * 0.7f;
		}

		// The aiming stance: other players' screens show the weapon raised (instead of the idle pose) while the pawn's replicated bIsTargeting is on.
		{
			auto Held = Pawn->GetCurrentWeapon();
			const bool bHoldingRanged = Held && GetItemInfo((UObject*)Held->GetWeaponData()).Kind == EBotItemKind::Gun;
			const bool bWantAim = bCombat && bHoldingRanged && Brain.AimTarget && Now - Brain.AimTargetTime < 1.0f;

			if (bWantAim != Brain.bTargetingShown)
			{
				Brain.bTargetingShown = bWantAim;
				SetPawnBit(Pawn, "bIsTargeting", bWantAim);
				CallOnRep(Pawn, L"/Script/FortniteGame.FortPlayerPawn.OnRep_IsTargeting", !bWantAim);
			}
		}

		// building comes first: the bot turns to each piece it places (see StepBuildQueue)
		const bool bBuildingNow = StepBuildQueue(Brain, Pawn, PlayerState, Now, MoveDt, Id);

		// while fighting, keep re-applying the facing every tick so nothing turns the bot back towards where it walks
		if (bBuildingNow || (Brain.bHaveFaceYaw && (Action.bFace || bCombat)))
			SetYaw(Pawn, Brain.AimYaw);

		if (Action.bSetPitch || bCombat)
			SetAimPitch(Pawn, Brain.AimPitch);

		if (bUseInput && AddMoveInput(Pawn, Action.DirX, Action.DirY))
		{
			// moved by the character movement itself
		}
		else if (bOnGround && Now < Brain.PlantUntil)
		{
			// in the middle of a swing: planted
			if (bRampMotion)
			{
				RampVelocity(Velocity, Acceleration, 0.0f, 0.0f, MoveDt);
			}
			else
			{
				Velocity.X = 0.0f;
				Velocity.Y = 0.0f;
			}
		}
		else if (Action.bDriveXY)
		{
			float Dx = Action.DirX, Dy = Action.DirY;

			// nobody walks a ruler-straight line: a gentle drift to either side, a few degrees at a time
			if (!bCombat && bOnGround && MoveSpeed > 250.0f && Brain.BlockedAccum < 0.2f)
			{
				const float Drift = 6.0f * std::sin(Now * 0.8f + Brain.WeavePhase) + 3.0f * std::sin(Now * 2.1f + Brain.WeavePhase * 1.7f);
				RotateDir(Dx, Dy, Drift);
			}

			if (bOnGround && bRampMotion)
			{
				RampVelocity(Velocity, Acceleration, Dx * MoveSpeed, Dy * MoveSpeed, MoveDt);
			}
			else
			{
				Velocity.X = Dx * MoveSpeed;
				Velocity.Y = Dy * MoveSpeed;
				Acceleration.X = Dx * MoveSpeed;
				Acceleration.Y = Dy * MoveSpeed;
			}

			// the movement component rebuilds its acceleration from the pending input every tick (zero for a bot), and that is what gets replicated
			// to the other players, whose walk animation depends on it. The pawn packs that acceleration for the other players in absolute units
			// (AFortPlayerPawn::PreReplicateAcceleration: size/4096 in a byte, plus a direction byte) and their side then uses it as is
			// (OnRep_AccelerationPack -> UpdateProxyAcceleration), so it has to be the size a real player's is: someone holding a key down sends the
			// full 1000. At the old 15% the bots looked like players barely touching the stick while moving at a full run, which is the sliding.
			// The input cannot speed the bot up: the game's max speed reads 0 for a bot, so the movement update only ever keeps the size of the velocity.
			if (gMoveAssist && bOnGround && MoveSpeed > 50.0f)
				AddMoveInput(Pawn, Dx, Dy, MoveSpeed >= WalkSpeed * 0.8f ? 1.0f : (std::max)(gMoveAssistScale, MoveSpeed / (std::max)(WalkSpeed, 1.0f)));

			// players hop about while they run (about four times a minute here), more in a fight
			if (bOnGround && MoveSpeed > 350.0f && Now >= Brain.NextHop)
			{
				Brain.NextHop = Now + (bCombat ? RandRange(3.0f, 9.0f) : RandRange(4.0f, 14.0f));

				if (Brain.State != EBotBrainState::Wander)
					BotJump(Pawn, PlayerState, &Brain);
			}
		}
		else if (Action.bStop)
		{
			if (bOnGround && bRampMotion)
			{
				RampVelocity(Velocity, Acceleration, 0.0f, 0.0f, MoveDt);
			}
			else
			{
				Velocity.X = 0.0f;
				Velocity.Y = 0.0f;
				Acceleration.X = 0.0f;
				Acceleration.Y = 0.0f;
			}
		}
		else if (bOnGround)
		{
			Velocity.X *= 0.5f; // nothing asked for: with the braking off it would otherwise slide on
			Velocity.Y *= 0.5f;
		}

		{
			const FVector Here = Pawn->GetActorLocation();
			const float RealDt = Now - Brain.RealPrevTime;

			if (bOnGround && Brain.RealPrevTime > 0.0f && RealDt > 0.01f && RealDt < 0.25f)
			{
				const float RealX = (Here.X - Brain.RealPrevPos.X) / RealDt, RealY = (Here.Y - Brain.RealPrevPos.Y) / RealDt;
				const float Asked = std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y), Real = std::sqrt(RealX * RealX + RealY * RealY);

				// three updates in a row at under half the speed asked for: held up, not just setting off
				Brain.HeldUpFrames = (Asked > 150.0f && Real < Asked * 0.5f) ? Brain.HeldUpFrames + 1 : 0;

				if (Brain.HeldUpFrames >= 3 && gShownVelocity.size() < 256)
					gShownVelocity.push_back({ &Velocity, RealX, RealY, 0.0f, 0.0f });
			}
			else
			{
				Brain.HeldUpFrames = 0;
			}

			Brain.RealPrevPos = Here;
			Brain.RealPrevTime = Now;
		}

		if (Action.bDriveZ)
			Velocity.Z = Action.VerticalSpeed;

		if (Action.bWarp)
		{
			Pawn->TeleportTo(Action.WarpPos, Pawn->GetActorRotation());
			Velocity.X = Velocity.Y = Velocity.Z = 0.0f;
		}

		if (Action.bJump)
			BotJump(Pawn, PlayerState, &Brain);

		if (Action.bEmote)
		{
			PlayEmote(PlayerState, Action.EmoteIndex, Id);
			SetHeldWeaponVisible(Pawn, false); // a player's pickaxe or gun goes away while they dance
			Brain.bWeaponHidden = true;
			Brain.WeaponHiddenUntil = UGameplayStatics::GetTimeSeconds(GetWorld()) + 6.0f;
		}

		if (Brain.bWeaponHidden && (UGameplayStatics::GetTimeSeconds(GetWorld()) >= Brain.WeaponHiddenUntil || Action.bAttackRanged || Action.bAttackMelee || (Brain.State != EBotBrainState::Wander && UGameplayStatics::GetTimeSeconds(GetWorld()) >= Brain.DanceUntil)))
		{
			Brain.bWeaponHidden = false;
			SetHeldWeaponVisible(Pawn, true);
		}

		if (Action.bHarvest)
			HarvestHit(Brain, Pawn, PlayerState, Id);

		if (Action.bBuildWall)
			QueueWallToward(Brain, Pawn, Action.BuildToward, Now);

		if (Action.bBoxUp)
			QueueBox(Brain, Pawn, Now, Action.bBoxRoof);

		for (auto& Piece : Action.Pieces)
		{
			BotBrainState::BuildOrder Order;
			Order.Kind = Piece.Kind;
			Order.Location = Piece.Location;
			Order.Yaw = Piece.Yaw;
			Order.FaceYaw = Piece.Yaw; // ramps and their walls go up ahead, the way the run goes
			Order.bPlanRamp = Piece.bPlanRamp;
			Order.QueuedAt = Now;
			Brain.BuildQueue.push_back(Order);
		}

		StepWindow(Brain, Now, Id);

		if (Action.bSearchChest)
			SearchChest(Brain, Pawn, Id);

		if (Action.bPickup)
			PickUp(Brain, Pawn, Now, Id);

		if (Action.bEquip && Action.EquipDef)
		{
			Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Action.EquipDef, Action.EquipGuid);
			Brain.NextWeaponShowCheck = 0.0f; // make sure the new weapon is shown on the very next tick
			LOG_INFO(LogBots, "BotLoot #{} equipped {}", Id, Action.EquipDef->GetPathName());
		}

		// using a healing item the way a player does: once it is in hand, start its own use ability (animation, healing and spending the item come with it)
		if (Brain.State == EBotBrainState::Heal && Brain.HealStart > 0.0f && !Brain.bHealAbilityFired && Now - Brain.HealStart > 0.4f)
		{
			auto Held = Pawn->GetCurrentWeapon();

			if (Held && (UObject*)Held->GetWeaponData() == Brain.HealDefPtr)
			{
				Brain.bHealAbilityFired = true;
				Brain.bHealAbilityOk = Attack(Brain, Pawn, PlayerState, false, Id, nullptr, nullptr, true);

				static int Fired = 0;

				if (++Fired <= 12)
					LOG_INFO(LogBots, "BotHeal #{} started the item's own use ability: {}", Id, Brain.bHealAbilityOk);
			}
		}

		if (Action.bUseHeal)
		{
			// The use finished: spend one item and add its effect. If the item can't be taken out of the inventory nothing is healed,
			// otherwise a stuck inventory would give a bot endless free healing.
			const float Health = Pawn->GetHealth();
			const float Shield = Pawn->GetShield();
			bool bShouldUpdate = false;

			if (Inventory && Inventory->RemoveItem(Action.HealGuid, &bShouldUpdate, 1, false))
			{
				if (bShouldUpdate)
					Inventory->Update();

				const float NewHealth = (Action.HealHealth > 0.0f && Health < Action.HealCapHealth) ? (std::min)(Action.HealCapHealth, Health + Action.HealHealth) : Health;
				const float NewShield = (Action.HealShield > 0.0f && Shield < Action.HealCapShield) ? (std::min)(Action.HealCapShield, Shield + Action.HealShield) : Shield;

				if (NewHealth != Health)
					Pawn->SetHealth(NewHealth);

				if (NewShield != Shield)
					Pawn->SetShield(NewShield);

				++Brain.Heals;

				if (Brain.Heals <= 6)
					LOG_INFO(LogBots, "BotHeal #{} healed: health {:.0f} -> {:.0f}, shield {:.0f} -> {:.0f}", Id, Health, NewHealth, Shield, NewShield);
			}
			else
			{
				Brain.NoHealUntil = Now + 30.0f;
				LOG_WARN(LogBots, "BotHeal #{} could not spend its healing item, no healing for a while", Id);
			}
		}

		if (Action.bAttackMelee)
			MeleeHit(Brain, Pawn, PlayerState, Id);

		if (Action.bAttackRanged && Brain.BuildQueue.empty()) // hands full while building
			FireShot(Brain, Pawn, PlayerState, Id);
	}

	// ---- entry points used by bots.h ----
	// What the game does to a real player as it boards the bus (Reboot's EnterAircraftHook): everything that can be dropped goes, so nothing
	// picked up on the spawn island (weapons, wood) is carried into the match. The pickaxe and the building tools stay.
	inline void ClearIslandItems(AFortInventory* Inventory, int Id)
	{
		std::vector<std::pair<FGuid, int>> ToRemove;
		auto& Items = Inventory->GetItemList().GetItemInstances();

		for (int i = 0; i < Items.Num(); ++i)
		{
			auto Item = Items.at(i);
			auto Entry = Item ? Item->GetItemEntry() : nullptr;
			auto Definition = Entry ? Cast<UFortWorldItemDefinition>(Entry->GetItemDefinition()) : nullptr;

			if (!Definition || !Definition->CanBeDropped())
				continue;

			ToRemove.push_back({ Entry->GetItemGuid(), Entry->GetCount() });
		}

		for (auto& Pair : ToRemove)
			Inventory->RemoveItem(Pair.first, nullptr, Pair.second, true);

		static int Logged = 0;

		if (++Logged <= 10)
			LOG_INFO(LogBots, "BotIsland #{} boarded the bus: {} item stacks left behind", Id, ToRemove.size());
	}

	inline float DistanceToNearestHuman(const FVector& Pos)
	{
		float Best = 1e30f;

		for (auto& Other : gPawns)
		{
			if (Other.bIsBot || !Other.Pawn)
				continue;

			Best = (std::min)(Best, Dist2D(Pos, Other.Pos));
		}

		return Best;
	}

	// A bot lost health and no bot is on record for it: if a real player fired a moment ago and is within range, that player did it. Recorded now, at the
	// moment of the hit, so that the kill is credited to them (and the kill feed names them) when the bot dies, the way a player's kill is.
	inline void AttributeDamageToHuman(AFortPlayerPawnAthena* Victim, float Now)
	{
		auto Recent = gLastDamageTime.find(Victim);

		if (Recent != gLastDamageTime.end() && Now - Recent->second < 0.7f)
			return; // a bot's own shot just did this

		const FVector At = Victim->GetActorLocation();
		const BotPawnInfo* Best = nullptr;
		float BestDistance = 12000.0f;

		for (auto& Other : gPawns)
		{
			if (!Other.Pawn || Other.bIsBot || Other.Pawn == Victim || Now - Other.LastFired > 1.6f)
				continue;

			const float Distance = Dist2D(Other.Pos, At);

			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = &Other;
			}
		}

		if (!Best)
			return;

		auto State = Cast<AFortPlayerStateAthena>((UObject*)Best->Pawn->GetPlayerState());

		if (!State)
			return;

		gLastDamager[Victim] = State;
		gLastDamageTime[Victim] = Now;

		if (auto Held = Best->Pawn->GetCurrentWeapon())
		{
			if (auto Def = (UObject*)Held->GetWeaponData())
				gLastWeaponPath[Victim] = Lower(Def->GetPathName());
		}
	}

	inline void Update(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, AController* Controller, AFortInventory* Inventory, int Id, EAthenaGamePhase Phase, float Now, bool bJumped, bool bMayDecide = true)
	{
		auto Movement = GetMovement(Pawn);

		if (!Movement)
			return;

		Brain.Controller = Controller;

		{
			const float Total = Pawn->GetHealth() + Pawn->GetShield();

			if (Brain.LastHealthTotal > 0.0f && Total < Brain.LastHealthTotal - 3.0f)
				AttributeDamageToHuman(Pawn, Now);

			Brain.LastHealthTotal = Total;
		}

		if (Brain.Curiosity < 0.0f)
		{
			Brain.Curiosity = RandRange(0.3f, 0.9f);
			Brain.Bravery = RandRange(0.75f, 1.35f);
			Brain.Alertness = RandRange(0.7f, 1.4f);
			Brain.SprintShare = RandRange(0.2f, 0.55f);
			Brain.WeavePhase = RandRange(0.0f, 6.28f);
			Brain.Aggression = (RandRange(0.05f, 0.65f) * RandRange(0.4f, 1.0f) + 0.05f) * 1.35f; // most bots are loot-first; a few are fighters
			Brain.Skill = RollSkill();

			// Real battle royale lobbies have a long tail: most players get 0-1 kills and a few get five or more (real solo data: 56% of players 0 kills, 3% five or more).
			// A few bots are those players: sharper, bolder, and keen to fight.
			if (gSkillFixed < 0.0f && RandRange(0.0f, 1.0f) < 0.07f)
			{
				Brain.Skill = RandRange(72.0f, 95.0f);
				Brain.Aggression = (std::max)(Brain.Aggression, RandRange(0.8f, 1.2f));
				Brain.Bravery = RandRange(1.25f, 1.6f);
				Brain.Alertness = RandRange(1.2f, 1.6f);
				Brain.bStrong = true;
			}

			static int LoggedSkills = 0;

			if (++LoggedSkills <= 120)
				LOG_INFO(LogBots, "BotSkill #{} skill {:.0f} (aim spread {:.1f} degrees, turns {:.0f} degrees/s)", Id, Brain.Skill, AimSigma(Brain.Skill), TurnRate(Brain.Skill));
		}

		// the bus has left: nothing from the spawn island goes with the bot
		if (Phase == EAthenaGamePhase::Aircraft && !Brain.bClearedIsland && Inventory)
		{
			Brain.bClearedIsland = true;
			ClearIslandItems(Inventory, Id);

			if (Brain.PickaxeDef)
				Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Brain.PickaxeDef, Brain.PickaxeGuid);
		}

		// phase driven transitions
		if (!bJumped)
		{
			if (Phase == EAthenaGamePhase::Setup || Phase == EAthenaGamePhase::Warmup || Phase == EAthenaGamePhase::None)
				SetState(Brain, EBotBrainState::Wander, Now, Id);
			else if (Phase == EAthenaGamePhase::Aircraft)
				SetState(Brain, EBotBrainState::Idle, Now, Id);
		}

		// decisions at ~6Hz (about 2Hz for bots nobody is near), but the movement is re-applied every tick so the CMC doesn't bleed the velocity off.
		// The server frame has a budget for decisions: a bot whose turn comes when it is used up simply waits for the next frame.
		const double TimeStart = PerfNow();
		double TimeAfterDecide = TimeStart;

		if (Brain.NextDecisionTime <= Now)
		{
			if (!bMayDecide)
			{
				++gDecisionsSkipped;
			}
			else
			{
				const float HumanDistance = DistanceToNearestHuman(Pawn->GetActorLocation());
				const bool bFar = HumanDistance < 1e29f && HumanDistance > 30000.0f && Brain.State != EBotBrainState::Engage; // with nobody to watch (a bots-only test) everyone decides at the normal rate
				Brain.NextDecisionTime = Now + (bFar ? 0.45f : 0.16f) + 0.001f * (Id % 40);
				++gDecisions;
				gDecisionsFar += bFar;

				const int SelfTeam = PlayerState ? (int)PlayerState->GetTeamIndex() : -1;
				const double TimeBeforeObserve = PerfNow();
				gUpdParts[0] += TimeBeforeObserve - TimeStart;
				const auto Obs = Observe(Brain, Pawn, Movement, Inventory, Phase, Now, SelfTeam);
				const double TimeAfterObserve = PerfNow();
				gUpdParts[1] += TimeAfterObserve - TimeBeforeObserve;
				gTimeObserve += TimeAfterObserve - TimeStart;
				Brain.LastAction = Decide(Brain, Obs, Id);
				TimeAfterDecide = PerfNow();
				gTimeDecide += TimeAfterDecide - TimeAfterObserve;

				if (Brain.State == EBotBrainState::Ground || Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat)
					ManageAmmo(Brain, Pawn, Inventory, Now);
			}
		}

		const double TimeBeforeApply = PerfNow();
		Apply(Brain, Pawn, PlayerState, Inventory, Movement, Brain.LastAction, Now, Id);
		gTimeApply += PerfNow() - TimeBeforeApply;

		// Falls: note how far the bot dropped, and shortly after the landing check what the engine did to it. If the engine
		// did not hurt it (these bots have no controller) apply the damage here, so a bad jump can kill a bot.
		{
			static auto ModeOffset = Movement->GetOffset("MovementMode");
			const uint8_t Mode = Movement->Get<uint8_t>(ModeOffset);
			const bool bOnTheIsland = Brain.State == EBotBrainState::Ground || Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Retreat
				|| Brain.State == EBotBrainState::Heal || Brain.State == EBotBrainState::Wander;
			const float Z = Pawn->GetActorLocation().Z;

			if (bOnTheIsland && Mode == 3)
			{
				if (!Brain.bFalling)
				{
					Brain.bFalling = true;
					Brain.FallPeakZ = Z;
					Brain.FallStartTotal = Pawn->GetHealth() + Pawn->GetShield();
				}
				else
				{
					Brain.FallPeakZ = (std::max)(Brain.FallPeakZ, Z);
				}
			}
			else if (Brain.bFalling && Mode == 1)
			{
				Brain.bFalling = false;

				if (Brain.FallPeakZ - Z > 800.0f)
				{
					Brain.FallHeight = Brain.FallPeakZ - Z;
					Brain.FallCheckAt = Now + 0.3f;
				}
			}
			else if (Brain.bFalling && !bOnTheIsland)
			{
				Brain.bFalling = false;
			}

			if (Brain.FallCheckAt > 0.0f && Now >= Brain.FallCheckAt)
			{
				Brain.FallCheckAt = 0.0f;
				const float Expected = FallDamageFor(Brain.FallHeight);
				const float Health = Pawn->GetHealth();
				const float Taken = Brain.FallStartTotal - (Health + Pawn->GetShield());

				if (Expected > 1.0f && Health > 0.0f && Taken < Expected * 0.6f)
				{
					const float Extra = Expected - (std::max)(Taken, 0.0f);
					Pawn->SetHealth((std::max)(0.0f, Health - Extra));
					gLastWeaponPath[Pawn] = "\x01" "fall";
					gFallTime[Pawn] = UGameplayStatics::GetTimeSeconds(GetWorld());

					if (++Brain.FallsLogged <= 8)
						LOG_INFO(LogBots, "BotFall #{} fell {:.0f} units: the engine did {:.0f}, applying {:.0f} more (health {:.0f} -> {:.0f})", Id, Brain.FallHeight, Taken, Extra, Health, (std::max)(0.0f, Health - Extra));
				}
				else if (Expected > 1.0f && ++Brain.FallsLogged <= 8)
				{
					LOG_INFO(LogBots, "BotFall #{} fell {:.0f} units and the engine already did {:.0f} (expected {:.0f})", Id, Brain.FallHeight, Taken, Expected);
				}
			}
		}

		if (Brain.State != EBotBrainState::Idle && Brain.State != EBotBrainState::Skydive && Brain.State != EBotBrainState::Glide)
			AutoOpenDoors(Brain, Pawn->GetActorLocation(), Now);

		// the held weapon must stay visible, and the storm hurts bots like everybody else
		if (Brain.State == EBotBrainState::Ground || Brain.State == EBotBrainState::Engage || Brain.State == EBotBrainState::Wander || Brain.State == EBotBrainState::Retreat)
			EnsureWeaponShown(Brain, Pawn, Now, Id);

		if (Brain.State != EBotBrainState::Idle && bJumped && gZoneKnown && Brain.NextStormTick <= Now)
			ApplyStormDamage(Brain, Pawn, Pawn->GetActorLocation(), Now, Id);

		// one-shot parts of the action must not repeat until the next decision
		Brain.LastAction.bJump = false;
		Brain.LastAction.bFace = false;
		Brain.LastAction.bSetPitch = false;
		Brain.LastAction.bAttackMelee = false;
		Brain.LastAction.bAttackRanged = false;
		Brain.LastAction.bSearchChest = false;
		Brain.LastAction.bPickup = false;
		Brain.LastAction.bEquip = false;
		Brain.LastAction.bUseHeal = false;
		Brain.LastAction.bEmote = false;
		Brain.LastAction.bHarvest = false;
		Brain.LastAction.bBuildWall = false;
		Brain.LastAction.bBoxUp = false;
		Brain.LastAction.bBoxRoof = false;
		Brain.LastAction.Pieces.clear();
	}

	// The bot just left the bus: head for the landing target chosen before the jump.
	inline void OnJump(BotBrainState& Brain, AFortPlayerPawnAthena* Pawn, const FVector& BusLocation, float Now, int Id)
	{
		if (!Brain.bLandingChosen)
			ChooseLanding(Brain, BusLocation, Id);

		Brain.State = EBotBrainState::Idle; // so SetState below logs the transition
		SetState(Brain, EBotBrainState::Skydive, Now, Id);
		Brain.Target = Brain.LandingTarget;
		Brain.bHasTarget = true;
		Brain.bHaveGroundZ = false;
		Brain.NextGroundTraceTime = 0.0f;
		Brain.LastAction = BotAction{};

		LOG_INFO(LogBots, "BotBrain #{} left the bus, target ({:.0f}, {:.0f}) distance {:.0f}", Id, Brain.Target.X, Brain.Target.Y, Dist2D(Brain.Target, BusLocation));
	}

	// A bot that never left the bus (e.g. the phase moved on) is dropped above its landing target.
	inline FVector PickLateDropLocation(BotBrainState& Brain, const FVector& BusLocation, int Id)
	{
		if (!Brain.bLandingChosen)
			ChooseLanding(Brain, BusLocation, Id);

		FVector Drop = Brain.LandingTarget;
		Drop.Z += 12000.0f;
		return Drop;
	}
}
