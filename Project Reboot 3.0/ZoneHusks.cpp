#include "ZoneHusks.h"
#include <unordered_set>
#include <regex>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include "reboot.h"
#include "GameplayStatics.h"
#include "Actor.h"
#include "FortPawn.h"
#include "Pawn.h"
#include "Rotator.h"
#include "Vector.h"
#include "BuildingActor.h"
#include "BuildingSMActor.h"
#include "globals.h"
#include "ZoneSearch.h" // Zone::RandRange
#include "GameplayTagContainer.h"
#include "log.h"
#include "RebootConfig.h"
#include "Stack.h"
#include <unordered_map>
#include <chrono>
#include <cmath>
#include "hooking.h" // GetFunctionIdxOrPtr
#include "ZoneFinder.h"

namespace
{
	// every husk that is out there; also drives the trickle cap
	std::vector<AFortPawn*> Husks;
	constexpr int HuskCap = 12;

	// husk melee (chunk 1): ApplyDamage is a no-op on this game's pawns, so the attack is a direct
	// health write on a short cadence. Floor at 1 HP for now - downed/revive is a later chunk.
	constexpr float HuskMeleeRange = 170.f;   // arrival threshold is 160, so this triggers right on arrival
	constexpr float HuskMeleeDamage = 15.f;
	constexpr float HuskAttackIntervalSec = 1.2f;

	// stuck husks bite the nearest building piece that is actually in their way
	constexpr float HuskBuildingAttackRange = 400.f;
	constexpr float HuskBuildingDamage = 150.f;
	constexpr float StuckSecondsBeforeAttack = 1.0f;

	// dead husks still render + bounce bots around; destroy the body a few seconds after IsDead
	constexpr float HuskCorpseLifetimeSec = 7.f;

	// trickle husks spawn on a ring this far from the player, never inside the player's capsule
	// (spawning on top of the player is what left husks hanging in MOVE_Falling)
	constexpr float HuskTrickleRadius = 700.f;

	// a falling husk whose height has not changed for this long is pushed into walking mode
	constexpr float HuskFallStillSec = 2.f;

	// share of trickle husks that are armored (0-100). 0 = off until the armored husk is tested in a Zone match
	constexpr int HuskArmoredPercent = 0;

	// a lethal husk hit downs the player (DBNO) instead of flooring health; unrevived, they bleed out after this
	constexpr float HuskBleedoutSec = 30.f;

	// downed players and how long they have been down; husks ignore them, and they die when the timer runs out
	std::unordered_map<AFortPawn*, float> DownedFor;

	struct FHuskState
	{
		std::chrono::steady_clock::time_point NextPlayerAttack{};
		std::chrono::steady_clock::time_point NextBuildingAttack{};

		FVector LastPos{ 0, 0, 0 };
		bool bHasLastPos = false;
		float StuckSeconds = 0.f;

		float MoveDirX = 0.f; // shamble direction, used to pick the blocking wall
		float MoveDirY = 0.f;

		ABuildingActor* TargetBuilding = nullptr;

		// falling husks that stop changing height (hung in mid-air) get walking forced after a while
		float FallStillSeconds = 0.f;
		float LastFallZ = 0.f;

		// one-shot diagnostics so a broken gate appears once in the log instead of every tick
		bool bLoggedMovementSkip = false;
		bool bLoggedNoPlayer = false;
		bool bNudging = false; // an AI-driven husk the tree left standing, now walked by us
	};

	std::unordered_map<AFortPawn*, FHuskState> States;

	struct FHuskCorpse
	{
		AFortPawn* Pawn;
		float TimeLeft;
	};
	std::vector<FHuskCorpse> Corpses;

	// the game's own husk blueprint; every variant (armored, beehive, dwarf ...) inherits from it
	UClass* GetHuskClass()
	{
		static UClass* Cached = nullptr;
		static bool bTried = false;

		if (bTried)
			return Cached;

		bTried = true;
		Cached = FindObject<UClass>(L"/Game/Characters/Enemies/Husk/Blueprints/HuskPawn.HuskPawn_C");

		if (!Cached)
			Cached = LoadObject<UClass>(L"/Game/Characters/Enemies/Husk/Blueprints/HuskPawn.HuskPawn_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));

		if (Cached)
		{
			LOG_INFO(LogDev, "Zone: husk pawn class found");
			return Cached;
		}

		LOG_ERROR(LogDev, "Zone: husk pawn class not found in the paks");
		return Cached;
	}

	// the alive players give the husks something to shamble at; nearest guess is good enough for the shamble
	UClass* GetPlayerPawnClass()
	{
		static auto Cached = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerPawn");
		return Cached;
	}

	// dead players are not targets: nearest-player search skips dead or not-yet-valid pawns
	bool bIsAlivePawn(AActor* Actor)
	{
		auto Pawn = Cast<AFortPawn>(Actor);

		if (!Pawn || !Pawn->IsValidLowLevel())
			return false;

		static auto IsDeadFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.IsDead");

		bool bDead = false;
		if (!IsDeadFn)
			return true; // can't check, treat as alive
		Pawn->ProcessEvent(IsDeadFn, &bDead);
		return !bDead;
	}

	// one scan of the living player pawns per tick, shared by every husk (it used to be a full actor
	// scan per husk per tick, 120 scans a second with a full husk count)
	void CollectAlivePlayers(std::vector<AFortPawn*>& Out)
	{
		Out.clear();

		auto PawnClass = GetPlayerPawnClass();

		if (!PawnClass)
			return;

		auto Players = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PawnClass);

		for (int i = 0; i < Players.Num(); ++i)
		{
			auto Pawn = Players.at(i);

			// downed players are not targets: husks go for the next standing player
			auto Fort = Cast<AFortPawn>(Pawn);

			if (Fort && bIsAlivePawn(Pawn) && !Fort->IsDBNO())
				Out.push_back(Fort);
		}

		Players.Free();
	}

	AActor* FindNearestPlayerPawn(AFortPawn* Husk, const std::vector<AFortPawn*>& Players)
	{
		AActor* Best = nullptr;
		float BestDistSq = 0;
		auto HuskPos = Husk->GetActorLocation();

		for (AFortPawn* Pawn : Players)
		{
			if (!Pawn || !Pawn->IsValidLowLevel())
				continue;

			auto PlayerPos = Pawn->GetActorLocation();
			float Dx = PlayerPos.X - HuskPos.X, Dy = PlayerPos.Y - HuskPos.Y;
			float DistSq = Dx * Dx + Dy * Dy;

			if (!Best || DistSq < BestDistSq)
			{
				Best = Pawn;
				BestDistSq = DistSq;
			}
		}

		return Best;
	}

	// a point HuskTrickleRadius away from the centre in a random direction, at the centre's height
	FVector RingAround(const FVector& Center, float Radius)
	{
		float Angle = (float)Zone::RandRange(0, 359) * 3.1415926535f / 180.f;
		return FVector{ Center.X + std::cos(Angle) * Radius, Center.Y + std::sin(Angle) * Radius, Center.Z };
	}

	// the mission's GameDifficulty (replicated float, 1.0 by default; dllmain sets it before the mission loads),
	// clamped so a bad value cannot make husks harmless or unkillable
	float ZoneDifficulty()
	{
		auto GameState = ((UObject*)GetWorld())->Get<UObject*>("GameState");

		if (!GameState)
			return 1.f;

		static int DifficultyOffset = GameState->GetOffset("GameDifficulty", false);

		if (DifficultyOffset == -1)
			return 1.f;

		float Difficulty = GameState->Get<float>(DifficultyOffset);

		if (Difficulty <= 0.f)
			return 1.f;

		return Difficulty < 0.5f ? 0.5f : (Difficulty > 2.f ? 2.f : Difficulty);
	}

	// Movement mode can be None (the spawn state, the engine never simulates it) or stuck in Falling. Walking
	// is set through the real CharacterMovementComponent.SetMovementMode so the component's own state
	// follows; the raw byte write is only a fallback when the function's parameters cannot be resolved.
	void ForceWalking(UObject* Movement)
	{
		static auto SetModeFn = FindObject<UFunction>(L"/Script/Engine.CharacterMovementComponent.SetMovementMode");
		static int ModeParamOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent.SetMovementMode", "NewMovementMode", false);
		static int CustomParamOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent.SetMovementMode", "NewCustomMode", false);
		static int MovementModeOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "MovementMode", false);

