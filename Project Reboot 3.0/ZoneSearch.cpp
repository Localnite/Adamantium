#include "ZoneSearch.h"
#include "ZoneMatchmaking.h"
#include "reboot.h"
#include "BuildingContainer.h"
#include "BuildingSMActor.h"
#include "Actor.h"
#include "FortPlayerController.h"
#include "FortPlayerState.h"
#include "AbilitySystemComponent.h"
#include "FortInventory.h"
#include "FortItemDefinition.h"
#include "FortItem.h"
#include "FortWorldItemDefinition.h"
#include "FortPickup.h"
#include "FortQuickBars.h"
#include "FortPlayerPawn.h"
#include "Pawn.h"
#include "globals.h"
#include "Stack.h"
#include "KismetStringLibrary.h"
#include <unordered_set>
#include <unordered_map>
#include "GameplayStatics.h"
#include "log.h"
#include <chrono>
#include <vector>
#include <thread>
#include <mutex>
#include <set>
#include <algorithm>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include "PlayerState.h"
#include "ZoneFinder.h"

// STW Zone searchables (chunk 1: loot for a search). The Athena loot picker does not fit STW containers (their
// tier groups live in the zone-runtime tables that 7.30 dedicated never loads), so the pools are hand-picked item
// definitions verified present in the installed STW paks (tools/s7_files.txt).
//
// Search loot spawns as pickups that pop out of the container and fly into the searcher, like real STW (the player does not
// walk over anything). The item lands in the world inventory when that animation completes, with the ShouldShowItemToast
// entry state set, which is what makes the client show the STW pickup feed ("x2 [Crafting Item] Twine" with the icon;
// weapons show just the icon + name). Direct grants (harvesting, the mission's own give functions) use the same toast.

namespace
{
	struct FPoolItem { const wchar_t* Path; int Min; int Max; int ChancePct; };

	// LoadObject is cheap after the first load of a package; every definition here is verified present in the 7.30 paks
	UFortItemDefinition* ZoneFindItem(const wchar_t* Path)
	{
		static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
		return LoadObject<UFortItemDefinition>(Path, ItemDefinitionClass);
	}

	std::string NarrowPath(const wchar_t* Path)
	{
		std::wstring Wide = Path;
		return std::string(Wide.begin(), Wide.end());
	}

	uint32_t ZoneSeed = 0x53742030;

	// xorshift is enough for loot picks
	int ZoneRandRange(int Min, int Max)
	{
		if (Min >= Max)
			return Min;

		ZoneSeed ^= ZoneSeed << 13;
		ZoneSeed ^= ZoneSeed >> 7;
		ZoneSeed ^= ZoneSeed << 17;

		return Min + (int)(ZoneSeed % (uint32_t)(Max - Min + 1));
	}
}

namespace Zone
{
	// public so the other Zone chunks (harvest, husks) share the pick rolls
	int RandRange(int Min, int Max)
	{
		return ZoneRandRange(Min, Max);
	}

	// Marks an entry (and its replicated copy) with the ShouldShowItemToast state, which the client HUD turns into the
	// STW pickup feed ("x<amount> [Crafting Item] <Name>"; weapons show just icon + name). An entry that already has the
	// state (a stack that grew, AddItem resets it to false) only has its value raised.
	static void MarkToast(AFortInventory* WorldInventory, UFortItem* ItemInstance)
	{
		static auto StateValueSize = FFortItemEntryStateValue::GetStructSize();

		if (!ItemInstance || !ItemInstance->GetItemEntry())
			return;

		auto Entry = ItemInstance->GetItemEntry();
		auto ReplicatedEntry = WorldInventory->FindReplicatedEntry(Entry->GetItemGuid());

		if (!ReplicatedEntry)
			return;

		auto SetOn = [&](TArray<FFortItemEntryStateValue>& States)
		{
			for (int i = 0; i < States.Num(); ++i)
			{
				auto& Existing = States.At(i, StateValueSize);

				if (Existing.GetStateType() == EFortItemEntryState::ShouldShowItemToast)
				{
					Existing.GetIntValue() = 1;
					return;
				}
			}

			// TArray::Add memcpy's StateValueSize bytes from the (empty) C++ object, so zero the new
			// slot before setting the fields, or the untouched ones would carry stack garbage
			States.Add(FFortItemEntryStateValue{}, StateValueSize);
			auto& NewState = States.At(States.Num() - 1, StateValueSize);
			memset(&NewState, 0, StateValueSize);
			NewState.GetStateType() = EFortItemEntryState::ShouldShowItemToast;
			NewState.GetIntValue() = 1;
		};

		SetOn(Entry->GetStateValues());
		SetOn(ReplicatedEntry->GetStateValues());
		WorldInventory->GetItemList().MarkItemDirty(ReplicatedEntry);
	}

	// weapons / traps / schematics go to the bars so the player can equip them; the engine's own AddItem quickbar path
	// only runs below engine 4.20, this server is 4.22
	static void EnsureOnQuickBar(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, UFortItem* ItemInstance)
	{
		if (!ItemInstance || !ItemInstance->GetItemEntry())
			return;

		auto Entry = ItemInstance->GetItemEntry();
		auto QuickBarsOffset = PlayerController->GetOffset("QuickBars", false);
		auto QuickBars = QuickBarsOffset != -1 ? PlayerController->Get<AActor*>(QuickBarsOffset) : nullptr;

		if (!QuickBars || ((AFortQuickBars*)QuickBars)->GetSlotIndex(Entry->GetItemGuid(), EFortQuickBars::Primary) != -1)
			return;

		static auto ServerAddItemInternalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");

		if (ServerAddItemInternalFn)
		{
			// IsPrimaryQuickbar is Battle Royale's rule, where gadgets are held like weapons. Here a gadget (the tutorial's air strike) is selected
			// like a trap, from the secondary bar's F keys, and equipping it hands out its deco tool (ServerExecuteInventoryItemHook)
			static auto GadgetClass = FindObject<UClass>(L"/Script/FortniteGame.FortGadgetItemDefinition");
			const bool bGadget = GadgetClass && Definition->IsA(GadgetClass);
			struct { FGuid Item; uint8_t InQuickBar; int Slot; } AddParams{ Entry->GetItemGuid(), (uint8_t)(!bGadget && IsPrimaryQuickbar(Definition) ? 0 : 1), -1 };
			if (bGadget)
			{
				LOG_INFO(LogDev, "Zone: gadget {} goes on the secondary quickbar", Definition->GetName());
			}
			QuickBars->ProcessEvent(ServerAddItemInternalFn, &AddParams);
		}
	}

	// Equipping a gadget (the tutorial's air strike on F1) makes the client run the item definition's gadget slot, which activates the ability spec whose
	// handle is stored in the item entry. The server code that grants the gadget's ability and fills that handle in is compiled out, so the handle stayed 0
	// and F1 did nothing. Grant the ability here and link it (the spec's source is the gadget definition, what the ability reads as its source item).
	static void LinkGadgetAbility(AFortPlayerController* PlayerController, AFortInventory* WorldInventory, UFortItemDefinition* Definition, UFortItem* ItemInstance)
	{
		static auto GadgetClass = FindObject<UClass>(L"/Script/FortniteGame.FortGadgetItemDefinition");
		if (!GadgetClass || !Definition->IsA(GadgetClass) || !ItemInstance || !ItemInstance->GetItemEntry())
			return;

		static auto GetAbilityFn = FindObject<UFunction>(L"/Script/FortniteGame.FortGadgetItemDefinition.GetGameplayAbility");
		UClass* AbilityClass = nullptr;

		if (GetAbilityFn)
		{
			struct { UClass* ReturnValue; } Params{};
			Definition->ProcessEvent(GetAbilityFn, &Params);
			AbilityClass = Params.ReturnValue;
		}

		auto State = Cast<AFortPlayerState>(PlayerController->GetPlayerState());
		auto AbilitySystem = State ? State->GetAbilitySystemComponent() : nullptr;

		if (!AbilityClass || !AbilitySystem)
		{
			LOG_WARN(LogDev, "Zone: gadget {} has no ability to link (ability class {}, ability system {})", Definition->GetName(), AbilityClass != nullptr, AbilitySystem != nullptr);
			return;
		}

		auto Handle = AbilitySystem->GiveAbilityEasy(AbilityClass, Definition, true);

		// GiveAbilityEasy hands back an empty handle when the ability is already there, and the game's own gadget apply grants it: find that spec instead
		if (Handle.Handle == 0)
		{
			LoopSpecs(AbilitySystem, [&](FGameplayAbilitySpec* Spec)
			{
				auto Ability = Spec ? Spec->GetAbility() : nullptr;
				if (Ability && Ability->ClassPrivate == AbilityClass)
					Handle = Spec->GetHandle();
			});
		}

		static int HandleOffset = FindOffsetStruct("/Script/FortniteGame.FortItemEntry", "GameplayAbilitySpecHandle");
		auto Entry = ItemInstance->GetItemEntry();
		*(FGameplayAbilitySpecHandle*)(__int64(Entry) + HandleOffset) = Handle;

		if (auto Replicated = WorldInventory->FindReplicatedEntry(Entry->GetItemGuid()))
		{
			*(FGameplayAbilitySpecHandle*)(__int64(Replicated) + HandleOffset) = Handle;
			WorldInventory->GetItemList().MarkItemDirty(Replicated);
		}

		LOG_INFO(LogDev, "Zone: gadget {} granted ability {} and linked spec handle {}", Definition->GetName(), AbilityClass->GetName(), Handle.Handle);
	}

