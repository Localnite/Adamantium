#include "ZoneHusks.h"
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
#include <unordered_map>
#include <chrono>
#include <cmath>

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

	void DamageBuilding(ABuildingActor* Building, float Damage)
	{
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
			Building->ProcessEvent(ForceBuildingHealthFn, &New);
	}
}

namespace Zone
{
	static UObject* gTutorialObjective = nullptr;
	static bool gTutorialHusksSpawned = false;

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
		gTutorialObjective = nullptr;
		gTutorialHusksSpawned = false;
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

	void SpawnInitialWave(AFortPawn* PlayerPawn)
	{
		if (!Globals::bZoneMode || !PlayerPawn)
			return;

		auto PlayerPos = PlayerPawn->GetActorLocation();
		SpawnHusk(RingAround(PlayerPos, HuskTrickleRadius));
		SpawnHusk(RingAround(PlayerPos, HuskTrickleRadius));
		SpawnHusk(RingAround(PlayerPos, HuskTrickleRadius));
	}

	// example of code that should be universal, we need to implement whatever it used just for EVERY mission, but for now this works until we can confirm we can get through the full tutorial (unless epic devs REALLY are that stupid and in their infinite wisdom designed it this way)
	// my guess would be some sort of SpawnHusk thing that took params?
	void SpawnTutorialHusks(UObject* Objective)
	{
		if (!Globals::bZoneMode || gTutorialHusksSpawned)
			return;

		gTutorialHusksSpawned = true;
		gTutorialObjective = Objective;

		LOG_INFO(LogDev, "Zone: Spawning tutorial husks for objective {}", Objective ? Objective->GetFullName() : "NULL");

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
		// same vtable slot dllmain.cpp (Zone_Objective_StopObjective) swaps on the default object; the
		// 20:12 live run showed this call reaching that hook with status 2. Keep the two in step.
		constexpr int StopObjectiveVSlot = 0x660 / 8;
		auto StopObjective = (void (*)(UObject*, uint8_t, void*, void*))(Obj->VFTable[StopObjectiveVSlot]);
		StopObjective(Obj, 2 /* EFortObjectiveStatus::Succeeded */, nullptr, nullptr);
	}

	void HusksTick()
	{
		if (!Globals::bZoneMode || (Husks.empty() && Corpses.empty() && DownedFor.empty()))
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

		// objective completion runs even when the list just went empty (the tick that killed the
		// last husk continues below, but a post-completion tick must not re-enter movement)
		if (Husks.empty())
		{
			if (gTutorialObjective && gTutorialHusksSpawned)
				CompleteTutorialObjective();
			return;
		}

		// trickle: keep a fresh husk or two coming while the tutorial objective is live and the
		// party is smaller than the cap - after OBJ_HuskProblem ends, this stops spawning forever
		static int TrickleTimer = 0;
		if (gTutorialObjective && HuskCount() < HuskCap && ++TrickleTimer >= 150) // 150 * 0.1s = 15s
		{
			TrickleTimer = 0;

			auto PawnClass = GetPlayerPawnClass();

			if (PawnClass)
			{
				auto Players = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PawnClass);

				if (Players.Num() > 0 && Players.at(0))
					SpawnHusk(RingAround(Players.at(0)->GetActorLocation(), HuskTrickleRadius), Zone::RandRange(0, 99) < HuskArmoredPercent);

				Players.Free();
			}
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
					LOG_INFO(LogDev, "Zone: husk died, body lingers {:.0f}s before cleanup", HuskCorpseLifetimeSec);
					States.erase(Husk);
					Corpses.push_back({ Husk, HuskCorpseLifetimeSec });
					Husks.erase(Husks.begin() + i);
					continue;
				}
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

		if (gTutorialObjective && gTutorialHusksSpawned && Husks.empty())
			CompleteTutorialObjective();
	}
}