		if (SetModeFn && ModeParamOffset != -1 && CustomParamOffset != -1)
		{
			alignas(8) uint8_t Params[16] = {};
			Params[ModeParamOffset] = 1; // EMovementMode::MOVE_Walking
			Params[CustomParamOffset] = 0;
			Movement->ProcessEvent(SetModeFn, Params);
		}
		else if (MovementModeOffset != -1)
		{
			Movement->Get<uint8_t>(MovementModeOffset) = 1;
		}
	}

	// shield absorbs first, whatever is left flows into health with a 1 HP floor: no ForceKill yet,
	// downed/revive is its own chunk
	void DamagePlayerPawn(AFortPawn* Victim, float Damage)
	{
		float Shield = Victim->GetShield();

		if (Shield > 0.f)
		{
			float Left = Shield > Damage ? Shield - Damage : 0.f;
			Victim->SetShield(Left);
			// shield left is read back here so a write that does not stick shows up in the log
			LOG_INFO(LogDev, "Zone: husk hit a player's shield ({:.0f} dmg, shield {:.0f} -> {:.0f}, read back {:.0f})",
				Damage, Shield, Left, Victim->GetShield());
		}
		else
		{
			float Health = Victim->GetHealth();
			float New = Health - Damage;

			if (New > 0.f)
			{
				Victim->SetHealth(New);
				LOG_INFO(LogDev, "Zone: husk hit a player ({:.0f} dmg, health {:.0f})", Damage, New);
				return;
			}

			// lethal hit: the player goes down (DBNO) rather than dying outright; health stays at 1 so the
			// downed state is not mistaken for a kill by the engine's own health checks
			if (!Victim->IsDBNO())
			{
				Victim->SetHealth(1.f);
				Victim->SetDBNO(true);
				Victim->OnRep_IsDBNO();
				DownedFor[Victim] = 0.f;
				LOG_INFO(LogDev, "Zone: husk downed a player (bleed out in {:.0f}s unless revived)", HuskBleedoutSec);
			}
		}
	}

	// nearest building piece in front of the shamble direction: the dot filter keeps husks from
	// chewing on their own shadow's wall behind them
	ABuildingActor* FindNearestBlockingBuilding(AFortPawn* Husk, float DirX, float DirY)
	{
		static auto BuildingClass = ::FindObject<UClass>(L"/Script/FortniteGame.BuildingActor");

		if (!BuildingClass)
			return nullptr;

		auto Buildings = UGameplayStatics::GetAllActorsOfClass(GetWorld(), BuildingClass);
		ABuildingActor* Best = nullptr;
		float BestDistSq = HuskBuildingAttackRange * HuskBuildingAttackRange;
		auto HuskPos = Husk->GetActorLocation();

		for (int i = 0; i < Buildings.Num(); ++i)
		{
			auto Building = Cast<ABuildingActor>(Buildings.at(i));

			if (!Building)
				continue;

			auto Pos = Building->GetActorLocation();
			float Dx = Pos.X - HuskPos.X, Dy = Pos.Y - HuskPos.Y;
			float DistSq = Dx * Dx + Dy * Dy;

			if (DistSq > BestDistSq)
				continue;

			float Dist = std::sqrt(DistSq);
			float Dot = Dist > 1.f ? ((Dx * DirX + Dy * DirY) / Dist) : 0.f;

			if (Dot < 0.2f)
				continue;

			Best = Building;
			BestDistSq = DistSq;
		}

		Buildings.Free();
		return Best;
	}

	// ForceBuildingHealth runs the engine's health-changed code, which dereferences a null on some pieces (a crash seen at exe+0x6cbec6, reached from here
	// while the airstrike killed husks nearby). Call it behind a guard, and never bite a piece that faulted again.
	static bool SafeForceHealth(UObject* Building, UFunction* Function, void* Params)
	{
		__try { Building->ProcessEvent(Function, Params); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static std::unordered_set<ABuildingActor*> gBittenAndFaulted;

	void DamageBuilding(ABuildingActor* Building, float Damage)
	{
		if (gBittenAndFaulted.count(Building))
			return;

		float Health = Building->GetHealth();

		if (Health <= 0.f)
			return;

		float New = Health - Damage;

		if (New <= 0.f)
		{
			LOG_INFO(LogDev, "Zone: husk destroyed a building piece");
			Building->K2_DestroyActor();
		}
		else if (auto ForceBuildingHealthFn = ::FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.ForceBuildingHealth"))
		{
			if (!SafeForceHealth(Building, ForceBuildingHealthFn, &New))
			{
				gBittenAndFaulted.insert(Building);
				LOG_WARN(LogDev, "Zone: damaging building piece {} faulted in the engine, husks leave it alone from now on", Building->GetName());
			}
		}
	}
}

namespace Zone
{
	static UObject* gTutorialObjective = nullptr;

	// The husks of a mission are spawned by the game itself: the objective's script starts its AI encounters
	// (AFortMission::StartMissionAIEncounterFromGeneratedProfile) and the AI director spawns the pawns at the encounter's
	// spawn points, wave after wave. Our tick only adopts those pawns (movement and bite, see AdoptEncounterHusks) and
	// reports on the director so a stalled encounter shows in the log. The manual fallback spawn below runs only when the
	// game's own path produced nothing.
	static bool gEncounterWatch = false;
	static bool gEverHadHusk = false;
	static bool gFallbackUsed = false;
	static std::chrono::steady_clock::time_point gEncounterStart{};
	static std::chrono::steady_clock::time_point gNextAdopt{};
	static std::chrono::steady_clock::time_point gNextDirectorLog{};
	static int gDirectorLogsLeft = 0;
	static bool gNavDone = false; // NavigationTick below has finished (or given up) for this world
	static std::unordered_set<AFortPawn*> gAIDriven; // husks whose behaviour tree runs (their goal comes from UpdateGoal below)
	static std::unordered_map<AFortPawn*, std::pair<AActor*, std::chrono::steady_clock::time_point>> gGoals;
	// defender -> the husk it was given and when (UpdateDefenderGoals)
	static std::unordered_map<UObject*, std::pair<AActor*, std::chrono::steady_clock::time_point>> gDefenderGoals;
	static int gNavAttempts = 0;

	int HuskCount()
	{
		return (int)Husks.size();
	}

	// the husk lists hold raw pawn pointers; a new world frees the old pawns, so they must not survive into it
	void ResetForNewWorld()
	{
		LOG_INFO(LogDev, "Zone: clearing husk state for the new world (husks {}, corpses {})", Husks.size(), Corpses.size());
		Husks.clear();
		States.clear();
		Corpses.clear();
		DownedFor.clear();
		ResetSearchState(); // pending search pickups point into the old world too
		gTutorialObjective = nullptr;
		gEncounterWatch = false;
		gEverHadHusk = false;
		gFallbackUsed = false;
		gDirectorLogsLeft = 0;
		gNavDone = false;
		gNavAttempts = 0;
		gAIDriven.clear();
		gGoals.clear();
		gDefenderGoals.clear();
		gBittenAndFaulted.clear();
	}

	// the armored variant (HuskPawn_Armored, in the 7.30 paks) for a share of the trickle; falls back to the base husk
	// when it does not load. Off by default: its behaviour has not been tested in a Zone match yet.
	UClass* GetArmoredHuskClass()
	{
		static UClass* Cached = nullptr;
		static bool bTried = false;

		if (bTried)
			return Cached;

		bTried = true;
		Cached = FindObject<UClass>(L"/Game/Characters/Enemies/Husk/Blueprints/HuskPawn_Armored.HuskPawn_Armored_C");

		if (!Cached)
			Cached = LoadObject<UClass>(L"/Game/Characters/Enemies/Husk/Blueprints/HuskPawn_Armored.HuskPawn_Armored_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));

		// LOG_* expands to an if, so both branches are braced to keep the else attached to ours
		if (Cached)
		{
			LOG_INFO(LogDev, "Zone: armored husk class found");
		}
		else
		{
			LOG_WARN(LogDev, "Zone: armored husk class not found, base husks only");
		}

		return Cached;
	}

	// Server-side movement set-up of an AI pawn the engine does not simulate on its own (also what adopted encounter
	// husks get): physics without a controller, the movement component ticking, walking mode.
	void PrepareHusk(AFortPawn* Husk)
	{
		// Enable server-side physics for this controller-less AI pawn (same as BR bots):
		// bRunPhysicsWithNoController lets the movement component simulate on the server.
		// Also clear movement gates and set walking mode.
		{
			int CharacterMovementOffset = Husk->GetOffset("CharacterMovement");
			if (CharacterMovementOffset != -1)
			{
				auto CharacterMovement = Husk->Get<UObject*>(CharacterMovementOffset);
				if (CharacterMovement)
				{
					// Enable physics simulation without a controller
					int RunPhysicsOffset = CharacterMovement->GetOffset("bRunPhysicsWithNoController", false);
					void* RunPhysicsProp = CharacterMovement->GetProperty("bRunPhysicsWithNoController", false);
					if (RunPhysicsOffset != -1 && RunPhysicsProp)
						CharacterMovement->SetBitfieldValue(RunPhysicsOffset, GetFieldMask(RunPhysicsProp), true);

					// Make sure the component ticks and is active
					if (auto SetTickFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.SetComponentTickEnabled"))
					{
						struct { bool bEnabled; } Params{ true };
						CharacterMovement->ProcessEvent(SetTickFn, &Params);
					}
					if (auto ActivateFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.Activate"))
					{
						struct { bool bReset; } Params{ false };
						CharacterMovement->ProcessEvent(ActivateFn, &Params);
					}

					// Disable rotation to movement so AI can face independently
					for (const char* FlagName : { "bOrientRotationToMovement", "bUseControllerDesiredRotation" })
					{
						int FlagOffset = CharacterMovement->GetOffset(FlagName, false);
						void* FlagProp = CharacterMovement->GetProperty(FlagName, false);
						if (FlagOffset != -1 && FlagProp)
							CharacterMovement->SetBitfieldValue(FlagOffset, GetFieldMask(FlagProp), false);
					}

					// Set movement mode to Walking (1) instead of None (0)
					int MovementModeOffset = CharacterMovement->GetOffset("MovementMode", false);
					if (MovementModeOffset != -1)
						CharacterMovement->Get<uint8_t>(MovementModeOffset) = 1; // EMovementMode::MOVE_Walking
				}
			}

			// Pawn-level movement gates
			for (const char* FlagName : { "bSimGravityDisabled", "bDisableMovementAndTurnInPlace" })
			{
				int Offset = Husk->GetOffset(FlagName, false);
				void* Prop = Husk->GetProperty(FlagName, false);
				if (Offset != -1 && Prop)
					Husk->SetBitfieldValue(Offset, GetFieldMask(Prop), false);
			}
			{
				int Offset = Husk->GetOffset("bAllowMovement", false);
				void* Prop = Husk->GetProperty("bAllowMovement", false);
				if (Offset != -1 && Prop)
					Husk->SetBitfieldValue(Offset, GetFieldMask(Prop), true);
			}

			// Bump net update frequency so movement replicates smoothly
			if (Husk->GetNetUpdateFrequency() < 50.0f) Husk->GetNetUpdateFrequency() = 50.0f;
			if (Husk->GetMinNetUpdateFrequency() < 50.0f) Husk->GetMinNetUpdateFrequency() = 50.0f;
		}
	}

	void SpawnHusk(const FVector& SpawnPos, bool bArmored = false)
	{
		// Deferred spawn (CoreV2's CreateAIDirector pattern): allocate first, finish second. Bare SpawnActor
		// on HuskPawn_C crashes deterministically at engine +0xbd9660 (construction script / defaults init);
		// deferring lets us at least log whether the allocation itself survives.
		auto HuskClass = bArmored ? GetArmoredHuskClass() : nullptr;

		if (!HuskClass)
			HuskClass = GetHuskClass();

		if (!HuskClass)
			return;

		FTransform Transform;
		Transform.Translation = SpawnPos;
		Transform.Translation.Z += 80.f; // slightly above the floor so husks don't clip into terrain
		Transform.Rotation = FQuat(0, 0, 0, 1);
		Transform.Scale3D = FVector(1, 1, 1);

		auto NewActor = UGameplayStatics::BeginDeferredActorSpawnFromClass(GetWorld(), HuskClass, Transform);

		if (!NewActor)
		{
			LOG_ERROR(LogDev, "Zone: husk deferred spawn failed at ({:.0f},{:.0f},{:.0f})", SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
			return;
		}

		auto Husk = Cast<AFortPawn>(UGameplayStatics::FinishSpawningActor(NewActor, Transform));

		if (!Husk || !Husk->IsValidLowLevel())
		{
			LOG_ERROR(LogDev, "Zone: husk finish spawn failed at ({:.0f},{:.0f},{:.0f})", SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
			return;
		}

		PrepareHusk(Husk);

		States[Husk]; // fresh state (stuck timer, attack cadence) for this husk
		Husks.push_back(Husk);
		LOG_INFO(LogDev, "Zone: spawned husk #{} at ({:.0f},{:.0f},{:.0f})", HuskCount(), Transform.Translation.X, Transform.Translation.Y, Transform.Translation.Z);

		// Spawn AI controller for proper husk behavior (behavior tree, animations, combat AI)
		// Use the pawn's blueprint-specified AI controller class (like BR bots do via GetAIControllerClass)
		auto AIControllerClass = Husk->GetAIControllerClass();
		if (!AIControllerClass)
			AIControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIController");

		if (!AIControllerClass)
		{
			LOG_WARN(LogDev, "Zone: husk #{} has no AI controller class, running on the direct movement path only", HuskCount());
		}
		else
		{
			auto AIController = GetWorld()->SpawnActor<AActor>(AIControllerClass, Transform);
			if (!AIController)
				LOG_WARN(LogDev, "Zone: AI controller spawn returned null for husk #{}", HuskCount());
			if (AIController)
			{
				// Unpossess first (like EnableBotServerSimulation does for BR bots)
				static auto UnPossessFn = FindObject<UFunction>(L"/Script/Engine.Controller.UnPossess");
				if (UnPossessFn)
					AIController->ProcessEvent(UnPossessFn, nullptr);

				// Possess the husk pawn
				static auto PossessFn = FindObject<UFunction>(L"/Script/Engine.Controller.Possess");
				if (PossessFn)
				{
					struct { APawn* InPawn; } Params{ Husk };
					AIController->ProcessEvent(PossessFn, &Params);
				}

				// Run behavior tree from AI controller class defaults (if assigned in blueprint)
				static auto RunBehaviorTreeFn = FindObject<UFunction>(L"/Script/AIModule.AIController.RunBehaviorTree");
				static auto BehaviorTreeOffset = AIControllerClass->GetOffset("BehaviorTree");
				if (RunBehaviorTreeFn && BehaviorTreeOffset != -1)
				{
					UObject* BehaviorTree = AIControllerClass->Get<UObject*>(BehaviorTreeOffset);
					if (BehaviorTree)
					{
						struct { UObject* BehaviorTreeAsset; } Params{ BehaviorTree };
						AIController->ProcessEvent(RunBehaviorTreeFn, &Params);
						LOG_INFO(LogDev, "Zone: spawned AI controller with behavior tree for husk #{}", HuskCount());
					}
					else
					{
						LOG_INFO(LogDev, "Zone: spawned AI controller for husk #{} (no behavior tree assigned)", HuskCount());
					}
				}
			}
		}
	}

	// Fallback only: spawns the farmstead tutorial husks by hand at the mission's BP_Onboarding_SpawnPoints_C actors when the
	// game's own encounter produced no husk (see WatchEncounter). The real spawn path is not this.
	void SpawnFallbackTutorialHusks()
	{
		if (!Globals::bZoneMode)
			return;

		LOG_WARN(LogDev, "Zone: FALLBACK husk spawn for objective {} (the game's own encounter spawned nothing)", gTutorialObjective ? gTutorialObjective->GetFullName() : "NULL");

		static auto SpawnPointClass = FindObject<UClass>(L"/Game/Missions/Primary/OB_FarmsteadFort/BP_Onboarding_SpawnPoints.BP_Onboarding_SpawnPoints_C");
		if (!SpawnPointClass)
			SpawnPointClass = FindObject<UClass>(L"BP_Onboarding_SpawnPoints_C");

		if (SpawnPointClass)
		{
			auto SpawnPoints = UGameplayStatics::GetAllActorsOfClass(GetWorld(), SpawnPointClass);
			LOG_INFO(LogDev, "Zone: found {} BP_Onboarding_SpawnPoints_C actors", SpawnPoints.Num());

			if (SpawnPoints.Num() > 0)
			{
				for (int i = 0; i < SpawnPoints.Num(); i++)
				{
					if (auto Pt = SpawnPoints.at(i))
					{
						auto Loc = Pt->GetActorLocation();
						LOG_INFO(LogDev, "Zone: BP_Onboarding_SpawnPoints_C #{} at ({:.0f}, {:.0f}, {:.0f})", i, Loc.X, Loc.Y, Loc.Z);
						SpawnHusk(Loc);
					}
				}

				// top-up only when the first pass came up short; the first entry can be null, so check it
				if (auto First = SpawnPoints.at(0))
				{
					if (HuskCount() == 1)
					{
						auto Loc = First->GetActorLocation();
						SpawnHusk(Loc + FVector(120.f, 120.f, 0.f));
						SpawnHusk(Loc + FVector(-120.f, -120.f, 0.f));
					}
					else if (HuskCount() == 2)
					{
						auto Loc = First->GetActorLocation();
						SpawnHusk(Loc + FVector(100.f, 100.f, 0.f));
					}
				}

				SpawnPoints.Free();
				LOG_INFO(LogDev, "Zone: spawned {} tutorial husks at BP_Onboarding_SpawnPoints_C locations", HuskCount());
				return;
			}
			SpawnPoints.Free();
		}

		LOG_WARN(LogDev, "Zone: no BP_Onboarding_SpawnPoints_C found, falling back to player location");

		auto PawnClass = GetPlayerPawnClass();
		if (!PawnClass)
			return;

		auto Players = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PawnClass);
		if (Players.Num() == 0 || !Players.at(0))
		{
			Players.Free();
			return;
		}

		auto Player = Cast<AFortPawn>(Players.at(0));
		auto PlayerPos = Player->GetActorLocation();

		static auto K2GetActorRotationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_GetActorRotation");
		FRotator Rot{ 0, 0, 0 };
		if (K2GetActorRotationFn)
			Player->ProcessEvent(K2GetActorRotationFn, &Rot);

		float YawRad = Rot.Yaw * 3.1415926535f / 180.f;
		FVector Forward{ std::cos(YawRad), std::sin(YawRad), 0.f };
		FVector Right{ -std::sin(YawRad), std::cos(YawRad), 0.f };

		FVector Center = PlayerPos + Forward * 700.f;

		SpawnHusk(Center);
		SpawnHusk(Center + Right * 160.f);
		SpawnHusk(Center - Right * 160.f);

		Players.Free();
		LOG_INFO(LogDev, "Zone: spawned 3 tutorial husks for OBJ_HuskProblem (fallback)");
	}

	// all live husks gone: call the objective's own StopObjective vtable slot (this exact call is
	// what completes OBJ_HuskProblem on the live 20:12 run, status 2 = Succeeded)
	void CompleteTutorialObjective()
	{
		LOG_INFO(LogDev, "Zone: all tutorial husks defeated! Completing OBJ_HuskProblem");
		auto Obj = gTutorialObjective;
		gTutorialObjective = nullptr;

		// the objective can be torn down with its mission before the last husk dies; never call through a dead object
		if (!Obj || !Obj->IsValidLowLevel() || !Obj->VFTable)
		{
			LOG_WARN(LogDev, "Zone: tutorial objective is gone, nothing to complete");
			return;
		}

		// the mission script may have finished the objective on its own already
		static int StatusOffset = Obj->GetOffset("ObjectiveStatus", false);
		if (StatusOffset != -1 && *((uint8_t*)Obj + StatusOffset) != 1 /* EFortObjectiveStatus::InProgress */)
		{
			LOG_INFO(LogDev, "Zone: tutorial objective is no longer in progress, not completing it again");
			return;
		}
		// same vtable slot dllmain.cpp (Zone_Objective_StopObjective) swaps on the default object; the
		// 20:12 live run showed this call reaching that hook with status 2. Keep the two in step.
		constexpr int StopObjectiveVSlot = ZoneFinder::Fixed730::ObjectiveStopSlot / 8;
		auto StopObjective = (void (*)(UObject*, uint8_t, void*, void*))(Obj->VFTable[StopObjectiveVSlot]);
		StopObjective(Obj, 2 /* EFortObjectiveStatus::Succeeded */, nullptr, nullptr);
	}

	// ---- mission encounter set-up ----
	// AFortMission::StartMissionAIEncounterFromGeneratedProfile (what the mission script calls to start every wave) reads the
	// mission's GeneratedMissionOptions.GeneratedEncounterProfiles[index]. On the game's servers the mission generator fills
	// that list from the FortMissionInfo (its EncounterProfiles) when it builds the mission; LoadMission on this server never
	// ran the generator, so the list was empty and every call returned null without creating an encounter (director state:
	// active 0, inactive 0). The call itself regenerates a stale entry from the mission's info (UpdateGeneratedEncounterProfile)
	// when the entry's difficulty differs from the game's, but only if the mission knows its info (a pointer the loader also
	// leaves unset) - so: give the mission its info, and give the list one blank entry per info profile for it to regenerate.
	// (Another Save the World server, Limbo, found the same empty list on outposts and fills it the same way.)
	static UObject* gMissionInfo = nullptr;

	void SetMissionInfo(UObject* Info)
	{
		gMissionInfo = Info;
	}

	void PrepareMissionEncounters(UObject* Mission)
	{
		if (!Mission || !gMissionInfo)
		{
			LOG_WARN(LogDev, "Zone: encounter set-up skipped (mission {}, info {})", Mission != nullptr, gMissionInfo != nullptr);
			return;
		}

		const bool b730 = Engine_Version >= 422 && Engine_Version <= 424;

		// the mission's FortMissionInfo (not a reflected property; seen in 7.30.1's regenerate-profile code, see ZoneFinder.h)
		constexpr int InfoPointerOffset = ZoneFinder::Fixed730::MissionInfoPointer;
		UObject*& InfoPointer = *(UObject**)((uint8_t*)Mission + InfoPointerOffset);

		int OptionsOffset = Mission->GetOffset("GeneratedMissionOptions", false);
		int ProfilesOffset = FindOffsetStruct("/Script/FortniteGame.FortGeneratedDifficultyOptions", "GeneratedEncounterProfiles", false);
		int InfoProfilesOffset = gMissionInfo->GetOffset("EncounterProfiles", false);
		static auto ProfileStruct = FindObject<UStruct>(L"/Script/FortniteGame.FortGeneratedEncounterProfile");
		int IndexOffset = FindOffsetStruct("/Script/FortniteGame.FortGeneratedEncounterProfile", "GeneratedEncounterIndex", false);

		if (OptionsOffset == -1 || ProfilesOffset == -1 || InfoProfilesOffset == -1 || !ProfileStruct)
		{
			LOG_WARN(LogDev, "Zone: encounter set-up skipped, offsets missing (options {}, profiles {}, info profiles {}, struct {})", OptionsOffset, ProfilesOffset, InfoProfilesOffset, ProfileStruct != nullptr);
			return;
		}

		auto& Profiles = *(TArray<uint8_t>*)((uint8_t*)Mission + OptionsOffset + ProfilesOffset);
		int InfoCount = ((TArray<uint8_t>*)((uint8_t*)gMissionInfo + InfoProfilesOffset))->Num();
		int ElementSize = ProfileStruct->GetPropertiesSize();

		LOG_INFO(LogDev, "Zone: encounter set-up: mission info pointer {} (0x378), GeneratedMissionOptions at +0x{:x} (7.30.1 code reads the profile list at +0x660), profiles {} / info profiles {}, profile size {}",
			InfoPointer ? InfoPointer->GetName() : std::string("NULL"), OptionsOffset + ProfilesOffset, Profiles.Num(), InfoCount, ElementSize);

		if (b730 && !InfoPointer)
		{
			InfoPointer = gMissionInfo;
			LOG_INFO(LogDev, "Zone: gave the mission its info {}", gMissionInfo->GetName());
		}

		while (Profiles.Num() < InfoCount)
		{
			int Index = Profiles.AddUninitialized2(ElementSize);
			auto Element = (uint8_t*)Profiles.GetData() + (size_t)Index * ElementSize;
			memset(Element, 0, ElementSize);

			if (IndexOffset != -1)
				*(int*)(Element + IndexOffset) = Index;
		}

		LOG_INFO(LogDev, "Zone: mission now has {} generated encounter profile entries", Profiles.Num());
	}

	// ---- husks spawned by the game's own mission encounters ----

	static bool IsEnemyAIPawn(AFortPawn* Pawn)
	{
		// the enemy blueprints live under /Game/Characters/Enemies (husks and all their variants); defenders and
		// survivors (allies) are AI pawns too and must not be walked at the player
		return Pawn && Pawn->ClassPrivate && Pawn->ClassPrivate->GetPathName().contains("/Enemies/");
	}

	static bool InsideExe(void* Address); // defined with the navigation code below

	// ---- director defaults: export from the Blueprint once, import onto the native director
	namespace
	{
		// the director Blueprint's own default values that the encounter path reads (names from its class defaults; BP-only variables are skipped on import
		// because the native class has no such property)
		const char* DirectorDefaultNames[] = {
			"BaseEncounterClass", "DummyDebugEncounter", "MaxActiveAlive", "MaxAliveCurve", "MinAliveCurve", "SpawnLocationPlacementQueries", "EncounterSpawnPointsSettings",
			"EncounterSpawnGroupCapSettings", "EncounterPawnDifficultyLevelModifiers", "SpawnGroupUpgrades", "PIDControllerSettings", "IntensityInfo", "UtilityContributionData",
			"PlayerPerformanceEstimateSettings", "DiscreteEncounterUtilityDesireMappings", "InitialDynamicUtilities", "DataTrackingSettings", "BaseLootDropData", "LootDropModifiers",
			"LootDropDenialQuery", "DefaultEncounterPawnCaps", "EncounterPawnSpawnInterval", "BurstSpawnThreatVisualsEndDelay", "NumWorldSubdivides", "MinAISpawnDistanceFromPlayers",
			"MaxNormalLODDistanceToPlayer", "AIRelevantDistanceToPlayer", "EncounterRelevantDistanceToPlayer", "AmbientThreatPreferredPlacementActorTags",
			"AmbientThreatRequiredPlacementActorTags", "PatrolWardBaseBuildingSafeDistance", "PatrolWardActiveEncounterSafeDistance"
		};

		// UProperty virtuals on 7.30 (read from the Xbox build; verified to land in the exe before use): ExportTextItem and ImportText_Internal
		constexpr int ExportSlot = ZoneFinder::Fixed730::PropertyExportTextItemSlot / 8, ImportSlot = ZoneFinder::Fixed730::PropertyImportTextSlot / 8;
		const char* DefaultsFile = "zone_director_defaults.txt";

		// a no-op FOutputDevice so the import can report errors without a console (vtable: destructor, Serialize, ...)
		static void NoOpDevice() {}
		static void* NoOpVTable[16] = { (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice,
			(void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice, (void*)&NoOpDevice };
		struct FakeOutputDevice { void** VTable = NoOpVTable; bool bSuppressEventTag = false; bool bAutoEmitLineTerminator = false; };

		bool SafeExportText(void* Function, void* Property, FString* Out, void* Value, UObject* Parent)
		{
			__try { ((void(*)(void*, FString&, const void*, const void*, UObject*, int, UObject*))Function)(Property, *Out, Value, nullptr, Parent, 0, nullptr); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SafeImportText(void* Function, void* Property, const wchar_t* Text, void* Value, UObject* Owner, FakeOutputDevice* Device, const wchar_t** Result)
		{
			__try { *Result = ((const wchar_t*(*)(void*, const wchar_t*, void*, int, UObject*, void*))Function)(Property, Text, Value, 0, Owner, Device); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool SlotIsOurs(void* Property, int Slot)
		{
			if (!Property) return false;
			auto VTable = *(void***)Property;
			return VTable && InsideExe(VTable[Slot]);
		}
	}

	void SyncDirectorDefaults(UObject* Director)
	{
		if (!Director || Fortnite_Version < 7.3 || Fortnite_Version >= 7.31)
			return;

		const bool bBlueprint = Director->ClassPrivate && Director->ClassPrivate->GetName().starts_with("AIDirector_Fortnite");
		const uint64_t Base = (uint64_t)GetModuleHandleW(0);

		if (bBlueprint)
		{
			if (std::ifstream(DefaultsFile).good())
				return; // already exported

			std::ofstream File(DefaultsFile);
			int Written = 0;

			for (auto Name : DirectorDefaultNames)
			{
				void* Property = Director->GetProperty(Name, false);
				int Offset = Director->GetOffset(Name, false);

				if (!Property || Offset == -1 || !SlotIsOurs(Property, ExportSlot))
				{
					LOG_WARN(LogDev, "Zone: director defaults: cannot export {} (property {}, offset {}, ExportTextItem slot {})", Name, Property != nullptr, Offset,
						Property && (*(void***)Property) ? (uint64_t)(*(void***)Property)[ExportSlot] - Base : 0);
					continue;
				}

				const int ArrayDim = *(int*)((uint8_t*)Property + ZoneFinder::Fixed730::FieldArrayDim);
				const int ElementSize = *(int*)((uint8_t*)Property + ZoneFinder::Fixed730::FieldElementSize);

				if (ArrayDim < 1 || ArrayDim > 64 || ElementSize < 1 || ElementSize > 0x10000)
				{
					LOG_WARN(LogDev, "Zone: director defaults: {} has implausible dim {} / size {}", Name, ArrayDim, ElementSize);
					continue;
				}

				for (int Index = 0; Index < ArrayDim; ++Index)
				{
					FString Text;
					if (!SafeExportText((*(void***)Property)[ExportSlot], Property, &Text, (uint8_t*)Director + Offset + Index * ElementSize, Director) || !Text.Data.Data)
					{
						LOG_WARN(LogDev, "Zone: director defaults: exporting {}[{}] faulted or gave nothing", Name, Index);
						continue;
					}

					std::wstring Value(Text.Data.Data);
					File << Name << (ArrayDim > 1 ? "[" + std::to_string(Index) + "]" : std::string()) << "=" << std::string(Value.begin(), Value.end()) << "\n";
					++Written;
				}
			}

			LOG_INFO(LogDev, "Zone: director defaults: wrote {} values to {}", Written, DefaultsFile);
			return;
		}

		// native director: import them
		std::ifstream File(DefaultsFile);
		if (!File.good())
		{
			LOG_WARN(LogDev, "Zone: director defaults: no {} next to the log; the native director keeps its empty settings (export once with the Xbox director pak installed)", DefaultsFile);
			return;
		}

		std::vector<std::string> Lines;
		for (std::string Line; std::getline(File, Line);)
			if (!Line.empty()) Lines.push_back(Line);

		// referenced assets must be loaded before the text can name them
		static auto ObjectClass = FindObject<UClass>(L"/Script/CoreUObject.Object");
		static const std::regex PathRegex("'(/[A-Za-z0-9_/.\\-]+)'");
		std::unordered_set<std::string> Preloaded;

		for (auto& Line : Lines)
		{
			for (std::sregex_iterator It(Line.begin(), Line.end(), PathRegex), End; It != End; ++It)
			{
				std::string Path = (*It)[1];
				if (!Preloaded.insert(Path).second) continue;
				std::wstring Wide(Path.begin(), Path.end());
				LoadObject<UObject>(Wide.c_str(), ObjectClass);
			}
		}

		FakeOutputDevice Device;
		int Imported = 0, Failed = 0;

		for (auto& Line : Lines)
		{
			auto Equals = Line.find('=');
			if (Equals == std::string::npos) continue;

			std::string Name = Line.substr(0, Equals);
			int Index = 0;
			if (auto Bracket = Name.find('['); Bracket != std::string::npos) { Index = atoi(Name.c_str() + Bracket + 1); Name = Name.substr(0, Bracket); }

			void* Property = Director->GetProperty(Name, false);
			int Offset = Director->GetOffset(Name, false);

			if (!Property || Offset == -1)
				continue; // a Blueprint-only variable

			if (!SlotIsOurs(Property, ImportSlot))
			{
				LOG_WARN(LogDev, "Zone: director defaults: ImportText slot of {} is not in the exe, giving up", Name);
				return;
			}

			const int ElementSize = *(int*)((uint8_t*)Property + ZoneFinder::Fixed730::FieldElementSize);
			std::string Value = Line.substr(Equals + 1);
			std::wstring Wide(Value.begin(), Value.end());
			const wchar_t* Result = nullptr;
			const bool bOk = SafeImportText((*(void***)Property)[ImportSlot], Property, Wide.c_str(), (uint8_t*)Director + Offset + Index * ElementSize, Director, &Device, &Result);

			if (bOk && Result) ++Imported;
			else { ++Failed; LOG_WARN(LogDev, "Zone: director defaults: importing {} {}", Name, bOk ? "was rejected" : "FAULTED"); }
		}

		LOG_INFO(LogDev, "Zone: director defaults: imported {} values onto the native director ({} failed)", Imported, Failed);

		// the director culls AI that is farther than these from every player. The tutorial's cave husks start 6000+ units from a player still doing the intro and were
		// dying within seconds of spawning, so a Zone (one small map) must never cull by distance
		for (auto Name : { "AIRelevantDistanceToPlayer", "MaxNormalLODDistanceToPlayer", "EncounterRelevantDistanceToPlayer" })
		{
			int Offset = Director->GetOffset(Name, false);
			if (Offset != -1) Director->Get<float>(Offset) = 1000000.f;
		}
	}

	// ---- husks run by the game's own AI (behaviour tree + goal) instead of our velocity writes
	static void* gBasePossess = nullptr;
	static int gTreeLogs = 6;

	// AAIController::RunBehaviorTree with the tree the pawn names; true when the controller accepted it
	static bool RunTreeFor(UObject* Controller, UObject* Pawn)
	{
		static auto RunFn = FindObject<UFunction>(L"/Script/AIModule.AIController.RunBehaviorTree");
		static const char* RunPath = "/Script/AIModule.AIController.RunBehaviorTree";
		int TreeOffset = Pawn ? Pawn->GetOffset("BehaviorTree", false) : -1;
		auto Tree = TreeOffset != -1 ? Pawn->Get<UObject*>(TreeOffset) : nullptr;

		if (!Controller || !RunFn || !Tree)
		{
			if (gTreeLogs > 0) { --gTreeLogs; LOG_WARN(LogDev, "Zone: cannot run a behaviour tree: controller {}, RunBehaviorTree {}, pawn tree {}", Controller != nullptr, RunFn != nullptr, Tree ? Tree->GetName() : std::string("NONE")); }
			return false;
		}

		alignas(8) uint8_t Buffer[32] = {};
		*(UObject**)(Buffer + FindOffsetStruct(RunPath, "BTAsset")) = Tree;
		Controller->ProcessEvent(RunFn, Buffer);
		const bool bOk = *(bool*)(Buffer + FindOffsetStruct(RunPath, "ReturnValue"));

		if (gTreeLogs > 0) { --gTreeLogs; LOG_INFO(LogDev, "Zone: RunBehaviorTree({}) on {} for {} -> {}", Tree->GetName(), Controller->GetName(), Pawn->GetName(), bOk); }
		if (bOk)
			return true;

		// refused: RunBehaviorTree fails when the controller already owns a blackboard for a different blackboard asset. Say what it has.
		auto Brain = Controller->Get<UObject*>("BrainComponent");
		auto Board = Controller->Get<UObject*>("Blackboard");
		auto TreeBoard = Tree->Get<UObject*>("BlackboardAsset");
		auto BoardAsset = Board ? Board->Get<UObject*>("BlackboardAsset") : nullptr;

		bool bBrainRunning = false;
		static auto IsRunningFn = FindObject<UFunction>(L"/Script/AIModule.BrainComponent.IsRunning");
		if (Brain && IsRunningFn) { struct { bool ReturnValue; } R{}; Brain->ProcessEvent(IsRunningFn, &R); bBrainRunning = R.ReturnValue; }

		static int DiagLogs = 4;
		if (DiagLogs > 0)
		{
			--DiagLogs;
			LOG_INFO(LogDev, "Zone: tree refused: brain {} (running {}), controller blackboard {} (asset {}), tree wants blackboard {}", Brain ? Brain->GetName() : std::string("NONE"), bBrainRunning,
				Board ? Board->GetName() : std::string("NONE"), BoardAsset ? BoardAsset->GetName() : std::string("none"), TreeBoard ? TreeBoard->GetName() : std::string("none"));
		}

		// a tree is already running on this controller: the game started it itself, we only need to feed the goal
		if (bBrainRunning)
			return true;

		// a blackboard for another asset is in the way: remove it and let RunBehaviorTree build the one this tree uses
		if (Board && BoardAsset != TreeBoard)
		{
			static auto DestroyFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.K2_DestroyComponent");
			static int BoardOffset = Controller->GetOffset("Blackboard", false);

			if (DestroyFn && BoardOffset != -1)
			{
				struct { UObject* Object; } D{ Board };
				Board->ProcessEvent(DestroyFn, &D);
				Controller->Get<UObject*>(BoardOffset) = nullptr;

				alignas(8) uint8_t Retry[32] = {};
				*(UObject**)(Retry + FindOffsetStruct(RunPath, "BTAsset")) = Tree;
				Controller->ProcessEvent(RunFn, Retry);
				const bool bRetry = *(bool*)(Retry + FindOffsetStruct(RunPath, "ReturnValue"));
				static int RetryLogs = 4;
				if (RetryLogs > 0) { --RetryLogs; LOG_INFO(LogDev, "Zone: retried RunBehaviorTree after removing the old blackboard -> {}", bRetry); }
				return bRetry;
			}
		}

		return false;
	}

	static bool IsEnemyClassPawn(UObject* Pawn)
	{
		return Pawn && Pawn->ClassPrivate && Pawn->ClassPrivate->GetPathName().contains("/Enemies/");
	}

	// replacement for AFortAIController's Possess slot (see ZoneHusks.h)
	static void PossessHook(UObject* Controller, UObject* Pawn)
	{
		if (gBasePossess)
			((void(*)(UObject*, UObject*))gBasePossess)(Controller, Pawn);

		if (Globals::bZoneMode && Pawn && IsEnemyClassPawn(Pawn) && RunTreeFor(Controller, Pawn))
			gAIDriven.insert((AFortPawn*)Pawn);
	}

	void InstallAIControllerPossess()
	{
		if (Fortnite_Version < 7.3 || Fortnite_Version >= 7.31)
			return;

		static bool bDone = false;
		if (bDone) return;
		bDone = true;

		auto FortDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortAIController");
		auto BaseDefault = FindObject<UObject>(L"/Script/AIModule.Default__AIController");

		if (!FortDefault || !BaseDefault || !FortDefault->VFTable || !BaseDefault->VFTable)
		{
			LOG_WARN(LogDev, "Zone: possess: controller defaults not found (Fort {}, AI {})", FortDefault != nullptr, BaseDefault != nullptr);
			return;
		}

		// Possess's slot, read from the Controller.Possess UFunction's native thunk (call [rax+slot]) the Reboot way
		static auto PossessFn = FindObject<UFunction>(L"/Script/Engine.Controller.Possess");
		const int Slot = PossessFn ? (int)(GetFunctionIdxOrPtr(PossessFn) / 8) : 0;
		if (Slot <= 0 || Slot > 0x400) // GetFunctionIdxOrPtr answers an address when the thunk has no virtual call
		{
			LOG_WARN(LogDev, "Zone: possess: the Possess vtable slot was not found");
			return;
		}

		auto FortFn = (uint8_t*)FortDefault->VFTable[Slot];
		auto BaseFn = (uint8_t*)BaseDefault->VFTable[Slot];
		const uint64_t Base = (uint64_t)GetModuleHandleW(0);

		LOG_INFO(LogDev, "Zone: possess: FortAIController slot 0x{:x} -> exe+0x{:x} [{:02x} {:02x} {:02x} {:02x}], AIController slot -> exe+0x{:x}", Slot * 8, (uint64_t)FortFn - Base, FortFn[0], FortFn[1], FortFn[2], FortFn[3], (uint64_t)BaseFn - Base);

		if (FortFn == BaseFn)
		{
			LOG_INFO(LogDev, "Zone: possess: already AAIController::Possess, nothing to replace");
			return;
		}

		// a stub: ret, ret imm16, or xor eax,eax / ret
		const bool bStub = FortFn[0] == 0xC3 || FortFn[0] == 0xC2 || (FortFn[0] == 0x33 && FortFn[1] == 0xC0 && FortFn[2] == 0xC3) || (FortFn[0] == 0x32 && FortFn[1] == 0xC0 && FortFn[2] == 0xC3);

		if (!bStub)
		{
			LOG_WARN(LogDev, "Zone: possess: the Fort slot is real code, not replacing it");
			return;
		}

		gBasePossess = BaseFn;
		VirtualSwap(FortDefault->VFTable, Slot, (void*)&PossessHook);
		LOG_INFO(LogDev, "Zone: possess: FortAIController::Possess was a stub, now AAIController::Possess + RunBehaviorTree");
	}

	// Gives an AI-driven husk its goal: the nearest living player. SetGoalActor(goal, bLocationAlwaysKnown) makes the tree skip hunting and go straight at the target
	// (BT_HuskBase sleeps until the blackboard has a GoalActor, and nothing else on this server fills it: the goal manager's own update never lists these pawns).
	static void UpdateGoal(AFortPawn* Husk, const std::vector<AFortPawn*>& Players, std::chrono::steady_clock::time_point Now)
	{
		static auto SetGoalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortAIController.SetGoalActor");
		static const char* GoalPath = "/Script/FortniteGame.FortAIController.SetGoalActor";
		auto Controller = Husk->GetController();

		if (!Controller || !SetGoalFn)
			return;

		auto Nearest = FindNearestPlayerPawn(Husk, Players);
		auto& Last = gGoals[Husk];

		// keep the goal; renew when it changed or every 3 s (a lost goal is picked up again)
		if (Last.first == (AActor*)Nearest && Now - Last.second < std::chrono::seconds(3))
			return;

		Last = { (AActor*)Nearest, Now };

		if (!Nearest)
			return;

		alignas(8) uint8_t Buffer[16] = {};
		*(UObject**)(Buffer + FindOffsetStruct(GoalPath, "InActor")) = Nearest;
		*(bool*)(Buffer + FindOffsetStruct(GoalPath, "bLocationAlwaysKnown")) = true;
		Controller->ProcessEvent(SetGoalFn, Buffer);
	}

	static bool ProbeCall(UObject* Object, UFunction* Function, void* Params)
	{
		__try { Object->ProcessEvent(Function, Params); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Defenders (Defender_*_C, Survivor_C children) stand on their pads holding a gun and never shoot: BT_SurvivorDefender waits for a GoalActor like the husk tree,
	// and the goal manager that would pick one (PerceivedEnemiesAndDefaultWorld_Defender) never lists our pawns. Give each the nearest living husk within reach,
	// preferring one it can see.
	static constexpr float DefenderRange = 4000.f;

	static void UpdateDefenderGoals(std::chrono::steady_clock::time_point Now)
	{
		static auto LastRun = Now;
		if (Now - LastRun < std::chrono::milliseconds(500))
			return;
		LastRun = Now;

		static auto SurvivorClass = FindObject<UClass>(L"/Game/Characters/Survivors/Survivor.Survivor_C");
		if (!SurvivorClass) SurvivorClass = FindObject<UClass>(L"/Game/Characters/Survivors/Survivor.Survivor_C");
		static auto SetGoalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortAIController.SetGoalActor");
		static auto IsDeadFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.IsDead");
		static auto LineOfSightFn = FindObject<UFunction>(L"/Script/Engine.Controller.LineOfSightTo");
		static const char* GoalPath = "/Script/FortniteGame.FortAIController.SetGoalActor";
		static const char* SightPath = "/Script/Engine.Controller.LineOfSightTo";

		if (!SurvivorClass || !SetGoalFn || !GetWorld() || Husks.empty())
			return;

		std::vector<AFortPawn*> Living;
		for (auto Husk : Husks)
		{
			if (!Husk || !Husk->IsValidLowLevel()) continue;
			bool bDead = false;
			if (IsDeadFn) Husk->ProcessEvent(IsDeadFn, &bDead);
			if (!bDead) Living.push_back(Husk);
		}

		auto Survivors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), SurvivorClass);
		static std::unordered_set<UObject*> Logged;

		for (int i = 0; i < Survivors.Num(); ++i)
		{
			auto Defender = (AFortPawn*)Survivors.at(i);
			if (!Defender || !Defender->ClassPrivate || Defender->ClassPrivate->GetName().find("Defender") == std::string::npos)
				continue;

			auto Controller = Defender->GetController();
			if (!Controller)
				continue;

			if (!Logged.count(Defender))
			{
				Logged.insert(Defender);
				int TreeOffset = Defender->GetOffset("BehaviorTree", false);
				auto Tree = TreeOffset != -1 ? Defender->Get<UObject*>(TreeOffset) : nullptr;
				LOG_INFO(LogDev, "Zone: defender {} runs tree {}; we pick its husk goal within {:.0f}uu", Defender->GetName(), Tree ? Tree->GetName() : std::string("NONE"), DefenderRange);
			}

			const FVector Pos = Defender->GetActorLocation();
			AActor* Best = nullptr; float BestScore = 1e30f;

			for (auto Husk : Living)
			{
				auto H = Husk->GetActorLocation();
				float Dist = std::sqrt((H.X - Pos.X) * (H.X - Pos.X) + (H.Y - Pos.Y) * (H.Y - Pos.Y) + (H.Z - Pos.Z) * (H.Z - Pos.Z));
				if (Dist > DefenderRange) continue;

				// a husk behind a wall counts as twice as far, so a visible one wins unless it is much farther
				bool bSeen = true;
				if (LineOfSightFn)
				{
					alignas(8) uint8_t B[64] = {};
					*(UObject**)(B + FindOffsetStruct(SightPath, "Other")) = Husk;
					*(FVector*)(B + FindOffsetStruct(SightPath, "ViewPoint")) = FVector{ 0.f, 0.f, 0.f };
					if (ProbeCall(Controller, LineOfSightFn, B))
						bSeen = *(bool*)(B + FindOffsetStruct(SightPath, "ReturnValue"));
				}
				float Score = bSeen ? Dist : Dist * 2.f;
				if (Score < BestScore) { BestScore = Score; Best = (AActor*)Husk; }
			}

			auto& Last = gDefenderGoals[Defender];
			if (!Best || (Last.first == Best && Now - Last.second < std::chrono::seconds(3)))
				continue;

			if (Last.first != Best)
			{
				static int GoalLogs = 12;
				if (GoalLogs > 0) { --GoalLogs; LOG_INFO(LogDev, "Zone: defender {} now targets {} ({:.0f}uu score)", Defender->GetName(), Best->GetName(), BestScore); }
			}

			Last = { Best, Now };
			alignas(8) uint8_t Buffer[16] = {};
			*(UObject**)(Buffer + FindOffsetStruct(GoalPath, "InActor")) = Best;
			*(bool*)(Buffer + FindOffsetStruct(GoalPath, "bLocationAlwaysKnown")) = true;
			ProbeCall(Controller, SetGoalFn, Buffer);
		}

		Survivors.Free();
	}

	// Why does a husk stand still? Asks the navigation system the same questions the tree's MoveTo does: is there navmesh under the husk and under the player, does a
	// path between them exist, and what is the controller's move status. Logged a few times per run.
	static void ProbeHuskNavigation(AFortPawn* Husk, AActor* Target)
	{
		static int ProbesLeft = 8;
		if (ProbesLeft <= 0 || !Husk || !Target) return;
		--ProbesLeft;

		auto World = (UObject*)GetWorld();
		static int NavOffset = World->GetOffset("NavigationSystem", false);
		auto Nav = NavOffset != -1 ? World->Get<UObject*>(NavOffset) : nullptr;
		if (!Nav) { LOG_WARN(LogDev, "Zone: nav probe: no navigation system"); return; }

		const FVector From = Husk->GetActorLocation(), To = Target->GetActorLocation();

		auto Project = [&](const FVector& Point, const char* Label)
		{
			static const char* Path = "/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation";
			static auto Fn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
			if (!Fn) return;
			alignas(8) uint8_t B[128] = {};
			*(UObject**)(B + FindOffsetStruct(Path, "WorldContextObject")) = World;
			*(FVector*)(B + FindOffsetStruct(Path, "Point")) = Point;
			*(FVector*)(B + FindOffsetStruct(Path, "QueryExtent")) = FVector{ 300.f, 300.f, 500.f };
			const bool bRan = ProbeCall(Nav, Fn, B);
			const bool bHit = bRan && *(bool*)(B + FindOffsetStruct(Path, "ReturnValue"));
			auto Out = *(FVector*)(B + FindOffsetStruct(Path, "ProjectedLocation"));
			LOG_INFO(LogDev, "Zone: nav probe: {} at ({:.0f},{:.0f},{:.0f}) -> {}{}", Label, Point.X, Point.Y, Point.Z, bRan ? (bHit ? "ON the navmesh, projected z " : "NO navmesh within 300x500") : "FAULTED",
				bHit ? std::to_string((int)Out.Z) : std::string());
		};
		Project(From, "husk");
		Project(To, "player");

		{
			static const char* Path = "/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously";
			static auto Fn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously");
			if (Fn)
			{
				alignas(8) uint8_t B[128] = {};
				*(UObject**)(B + FindOffsetStruct(Path, "WorldContextObject")) = World;
				*(FVector*)(B + FindOffsetStruct(Path, "PathStart")) = From;
				*(FVector*)(B + FindOffsetStruct(Path, "PathEnd")) = To;
				const bool bRan = ProbeCall(Nav, Fn, B);
				auto NavPath = *(UObject**)(B + FindOffsetStruct(Path, "ReturnValue"));
				bool bValid = false, bPartial = false;
				if (bRan && NavPath)
				{
					static auto IsValidFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationPath.IsValid");
					static auto IsPartialFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationPath.IsPartial");
					struct { bool ReturnValue; } R{};
					if (IsValidFn) { ProbeCall(NavPath, IsValidFn, &R); bValid = R.ReturnValue; }
					R = {};
					if (IsPartialFn) { ProbeCall(NavPath, IsPartialFn, &R); bPartial = R.ReturnValue; }
				}
				LOG_INFO(LogDev, "Zone: nav probe: path husk -> player: {} (valid {}, partial {})", !bRan ? "FAULTED" : (NavPath ? "path object returned" : "no path object"), bValid, bPartial);
			}
		}

		if (auto Controller = Husk->GetController())
		{
			static const char* Path = "/Script/AIModule.AIController.GetMoveStatus";
			static auto Fn = FindObject<UFunction>(L"/Script/AIModule.AIController.GetMoveStatus");
			if (Fn)
			{
				alignas(8) uint8_t B[16] = {};
				ProbeCall(Controller, Fn, B);
				LOG_INFO(LogDev, "Zone: nav probe: controller move status {} (0 idle, 1 waiting, 2 paused, 3 moving)", (int)*(uint8_t*)(B + FindOffsetStruct(Path, "ReturnValue")));
			}
		}
	}

	// Is there a full navmesh path from the husk to its target? The game's own movement handles every husk that has one; only one without a path gets walked
	// by us (the stand-in from before the navmesh existed).
	static bool HasFullNavPath(AFortPawn* Husk, AActor* Target)
	{
		auto World = (UObject*)GetWorld();
		static int NavOffset = World->GetOffset("NavigationSystem", false);
		auto Nav = NavOffset != -1 ? World->Get<UObject*>(NavOffset) : nullptr;
		static auto Fn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously");
		static auto IsValidFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationPath.IsValid");
		static auto IsPartialFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationPath.IsPartial");
		static const char* Path = "/Script/NavigationSystem.NavigationSystemV1.FindPathToLocationSynchronously";
		if (!Nav || !Fn || !IsValidFn || !IsPartialFn) return false;

		alignas(8) uint8_t B[128] = {};
		*(UObject**)(B + FindOffsetStruct(Path, "WorldContextObject")) = World;
		*(FVector*)(B + FindOffsetStruct(Path, "PathStart")) = Husk->GetActorLocation();
		*(FVector*)(B + FindOffsetStruct(Path, "PathEnd")) = Target->GetActorLocation();
		if (!ProbeCall(Nav, Fn, B)) return false;
		auto NavPath = *(UObject**)(B + FindOffsetStruct(Path, "ReturnValue"));
		if (!NavPath) return false;

		struct { bool ReturnValue; } R{};
		ProbeCall(NavPath, IsValidFn, &R);
		if (!R.ReturnValue) return false;
		R = {};
		ProbeCall(NavPath, IsPartialFn, &R);
		return !R.ReturnValue;
	}

	// Takes over every enemy AI pawn the director spawned: the husk gets the server-side movement set-up and the shamble/bite
	// of the tick below (their own AI does not run on this server yet), and counts toward the mission's husk bookkeeping.
	static void AdoptEncounterHusks()
	{
		static auto AIPawnClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIPawn");

		if (!AIPawnClass)
			return;

		auto Pawns = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AIPawnClass);

		for (int i = 0; i < Pawns.Num(); ++i)
		{
			auto Pawn = Cast<AFortPawn>(Pawns.at(i));

			if (!Pawn || !IsEnemyAIPawn(Pawn) || States.find(Pawn) != States.end() || !bIsAlivePawn(Pawn))
				continue;

			bool bCorpse = false;
			for (auto& Corpse : Corpses)
				bCorpse |= Corpse.Pawn == Pawn;

			if (bCorpse)
				continue;

			PrepareHusk(Pawn);
			States[Pawn];
			Husks.push_back(Pawn);

			if (!gAIDriven.count(Pawn) && Pawn->GetController() && RunTreeFor(Pawn->GetController(), Pawn))
				gAIDriven.insert(Pawn);
			gEverHadHusk = true;

			auto Pos = Pawn->GetActorLocation();
			LOG_INFO(LogDev, "Zone: adopted encounter husk #{} {} at ({:.0f},{:.0f},{:.0f}) controller {}", HuskCount(), Pawn->ClassPrivate->GetName(),
				Pos.X, Pos.Y, Pos.Z, Pawn->GetController() ? "yes" : "none");
		}

		Pawns.Free();
	}

	// What the AI director thinks it is doing, for the log: the numbers that say why an encounter spawns nothing (not active,
	// no spawn points, caps at zero, the director switched off ...)
	static void LogDirectorState(bool bQuietWhenIdle = false)
	{
		auto GameMode = ((UObject*)GetWorld())->Get<UObject*>("AuthorityGameMode");
		static int DirectorOffset = GameMode ? GameMode->GetOffset("AIDirector", false) : -1;
		auto Director = (GameMode && DirectorOffset != -1) ? GameMode->Get<UObject*>(DirectorOffset) : nullptr;

		if (!Director)
		{
			LOG_WARN(LogDev, "Zone: director state: the game mode has NO AI director");
			return;
		}

		auto Int = [](UObject* Object, const char* Name) -> int
		{
			int Offset = Object->GetOffset(Name, false);
			return Offset == -1 ? -1 : Object->Get<int>(Offset);
		};
		auto Byte = [](UObject* Object, const char* Name) -> int
		{
			int Offset = Object->GetOffset(Name, false);
			return Offset == -1 ? -1 : (int)Object->Get<uint8_t>(Offset);
		};
		auto Bit = [](UObject* Object, const char* Name) -> int
		{
			int Offset = Object->GetOffset(Name, false);
			void* Prop = Object->GetProperty(Name, false);
			return (Offset == -1 || !Prop) ? -1 : (int)Object->ReadBitfieldValue(Offset, GetFieldMask(Prop));
		};
		auto Num = [](UObject* Object, const char* Name) -> int
		{
			int Offset = Object->GetOffset(Name, false);
			return Offset == -1 ? -1 : Object->Get<TArray<UObject*>>(Offset).Num();
		};

		// the periodic report stays silent while the director has nothing going on
		if (bQuietWhenIdle && Num(Director, "ActiveEncounters") <= 0 && Num(Director, "InactiveEncounters") <= 0 && Num(Director, "PendingSpawns") <= 0
			&& Int(Director, "NumActiveAlive") <= 0 && Num(Director, "AIPawns") <= 0)
			return;

		LOG_INFO(LogDev, "Zone: director state: active encounters {}, inactive {}, pending spawns {}, alive {} / max {} (total cap {}), AI pawns {}, bAIDisabled {}, bUseSpawnCap {}",
			Num(Director, "ActiveEncounters"), Num(Director, "InactiveEncounters"), Num(Director, "PendingSpawns"),
			Int(Director, "NumActiveAlive"), Int(Director, "MaxActiveAlive"), Int(Director, "MaxTotalActiveAliveAI"), Num(Director, "AIPawns"),
			Bit(Director, "bAIDisabled"), Bit(Director, "bUseSpawnCap"));

		{
			// the encounters' spawn rifts run environment queries against the navigation mesh: without one nothing spawns
			auto World = (UObject*)GetWorld();
			static int NavOffset = World->GetOffset("NavigationSystem", false);
			auto Count = [&](const wchar_t* Path)
			{
				auto Class = FindObject<UClass>(Path);
				if (!Class) return -1;
				auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Class);
				int N = Actors.Num();
				Actors.Free();
				return N;
			};
			LOG_INFO(LogDev, "Zone:   navigation: system {}, recast meshes {}, bounds volumes {}",
				(NavOffset != -1 && World->Get<UObject*>(NavOffset)) ? "present" : "NONE",
				Count(L"/Script/NavigationSystem.RecastNavMesh"), Count(L"/Script/NavigationSystem.NavMeshBoundsVolume"));

			if (NavOffset != -1 && World->Get<UObject*>(NavOffset))
			{
				auto Nav = World->Get<UObject*>(NavOffset);
				LOG_INFO(LogDev, "Zone:   nav lock flags 0x{:x}, rift rifts-actor count {}", (int)*((uint8_t*)Nav + ZoneFinder::Fixed730::NavBuildingLockFlags),
					Count(L"/Game/VisualThreatManager/Rifts/Blueprints/B_Rift_Portals.B_Rift_Portals_C"));
			}
		}

		{
			// the rift manager finds spawn rifts with environment queries, which need the AI system's query manager
			auto World = (UObject*)GetWorld();
			static int AISystemOffset = World->GetOffset("AISystem", false);
			auto AISystem = AISystemOffset != -1 ? World->Get<UObject*>(AISystemOffset) : nullptr;
			auto QueryManager = AISystem ? AISystem->Get<UObject*>("EnvironmentQueryManager") : nullptr;
			LOG_INFO(LogDev, "Zone:   ai system: {}, environment query manager: {}", AISystem ? AISystem->GetFullName() : "NONE", QueryManager ? QueryManager->GetFullName() : "NONE");
		}

		for (const char* ListName : { "ActiveEncounters", "InactiveEncounters" })
		{
			int ListOffset = Director->GetOffset(ListName, false);

			if (ListOffset == -1)
				continue;

			auto& List = Director->Get<TArray<UObject*>>(ListOffset);

			for (int i = 0; i < List.Num() && i < 6; ++i)
			{
				auto Encounter = List.at(i);

				if (!Encounter || !Encounter->IsValidLowLevel())
					continue;

				int GroupsOffset = Encounter->GetOffset("ActiveSpawnGroups", false);
				int Groups = GroupsOffset == -1 ? -1 : Encounter->Get<TArray<UObject*>>(GroupsOffset).Num();

				auto Float = [](UObject* Object, const char* Name) -> float
				{
					int Offset = Object->GetOffset(Name, false);
					return Offset == -1 ? -1.f : Object->Get<float>(Offset);
				};
				int RiftManagerOffset = Encounter->GetOffset("RiftManager", false);
				int TargetOffset = Encounter->GetOffset("TargetObjective", false);

				LOG_INFO(LogDev, "Zone:   {} encounter {}: state {}, pacing {}, spawn points {}/{}, active spawn groups {}, bCanBeActive {}, active cap {}",
					ListName, Encounter->GetName(), Byte(Encounter, "EncounterState"), Byte(Encounter, "PacingState"),
					Int(Encounter, "CurrentSpawnPointsUsed"), Int(Encounter, "CurrentSpawnPointsCap"), Groups, Bit(Encounter, "bCanBeActive"),
					Int(Encounter, "ActiveEnemyCap"));
				LOG_INFO(LogDev, "Zone:     rifts {}/{}, rift manager {}, assignments {}, enemy spawn data {}, max alive {}, hostility {:.2f}, difficulty {:.1f}, target objective {}",
					Int(Encounter, "NumRiftsUsed"), Int(Encounter, "NumRiftsToUse"), (RiftManagerOffset != -1 && Encounter->Get<UObject*>(RiftManagerOffset)) ? "yes" : "NO",
					Num(Encounter, "EncounterAssignments"), Num(Encounter, "EnemySpawnData"), Int(Encounter, "MaxActiveAlive"),
					Float(Encounter, "CurrentHostilityLevel"), Float(Encounter, "DesiredDifficultyLevel"), (TargetOffset != -1 && Encounter->Get<UObject*>(TargetOffset)) ? "yes" : "none");
			}
		}
	}

	// Called every tick while a mission's husk objective is live
	static void WatchEncounter(std::chrono::steady_clock::time_point Now)
	{
		if (Now >= gNextAdopt)
		{
			gNextAdopt = Now + std::chrono::milliseconds(500);
			AdoptEncounterHusks();
		}

		// every 5 s for as long as the director has encounters (the later waves, e.g. defend the fort, start well after the husk objective)
		if (Now >= gNextDirectorLog)
		{
			gNextDirectorLog = Now + std::chrono::seconds(5);
			LogDirectorState(true);
		}

		float Elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(Now - gEncounterStart).count();
		auto& Stw = RebootConfig::Get().Stw;

		if (!gEverHadHusk && !gFallbackUsed && Stw.bHuskFallback && Elapsed >= Stw.HuskFallbackSeconds)
		{
			gFallbackUsed = true;
			LOG_WARN(LogDev, "Zone: no encounter husk appeared {:.0f}s after the objective started, using the manual fallback spawn", Elapsed);
			LogDirectorState();
			SpawnFallbackTutorialHusks();
			gEverHadHusk = true;
		}
	}

	void (*StartEncounterTraceOriginal)(UObject*, FFrame&, void*) = nullptr;

	// the original behind a structured-exception guard (no C++ objects here, so __try is allowed)
	static bool StartEncounterGuarded(UObject* Mission, FFrame& Stack, void* Ret)
	{
		__try { StartEncounterTraceOriginal(Mission, Stack, Ret); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	void StartEncounterTraceHook(UObject* Mission, FFrame& Stack, void* Ret)
	{
		if (!Globals::bZoneMode || !Mission)
			return StartEncounterTraceOriginal(Mission, Stack, Ret);

		// the state the native's silent exits depend on (the mission's authority role, the director's encounter class)
		auto GameMode = ((UObject*)GetWorld())->Get<UObject*>("AuthorityGameMode");
		static int DirectorOffset = GameMode ? GameMode->GetOffset("AIDirector", false) : -1;
		auto Director = (GameMode && DirectorOffset != -1) ? GameMode->Get<UObject*>(DirectorOffset) : nullptr;

		static int MissionRoleOffset = Mission->GetOffset("Role", false);
		constexpr int EncounterClassOffset = ZoneFinder::Fixed730::DirectorEncounterClass;
		LOG_INFO(LogDev, "Zone: StartMissionAIEncounterFromGeneratedProfile called on {}: role {}, director {} ({}), encounter class {}",
			Mission->GetName(), MissionRoleOffset != -1 ? (int)*((uint8_t*)Mission + MissionRoleOffset) : -1, Director ? Director->GetName() : std::string("NONE"),
			Director && Director->ClassPrivate ? Director->ClassPrivate->GetName() : std::string("-"),
			Director ? *(void**)((uint8_t*)Director + EncounterClassOffset) : nullptr);

		// StartEncounterWithInitData returns null straight away unless the director's encounter class (not reflected, see ZoneFinder.h) holds a CLASS deriving
		// from FortAIEncounterInfo (it checks the class hierarchy, then constructs the encounter object from it). The Blueprint director sets it and the
		// native one leaves it empty, so the constructor's zero stayed and nothing was ever created.
		if (Director && !*(UObject**)((uint8_t*)Director + EncounterClassOffset))
		{
			static auto EncounterInfoClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIEncounterInfo");
			static int BaseClassOffset = Director->GetOffset("BaseEncounterClass", false);
			auto BaseClass = BaseClassOffset != -1 ? Director->Get<UClass*>(BaseClassOffset) : nullptr;

			// FortAIEncounterInfo itself cannot be constructed (the native's construct failed and its failure message crashed); the game's own
			// concrete encounter Blueprint for defending the base ships in the paks, the director Blueprint that normally names it does not
			static UClass* GateEncounterClass = LoadObject<UClass>(L"/Game/AIDirector/Encounters/BaseGateEncounter.BaseGateEncounter_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
			if (!BaseClass && GateEncounterClass)
				BaseClass = GateEncounterClass;

			auto Chosen = BaseClass ? BaseClass : EncounterInfoClass;

			if (Chosen)
			{
				*(UObject**)((uint8_t*)Director + EncounterClassOffset) = Chosen;
				LOG_INFO(LogDev, "Zone: set the director's encounter class to {} (from {}); BaseEncounterClass offset is 0x{:x}",
					Chosen->GetFullName(), BaseClass ? "BaseEncounterClass or BaseGateEncounter" : "FortAIEncounterInfo", BaseClassOffset);
			}
			else
			{
				LOG_WARN(LogDev, "Zone: no encounter class to give the director");
			}
		}

		// FCurveTableRowHandle::Eval warns "No CurveTable for row %s" when a row name is set and its table is not, and the AI director's curve
		// tables are server data that is not in the client paks. Formatting that warning faults in memcpy (the name it prints is not valid), once on our call
		// and again every time the director ticks the encounter. Below verbosity 3 the same code skips the warning and carries on with the default value.
		{
			static bool bQuietCurveWarning = false;
			if (!bQuietCurveWarning)
			{
				bQuietCurveWarning = true;
				// the verbosity byte Eval compares against Warning, read from that compare (ZoneFinder.h)
				auto Verbosity = ZoneFinder::CurveTableWarningVerbosity();
				DWORD OldProtect = 0;
				if (Verbosity && *Verbosity >= 3 && VirtualProtect(Verbosity, 1, PAGE_READWRITE, &OldProtect))
				{
					*Verbosity = 2;
					VirtualProtect(Verbosity, 1, OldProtect, &OldProtect);
					LOG_INFO(LogDev, "Zone: silenced the missing curve table warning");
				}
			}
		}

		// The native now gets past the class check and into the director's own setup, which reads data only the Blueprint director fills in (a null
		// curve/graph faults in a message builder). A fault there must not take the server down: the fallback husks still work.
		if (!StartEncounterGuarded(Mission, Stack, Ret))
		{
			LOG_WARN(LogDev, "Zone: StartMissionAIEncounterFromGeneratedProfile faulted inside the engine");
			if (Ret) *(UObject**)Ret = nullptr;
		}

		auto Result = Ret ? *(UObject**)Ret : nullptr;
		LOG_INFO(LogDev, "Zone: StartMissionAIEncounterFromGeneratedProfile returned {}", Result ? Result->GetFullName() : std::string("NULL (no encounter created)"));

		if (!Result)
			LogDirectorState();
	}

	void OnHuskObjectiveStarted(UObject* Objective)
	{
		if (!Globals::bZoneMode)
			return;

		gTutorialObjective = Objective;
		gEncounterWatch = true;
		gEverHadHusk = false;
		gFallbackUsed = false;
		gEncounterStart = std::chrono::steady_clock::now();
		gNextAdopt = gEncounterStart;
		gNextDirectorLog = gEncounterStart + std::chrono::seconds(2);
		gDirectorLogsLeft = 12;

		LOG_INFO(LogDev, "Zone: husk objective {} started, watching for the game's own encounter husks", Objective ? Objective->GetFullName() : "NULL");
	}

	// ---- navigation for the AI director's rifts (plan: scratchpad husk_plan.md, root cause R1)
	// The fort waves' encounters are the regular kind: a rift manager finds spawn rifts with environment queries, and does not start until the navigation system
	// has no build lock and a navmesh exists around the encounter's goals. On this client exe the initial lock (UNavigationSystemV1::NavBuildingLockFlags,
	// +0x398 on 7.30.1) is never released, and the zone's only nav bounds volume (UnitNavMeshBounds, in the /Game/Maps/NavMeshBounds sublevel) is one unit
	// big at the origin. Limbo, another STW server on the same exe family, fixes both: scale the volume over the zone, release the lock, rebuild.
	// these two take a raw function pointer so the __try holds no C++ objects
	static bool SafeVirtualCall(void* Function, UObject* Object, void* Argument)
	{
		__try { ((void(*)(UObject*, void*))Function)(Object, Argument); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static bool InsideExe(void* Address)
	{
		auto Base = (uint64_t)GetModuleHandleW(0);
		auto Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
		return (uint64_t)Address >= Base && (uint64_t)Address < Base + Nt->OptionalHeader.SizeOfImage;
	}

	// The client cook leaves out every static mesh's UNavCollision (UNavCollision::NeedsLoadForClient is false), and UStaticMeshComponent::IsNavigationRelevant
	// (Xbox build) returns false for a mesh without one. So no floor, slope or cave mesh ever reached the nav octree and the generated tiles were empty: only odd
	// pieces with other component types got a navmesh. A server cook keeps the object; here one is created for every mesh that says it has navigation data.
	// A fresh UNavCollision is no dynamic obstacle and has no convex shapes of its own, so UStaticMeshComponent::DoCustomNavigableGeometryExport falls back to
	// exporting the mesh's body setup collision, the same geometry the physics uses.
	static int EnsureNavCollisions()
	{
		static auto MeshClass = FindObject<UClass>(L"/Script/Engine.StaticMesh");
		static auto NavCollisionClass = FindObject<UClass>(L"/Script/NavigationSystem.NavCollision");
		static int NavCollisionOffset = FindOffsetStruct("/Script/Engine.StaticMesh", "NavCollision", false);
		static int BodySetupOffset = FindOffsetStruct("/Script/Engine.StaticMesh", "BodySetup", false);
		static int HasNavOffset = FindOffsetStruct("/Script/Engine.StaticMesh", "bHasNavigationData", false);

		if (!MeshClass || !NavCollisionClass || NavCollisionOffset == -1 || BodySetupOffset == -1 || HasNavOffset == -1)
		{
			LOG_WARN(LogDev, "Zone: nav collision: classes or offsets missing ({} {} {} {} {})", MeshClass != nullptr, NavCollisionClass != nullptr, NavCollisionOffset, BodySetupOffset, HasNavOffset);
			return 0;
		}

		int Created = 0, Meshes = 0;
		uint8_t HasNavMask = 0;

		for (auto Mesh : GetAllObjectsOfClass(MeshClass))
		{
			if (!Mesh || Mesh->GetName().starts_with("Default__"))
				continue;

			++Meshes;
			if (!HasNavMask) HasNavMask = GetFieldMask(Mesh->GetProperty("bHasNavigationData"));
			if (!HasNavMask || !Mesh->ReadBitfieldValue(HasNavOffset, HasNavMask))
				continue;
			if (Mesh->Get<UObject*>(NavCollisionOffset) || !Mesh->Get<UObject*>(BodySetupOffset))
				continue;

			auto NavCollision = UGameplayStatics::SpawnObject(NavCollisionClass, Mesh);
			if (!NavCollision)
				continue;

			Mesh->Get<UObject*>(NavCollisionOffset) = NavCollision;
			++Created;
		}

		// a component caches its navigation relevance when it registers (UActorComponent byte +0xae: 0x8 bCanEverAffectNavigation, 0x4 the cached
		// bNavigationRelevant; Xbox SetCanEverAffectNavigation) and the nav octree only takes components whose cache says yes. Every mesh component that was
		// registered before its mesh got the object above still says no: switching bCanEverAffectNavigation off and on is the engine's own way to recompute
		// it and (for a registered component) to add it to the octree.
		int Refreshed = 0;
		if (Created > 0)
		{
			static auto ComponentClass = FindObject<UClass>(L"/Script/Engine.StaticMeshComponent");
			static int MeshOffset = FindOffsetStruct("/Script/Engine.StaticMeshComponent", "StaticMesh", false);

			static int AffectOffset = FindOffsetStruct("/Script/Engine.ActorComponent", "bCanEverAffectNavigation", false);
			int States[4] = {}, Total = 0, WithNavMesh = 0;

			if (ComponentClass && MeshOffset != -1)
			{
				for (auto Component : GetAllObjectsOfClass(ComponentClass))
				{
					if (!Component || Component->GetName().starts_with("Default__"))
						continue;

					constexpr int NavFlagsOffset = ZoneFinder::Fixed730::PrimitiveNavRelevancyFlags;
					const uint8_t Flags = *((uint8_t*)Component + NavFlagsOffset);
					++Total;
					auto CheckMesh = Component->Get<UObject*>(MeshOffset);
					if (CheckMesh && CheckMesh->Get<UObject*>(NavCollisionOffset)) { ++WithNavMesh; ++States[(Flags >> 2) & 3]; }
					if (!(Flags & 0x8) || (Flags & 0x4))
						continue; // never affects navigation, or already relevant

					auto Mesh = Component->Get<UObject*>(MeshOffset);
					if (!Mesh || !Mesh->Get<UObject*>(NavCollisionOffset))
						continue;

					// what IsNavigationRelevant now answers for this component (its mesh has the object); PrepareBoundsAndUnlock's octree refill reads the cache
					*((uint8_t*)Component + NavFlagsOffset) |= 0x4;
					++Refreshed;
				}
			}

			LOG_INFO(LogDev, "Zone: nav collision: {} mesh components, {} with a fixed mesh; before: (relevant cache, can affect) 00:{} 01:{} 10:{} 11:{} (bCanEverAffectNavigation at +0x{:x})",
				Total, WithNavMesh, States[0], States[2], States[1], States[3], AffectOffset);
		}

		static int Logs = 6;
		if (Created > 0 && Logs > 0)
		{
			--Logs;
			LOG_INFO(LogDev, "Zone: nav collision: gave {} of {} static meshes the UNavCollision the client cook left out, refreshed {} mesh components", Created, Meshes, Refreshed);
		}
		return Created;
	}

	// What is the ground under a point, and is it something the navmesh can be built from? Traces down and reports the hit actor and component, the
	// component's bCanEverAffectNavigation and, for a static mesh, whether its mesh has navigation data. Only the navmesh diagnostics use it.
	static void LogGroundUnder(const FVector& Point, const char* Label)
	{
		static auto TraceFn = FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.LineTraceSingle");
		static auto LibraryDefault = FindObject(L"/Script/Engine.Default__KismetSystemLibrary");
		static const char* T = "/Script/Engine.KismetSystemLibrary.LineTraceSingle";
		if (!TraceFn || !LibraryDefault) return;

		std::vector<uint8_t> P(TraceFn->GetPropertiesSize() + 64, 0);
		*(UObject**)(P.data() + FindOffsetStruct(T, "WorldContextObject")) = GetWorld();
		*(FVector*)(P.data() + FindOffsetStruct(T, "Start")) = FVector(Point.X, Point.Y, Point.Z + 300.f);
		*(FVector*)(P.data() + FindOffsetStruct(T, "End")) = FVector(Point.X, Point.Y, Point.Z - 1500.f);
		*(bool*)(P.data() + FindOffsetStruct(T, "bIgnoreSelf")) = true;
		if (!ProbeCall(LibraryDefault, TraceFn, P.data())) return;

		const bool bHit = *(bool*)(P.data() + FindOffsetStruct(T, "ReturnValue"));
		const int HitOffset = FindOffsetStruct(T, "OutHit");
		auto Resolve = [&](const char* Field) -> UObject*
		{
			int Off = FindOffsetStruct("/Script/Engine.HitResult", Field, false);
			if (Off == -1) return nullptr;
			auto Item = GetItemByIndex(*(int*)(P.data() + HitOffset + Off));
			return Item ? (UObject*)Item->Object : nullptr;
		};
		auto Actor = bHit ? Resolve("Actor") : nullptr;
		auto Component = bHit ? Resolve("Component") : nullptr;
		auto Impact = *(FVector*)(P.data() + HitOffset + FindOffsetStruct("/Script/Engine.HitResult", "ImpactPoint", false));

		std::string Nav = "?";
		if (Component)
		{
			static int AffectOffset = FindOffsetStruct("/Script/Engine.ActorComponent", "bCanEverAffectNavigation", false);
			static auto AffectMask = GetFieldMask(Component->GetProperty("bCanEverAffectNavigation"));
			Nav = AffectOffset != -1 ? (Component->ReadBitfieldValue(AffectOffset, AffectMask) ? "affects nav" : "does NOT affect nav") : "?";
			static int MeshOffset = FindOffsetStruct("/Script/Engine.StaticMeshComponent", "StaticMesh", false);
			if (MeshOffset != -1 && Component->ClassPrivate && Component->ClassPrivate->GetName().find("StaticMesh") != std::string::npos)
			{
				auto Mesh = Component->Get<UObject*>(MeshOffset);
				static int NavCollisionOffset = FindOffsetStruct("/Script/Engine.StaticMesh", "NavCollision", false);
				static int HasNavOffset = FindOffsetStruct("/Script/Engine.StaticMesh", "bHasNavigationData", false);
				static auto HasNavMask = Mesh ? GetFieldMask(Mesh->GetProperty("bHasNavigationData")) : 0;
				if (Mesh)
					Nav += ", mesh " + Mesh->GetName() + " navcollision " + ((NavCollisionOffset != -1 && Mesh->Get<UObject*>(NavCollisionOffset)) ? "yes" : "NO") +
						" hasnavdata " + ((HasNavOffset != -1 && HasNavMask) ? (Mesh->ReadBitfieldValue(HasNavOffset, HasNavMask) ? "yes" : "no") : "?");
			}
		}

		LOG_INFO(LogDev, "Zone: ground under {} ({:.0f},{:.0f},{:.0f}): {} at z {:.0f}: actor {} ({}), component {} ({}), {}", Label, Point.X, Point.Y, Point.Z, bHit ? "hit" : "NOTHING", Impact.Z,
			Actor ? Actor->GetName() : std::string("-"), Actor && Actor->ClassPrivate ? Actor->ClassPrivate->GetName() : std::string("-"),
			Component ? Component->GetName() : std::string("-"), Component && Component->ClassPrivate ? Component->ClassPrivate->GetName() : std::string("-"), Nav);
	}

	// Asks every navigation data object (not just the main one) whether it has navmesh at two fixed points: the tutorial cave where the first husks spawn and the
	// fort's encounter target. Run before and after each of our navigation steps, so a step that empties the cooked navmesh shows up between two lines.
	static void LogNavDataSet(UObject* Nav, const char* Stage)
	{
		static int LogsLeft = 80;
		if (!Nav || LogsLeft <= 0) return;
		--LogsLeft;

		static int SetOffset = Nav->GetOffset("NavDataSet", false);
		static int MainOffset = Nav->GetOffset("MainNavData", false);
		if (SetOffset == -1) return;

		auto& Set = *(TArray<UObject*>*)((uint8_t*)Nav + SetOffset);
		auto Main = MainOffset != -1 ? Nav->Get<UObject*>(MainOffset) : nullptr;
		static int InvokersOffset = Nav->GetOffset("bGenerateNavigationOnlyAroundNavigationInvokers", false);
		static auto InvokersMask = InvokersOffset != -1 ? GetFieldMask(Nav->GetProperty("bGenerateNavigationOnlyAroundNavigationInvokers")) : 0;
		LOG_INFO(LogDev, "Zone: navdata [{}]: {} entries, main {}, only around invokers {}", Stage, Set.Num(), Main ? Main->GetName() : std::string("NONE"),
			InvokersOffset != -1 ? (Nav->ReadBitfieldValue(InvokersOffset, InvokersMask) ? "yes" : "no") : "?");

		static auto Fn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
		static const char* Path = "/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation";
		const FVector Points[] = { FVector(-3563.f, -2063.f, -1455.f), FVector(5120.f, 17664.f, 384.f) };

		for (int i = 0; i < Set.Num() && i < 8; ++i)
		{
			auto Data = Set.at(i);
			if (!Data || !RebootIsReadable(Data, 8)) continue;

			static int GenOffset = Data->GetOffset("RuntimeGeneration", false);
			static int RadiusOffset = -2;
			if (RadiusOffset == -2) RadiusOffset = FindOffsetStruct("/Script/NavigationSystem.RecastNavMesh", "AgentRadius", false);
			const bool bRecast = Data->ClassPrivate && Data->ClassPrivate->GetName().find("NavMesh") != std::string::npos;

			std::string Hits;
			for (auto& Point : Points)
			{
				bool bHit = false;
				if (Fn)
				{
					alignas(8) uint8_t B[128] = {};
					*(UObject**)(B + FindOffsetStruct(Path, "WorldContextObject")) = GetWorld();
					*(FVector*)(B + FindOffsetStruct(Path, "Point")) = Point;
					*(UObject**)(B + FindOffsetStruct(Path, "NavData")) = Data;
					*(FVector*)(B + FindOffsetStruct(Path, "QueryExtent")) = FVector{ 300.f, 300.f, 500.f };
					bHit = ProbeCall(Nav, Fn, B) && *(bool*)(B + FindOffsetStruct(Path, "ReturnValue"));
				}
				Hits += bHit ? " yes" : " no";
			}

			if (bRecast)
			{
				static int PoolOffset = FindOffsetStruct("/Script/NavigationSystem.RecastNavMesh", "TilePoolSize", false);
				LOG_INFO(LogDev, "Zone: navdata [{}]   {} TilePoolSize {}", Stage, Data->GetName(), PoolOffset != -1 ? Data->Get<int>(PoolOffset) : -1);
			}

			// any polygon anywhere? a random navigable point within 60000 of the origin
			{
				static auto RandomFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_GetRandomPointInNavigableRadius");
				static const char* RPath = "/Script/NavigationSystem.NavigationSystemV1.K2_GetRandomPointInNavigableRadius";
				if (RandomFn && bRecast)
				{
					alignas(8) uint8_t B[128] = {};
					*(UObject**)(B + FindOffsetStruct(RPath, "WorldContextObject")) = GetWorld();
					*(FVector*)(B + FindOffsetStruct(RPath, "Origin")) = FVector(0.f, 0.f, 0.f);
					*(float*)(B + FindOffsetStruct(RPath, "Radius")) = 60000.f;
					*(UObject**)(B + FindOffsetStruct(RPath, "NavData")) = Data;
					const bool bRan = ProbeCall(Nav, RandomFn, B);
					const bool bOk = bRan && *(bool*)(B + FindOffsetStruct(RPath, "ReturnValue"));
					auto P = *(FVector*)(B + FindOffsetStruct(RPath, "RandomLocation"));
					LOG_INFO(LogDev, "Zone: navdata [{}]   {} random navigable point: {} ({:.0f},{:.0f},{:.0f})", Stage, Data->GetName(), bRan ? (bOk ? "found" : "NONE anywhere") : "FAULTED", P.X, P.Y, P.Z);
				}
			}

			LOG_INFO(LogDev, "Zone: navdata [{}]   {} ({}): runtime generation {}, agent radius {:.0f}, navmesh at cave/fort:{}", Stage, Data->GetName(),
				Data->ClassPrivate ? Data->ClassPrivate->GetName() : std::string("?"), GenOffset != -1 ? (int)Data->Get<uint8_t>(GenOffset) : -1,
				bRecast && RadiusOffset != -1 ? Data->Get<float>(RadiusOffset) : -1.f, Hits);
		}
	}

	void NavigationTick()
	{
		if (!Globals::bZoneMode || gNavDone || !GetWorld() || Fortnite_Version < 7.3 || Fortnite_Version >= 7.31)
			return;

		static auto LastTry = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastTry).count() < 1000)
			return;

		LastTry = Now;

		auto World = (UObject*)GetWorld();
		static int NavOffset = World->GetOffset("NavigationSystem", false);
		auto Nav = NavOffset != -1 ? World->Get<UObject*>(NavOffset) : nullptr;

		// the volume lives in a streamed sublevel: wait for it
		static auto VolumeClass = FindObject<UClass>(L"/Script/NavigationSystem.NavMeshBoundsVolume");
		UObject* Volume = nullptr;

		if (VolumeClass)
		{
			auto Volumes = UGameplayStatics::GetAllActorsOfClass(GetWorld(), VolumeClass);
			Volume = Volumes.Num() > 0 ? Volumes.at(0) : nullptr;
			Volumes.Free();
		}

		++gNavAttempts;

		if (!Nav || !Volume)
		{
			if (gNavAttempts == 5 || gNavAttempts == 30)
			{
				LOG_WARN(LogDev, "Zone: nav: waiting ({}s) for the navigation system ({}) and the nav bounds volume ({})", gNavAttempts, Nav != nullptr, Volume != nullptr);
			}
			if (gNavAttempts >= 60)
				gNavDone = true;
			return;
		}

		gNavDone = true;
		LogNavDataSet(Nav, "on load, before our steps");
		EnsureNavCollisions(); // before PrepareBoundsAndUnlock refills the nav octree
		uint8_t* Lock = (uint8_t*)Nav + ZoneFinder::Fixed730::NavBuildingLockFlags;
		const uint8_t LockBefore = *Lock;

		LOG_INFO(LogDev, "Zone: nav: system {} (class {}), lock flags 0x{:x}", Nav->GetName(), Nav->ClassPrivate ? Nav->ClassPrivate->GetName() : std::string("?"), (int)LockBefore);

		// a) bounds: cover the zone. The encounter target is at (5120,17664,384) and its rifts sit 4000-8000 units from it; the husk cave is near (-3500,-2500,-1455).
		// The volume is a static level actor: K2_SetActorLocation leaves it at the origin (the log showed origin (0,0,0) after the call), so the box stays centred
		// there and is made big enough instead: X -15000..15000, Y -27000..27000 covers the cave, the player's start, the fort and the whole rift ring.
		const FVector Center(0.f, 0.f, 0.f);
		const FVector Half(15000.f, 27000.f, 4000.f);

		auto Actor = (AActor*)Volume;
		static const char* BoundsFn = "/Script/Engine.Actor.GetActorBounds";
		static auto GetBoundsFn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorBounds");
		FVector Extent0(0.f, 0.f, 0.f);

		if (GetBoundsFn)
		{
			alignas(16) uint8_t Buffer[64] = {};
			*(bool*)(Buffer + FindOffsetStruct(BoundsFn, "bOnlyCollidingComponents")) = false;
			Actor->ProcessEvent(GetBoundsFn, Buffer);
			Extent0 = *(FVector*)(Buffer + FindOffsetStruct(BoundsFn, "BoxExtent"));
			auto Origin0 = *(FVector*)(Buffer + FindOffsetStruct(BoundsFn, "Origin"));
			LOG_INFO(LogDev, "Zone: nav: volume {} bounds before: origin ({:.1f},{:.1f},{:.1f}) extent ({:.2f},{:.2f},{:.2f})", Volume->GetName(), Origin0.X, Origin0.Y, Origin0.Z, Extent0.X, Extent0.Y, Extent0.Z);
		}

		// The map's own volume (UnitNavMeshBounds, a unit box at the origin) is left as it is: on a real server the zone's bounds come from
		// PrepareBoundsAndUnlock below ("Auto generating navigation bounds"), not from a volume. Scaling the volume never produced a tile.

		// b) release the initial build lock the way the real server does: UFortNavSystem::PrepareBoundsAndUnlock (Athena's override jumps to it) builds the
		// FortNavigationGraph the rift queries need and releases the lock, which rebuilds. It takes an FNavigationBounds (0x2c bytes), started from the nav
		// system's own copy and given our box. Both functions and the copy's offset are found by signature (ZoneFinder.h); the slot is wherever this nav
		// system's vtable holds one of them.
		const uint64_t Base = (uint64_t)GetModuleHandleW(0);
		const uint64_t PrepareFns[] = { ZoneFinder::PrepareBoundsAndUnlock(), ZoneFinder::AthenaPrepareBoundsAndUnlock() };
		const int BoundsOffset = ZoneFinder::NavSystemBoundsOffset();
		void* PrepareSlot = nullptr;
		int PrepareIndex = -1;
		for (int i = 0; Nav->VFTable && i < 0x200 && !PrepareSlot; ++i)
		{
			for (auto Fn : PrepareFns)
			{
				if (Fn && (uint64_t)Nav->VFTable[i] == Fn) { PrepareSlot = Nav->VFTable[i]; PrepareIndex = i; }
			}
		}

		LOG_INFO(LogDev, "Zone: nav: PrepareBoundsAndUnlock {} (vtable slot 0x{:x}), bounds copy at 0x{:x}", PrepareSlot ? "found" : "NOT in this nav system's vtable",
			PrepareIndex * 8, BoundsOffset);

		if (PrepareSlot && BoundsOffset > 0)
		{
			alignas(8) uint8_t Bounds[0x2c];
			memcpy(Bounds, (uint8_t*)Nav + BoundsOffset, sizeof(Bounds));
			*(FVector*)(Bounds + 0x04) = FVector(Center.X - Half.X, Center.Y - Half.Y, Center.Z - Half.Z);
			*(FVector*)(Bounds + 0x10) = FVector(Center.X + Half.X, Center.Y + Half.Y, Center.Z + Half.Z);
			Bounds[0x1c] = 1;

			// +0x24 is the bounds' level, a weak pointer (object index, serial number). UFortNavSystem::GatherNavigationBounds drops the bounds when it does not
			// resolve, which is why the unlock used to build nothing: point it at the persistent level. A serial number of 0 means no weak pointer was ever made
			// to the level; one is given the way FUObjectArray::AllocateSerialNumber would, from a range the engine's counter does not reach.
			static int LevelOffset = World->GetOffset("PersistentLevel", false);
			auto Level = LevelOffset != -1 ? World->Get<UObject*>(LevelOffset) : nullptr;
			auto Item = Level ? GetItemByIndex(Level->InternalIndex) : nullptr;
			if (Item && Item->SerialNumber == 0)
				Item->SerialNumber = 0x40000000 + Level->InternalIndex;
			*(int*)(Bounds + 0x24) = Level ? Level->InternalIndex : -1;
			*(int*)(Bounds + 0x28) = Item ? Item->SerialNumber : 0;
			LOG_INFO(LogDev, "Zone: nav: bounds level {} (index {}, serial {})", Level ? Level->GetName() : std::string("NONE"), *(int*)(Bounds + 0x24), *(int*)(Bounds + 0x28));

			const bool bOk = SafeVirtualCall(PrepareSlot, Nav, Bounds);
			LOG_INFO(LogDev, "Zone: nav: PrepareBoundsAndUnlock {}, lock 0x{:x} -> 0x{:x}", bOk ? "ran" : "FAULTED", (int)LockBefore, (int)*Lock);
			LogNavDataSet(Nav, "right after PrepareBoundsAndUnlock");
		}

		// c) the lock is still set: ReleaseInitialBuildingLock clears the initial lock and rebuilds; failing that clear the bit as Limbo does
		if (*Lock != 0)
		{
			void* ReleaseSlot = Nav->VFTable ? Nav->VFTable[ZoneFinder::Fixed730::NavReleaseInitialBuildingLockSlot / 8] : nullptr;
			if (ReleaseSlot && InsideExe(ReleaseSlot))
			{
				const bool bOk = SafeVirtualCall(ReleaseSlot, Nav, nullptr);
				LOG_INFO(LogDev, "Zone: nav: ReleaseInitialBuildingLock (exe+0x{:x}) {}, lock now 0x{:x}", (uint64_t)ReleaseSlot - Base, bOk ? "ran" : "FAULTED", (int)*Lock);
			}

			if (*Lock != 0)
			{
				*Lock &= ~4;
				LOG_INFO(LogDev, "Zone: nav: cleared the initial lock bit by hand, lock now 0x{:x}", (int)*Lock);
			}
		}
	}

	void HusksTick()
	{
		NavigationTick();

		// meshes loaded after the unlock (streamed or first spawned) need the same; their components enter the octree when they register
		if (gNavDone && GetWorld())
		{
			static auto LastNavCollisionScan = std::chrono::steady_clock::now();
			if (std::chrono::steady_clock::now() - LastNavCollisionScan > std::chrono::seconds(15))
			{
				LastNavCollisionScan = std::chrono::steady_clock::now();
				EnsureNavCollisions();
			}
		}

		// the rebuild a lock release starts is asynchronous: look again 5, 20 and 60 s later
		if (gNavDone && GetWorld())
		{
			static auto NavDoneAt = std::chrono::steady_clock::now();
			static int NextCheck = 0;
			const int Checks[] = { 5, 20, 60, 120, 180, 300, 480, 600 };
			auto Since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - NavDoneAt).count();
			if (NextCheck < 8 && Since >= Checks[NextCheck])
			{
				auto World = (UObject*)GetWorld();
				static int NavOffset = World->GetOffset("NavigationSystem", false);
				char Label[48]; snprintf(Label, sizeof(Label), "%ds after our steps", Checks[NextCheck]);
				LogNavDataSet(NavOffset != -1 ? World->Get<UObject*>(NavOffset) : nullptr, Label);
				if (NextCheck == 0)
				{
					LogGroundUnder(FVector(-3563.f, -2063.f, -1455.f), "the cave husks");
					LogGroundUnder(FVector(5120.f, 17664.f, 384.f), "the fort target");
					LogGroundUnder(FVector(4671.f, 10954.f, 77.f), "the path to the fort");
					LogGroundUnder(FVector(2794.f, 4351.f, 461.f), "the player's start");
				}
				++NextCheck;
			}
		}

		if (!Globals::bZoneMode || (!gEncounterWatch && Husks.empty() && Corpses.empty() && DownedFor.empty()))
			return;

		static auto LastTick = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();
		float Dt = std::chrono::duration_cast<std::chrono::duration<float>>(Now - LastTick).count();

		if (Dt < 0.1f)
			return;

		LastTick = Now;

		// corpses: the body lingers for a few seconds so the death is visible, then it is cleaned up.
		for (int i = (int)Corpses.size() - 1; i >= 0; --i)
		{
			auto& Corpse = Corpses.at(i);
			Corpse.TimeLeft -= Dt;

			if (Corpse.TimeLeft > 0.f)
				continue;

			if (Corpse.Pawn && Corpse.Pawn->IsValidLowLevel())
				Corpse.Pawn->K2_DestroyActor();

			States.erase(Corpse.Pawn);
			Corpses.erase(Corpses.begin() + i);
		}

		// downed players: a revive clears DBNO and drops the timer; otherwise they bleed out and die
		for (auto It = DownedFor.begin(); It != DownedFor.end();)
		{
			AFortPawn* Downed = It->first;

			if (!Downed || !Downed->IsValidLowLevel() || !Downed->IsDBNO())
			{
				It = DownedFor.erase(It);
				continue;
			}

			It->second += Dt;

			if (It->second >= HuskBleedoutSec)
			{
				LOG_INFO(LogDev, "Zone: downed player bled out, forcing the death");
				static auto ForceKillFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.ForceKill");

				if (ForceKillFn)
				{
					struct { FGameplayTag DeathReason; AController* KillerController; AActor* KillerActor; } Params{ FGameplayTag{}, nullptr, nullptr };
					Downed->ProcessEvent(ForceKillFn, &Params);
				}

				It = DownedFor.erase(It);
				continue;
			}

			++It;
		}

		if (gEncounterWatch)
			WatchEncounter(Now);

		UpdateDefenderGoals(Now);

		// Our own completion only applies to the fallback husks: with the game's encounters the mission script ends its objective
		// itself when its kills are in (the husk list is also empty between waves, which says nothing)
		if (Husks.empty())
		{
			if (gFallbackUsed && gTutorialObjective && gEverHadHusk)
				CompleteTutorialObjective();
			return;
		}

		static auto VelocityOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "Velocity", false);
		static auto MovementModeOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "MovementMode", false);
		static auto MaxWalkSpeedOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "MaxWalkSpeed", false);
		static auto GroundFrictionOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "GroundFriction", false);
		static auto BrakingDecelOffset = FindOffsetStruct("/Script/Engine.CharacterMovementComponent", "BrakingDecelerationWalking", false);
		static auto K2SetRotationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_SetActorRotation");
		static bool bLoggedMissing = false;

		static auto IsDeadFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.IsDead");

		std::vector<AFortPawn*> AlivePlayers;
		CollectAlivePlayers(AlivePlayers);

		// status line every 10 s: how many husks are in each movement state, so a stall is visible in the log
		static auto LastStatusLog = Now;
		int ModeWalking = 0, ModeNone = 0, ModeFalling = 0, ModeOther = 0, MeleeCount = 0;

		for (int i = (int)Husks.size() - 1; i >= 0; --i)
		{
			auto Husk = Husks.at(i);

			if (!Husk || !Husk->IsValidLowLevel())
			{
				// drop the state too: its key is a raw pointer that the allocator may hand to a new pawn
				gAIDriven.erase(Husk);
				gGoals.erase(Husk);
				States.erase(Husk);
				Husks.erase(Husks.begin() + i);
				continue;
			}

			if (IsDeadFn)
			{
				bool bDead = false;
				Husk->ProcessEvent(IsDeadFn, &bDead);
				if (bDead)
				{
					{
						auto DeathPos = Husk->GetActorLocation();
						auto Near = FindNearestPlayerPawn(Husk, AlivePlayers);
						float Away = -1.f;
						if (Near) { auto P = Near->GetActorLocation(); Away = std::hypot(P.X - DeathPos.X, P.Y - DeathPos.Y); }
						LOG_INFO(LogDev, "Zone: husk died at ({:.0f},{:.0f},{:.0f}), nearest player {:.0f}uu away, body lingers {:.0f}s before cleanup", DeathPos.X, DeathPos.Y, DeathPos.Z, Away, HuskCorpseLifetimeSec);
					}
					gAIDriven.erase(Husk);
					gGoals.erase(Husk);
					States.erase(Husk);
					Corpses.push_back({ Husk, HuskCorpseLifetimeSec });
					Husks.erase(Husks.begin() + i);
					continue;
				}
			}

			// the game's behaviour tree runs this one: it only needs its goal, not our velocity writes and bite
			if (gAIDriven.count(Husk))
			{
				UpdateGoal(Husk, AlivePlayers, Now);
				++ModeWalking;

				// the tree animates and bites, but its MoveTo can find no path on parts of the fort (the navmesh covers little there), leaving the husk
				// standing out of reach. When it has not moved for a while and the player is not in reach, walk it toward the player ourselves; the tree
				// keeps doing the attacking.
				auto& Stall = States[Husk];
				auto Pos = Husk->GetActorLocation();
				auto Target = FindNearestPlayerPawn(Husk, AlivePlayers);
				static int AIMovementOffset = 0;
				if (AIMovementOffset == 0) AIMovementOffset = Husk->GetOffset("CharacterMovement");
				UObject* AIMovement = AIMovementOffset > 0 ? Husk->Get<UObject*>(AIMovementOffset) : nullptr;

				if (Target && AIMovement && VelocityOffset != -1 && MaxWalkSpeedOffset != -1)
				{
					auto TargetPos = Target->GetActorLocation();
					float Dx = TargetPos.X - Pos.X, Dy = TargetPos.Y - Pos.Y;
					float Dist = std::sqrt(Dx * Dx + Dy * Dy);
					float Moved = Stall.bHasLastPos ? std::hypot(Pos.X - Stall.LastPos.X, Pos.Y - Stall.LastPos.Y) : 100.f;
					Stall.LastPos = Pos;
					Stall.bHasLastPos = true;

					if (Dist > HuskMeleeRange && Dist < 20000.f && Moved < 2.f)
						Stall.StuckSeconds += Dt;
					else if (Moved >= 2.f && !Stall.bNudging)
						Stall.StuckSeconds = 0.f;

					// a husk with a full path is the game's to move, even while it stands (a tree waiting, a barrier it is meant to stop at)
					if (Stall.StuckSeconds >= 1.5f && !Stall.bNudging && HasFullNavPath(Husk, Target))
					{
						Stall.StuckSeconds = 0.f;
					}

					if (Stall.StuckSeconds >= 1.5f)
					{
						if (!Stall.bNudging)
						{
							ProbeHuskNavigation(Husk, Target);
							Stall.bNudging = true;
							LOG_INFO(LogDev, "Zone: AI-driven husk stood still {:.0f}uu from the player with NO navmesh path, walking it there ourselves", Dist);
						}

						uint8_t Mode = MovementModeOffset != -1 ? AIMovement->Get<uint8_t>(MovementModeOffset) : 255;
						if (Mode == 0) ForceWalking(AIMovement);

						if (Dist > HuskMeleeRange)
						{
							Dx /= Dist; Dy /= Dist;
							AIMovement->Get<float>(MaxWalkSpeedOffset) = 160.f;
							AIMovement->Get<FVector>(VelocityOffset) = FVector{ Dx * 160.f, Dy * 160.f, AIMovement->Get<FVector>(VelocityOffset).Z };

							if (K2SetRotationFn)
							{
								FRotator Rot; Rot.Pitch = 0; Rot.Yaw = std::atan2(Dy, Dx) * 180.f / 3.14159265f; Rot.Roll = 0;
								Husk->ProcessEvent(K2SetRotationFn, &Rot);
							}
						}
						else
						{
							Stall.StuckSeconds = 0.f;
							Stall.bNudging = false;
						}
					}
				}
				continue;
			}

			// CharacterMovement is a member of Character, not Pawn: an offset lookup against the Pawn
			// script struct fails (-1) and froze every husk in place; resolve it from the pawn class
			// like bots.h does
			static int CharacterMovementOffset = 0; // 0 = uninitialized, -1 = missing

			if (CharacterMovementOffset == 0)
				CharacterMovementOffset = Husk->GetOffset("CharacterMovement");

			UObject* Movement = CharacterMovementOffset > 0 ? Husk->Get<UObject*>(CharacterMovementOffset) : nullptr;
			auto& State = States[Husk];

			uint8_t MoveMode = (Movement && MovementModeOffset != -1) ? Movement->Get<uint8_t>(MovementModeOffset) : 255;

			if (MoveMode == 1) ++ModeWalking;
			else if (MoveMode == 0) ++ModeNone;
			else if (MoveMode == 3) ++ModeFalling;
			else ++ModeOther;

			if (!Movement || MovementModeOffset == -1 || MoveMode != 1)
			{
				// this once-per-husk line is the only trace of why a husk is frozen
				if (!State.bLoggedMovementSkip)
				{
					State.bLoggedMovementSkip = true;
					LOG_WARN(LogDev, "Zone: husk not walking: movement={} ({}) mode={} loc=({:.0f},{:.0f},{:.0f})",
						Movement != nullptr, Movement ? "component found" : "CHARACTERMOVEMENT MISSING", MoveMode,
						Husk->GetActorLocation().X, Husk->GetActorLocation().Y, Husk->GetActorLocation().Z);
				}

				// MOVE_None (0) is the spawn state the engine never leaves on its own: walk it
				if (Movement && MoveMode == 0)
				{
					ForceWalking(Movement);
				}
				// MOVE_Falling (3) that stops changing height is hung in mid-air: walk it
				else if (Movement && MoveMode == 3)
				{
					float Z = Husk->GetActorLocation().Z;

					if (std::fabs(Z - State.LastFallZ) < 1.f)
						State.FallStillSeconds += Dt;
					else
						State.FallStillSeconds = 0.f;

					State.LastFallZ = Z;

					if (State.FallStillSeconds >= HuskFallStillSec)
					{
						LOG_WARN(LogDev, "Zone: husk hung in falling at Z {:.0f}, forcing walking", Z);
						State.FallStillSeconds = 0.f;
						ForceWalking(Movement);
					}
				}

				continue;
			}

			State.FallStillSeconds = 0.f;

			if (VelocityOffset == -1 || MaxWalkSpeedOffset == -1)
			{
				if (!bLoggedMissing)
				{
					bLoggedMissing = true;
					LOG_ERROR(LogDev, "Zone: character movement offsets missing ({} {})", VelocityOffset, MaxWalkSpeedOffset);
				}

				continue;
			}

			auto HuskPos = Husk->GetActorLocation();
			auto PlayerPawn = FindNearestPlayerPawn(Husk, AlivePlayers);

			if (!PlayerPawn)
			{
				if (!State.bLoggedNoPlayer)
				{
					State.bLoggedNoPlayer = true;
					LOG_WARN(LogDev, "Zone: husk sees no player to walk at (player pawn class {})",
						GetPlayerPawnClass() ? GetPlayerPawnClass()->GetFullName() : "null");
				}

				continue;
			}

			{
				auto PlayerPos = PlayerPawn->GetActorLocation();
				float Dx = PlayerPos.X - HuskPos.X, Dy = PlayerPos.Y - HuskPos.Y;
				float DistSq = Dx * Dx + Dy * Dy;

				// STW husks shamble, not sprint: the walk pace, and a rotation that faces where it shuffles
				constexpr float ShuffleSpeed = 160.f;

				if (DistSq > HuskMeleeRange * HuskMeleeRange)
				{
					float Dist = std::sqrt(DistSq);
					Dx /= Dist;
					Dy /= Dist;

					State.MoveDirX = Dx;
					State.MoveDirY = Dy;

					// keep the brake off so the written velocity survives the frame (BotBrain's trick)
					if (GroundFrictionOffset != -1)
						Movement->Get<float>(GroundFrictionOffset) = 0;
					if (BrakingDecelOffset != -1)
						Movement->Get<float>(BrakingDecelOffset) = 0;

					// stuck detection: wanted to move but didn't actually cover ground
					if (State.bHasLastPos)
					{
						float MoveDx = HuskPos.X - State.LastPos.X, MoveDy = HuskPos.Y - State.LastPos.Y;
						float MoveDistSq = MoveDx * MoveDx + MoveDy * MoveDy;

						if (MoveDistSq < 4.f) // < 2uu per tick while fully shamble speed = wedged
							State.StuckSeconds += Dt;
						else
							State.StuckSeconds = 0.f;
					}

					State.LastPos = HuskPos;
					State.bHasLastPos = true;

					if (State.StuckSeconds >= StuckSecondsBeforeAttack)
					{
						// wedged against something: bite the wall until it's gone (or we're not stuck anymore)
						Movement->Get<FVector>(VelocityOffset) = FVector{ 0, 0, 0 };

						if (!State.TargetBuilding || !State.TargetBuilding->IsValidLowLevel())
						{
							State.TargetBuilding = FindNearestBlockingBuilding(Husk, State.MoveDirX, State.MoveDirY);
							if (State.TargetBuilding)
								LOG_INFO(LogDev, "Zone: husk is wedged, biting the building piece in front ({:.0f} health)", State.TargetBuilding->GetHealth());
						}

						if (State.TargetBuilding)
						{
							if (Now >= State.NextBuildingAttack)
							{
								State.NextBuildingAttack = Now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(HuskAttackIntervalSec));
								DamageBuilding(State.TargetBuilding, HuskBuildingDamage);
							}
						}
						else
						{
							State.StuckSeconds = 0.f; // nothing to bite: try walking again
						}
					}
					else
					{
						Movement->Get<float>(MaxWalkSpeedOffset) = ShuffleSpeed;
						Movement->Get<FVector>(VelocityOffset) = FVector{ Dx * ShuffleSpeed, Dy * ShuffleSpeed, Movement->Get<FVector>(VelocityOffset).Z };

						if (K2SetRotationFn)
						{
							FRotator Rot;
							Rot.Pitch = 0;
							Rot.Yaw = std::atan2(Dy, Dx) * 180.f / 3.14159265f;
							Rot.Roll = 0;
							Husk->ProcessEvent(K2SetRotationFn, &Rot);
						}
					}
				}
				else
				{
					// in melee range: planted in place, swing on a cadence
					++MeleeCount;
					State.StuckSeconds = 0.f;
					State.TargetBuilding = nullptr;
					Movement->Get<FVector>(VelocityOffset) = FVector{ 0, 0, 0 };

					if (Now >= State.NextPlayerAttack)
					{
						State.NextPlayerAttack = Now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(HuskAttackIntervalSec));
						DamagePlayerPawn(Cast<AFortPawn>(PlayerPawn), HuskMeleeDamage * ZoneDifficulty());
					}
				}
			}
		}

		if (std::chrono::duration_cast<std::chrono::seconds>(Now - LastStatusLog).count() >= 10)
		{
			LastStatusLog = Now;
			LOG_INFO(LogDev, "Zone: husk status: alive {} (walking {}, none {}, falling {}, other {}), in melee {}, corpses {}",
				Husks.size(), ModeWalking, ModeNone, ModeFalling, ModeOther, MeleeCount, Corpses.size());
		}

		if (gFallbackUsed && gTutorialObjective && Husks.empty())
			CompleteTutorialObjective();
	}
}