	// Adds the item to the player's world inventory (toast + quickbar). Used by the search pools, the harvest chunk, the
	// pickup animation and the mission's own give functions.
	void GrantItemWithToast(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, int Count, bool bShowToast)
	{
		if (!PlayerController || !Definition || Count <= 0)
			return;

		// Schematics (FortSchematicItemDefinition) are NOT FortWorldItemDefinitions — they go through a
		// different inventory system (the schematic book), not the regular world inventory. Granting one
		// through AddItem crashes, so skip them here.
		if (!Definition->IsA(UFortWorldItemDefinition::StaticClass()))
		{
			LOG_WARN(LogDev, "Zone: grant skipped — {} is not a FortWorldItemDefinition (likely a schematic)", Definition->GetPathName());
			return;
		}

		auto WorldInventory = PlayerController->GetWorldInventory();
		if (!WorldInventory)
			return;

		bool bShouldUpdate = false;
		auto Added = WorldInventory->AddItem(Definition, &bShouldUpdate, Count);

		// a grant that lands on an existing stack creates no instance (it is in the second list), which is not a failure
		if (Added.first.empty() && Added.second.empty())
		{
			LOG_WARN(LogDev, "Zone: grant did not create or grow an item instance for {}", Definition->GetPathName());
			return;
		}

		for (auto ItemInstance : Added.first)
		{
			if (bShowToast)
				MarkToast(WorldInventory, ItemInstance);

			EnsureOnQuickBar(PlayerController, Definition, ItemInstance);
			LinkGadgetAbility(PlayerController, WorldInventory, Definition, ItemInstance);
		}

		for (auto ItemInstance : Added.second)
		{
			if (bShowToast)
				MarkToast(WorldInventory, ItemInstance);

			bShouldUpdate = true; // a grown stack still needs its replication pushed
		}

		if (bShouldUpdate)
			WorldInventory->Update();
	}

	void (*ServerSpawnDecoOriginal)(UObject*, FFrame&, void*) = nullptr;

	static std::vector<AActor*> PlacedTraps()
	{
		std::vector<AActor*> Out;
		static auto TrapClass = FindObject<UClass>(L"/Script/FortniteGame.BuildingTrap");
		if (!TrapClass) return Out;
		auto Traps = UGameplayStatics::GetAllActorsOfClass(GetWorld(), TrapClass);
		for (int i = 0; i < Traps.Num(); ++i)
			if (Traps.at(i)) Out.push_back(Traps.at(i));
		Traps.Free();
		return Out;
	}

	// The tutorial's trap step counts "PlayerBuilt" mission events whose params say building type 10 (trap) and whose building is a
	// BuildingTrapFloor (Mission_FarmsteadFort HandleMissionEvent_PlayerBuilt). For walls and floors the game raises that event itself
	// when the piece is placed; the deco tool's placement raises nothing on this server, so the trap was placed and never heard of.
	// ProcessEvent behind a structured-exception guard (no C++ objects here, so __try is allowed)
	static bool SafeProcessEvent(UObject* Object, UFunction* Function, void* Params)
	{
		__try { Object->ProcessEvent(Function, Params); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static void RaisePlayerBuilt(AFortPlayerController* Builder, AActor* Building, uint8_t BuildingType)
	{
		static auto ParamsObject = FindObject<UObject>(L"/Script/FortniteGame.Default__FortPlayerBuiltParams");
		static auto SetParamsFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerBuiltParams.SetParams");
		static auto RaiseFn = FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary.ExecuteMissionEventEx");
		static auto LibraryObject = FindObject<UObject>(L"/Script/FortniteGame.Default__FortMissionLibrary");

		if (!ParamsObject || !SetParamsFn || !RaiseFn || !LibraryObject)
		{
			LOG_WARN(LogDev, "Zone: cannot raise PlayerBuilt (params {}, SetParams {}, ExecuteMissionEventEx {}, library {})", ParamsObject != nullptr, SetParamsFn != nullptr, RaiseFn != nullptr, LibraryObject != nullptr);
			return;
		}

		// the params object is the class default object: the event is dispatched synchronously, so one shared instance is enough
		static const char* SetParamsFile = "/Script/FortniteGame.FortPlayerBuiltParams.SetParams";
		alignas(16) uint8_t SetBuffer[64] = {};
		*(UObject**)(SetBuffer + FindOffsetStruct(SetParamsFile, "ThisParam")) = ParamsObject;
		*(UObject**)(SetBuffer + FindOffsetStruct(SetParamsFile, "_Builder")) = Builder;
		*(uint8_t*)(SetBuffer + FindOffsetStruct(SetParamsFile, "_BuildingType")) = BuildingType;
		*(UObject**)(SetBuffer + FindOffsetStruct(SetParamsFile, "_Building")) = Building;
		if (!SafeProcessEvent(ParamsObject, SetParamsFn, SetBuffer))
		{
			LOG_WARN(LogDev, "Zone: FortPlayerBuiltParams.SetParams faulted");
			return;
		}

		// The two-name ExecuteMissionEventEx crashed inside the dispatch (null object, exe+0x18e7720); the game's own PlayerBuilt raise uses
		// one global name, so use the plain ExecuteMissionEvent with that name. Params stay null: its DO_NOT_USE flag is off.
		static auto PlainRaiseFn = FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary.ExecuteMissionEvent");
		if (!PlainRaiseFn)
			return;

		static const char* RaiseFile = "/Script/FortniteGame.FortMissionLibrary.ExecuteMissionEvent";
		alignas(16) uint8_t RaiseBuffer[512] = {};
		*(UObject**)(RaiseBuffer + FindOffsetStruct(RaiseFile, "WorldContextObject")) = (UObject*)GetWorld();
		*(FName*)(RaiseBuffer + FindOffsetStruct(RaiseFile, "EventType")) = UKismetStringLibrary::Conv_StringToName(L"PlayerBuilt");
		*(UObject**)(RaiseBuffer + FindOffsetStruct(RaiseFile, "EventContent")) = ParamsObject;
		*(UObject**)(RaiseBuffer + FindOffsetStruct(RaiseFile, "EventInstigator")) = Builder;
		if (!SafeProcessEvent(LibraryObject, PlainRaiseFn, RaiseBuffer))
			LOG_WARN(LogDev, "Zone: raising PlayerBuilt faulted, the trap will not be counted");
	}

	void ServerSpawnDecoHook(UObject* Tool, FFrame& Stack, void* Ret)
	{
		if (!Globals::bZoneMode || !Tool)
			return ServerSpawnDecoOriginal(Tool, Stack, Ret);

		static int ToolDefinitionOffset = Tool->GetOffset("ItemDefinition", false);
		auto Definition = ToolDefinitionOffset != -1 ? Tool->Get<UFortItemDefinition*>(ToolDefinitionOffset) : nullptr;
		auto Pawn = Cast<AFortPlayerPawn>(((AActor*)Tool)->GetOwner());
		auto Controller = Pawn ? Cast<AFortPlayerController>(Pawn->GetController()) : nullptr;

		int HeldBefore = Controller ? CountItemOnPlayer(Controller, Definition) : 0;
		auto TrapsBefore = PlacedTraps();

		ServerSpawnDecoOriginal(Tool, Stack, Ret);

		int HeldAfter = Controller ? CountItemOnPlayer(Controller, Definition) : 0;
		auto TrapsAfter = PlacedTraps();

		AActor* NewTrap = nullptr;
		{
			std::unordered_set<AActor*> Known(TrapsBefore.begin(), TrapsBefore.end());
			for (auto Trap : TrapsAfter)
				if (!Known.count(Trap)) NewTrap = Trap;
		}

		LOG_INFO(LogDev, "Zone: ServerSpawnDeco with {}: placed traps {} -> {}, held {} -> {}", Definition ? Definition->GetName() : std::string("NO ITEM DEFINITION ON THE TOOL"),
			TrapsBefore.size(), TrapsAfter.size(), HeldBefore, HeldAfter);

		if (!NewTrap)
			return;

		// the game placed a trap but kept the item: use it up
		if (Controller && Definition && HeldAfter >= HeldBefore && HeldBefore > 0)
		{
			RemoveItemFromPlayer(Controller, Definition, 1);
			LOG_INFO(LogDev, "Zone: the game did not use up the placed trap, removed 1 {}", Definition->GetName());
		}

		static int BuildingTypeOffset = NewTrap->GetOffset("BuildingType", false);
		int ActualType = BuildingTypeOffset != -1 ? (int)NewTrap->Get<uint8_t>(BuildingTypeOffset) : -1;
		static auto TrapFloorClass = FindObject<UClass>(L"/Script/FortniteGame.BuildingTrapFloor");
		LOG_INFO(LogDev, "Zone: new trap {} building type {} (BuildingTrapFloor: {})", NewTrap->GetFullName(), ActualType, TrapFloorClass && NewTrap->IsA(TrapFloorClass));

		// The mission library's event call faults in the engine (the Zone world has no mission manager behind it), and walls never needed it: placing one
		// ends in InitializeKismetSpawnedBuildingActor, whose native raises PlayerBuilt through the game state. The deco tool's spawn skips that call, so do
		// what the wall path does (FortPlayerController::ServerCreateBuildingActorHook): mark it player placed and initialize it.
		if (auto TrapBuilding = Cast<ABuildingSMActor>(NewTrap))
		{
			TrapBuilding->SetPlayerPlaced(true);

			static auto InitializeFn = FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.InitializeKismetSpawnedBuildingActor");
			struct { UObject* BuildingOwner; UObject* SpawningController; bool bUsePlayerBuildAnimations; UObject* ReplacedBuilding; } InitParams{ TrapBuilding, Controller, true, nullptr };

			if (InitializeFn && SafeProcessEvent(TrapBuilding, InitializeFn, &InitParams))
			{
				LOG_INFO(LogDev, "Zone: initialized the placed trap as a player build (the native raises PlayerBuilt)");
			}
			else
			{
				LOG_WARN(LogDev, "Zone: initializing the placed trap failed (function {})", InitializeFn != nullptr);
			}

			// A trap hurts whatever is not on its team. The STW player's team is FortPlayerState.PlayerTeam (a FortTeamInfo whose Team byte is the
			// EFortTeam); the deco spawn left the trap on another team, and it hurt the players. Put the trap on its placer's team.
			static int TrapTeamOffset = TrapBuilding->GetOffset("Team", false);
			auto State = Controller ? Controller->Get<UObject*>("PlayerState") : nullptr;
			static int PlayerTeamOffset = State ? State->GetOffset("PlayerTeam", false) : -1;
			auto TeamInfo = State && PlayerTeamOffset != -1 ? State->Get<UObject*>(PlayerTeamOffset) : nullptr;
			static int InfoTeamOffset = TeamInfo ? TeamInfo->GetOffset("Team", false) : -1;
			const int PlayerTeam = TeamInfo && InfoTeamOffset != -1 ? (int)TeamInfo->Get<uint8_t>(InfoTeamOffset) : -1;
			const int TrapTeam = TrapTeamOffset != -1 ? (int)TrapBuilding->Get<uint8_t>(TrapTeamOffset) : -1;

			if (PlayerTeam >= 0 && TrapTeam != PlayerTeam)
			{
				TrapBuilding->Get<uint8_t>(TrapTeamOffset) = (uint8_t)PlayerTeam;
				static auto OnRepTeam = FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.OnRep_Team");
				static auto OnInitTeam = FindObject<UFunction>(L"/Script/FortniteGame.BuildingTrap.OnInitTeam");
				if (OnRepTeam) SafeProcessEvent(TrapBuilding, OnRepTeam, nullptr);
				if (OnInitTeam) SafeProcessEvent(TrapBuilding, OnInitTeam, nullptr);
				TrapBuilding->ForceNetUpdate();
			}

			LOG_INFO(LogDev, "Zone: trap team was {}, placer's team {} ({}){}", TrapTeam, PlayerTeam, TeamInfo ? TeamInfo->GetName() : std::string("NO TEAM INFO"),
				PlayerTeam >= 0 && TrapTeam != PlayerTeam ? ", moved the trap to the placer's team" : "");
		}
	}

	int CountItemOnPlayer(AFortPlayerController* PlayerController, UFortItemDefinition* Definition)
	{
		auto WorldInventory = PlayerController ? PlayerController->GetWorldInventory() : nullptr;

		if (!WorldInventory || !Definition)
			return 0;

		int Total = 0;
		auto& Instances = WorldInventory->GetItemList().GetItemInstances();

		for (int i = 0; i < Instances.Num(); ++i)
		{
			auto Instance = Instances.at(i);
			auto Entry = Instance ? Instance->GetItemEntry() : nullptr;

			if (Entry && Entry->GetItemDefinition() == Definition)
				Total += Entry->GetCount();
		}

		return Total;
	}

	int RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, int Amount)
	{
		auto WorldInventory = PlayerController ? PlayerController->GetWorldInventory() : nullptr;

		if (!WorldInventory || !Definition || Amount <= 0)
			return 0;

		int Removed = 0;
		bool bUpdate = false;

		// the list changes under RemoveItem, so look the next stack up again each time
		while (Removed < Amount)
		{
			UFortItem* Stack = nullptr;
			auto& Instances = WorldInventory->GetItemList().GetItemInstances();

			for (int i = 0; i < Instances.Num(); ++i)
			{
				auto Instance = Instances.at(i);
				auto Entry = Instance ? Instance->GetItemEntry() : nullptr;

				if (Entry && Entry->GetItemDefinition() == Definition && Entry->GetCount() > 0)
				{
					Stack = Instance;
					break;
				}
			}

			if (!Stack)
				break;

			int Take = (std::min)(Amount - Removed, Stack->GetItemEntry()->GetCount());
			bool bShouldUpdate = false;
			WorldInventory->RemoveItem(Stack->GetItemEntry()->GetItemGuid(), &bShouldUpdate, Take, true);
			bUpdate |= bShouldUpdate;
			Removed += Take;
		}

		if (bUpdate)
			WorldInventory->Update();

		return Removed;
	}

	// ---- search loot as pickups that fly into the player ----
	// Real STW does not drop searched loot on the floor to walk over: the items pop out of the container and fly
	// into the searcher, and each is added to the inventory when its animation ends. The pickup's own animation is the
	// replicated PickupLocationData (target, fly time) that AFortPlayerPawn::ServerHandlePickupHook fills, and the end
	// of it is AFortPickup::CompletePickupAnimation (hooked in FortPickup.cpp), which grants the item with its toast.

	struct FPendingCollect
	{
		AFortPickup* Pickup;
		AFortPlayerController* Controller;
		std::chrono::steady_clock::time_point Due;
	};

	static std::vector<FPendingCollect> PendingCollects;

	static void SpawnLootPickup(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, int Count, const FVector& Location, float DelaySec)
	{
		auto ItemEntry = FFortItemEntry::MakeItemEntry(Definition, Count, 0, MAX_DURABILITY, 0);

		if (!ItemEntry)
		{
			LOG_WARN(LogDev, "Zone: could not make an item entry for {} x{}, granting directly", Definition->GetPathName(), Count);
			GrantItemWithToast(PlayerController, Definition, Count);
			return;
		}

		PickupCreateData CreateData;
		CreateData.bToss = true;
		CreateData.ItemEntry = ItemEntry;
		CreateData.SpawnLocation = Location;
		CreateData.SourceType = EFortPickupSourceTypeFlag::GetContainerValue();
		CreateData.bRandomRotation = true;
		CreateData.bShouldFreeItemEntryWhenDeconstructed = true;

		auto NewPickup = AFortPickup::SpawnPickup(CreateData);

		if (!NewPickup)
		{
			// no pickup means no animation, but the player still gets the loot
			LOG_WARN(LogDev, "Zone: search pickup for {} x{} did not spawn, granting directly", Definition->GetPathName(), Count);
			GrantItemWithToast(PlayerController, Definition, Count);
			return;
		}

		PendingCollects.push_back({ NewPickup, PlayerController, std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(DelaySec * 1000.f)) });
	}

	void ResetSearchState()
	{
		PendingCollects.clear();
	}

	// The Fort replication graph decides per class whether an actor is routed to clients at all (policy 0 = not routed). The class table is looked up and filled when
	// a class is first seen; the survivors' Blueprint class is first loaded long after the graph was set up (the table and its lookup are found by signature,
	// ZoneFinder.h, the same ones Zone_RouteQuickBars uses).
	static int* ReplicationPolicyOf(UClass* Class)
	{
		auto GetPolicy = (int* (*)(void*, UObject*))ZoneFinder::ClassRepNodePolicyLookup();
		const int PoliciesOffset = ZoneFinder::ClassRepNodePoliciesOffset();
		if (!Class || !GetPolicy || PoliciesOffset < 0)
			return nullptr;

		auto World = (UObject*)GetWorld();
		auto Driver = World ? World->Get<UObject*>("NetDriver") : nullptr;
		auto Graph = Driver ? Driver->Get<UObject*>("ReplicationDriver") : nullptr;

		if (!Graph)
			return nullptr;

		return GetPolicy((char*)Graph + PoliciesOffset, Class);
	}

	// Any AI pawn class whose Blueprint was first loaded after the replication graph was set up has no route in the graph's class table (policy 0 = not routed),
	// so its instances exist on the server and never reach the client: the fort's husk variants (HuskPawn_Bombshell_C, SmasherPawn_Onboarding_C). The survivors
	// and defenders come on route 1, which does not reach the client either (below). Give each AI pawn class the route its native base has, then add the pawns
	// of that class to the graph again.
	static void ScanAIPawnRouting()
	{
		static auto LastScan = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastScan).count() < 1000)
			return;

		LastScan = Now;

		static auto AIPawnBase = FindObject<UClass>(L"/Script/FortniteGame.FortAIPawn");
		if (!AIPawnBase || !GetWorld())
			return;

		static std::unordered_set<UClass*> Checked;

		// client announcements (Announce_RocketLaunchCine_C, the finale cinematic) are Blueprint classes first loaded when the mission spawns them: give a new
		// class with no route (0) the route of FortClientAnnouncement, then add its actors again
		static auto AnnouncementBase = FindObject<UClass>(L"/Script/FortniteGame.FortClientAnnouncement");
		static auto SetReplicatesFnA = FindObject<UFunction>(L"/Script/Engine.Actor.SetReplicates");
		if (AnnouncementBase)
		{
			auto Announcements = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AnnouncementBase);
			for (int i = 0; i < Announcements.Num(); ++i)
			{
				auto Actor = Announcements.at(i);
				auto Class = Actor ? Actor->ClassPrivate : nullptr;
				if (!Class || Checked.count(Class)) continue;
				Checked.insert(Class);

				int* Own = ReplicationPolicyOf(Class);
				int* Base = ReplicationPolicyOf(AnnouncementBase);
				LOG_INFO(LogDev, "Zone: replication policy of announcement {} is {} (FortClientAnnouncement {})", Class->GetName(), Own ? *Own : -1, Base ? *Base : -1);
				if (!Own || *Own != 0) continue;

				*Own = (Base && *Base != 0) ? *Base : 1;
				LOG_INFO(LogDev, "Zone: {} was not routed by the replication graph, now policy {}", Class->GetName(), *Own);
				for (int j = 0; j < Announcements.Num(); ++j)
				{
					auto Other = Announcements.at(j);
					if (!Other || Other->ClassPrivate != Class || !SetReplicatesFnA) continue;
					bool bOff = false, bOn = true;
					Other->ProcessEvent(SetReplicatesFnA, &bOff);
					Other->ProcessEvent(SetReplicatesFnA, &bOn);
				}
			}
			Announcements.Free();
		}

		auto Pawns = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AIPawnBase);
		std::unordered_map<UClass*, std::vector<AActor*>> NewClasses;

		for (int i = 0; i < Pawns.Num(); ++i)
		{
			auto Pawn = Pawns.at(i);
			auto Class = Pawn ? Pawn->ClassPrivate : nullptr;

			if (Class && !Checked.count(Class))
				NewClasses[Class].push_back(Pawn);
		}

		static auto SetReplicatesFn = FindObject<UFunction>(L"/Script/Engine.Actor.SetReplicates");

		for (auto& Entry : NewClasses)
		{
			Checked.insert(Entry.first);
			int* Own = ReplicationPolicyOf(Entry.first);
			int* Base = ReplicationPolicyOf(AIPawnBase);
			LOG_INFO(LogDev, "Zone: replication policy of {} is {} (FortAIPawn {}), {} pawns", Entry.first->GetName(), Own ? *Own : -1, Base ? *Base : -1, Entry.second.size());

			// policy 1 ("relevant to all connections") is what the survivors and the defenders (Defender_*) come with, and on this server it does not hold:
			// the client got the defenders once and dropped them all when the player walked ~1000 units away, and never got the bunker survivors standing
			// 170 units from the player. The husks, on FortAIPawn's spatialized route (3), replicate fine, so every AI pawn class goes on that route.
			if (!Own || !Base || *Base == 0 || *Own == *Base || (*Own != 0 && *Own != 1))
				continue;

			const int Was = *Own;
			*Own = *Base;
			LOG_INFO(LogDev, "Zone: {} was on replication route {}, now {} like FortAIPawn", Entry.first->GetName(), Was, *Own);

			for (auto Pawn : Entry.second)
			{
				if (!SetReplicatesFn) break;
				bool bOff = false, bOn = true;
				Pawn->ProcessEvent(SetReplicatesFn, &bOff);
				Pawn->ProcessEvent(SetReplicatesFn, &bOn);
			}
		}

		Pawns.Free();
	}

	// The tutorial's survivors (BP_ScriptedSurvivorParent spawns Survivor_C with a deferred spawn and sets AppearanceOverrideName) are heard and subtitled but not
	// seen. Say what the server has: whether the class is loaded, how many exist, where, and what mesh they carry, so the next run tells spawned-but-invisible
	// from never-spawned.
	static void ScanSurvivors()
	{
		static auto LastScan = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastScan).count() < 2000)
			return;

		LastScan = Now;

		static auto SurvivorClass = FindObject<UClass>(L"/Game/Characters/Survivors/Survivor.Survivor_C");
		static bool bLoggedClass = false;

		if (!SurvivorClass)
			SurvivorClass = FindObject<UClass>(L"/Game/Characters/Survivors/Survivor.Survivor_C");

		if (!bLoggedClass && SurvivorClass)
		{
			bLoggedClass = true;
			LOG_INFO(LogDev, "Zone: Survivor_C class is loaded");
		}

		if (!SurvivorClass || !GetWorld())
			return;

		static std::unordered_set<UObject*> Seen;
		static std::unordered_map<UObject*, int> Followed;
		auto Survivors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), SurvivorClass);

		for (int i = 0; i < Survivors.Num(); ++i)
		{
			auto Survivor = Survivors.at(i);

			if (!Survivor || Seen.count(Survivor))
				continue;

			Seen.insert(Survivor);
			Followed[Survivor] = 0;

			static int AppearanceOffset = Survivor->GetOffset("AppearanceOverrideName", false);
			std::string Appearance = AppearanceOffset != -1 ? Survivor->Get<FName>(AppearanceOffset).ToString() : std::string("(no such property)");

			auto Mesh = Survivor->Get<UObject*>("Mesh");
			auto SkeletalMesh = Mesh ? Mesh->Get<UObject*>("SkeletalMesh") : nullptr;
			static int RemoteRoleOffset = Survivor->GetOffset("RemoteRole", false);
			auto Location = Survivor->GetActorLocation();

			auto Bit = [](UObject* Object, const char* Name) -> int
			{
				int Offset = Object->GetOffset(Name, false);
				void* Prop = Object->GetProperty(Name, false);
				return (Offset == -1 || !Prop) ? -1 : (int)Object->ReadBitfieldValue(Offset, GetFieldMask(Prop));
			};

			LOG_INFO(LogDev, "Zone: survivor {} at ({:.0f},{:.0f},{:.0f}), appearance '{}', mesh {}, remote role {}, controller {}", Survivor->GetName(),
				Location.X, Location.Y, Location.Z, Appearance, SkeletalMesh ? SkeletalMesh->GetName() : std::string("NONE"),
				RemoteRoleOffset != -1 ? (int)Survivor->Get<uint8_t>(RemoteRoleOffset) : -1, Survivor->Get<UObject*>("Controller") ? "yes" : "no");
			LOG_INFO(LogDev, "Zone:   survivor replication: bReplicates {}, bAlwaysRelevantNetUpdate {}, bHidden {}, mesh bVisible {} bHiddenInGame {}, dormancy {}",
				Bit(Survivor, "bReplicates"), Bit(Survivor, "bAlwaysRelevant"), Bit(Survivor, "bHidden"), Mesh ? Bit(Mesh, "bVisible") : -1, Mesh ? Bit(Mesh, "bHiddenInGame") : -1,
				Survivor->GetOffset("NetDormancy", false) != -1 ? (int)Survivor->Get<uint8_t>(Survivor->GetOffset("NetDormancy", false)) : -1);

			// The server has the right mesh but the client draws nothing: the client builds the look from the replicated AppearanceOverrideEntryIndex (OnRep), not
			// from the mesh, so say what index the server holds. The held weapon tells whether a defender could fight at all.
			{
				int IndexOffset = Survivor->GetOffset("AppearanceOverrideEntryIndex", false);
				int EntriesOffset = Survivor->GetOffset("AppearanceOverrideEntries", false);
				int WeaponOffset = Survivor->GetOffset("CurrentWeapon", false);
				auto Weapon = WeaponOffset != -1 ? Survivor->Get<UObject*>(WeaponOffset) : nullptr;
				LOG_INFO(LogDev, "Zone:   survivor look: bUseAppearanceOverride {}, AppearanceOverrideEntryIndex {}, entries {}, class {}, weapon {}",
					Bit(Survivor, "bUseAppearanceOverride"), IndexOffset != -1 ? Survivor->Get<int>(IndexOffset) : -999,
					EntriesOffset != -1 ? Survivor->Get<TArray<uint8_t>>(EntriesOffset).Num() : -1,
					Survivor->ClassPrivate ? Survivor->ClassPrivate->GetFullName() : std::string("?"), Weapon ? Weapon->GetName() : std::string("none"));
			}

			// their AI: does the controller run a behaviour tree, on which blackboard
			if (auto Controller = ((APawn*)Survivor)->GetController())
			{
				auto Brain = Controller->GetOffset("BrainComponent", false) != -1 ? Controller->Get<UObject*>("BrainComponent") : nullptr;
				auto Board = Controller->GetOffset("Blackboard", false) != -1 ? Controller->Get<UObject*>("Blackboard") : nullptr;
				auto Asset = Board && Board->GetOffset("BlackboardAsset", false) != -1 ? Board->Get<UObject*>("BlackboardAsset") : nullptr;
				bool bRunning = false;
				static auto IsRunningFn = FindObject<UFunction>(L"/Script/AIModule.BrainComponent.IsRunning");
				if (Brain && IsRunningFn) { struct { bool R; } P{}; SafeProcessEvent(Brain, IsRunningFn, &P); bRunning = P.R; }
				LOG_INFO(LogDev, "Zone:   survivor AI: controller {} ({}), brain {} running {}, blackboard {}", Controller->GetName(),
					Controller->ClassPrivate ? Controller->ClassPrivate->GetName() : std::string("?"), Brain ? Brain->GetName() : std::string("NONE"), bRunning,
					Asset ? Asset->GetName() : std::string("none"));
			}
		}

		Survivors.Free();

		// Where the game meant them to be: each BP_ScriptedSurvivorParent spawns its survivor at its own SpawnTransform arrow. Report the parents once, the player
		// whenever a survivor is being followed, and the survivors' positions for a while after they spawn (a pawn that spawns where the ground has not loaded falls
		// out of the world and is "heard but not there").
		static auto ParentClass = FindObject<UClass>(L"/Game/Missions/Primary/OB_FarmsteadFort/ScriptedSurvivors/BP_ScriptedSurvivorParent.BP_ScriptedSurvivorParent_C");
		static std::unordered_set<UObject*> SeenParents;

		if (!ParentClass)
			ParentClass = FindObject<UClass>(L"/Game/Missions/Primary/OB_FarmsteadFort/ScriptedSurvivors/BP_ScriptedSurvivorParent.BP_ScriptedSurvivorParent_C");

		if (ParentClass)
		{
			auto Parents = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ParentClass);

			for (int i = 0; i < Parents.Num(); ++i)
			{
				auto Parent = Parents.at(i);

				if (!Parent || SeenParents.count(Parent))
					continue;

				SeenParents.insert(Parent);
				auto ParentLocation = Parent->GetActorLocation();
				auto SpawnArrow = Parent->Get<UObject*>("SpawnTransform");
				FVector SpawnAt = ParentLocation;

				static auto GetComponentLocationFn = FindObject<UFunction>(L"/Script/Engine.SceneComponent.K2_GetComponentLocation");
				if (SpawnArrow && GetComponentLocationFn)
					SpawnArrow->ProcessEvent(GetComponentLocationFn, &SpawnAt);

				LOG_INFO(LogDev, "Zone: survivor spawner {} at ({:.0f},{:.0f},{:.0f}), its spawn arrow at ({:.0f},{:.0f},{:.0f})", Parent->GetName(),
					ParentLocation.X, ParentLocation.Y, ParentLocation.Z, SpawnAt.X, SpawnAt.Y, SpawnAt.Z);
			}

			Parents.Free();
		}

		static auto LastFollow = std::chrono::steady_clock::now();

		if (!Followed.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastFollow).count() >= 5000)
		{
			LastFollow = Now;

			static auto PlayerPawnClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerPawn");
			if (PlayerPawnClass)
			{
				auto Players = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerPawnClass);
				for (int i = 0; i < Players.Num() && i < 2; ++i)
				{
					auto PlayerLocation = Players.at(i)->GetActorLocation();
					LOG_INFO(LogDev, "Zone: player pawn at ({:.0f},{:.0f},{:.0f})", PlayerLocation.X, PlayerLocation.Y, PlayerLocation.Z);
				}
				Players.Free();
			}

			for (auto It = Followed.begin(); It != Followed.end();)
			{
				auto Pawn = It->first;

				if (!Pawn || !Pawn->IsValidLowLevel() || ++It->second > 8)
				{
					It = Followed.erase(It);
					continue;
				}

				auto Position = ((AActor*)Pawn)->GetActorLocation();
				auto Movement = Pawn->Get<UObject*>("CharacterMovement");
				int ModeOffset = Movement ? Movement->GetOffset("MovementMode", false) : -1;
				LOG_INFO(LogDev, "Zone:   following survivor {}: ({:.0f},{:.0f},{:.0f}), movement mode {}", Pawn->GetName(), Position.X, Position.Y, Position.Z,
					ModeOffset != -1 ? (int)Movement->Get<uint8_t>(ModeOffset) : -1);
				++It;
			}
		}
	}

	// The mission's wall in front of the rocket launcher (OB_SpecialWallEdit_C) is an ordinary player-buildable wall with the edit pattern the objective wants, but the
	// client only starts an edit on a piece that is flagged player placed (the replicated bPlayerPlaced), and a level-placed wall is not. No edit request ever reached the
	// server for it. Flag it, as the placement of a wall does.
	static void ScanEditableWalls()
	{
		static auto LastScan = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(Now - LastScan).count() < 2000)
			return;

		LastScan = Now;

		static auto WallClass = FindObject<UClass>(L"/Game/Missions/Primary/OB_FarmsteadFort/Props/OB_SpecialWallEdit.OB_SpecialWallEdit_C");

		if (!WallClass)
			WallClass = FindObject<UClass>(L"/Game/Missions/Primary/OB_FarmsteadFort/Props/OB_SpecialWallEdit.OB_SpecialWallEdit_C");

		if (!WallClass || !GetWorld())
			return;

		auto Walls = UGameplayStatics::GetAllActorsOfClass(GetWorld(), WallClass);

		for (int i = 0; i < Walls.Num(); ++i)
		{
			auto Wall = Cast<ABuildingSMActor>(Walls.at(i));

			if (!Wall || Wall->IsPlayerPlaced())
				continue;

			Wall->SetPlayerPlaced(true);
			Wall->ForceNetUpdate();
			LOG_INFO(LogDev, "Zone: flagged the mission's edit wall {} as player placed so the client lets it be edited", Wall->GetName());
		}

		// What the client needs before it will even ask the server to edit a piece (Limbo's analysis of the same exe family): an EditModeSupport made from
		// EditModePatternData, bIsPlayerBuildable, nobody editing. A level-placed piece may never have had its edit support built; log it, and initialize the
		// wall as a player build once if it is missing (the same call a placed wall goes through).
		static std::unordered_set<UObject*> WallsInitialized;
		for (int i = 0; i < Walls.Num(); ++i)
		{
			auto Wall = Cast<ABuildingSMActor>(Walls.at(i));
			if (!Wall || WallsInitialized.count(Wall)) continue;

			int SupportOffset = Wall->GetOffset("EditModeSupport", false);
			auto Support = SupportOffset != -1 ? Wall->Get<UObject*>(SupportOffset) : nullptr;
			int PatternOffset = Wall->GetOffset("EditModePatternData", false);
			auto Pattern = PatternOffset != -1 ? Wall->Get<UObject*>(PatternOffset) : nullptr;
			LOG_INFO(LogDev, "Zone: edit wall {}: EditModeSupport {}, EditModePatternData {}", Wall->GetName(), Support ? Support->GetName() : std::string("NONE"), Pattern ? Pattern->GetName() : std::string("NONE"));

			WallsInitialized.insert(Wall);

			// ABuildingSMActor::CanBePlayerEdited (read on the Xbox build) refuses a piece that has bDestroyed set, and one whose team is neither 103 (any
			// player may edit it) nor the editor's own team. The level's wall has team 0. Clear the flag if it is set, and give the wall team 103.
			{
				static int DestroyedOffset = Wall->GetOffset("bDestroyed", false);
				static uint8_t DestroyedMask = DestroyedOffset != -1 ? GetFieldMask(Wall->GetProperty("bDestroyed")) : 0;
				const bool bDestroyed = DestroyedOffset != -1 && Wall->ReadBitfieldValue(DestroyedOffset, DestroyedMask);
				LOG_INFO(LogDev, "Zone: edit wall bDestroyed = {}", bDestroyed);

				if (bDestroyed)
				{
					Wall->SetBitfieldValue(DestroyedOffset, DestroyedMask, false);
					LOG_INFO(LogDev, "Zone: cleared bDestroyed on the edit wall (the client refuses to edit a piece with it set)");
				}

				Wall->SetTeam(103);
				Wall->ForceNetUpdate();
				LOG_INFO(LogDev, "Zone: edit wall {} team set to 103 so any player may edit it", Wall->GetName());
			}

			if (Support) continue;

			static auto InitFn = FindObject<UFunction>(L"/Script/FortniteGame.BuildingActor.InitializeKismetSpawnedBuildingActor");
			static auto PlayerControllerClass0 = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
			UObject* Controller = nullptr;
			if (PlayerControllerClass0)
			{
				auto Cs = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass0);
				Controller = Cs.Num() > 0 ? Cs.at(0) : nullptr;
				Cs.Free();
			}

			struct { UObject* BuildingOwner; UObject* SpawningController; bool bUsePlayerBuildAnimations; UObject* ReplacedBuilding; } InitParams{ Wall, Controller, false, nullptr };
			bool bOk = InitFn && SafeProcessEvent(Wall, InitFn, &InitParams);
			auto After = SupportOffset != -1 ? Wall->Get<UObject*>(SupportOffset) : nullptr;
			LOG_INFO(LogDev, "Zone: initialized edit wall {} as a player build ({}), EditModeSupport now {}", Wall->GetName(), bOk ? "ok" : "failed", After ? After->GetName() : std::string("NONE"));
		}

		// The client then still would not edit it: a wall the player places carries the player's team (103 on this server) while the level's wall has team 0, and
		// the edit check compares the two. Give the wall the team of the first player.
		static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
		int PlayerTeam = -1;

		if (PlayerControllerClass)
		{
			auto Controllers = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass);
			for (int i = 0; i < Controllers.Num() && PlayerTeam == -1; ++i)
			{
				auto State = Controllers.at(i) ? Controllers.at(i)->Get<UObject*>("PlayerState") : nullptr;
				int TeamOffset = State ? State->GetOffset("TeamIndex", false) : -1;
				if (TeamOffset != -1) PlayerTeam = (int)State->Get<uint8_t>(TeamOffset);
			}
			Controllers.Free();
		}

		if (PlayerTeam > 0)
		{
			for (int i = 0; i < Walls.Num(); ++i)
			{
				auto Wall = Cast<ABuildingSMActor>(Walls.at(i));
				static int WallTeamOffset = Wall ? Wall->GetOffset("Team", false) : -1;

				if (!Wall || WallTeamOffset == -1 || (int)Wall->Get<uint8_t>(WallTeamOffset) == PlayerTeam)
					continue;

				Wall->SetTeam((unsigned char)PlayerTeam);
				Wall->ForceNetUpdate();
				LOG_INFO(LogDev, "Zone: gave the mission's edit wall {} the player's team {}", Wall->GetName(), PlayerTeam);
			}
		}

		Walls.Free();
	}

	// ---- end of the zone (see ZoneSearch.h): back to the main menu a few seconds after the win
	namespace
	{
		enum class EEndStage { Idle, Waiting, Done };
		EEndStage gEndStage = EEndStage::Idle;
		std::chrono::steady_clock::time_point gEndAt{};
		// The rocket launch cinematic has already played when the mission succeeds (its announcement comes ~25 s before), so there is nothing to
		// wait for: the results layer and its 30 s countdown only kept the player a minute from the satellite movie, which the menu plays (2026-11-07).
		constexpr int LeaveDelaySeconds = 3;
		constexpr uint8_t GameplayStateLeavingZone = 4; // EFortGameplayState: NormalGameplay, WaitingToStart, EndOfZone, EnteringZone, LeavingZone
	}

	// A real Zone server credits the players' quests on the MCP when the mission is won; the frontend's HBOnboardingFlow then sees the
	// HomebaseOnboarding quest's "complete zone" objective done and plays the satellite movie and the homebase naming. Our backend
	// (LawinServerV2, port 5000) does that from this report: the names of the players in the zone (their backend usernames). Sent on a
	// thread of its own so a slow or absent backend never holds the game thread.
	static void ReportZoneCompleteToBackend()
	{
		static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
		auto Controllers = PlayerControllerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass) : TArray<AActor*>();
		std::string Body = "{\"usernames\":[";
		int Count = 0;

		for (int i = 0; i < Controllers.Num(); ++i)
		{
			auto Controller = Controllers.at(i);
			auto PlayerState = Controller ? Controller->Get<UObject*>("PlayerState") : nullptr;
			if (!PlayerState) continue;

			// FString::ToString on an empty string reads a null buffer
			auto Name = ((APlayerState*)PlayerState)->GetPlayerName();
			if (!Name.Data.Data || Name.Data.Num() <= 1) continue;

			std::string Text;
			for (char c : Name.ToString()) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) Text += c;
			Body += (Count++ ? ",\"" : "\"") + Text + "\"";
		}

		Controllers.Free();
		Body += "]}";

		if (Count == 0)
		{
			LOG_WARN(LogDev, "Zone: no player names to report the completed zone for");
			return;
		}

		LOG_INFO(LogDev, "Zone: reporting the completed zone to the backend: {}", Body);

		std::thread([Body]()
		{
			int Status = 0;
			if (HINTERNET Session = WinHttpOpen(L"RebootZone/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
			{
				WinHttpSetTimeouts(Session, 2000, 2000, 5000, 5000);
				if (HINTERNET Connection = WinHttpConnect(Session, L"127.0.0.1", 5000, 0))
				{
					if (HINTERNET Request = WinHttpOpenRequest(Connection, L"POST", L"/fortnite/api/game/v2/stwserver/zone-complete", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0))
					{
						if (WinHttpSendRequest(Request, L"Content-Type: application/json\r\n", (DWORD)-1, (LPVOID)Body.data(), (DWORD)Body.size(), (DWORD)Body.size(), 0)
							&& WinHttpReceiveResponse(Request, nullptr))
						{
							DWORD Code = 0, Size = sizeof(Code);
							if (WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX))
								Status = (int)Code;
						}
						WinHttpCloseHandle(Request);
					}
					WinHttpCloseHandle(Connection);
				}
				WinHttpCloseHandle(Session);
			}

			if (Status == 200)
			{
				LOG_INFO(LogDev, "Zone: the backend recorded the completed zone");
			}
			else
			{
				LOG_WARN(LogDev, "Zone: reporting the completed zone to the backend failed (HTTP status {})", Status);
			}
		}).detach();
	}

	// ---- the backpack (see ZoneSearch.h): the player's world inventory, kept in the backend's theater0 between missions
	namespace
	{
		// One request to the backend (LawinServerV2, 127.0.0.1:5000, like the zone report); the HTTP status, the body in Response. Blocking: call it on a thread of its own.
		int BackendRequest(const wchar_t* Method, const std::wstring& Path, const std::string& Body, std::string& Response)
		{
			int Status = 0;
			if (HINTERNET Session = WinHttpOpen(L"RebootZone/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
			{
				WinHttpSetTimeouts(Session, 2000, 2000, 5000, 5000);
				if (HINTERNET Connection = WinHttpConnect(Session, L"127.0.0.1", 5000, 0))
				{
					if (HINTERNET Request = WinHttpOpenRequest(Connection, Method, Path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0))
					{
						const wchar_t* Headers = Body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
						if (WinHttpSendRequest(Request, Headers, Body.empty() ? 0 : (DWORD)-1, Body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)Body.data(), (DWORD)Body.size(), (DWORD)Body.size(), 0)
							&& WinHttpReceiveResponse(Request, nullptr))
						{
							DWORD Code = 0, Size = sizeof(Code);
							if (WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX))
								Status = (int)Code;

							DWORD Available = 0;
							while (WinHttpQueryDataAvailable(Request, &Available) && Available > 0)
							{
								std::string Chunk(Available, '\0');
								DWORD Read = 0;
								if (!WinHttpReadData(Request, Chunk.data(), Available, &Read) || Read == 0) break;
								Response.append(Chunk.data(), Read);
							}
						}
						WinHttpCloseHandle(Request);
					}
					WinHttpCloseHandle(Connection);
				}
				WinHttpCloseHandle(Session);
			}
			return Status;
		}

		std::wstring BackpackPath(const std::string& Username)
		{
			std::wstring Path = L"/fortnite/api/game/v2/stwserver/backpack/";
			const char* Hex = "0123456789ABCDEF";
			for (unsigned char c : Username)
			{
				if (isalnum(c) || c == '-' || c == '_' || c == '.') Path += (wchar_t)c;
				else { Path += L'%'; Path += (wchar_t)Hex[c >> 4]; Path += (wchar_t)Hex[c & 15]; }
			}
			return Path;
		}

		std::string PlayerNameOf(UObject* Controller)
		{
			auto PlayerState = Controller ? Controller->Get<UObject*>("PlayerState") : nullptr;
			if (!PlayerState) return "";
			auto Name = ((APlayerState*)PlayerState)->GetPlayerName();
			if (!Name.Data.Data || Name.Data.Num() <= 1) return ""; // FString::ToString on an empty string reads a null buffer
			return Name.ToString();
		}

		struct FBackpackItem { std::string Path; int Count = 1; };
		struct FBackpackLoad { std::string Username; std::vector<FBackpackItem> Items; };

		std::mutex gBackpackMutex;
		std::vector<FBackpackLoad> gBackpackLoaded;        // read from the backend, waiting for the game thread
		std::set<std::string> gBackpackRequested;          // asked for once per player and server
		std::set<std::string> gBackpackReady;              // given: only these are saved (a save before the load would wipe the backpack)
		std::chrono::steady_clock::time_point gNextBackpackSave{};
		constexpr int BackpackSaveSeconds = 60;

		// What is the backpack in the player's inventory, as the backend's theater0 type ("" = not part of it): the building pieces and the
		// edit tool are given to everyone, the pickaxe and the gadgets come from the hero loadout.
		std::string BackpackType(UFortItemDefinition* Definition)
		{
			static auto BuildingClass = FindObject<UClass>(L"/Script/FortniteGame.FortBuildingItemDefinition");
			static auto EditToolClass = FindObject<UClass>(L"/Script/FortniteGame.FortEditToolItemDefinition");
			static auto RangedClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeaponRangedItemDefinition");
			static auto MeleeClass = FindObject<UClass>(L"/Script/FortniteGame.FortWeaponMeleeItemDefinition");
			static auto TrapClass = FindObject<UClass>(L"/Script/FortniteGame.FortTrapItemDefinition");
			static auto AmmoClass = FindObject<UClass>(L"/Script/FortniteGame.FortAmmoItemDefinition");
			static auto ResourceClass = FindObject<UClass>(L"/Script/FortniteGame.FortResourceItemDefinition");
			static auto IngredientClass = FindObject<UClass>(L"/Script/FortniteGame.FortIngredientItemDefinition");

			if ((BuildingClass && Definition->IsA(BuildingClass)) || (EditToolClass && Definition->IsA(EditToolClass))) return "";
			if ((RangedClass && Definition->IsA(RangedClass)) || (MeleeClass && Definition->IsA(MeleeClass)))
				return Definition->GetName().rfind("WID_Harvest", 0) == 0 ? "" : "Weapon";
			if (TrapClass && Definition->IsA(TrapClass)) return "Trap";
			if (AmmoClass && Definition->IsA(AmmoClass)) return "Ammo";
			if (ResourceClass && Definition->IsA(ResourceClass)) return "WorldItem";
			if (IngredientClass && Definition->IsA(IngredientClass)) return "Ingredient";
			return "";
		}

		// the player's backpack as the backend's JSON, or "" when the inventory is not there
		std::string BackpackJson(AFortPlayerController* Controller)
		{
			auto Inventory = Controller->GetWorldInventory();
			if (!Inventory) return "";

			auto Clean = [](const std::string& Text) { std::string Out; for (char c : Text) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) Out += c; return Out; };
			std::string Json = "{\"items\":[";
			int Count = 0;
			auto& Instances = Inventory->GetItemList().GetItemInstances();
			for (int i = 0; i < Instances.Num(); ++i)
			{
				auto Instance = Instances.at(i);
				auto Entry = Instance ? Instance->GetItemEntry() : nullptr;
				auto Definition = Entry ? Entry->GetItemDefinition() : nullptr;
				if (!Definition) continue;

				const std::string Type = BackpackType(Definition);
				if (Type.empty()) continue;

				std::string Name = Definition->GetName();
				std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char c) { return (char)tolower(c); });
				Json += std::string(Count++ ? "," : "") + "{\"templateId\":\"" + Type + ":" + Clean(Name) + "\",\"path\":\"" + Clean(Definition->GetPathName()) + "\",\"count\":"
					+ std::to_string(Entry->GetCount()) + ",\"loadedAmmo\":" + std::to_string(Entry->GetLoadedAmmo()) + ",\"level\":" + std::to_string(Entry->GetLevel())
					+ ",\"durability\":" + std::to_string(Entry->GetDurability()) + "}";
			}
			return Json + "]}";
		}
	}

	void LoadBackpack(AFortPlayerController* Controller)
	{
		const std::string Username = PlayerNameOf(Controller);
		if (Username.empty()) { LOG_WARN(LogDev, "Zone: backpack: no player name, not loaded"); return; }

		{
			std::lock_guard Lock(gBackpackMutex);
			if (!gBackpackRequested.insert(Username).second) return; // a respawn, the items are in the inventory already
		}

		std::thread([Username]()
		{
			std::string Response;
			const int Status = BackendRequest(L"GET", BackpackPath(Username), "", Response);

			FBackpackLoad Load{ Username, {} };
			size_t At = 0;
			while (Status == 200 && At < Response.size())
			{
				const size_t End = Response.find('\n', At);
				const std::string Line = Response.substr(At, End == std::string::npos ? std::string::npos : End - At);
				At = End == std::string::npos ? Response.size() : End + 1;

				const size_t Tab = Line.find('\t');
				if (Tab == std::string::npos || Line.empty() || Line[0] != '/') continue;
				FBackpackItem Item{ Line.substr(0, Tab), (std::max)(1, atoi(Line.c_str() + Tab + 1)) };
				Load.Items.push_back(Item);
			}

			if (Status != 200)
			{
				LOG_WARN(LogDev, "Zone: backpack of {} could not be read (HTTP status {}), it is not saved this time either", Username, Status);
				return;
			}

			std::lock_guard Lock(gBackpackMutex);
			gBackpackLoaded.push_back(std::move(Load));
		}).detach();
	}

	// Saves the backpack of every player whose backpack was given (on a thread). bNow: the zone is ending, do not wait for the minute.
	static void SaveBackpacks(bool bNow)
	{
		const auto Now = std::chrono::steady_clock::now();
		if (!bNow && Now < gNextBackpackSave) return;
		gNextBackpackSave = Now + std::chrono::seconds(BackpackSaveSeconds);

		static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
		auto Controllers = PlayerControllerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass) : TArray<AActor*>();
		for (int i = 0; i < Controllers.Num(); ++i)
		{
			auto Controller = (AFortPlayerController*)Controllers.at(i);
			const std::string Username = PlayerNameOf(Controller);
			{
				std::lock_guard Lock(gBackpackMutex);
				if (Username.empty() || !gBackpackReady.count(Username)) continue;
			}

			const std::string Json = BackpackJson(Controller);
			if (Json.empty()) continue;

			std::thread([Username, Json]()
			{
				std::string Response;
				const int Status = BackendRequest(L"POST", BackpackPath(Username), Json, Response);
				if (Status != 200) LOG_WARN(LogDev, "Zone: saving the backpack of {} failed (HTTP status {})", Username, Status);
			}).detach();
		}
		Controllers.Free();
	}

	// On the game thread: gives the backpacks read from the backend to their players.
	static void BackpackTick()
	{
		std::vector<FBackpackLoad> Loaded;
		{
			std::lock_guard Lock(gBackpackMutex);
			Loaded.swap(gBackpackLoaded);
		}

		static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
		static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
		for (auto& Load : Loaded)
		{
			AFortPlayerController* Controller = nullptr;
			auto Controllers = PlayerControllerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass) : TArray<AActor*>();
			for (int i = 0; i < Controllers.Num() && !Controller; ++i)
				if (PlayerNameOf(Controllers.at(i)) == Load.Username) Controller = (AFortPlayerController*)Controllers.at(i);
			Controllers.Free();

			if (!Controller) { std::lock_guard Lock(gBackpackMutex); gBackpackRequested.erase(Load.Username); continue; } // left again; read it on the next join

			int Given = 0;
			for (auto& Item : Load.Items)
			{
				const std::wstring WidePath(Item.Path.begin(), Item.Path.end());
				auto Definition = LoadObject<UFortItemDefinition>(WidePath.c_str(), ItemDefinitionClass);
				if (!Definition) { LOG_WARN(LogDev, "Zone: backpack item {} did not load", Item.Path); continue; }
				GrantItemWithToast(Controller, Definition, Item.Count, false);
				++Given;
			}

			{
				std::lock_guard Lock(gBackpackMutex);
				gBackpackReady.insert(Load.Username);
			}
			LOG_INFO(LogDev, "Zone: backpack of {} given: {} of {} item(s)", Load.Username, Given, Load.Items.size());
		}
	}

	void BeginEndOfZone()
	{
		SaveBackpacks(true); // what the players carry out of the zone
		if (gEndStage != EEndStage::Idle)
			return;

		ReportZoneCompleteToBackend();
		CloseMatchmaking(); // one mission per server: the backend stops offering it
		gEndStage = EEndStage::Waiting;
		gEndAt = std::chrono::steady_clock::now() + std::chrono::seconds(LeaveDelaySeconds);
		LOG_INFO(LogDev, "Zone: the mission succeeded, back to the main menu in {}s", LeaveDelaySeconds);
	}

	static void SetGameplayState(uint8_t State)
	{
		auto GameState = ((UObject*)GetWorld())->Get<UObject*>("GameState");
		if (!GameState) return;

		static int StateOffset = GameState->GetOffset("GameplayState", false);
		if (StateOffset == -1) { LOG_WARN(LogDev, "Zone: game state has no GameplayState"); return; }

		GameState->Get<uint8_t>(StateOffset) = State;
		static auto OnRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortGameState.OnRep_GameplayState");
		if (OnRepFn) GameState->ProcessEvent(OnRepFn);
		((AActor*)GameState)->ForceNetUpdate();
	}

	static void EndOfZoneTick()
	{
		if (gEndStage != EEndStage::Waiting || std::chrono::steady_clock::now() < gEndAt)
			return;

		gEndStage = EEndStage::Done;
		SetGameplayState(GameplayStateLeavingZone);

		static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
		static auto StayFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ClientStayWithParty");
		static auto ReturnFn = FindObject<UFunction>(L"/Script/Engine.PlayerController.ClientReturnToMainMenu");
		auto Controllers = PlayerControllerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass) : TArray<AActor*>();

		for (int i = 0; i < Controllers.Num(); ++i)
		{
			auto Controller = Controllers.at(i);
			if (!Controller) continue;

			if (StayFn) { bool bStay = true; Controller->ProcessEvent(StayFn, &bStay); }
			if (ReturnFn) { struct { const wchar_t* Data; int Num; int Max; } Reason{}; Controller->ProcessEvent(ReturnFn, &Reason); }
			LOG_INFO(LogDev, "Zone: sent {} back to the main menu", Controller->GetName());
		}

		Controllers.Free();
	}

	void SearchTick()
	{
		if (Globals::bZoneMode)
		{
			ScanAIPawnRouting();
			ScanSurvivors();
			ScanEditableWalls();
			EndOfZoneTick();
			BackpackTick();
			SaveBackpacks(false);
		}

		if (PendingCollects.empty())
			return;

		auto Now = std::chrono::steady_clock::now();

		for (int i = (int)PendingCollects.size() - 1; i >= 0; --i)
		{
			auto Pending = PendingCollects[i];

			if (Now < Pending.Due)
				continue;

			PendingCollects.erase(PendingCollects.begin() + i);

			if (!Pending.Pickup || !Pending.Pickup->IsValidLowLevel())
				continue; // already collected by hand and cleaned up, or gone with its world

			auto Pawn = Pending.Controller && Pending.Controller->IsValidLowLevel() ? Cast<AFortPlayerPawn>(Pending.Controller->GetMyFortPawn()) : nullptr;

			if (!Pawn)
				continue; // the searcher is gone; the pickup stays in the world for whoever comes next

			// the same call the client's pickup request makes: it starts the fly-in and a bPickedUp pickup is ignored
			AFortPlayerPawn::ServerHandlePickupHook(Pawn, Pending.Pickup, 0.40f, FVector(), true);
		}
	}

	// one verified gray-tier weapon per family, with the ammo the family uses in STW
	struct FZoneWeapon { const wchar_t* Weapon; const wchar_t* Ammo; int AmmoCount; };
	const FZoneWeapon ZoneWeapons[] = {
		{ L"/Game/Items/Weapons/Ranged/Assault/Auto/WID_Assault_Auto_C_Ore_T01.WID_Assault_Auto_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 20 },
		{ L"/Game/Items/Weapons/Ranged/Shotgun/Auto/WID_Shotgun_Auto_UC_Ore_T01.WID_Shotgun_Auto_UC_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 10 },
		{ L"/Game/Items/Weapons/Ranged/Shotgun/Break/WID_Shotgun_Break_C_Ore_T01.WID_Shotgun_Break_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 10 },
		{ L"/Game/Items/Weapons/Ranged/Sniper/BoltAction/WID_Sniper_BoltAction_C_Ore_T01.WID_Sniper_BoltAction_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy", 6 },
		{ L"/Game/Items/Weapons/Ranged/Pistol/Auto/WID_Pistol_Auto_C_Ore_T01.WID_Pistol_Auto_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 24 },
	};
	const int ZoneWeaponCount = sizeof(ZoneWeapons) / sizeof(ZoneWeapons[0]);

	// verified trap definitions from the STW paks (ceiling electric / floor freeze are the classic onboarding traps)
	const wchar_t* ZoneTrapPool[] = {
		L"/Game/Items/Traps/Ceiling/TID_Ceiling_Electric_Single_C_T01.TID_Ceiling_Electric_Single_C_T01",
		L"/Game/Items/Traps/Ceiling/TID_Ceiling_Falling_R_T01.TID_Ceiling_Falling_R_T01",
		L"/Game/Items/Traps/Floor/TID_Floor_Freeze_R_T01.TID_Floor_Freeze_R_T01",
	};
	const int ZoneTrapCount = sizeof(ZoneTrapPool) / sizeof(ZoneTrapPool[0]);

	// what the search grants, decided by what the prop is (names match the onboarding map's classes)
	bool SearchContainerSpawnLoot(ABuildingContainer* Container, AFortPlayerController* PlayerController)
	{
		if (!Container || !PlayerController)
			return false;

		// Mine carts marked "NoLoot" are genuine parts of the onboarding layout: STW leaves them empty on purpose, so do we
		auto ContainerName = Container->GetName();

		if (ContainerName.contains("NoLoot"))
		{
			LOG_INFO(LogDev, "Zone: searched prop is a deliberate no-loot prop");
			return false;
		}

		// The game picks a container's loot by its SearchLootTierGroup (a row group of the STW LootTierData table), so key on that and
		// log it for every search: it is how the groups of the other containers are found out.
		static int TierGroupOffset = Container->GetOffset("SearchLootTierGroup", false);
		std::string TierGroup = TierGroupOffset != -1 ? Container->Get<FName>(TierGroupOffset).ToString() : std::string();
		LOG_INFO(LogDev, "Zone: searched {} (loot tier group '{}')", ContainerName, TierGroup);

		const bool bOnboardingGroup = TierGroup.starts_with("Onboarding_Chest");
		bool bIsChest = ContainerName.contains("Chest") || bOnboardingGroup;
		bool bIsTutorialChest = bOnboardingGroup || ContainerName.contains("BP_SpecialChest_Pit_Child");
		bool bIsPlant = ContainerName.contains("Plant") || ContainerName.contains("Shrub") || ContainerName.contains("Hay") || ContainerName.contains("Bush");
		bool bIsMetalStorage = ContainerName.contains("Storage") || ContainerName.contains("Crate");

		// loot pops out of the container one item after another and flies into the searcher (see SpawnLootPickup)
		auto ContainerLocation = Container->GetActorLocation();
		int LootIndex = 0;

		auto Grant = [&](const wchar_t* Path, int Min, int Max, int ChancePct)
		{
			if (ZoneRandRange(1, 100) > ChancePct)
				return;

			int Count = ZoneRandRange(Min, Max);
			auto Definition = ZoneFindItem(Path);

			if (!Definition)
			{
				LOG_WARN(LogDev, "Zone: search loot definition did not load: {}", NarrowPath(Path));
				return;
			}

			FVector Spot = ContainerLocation;
			Spot.X += (float)ZoneRandRange(-60, 60);
			Spot.Y += (float)ZoneRandRange(-60, 60);
			Spot.Z += 60.f;

			SpawnLootPickup(PlayerController, Definition, Count, Spot, 0.35f + 0.18f * LootIndex++);
			LOG_INFO(LogDev, "Zone: search loot {} x{}", NarrowPath(Path), Count);
		};

		if (bIsPlant)
		{
			// real STW bushes/plants give crafting materials (twine), not building resources
			Grant(L"/Game/Items/Ingredients/Ingredient_Twine_T01.Ingredient_Twine_T01", 1, 3, 100);
			Grant(L"/Game/Items/Ingredients/Ingredient_Nuts_Bolts.Ingredient_Nuts_Bolts", 1, 2, 30);
			Grant(L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 4, 8, 35);
		}
		else if (bIsMetalStorage)
		{
			// metal storage: mechanical parts + ammo, a little metal stays
			Grant(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData", 5, 12, 100);
			Grant(L"/Game/Items/Ingredients/Ingredient_Mechanical_Parts_T01.Ingredient_Mechanical_Parts_T01", 1, 3, 60);
			Grant(L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 6, 12, 50);
			Grant(L"/Game/Items/Ammo/AmmoDataShells.AmmoDataShells", 4, 8, 30);
		}
		else if (bIsChest)
		{
			// chests: a weapon with its ammo, crafting ore and sometimes a trap
			if (bIsTutorialChest)
			{
				// The three tutorial chests have fixed pools: LootTierData_Client rows Onboarding_Chest1/2/3, each one loot package with one entry per
				// category and a drop count equal to the number of categories, all weights 1 (read from the Xbox One 7.30 build, which ships the STW
				// tables the PC build lacks). The pit chest (BP_SpecialChest_Pit_Child) is Chest2.
				const bool bChest1 = TierGroup == "Onboarding_Chest1";
				const bool bChest3 = TierGroup == "Onboarding_Chest3";

				if (bChest1)
				{
					// WorldPKG.Onboarding.Chest1
					Grant(L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 84, 84, 100);
					Grant(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData", 30, 30, 100);
					Grant(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData", 20, 20, 100);
				}
				else if (bChest3)
				{
					// WorldPKG.Onboarding.Chest3: the rocket launcher and its ammo
					Grant(L"/Game/Items/Weapons/Ranged/Launcher/Rocket/WID_Launcher_Rocket_R_Ore_T01.WID_Launcher_Rocket_R_Ore_T01", 1, 1, 100);
					Grant(L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 80, 80, 100);
				}
				else
				{
					// WorldPKG.Onboarding.Chest2
					Grant(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData", 20, 20, 100);
					Grant(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData", 10, 10, 100);
					Grant(L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 140, 140, 100);
					Grant(L"/Game/Items/Traps/Wall/OB_Trap_Floor_Spikes.OB_Trap_Floor_Spikes", 2, 2, 100);
					Grant(L"/Game/Items/Weapons/Melee/Edged/Sword_Medium_Low/WID_Edged_Sword_Medium_C_Ore_T00.WID_Edged_Sword_Medium_C_Ore_T00", 1, 1, 100);
				}
				return true;
			}
			{
				auto Pick = &ZoneWeapons[ZoneRandRange(0, ZoneWeaponCount - 1)];
				Grant(Pick->Weapon, 1, 1, 100);
				Grant(Pick->Ammo, Pick->AmmoCount / 2, Pick->AmmoCount, 100);
				if (ZoneRandRange(1, 100) <= 25)
					Grant(ZoneTrapPool[ZoneRandRange(0, ZoneTrapCount - 1)], 1, 1, 100);
			}

			Grant(L"/Game/Items/Ingredients/Ingredient_Ore_Copper.Ingredient_Ore_Copper", 1, 2, 60);
			Grant(L"/Game/Items/Ingredients/Ingredient_Nuts_Bolts.Ingredient_Nuts_Bolts", 2, 4, 70);
			Grant(L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 8, 16, 50);
		}
		else
		{
			// generic searchable (haystacks, drawers, ...): a small resource plus crafting odds
			Grant(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData", 8, 20, 100);
			Grant(L"/Game/Items/Ingredients/Ingredient_Twine_T01.Ingredient_Twine_T01", 1, 2, 40);
			Grant(L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 4, 8, 30);
		}

		return true;
	}

	// FortMissionLibrary.SpawnAndCollectPlayerPickups(WorldContextObject, PlayerController, StartPosition, LootTierGroup, LootTier): the storm shield
	// supply drop (B_SupplyTeleport SpawnLoot) hands out its loot with it, group Outpost_SuppyDrop_T1L1. The native gives nothing here (the player
	// opened the drop and got nothing, 2026-11-08) and the PC build has no STW loot tables, so the groups are filled from the Xbox One 7.30 build's
	// LootTierData_Client / LootPackages_Client: each group is one package with one entry per category, all weights 1, so the drop is fixed.
	// Without it the trap step had no traps to place: the first drop is where SSD 1 gives them.
	void (*SpawnAndCollectPlayerPickupsOriginal)(UObject* Context, FFrame& Stack, void* Ret) = nullptr;

	void SpawnAndCollectPlayerPickupsHook(UObject* Context, FFrame& Stack, void* Ret)
	{
		UObject* WorldContextObject = nullptr;
		AFortPlayerController* PlayerController = nullptr;
		FVector StartPosition{};
		FName LootTierGroup{};
		int LootTier = 0;
		// Called from the supply drop's script: the original reads the same parameters, so remember where they start to hand it
		// an unread frame, and when the original is skipped step past the end-of-parameters token like P_FINISH (without that
		// the VM ran the token as script and the game thread spun forever right after the loot came out)
		auto ParamsCode = Stack.Code;
		Stack.StepCompiledIn(&WorldContextObject);
		Stack.StepCompiledIn(&PlayerController);
		Stack.StepCompiledIn(&StartPosition);
		Stack.StepCompiledIn(&LootTierGroup);
		Stack.StepCompiledIn(&LootTier);

		const std::string Group = LootTierGroup.ToString();
		struct FDrop { const wchar_t* Path; int Count; };
		static const FDrop T1L1[] = { // WorldPKG.Outpost.SupplyDrop.T1L1
			{ L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData", 250 },
			{ L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData", 250 },
			{ L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData", 250 },
			{ L"/Game/Items/Ingredients/Ingredient_Mechanical_Parts_T01.Ingredient_Mechanical_Parts_T01", 4 },
			{ L"/Game/Items/Ingredients/Ingredient_Ore_Alloy.Ingredient_Ore_Alloy", 6 },
			{ L"/Game/Items/Ingredients/Ingredient_Nuts_Bolts.Ingredient_Nuts_Bolts", 10 },
			{ L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 200 },
			{ L"/Game/Items/Ingredients/Ingredient_Twine_T01.Ingredient_Twine_T01", 4 },
			{ L"/Game/Items/Traps/Floor/TID_Floor_Spikes_Wood_C_T01.TID_Floor_Spikes_Wood_C_T01", 3 },
			{ L"/Game/Items/Traps/Ceiling/TID_Ceiling_Electric_AOE_R_T01.TID_Ceiling_Electric_AOE_R_T01", 2 },
			{ L"/Game/Items/Traps/Wall/TID_Wall_Darts_R_T01.TID_Wall_Darts_R_T01", 2 },
		};
		static const FDrop T1L2[] = { // WorldPKG.Outpost.SupplyDrop.T1L2
			{ L"/Game/Items/Weapons/Ranged/Assault/Auto/WID_Assault_Auto_C_Ore_T00.WID_Assault_Auto_C_Ore_T00", 1 },
			{ L"/Game/Items/Traps/Floor/TID_Floor_Defender.TID_Floor_Defender", 4 },
			{ L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 200 },
		};

		const FDrop* Drops = nullptr;
		int DropCount = 0;
		if (Group == "Outpost_SuppyDrop_T1L1") { Drops = T1L1; DropCount = (int)(sizeof(T1L1) / sizeof(T1L1[0])); }
		else if (Group == "Outpost_SuppyDrop_T1L2") { Drops = T1L2; DropCount = (int)(sizeof(T1L2) / sizeof(T1L2[0])); }

		LOG_INFO(LogDev, "Zone: SpawnAndCollectPlayerPickups group '{}' tier {} for {} at ({:.0f}, {:.0f}, {:.0f}): {}", Group, LootTier,
			PlayerController ? PlayerController->GetName() : std::string("NO PLAYER"), StartPosition.X, StartPosition.Y, StartPosition.Z, Drops ? "known group" : "unknown group, left to the game");

		if (!Globals::bZoneMode || !Drops || !PlayerController)
		{
			Stack.Code = ParamsCode;
			return SpawnAndCollectPlayerPickupsOriginal(Context, Stack, Ret);
		}

		if (Stack.Code)
			Stack.Code++;

		// the same pop-out-and-fly-in pickups as a searched container
		for (int i = 0; i < DropCount; ++i)
		{
			auto Definition = ZoneFindItem(Drops[i].Path);
			if (!Definition)
			{
				LOG_WARN(LogDev, "Zone: supply drop item did not load: {}", NarrowPath(Drops[i].Path));
				continue;
			}
			FVector Spot = StartPosition;
			Spot.X += (float)ZoneRandRange(-60, 60);
			Spot.Y += (float)ZoneRandRange(-60, 60);
			Spot.Z += 60.f;
			SpawnLootPickup(PlayerController, Definition, Drops[i].Count, Spot, 0.35f + 0.18f * i);
			LOG_INFO(LogDev, "Zone: supply drop loot {} x{}", NarrowPath(Drops[i].Path), Drops[i].Count);
		}
	}
}
