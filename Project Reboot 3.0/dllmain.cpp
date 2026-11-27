#include <unordered_map>
#include <chrono>
#include <Windows.h>
#include <iostream>

#include "FortGameModeAthena.h"
#include "reboot.h"
#include "prodopts.h"
#include "RebootConfig.h"
#include "finder.h"
#include "hooking.h"
#include "GameSession.h"
#include "FortPlayerControllerAthena.h"
#include "AbilitySystemComponent.h"
#include "ZoneHusks.h"
#include "ZoneSearch.h"
#include "ZoneMatchmaking.h"
#include "ZoneFinder.h"
#include "FortWorldItemDefinition.h"
#include "FortQuickBars.h"
#include "FortPlayerPawn.h"
#include "globals.h"
#include "FortInventoryInterface.h"
#include <fstream>
#include "GenericPlatformTime.h"
#include "FortAthenaMutator_GiveItemsAtGamePhaseStep.h"
#include "FortGameStateAthena.h"
#include "BuildingGameplayActorSpawnMachine.h"
#include "BuildingWeapons.h"

#include "BuildingFoundation.h"
#include "Map.h"
#include "events.h"
#include "FortKismetLibrary.h"
#include "vehicles.h"
#include "UObjectArray.h"
#include "BuildingTrap.h"
#include "FortAthenaCreativePortal.h"
#include "commands.h"
#include "FortAthenaSupplyDrop.h"
#include "FortMinigame.h"
#include "KismetSystemLibrary.h"
#include "die.h"
#include "InventoryManagementLibrary.h"
#include "FortPlayerPawnAthena.h"
#include "FortWeaponRangedMountedCannon.h"
#include "gui.h"

#include "FortGameplayAbilityAthena_PeriodicItemGrant.h"
#include "vendingmachine.h"
#include "FortOctopusVehicle.h"
#include "FortVolumeManager.h"
#include "FortAthenaMutator_Barrier.h"

#include "PlaysetLevelStreamComponent.h"
#include "FortAthenaVehicleSpawner.h"
#include "FortGameSessionDedicatedAthena.h"
#include "FortAIEncounterInfo.h"
#include "FortServerBotManagerAthena.h"
#include "botnames.h"

/*

KNOWN VERSIONS BROKEN:
v8.30

*/

enum class EMeshNetworkNodeType : uint8_t
{
    Root = 0,
    Inner = 1,
    Edge = 2,
    Client = 3,
    Unknown = 4,
    EMeshNetworkNodeType_MAX = 5
};

enum ENetMode
{
    NM_Standalone,
    NM_DedicatedServer,
    NM_ListenServer,
    NM_Client,
    NM_MAX,
};

static EMeshNetworkNodeType GetMeshNetworkNodeTypeHook(__int64 a1)
{
    LOG_INFO(LogDev, "GetMeshNetworkNodeTypeHook Ret: 0x{:x}", __int64(_ReturnAddress()) - __int64(GetModuleHandleW(0)));
    return EMeshNetworkNodeType::Edge;
}

constexpr ENetMode NetMode = ENetMode::NM_DedicatedServer;

static ENetMode GetNetModeHook() { return NetMode; }
static ENetMode GetNetModeHook2() { return NetMode; }

static bool ReturnTrueHook() { return true; }
static bool ReturnFalseHook() { return false; }
static int Return2Hook() { return 2; }
static void EmptyHook() { return; }

static bool NoMCPHook() { return Globals::bNoMCP; }
static void CollectGarbageHook() { return; }

static __int64 (*ConstructEmptyQueryInfoOriginal)(FEncounterEnvironmentQueryInfo* a1);

static __int64 ConstructEmptyQueryInfoHook(FEncounterEnvironmentQueryInfo* a1)
{
    auto ret = ConstructEmptyQueryInfoOriginal(a1);

    a1->GetEnvironmentQuery() = FindObject<UEnvQuery>(L"/Game/Athena/Deimos/AIDirector/EQS/Deimos_EQS_RiftSlots.Deimos_EQS_RiftSlots");
    a1->IsDirectional() = true;

    LOG_INFO(LogDev, "ConstructEmptyQueryInfoHook!");

    return ret;
}

static __int64 (*DispatchRequestOriginal)(__int64 a1, __int64* a2, int a3);

static __int64 DispatchRequestHook(__int64 a1, __int64* a2, int a3)
{
    if (Globals::bNoMCP)
        return DispatchRequestOriginal(a1, a2, a3);

    if (Engine_Version >= 423)
        return DispatchRequestOriginal(a1, a2, 3); 

    // LOG_INFO(LogDev, "Dispatch Request!");

    static auto Offset = FindMcpIsDedicatedServerOffset();

    *(int*)(__int64(a2) + Offset) = 3;

    return DispatchRequestOriginal(a1, a2, 3);
}

static bool (*CanCreateInCurrentContextOriginal)(UObject* Template);

bool CanCreateInCurrentContextHook(UObject* Template)
{
    auto originalRet = CanCreateInCurrentContextOriginal(Template);

    if (!originalRet)
    {
        LOG_INFO(LogDev, "CanCreateInCurrentContextHook false but returning true for {}!", Template->IsValidLowLevel() ? Template->GetPathName() : "BadRead");
    }

    return true;
}

void (*ApplyHomebaseEffectsOnPlayerSetupOriginal)(
    __int64* GameState,
    __int64 a2,
    __int64 a3,
    __int64 a4,
    UObject* Hero,
    char a6,
    unsigned __int8 a7);

void __fastcall ApplyHomebaseEffectsOnPlayerSetupHook(
    __int64* GameState,
    __int64 a2,
    __int64 a3,
    __int64 a4,
    UObject* Hero,
    char a6,
    unsigned __int8 a7)
{
    if (!Hero)
        return ApplyHomebaseEffectsOnPlayerSetupOriginal(GameState, a2, a3, a4, Hero, a6, a7);

    LOG_INFO(LogDev, "Old hero: {}", Hero ? Hero->GetFullName() : "InvalidObject");

    UFortItemDefinition* HeroType = FindObject<UFortItemDefinition>(L"/Game/Athena/Heroes/HID_030_Athena_Commando_M_Halloween.HID_030_Athena_Commando_M_Halloween");

    if (Fortnite_Version == 1.72 || Fortnite_Version == 1.8)
    {
        auto AllHeroTypes = GetAllObjectsOfClass(FindObject<UClass>(L"/Script/FortniteGame.FortHeroType"));
        std::vector<UFortItemDefinition*> AthenaHeroTypes;

        for (int i = 0; i < AllHeroTypes.size(); i++)
        {
            auto CurrentHeroType = (UFortItemDefinition*)AllHeroTypes.at(i);

            if (CurrentHeroType->GetPathName().starts_with("/Game/Athena/Heroes/"))
                AthenaHeroTypes.push_back(CurrentHeroType);
        }

        if (AthenaHeroTypes.size() > 0)
        {
            HeroType = AthenaHeroTypes.at(std::rand() % AthenaHeroTypes.size() /* - 1 */);
        }
    }

    static auto ItemDefinitionOffset = Hero->GetOffset("ItemDefinition");
    Hero->Get<UFortItemDefinition*>(ItemDefinitionOffset) = HeroType;

    return ApplyHomebaseEffectsOnPlayerSetupOriginal(GameState, a2, a3, a4, Hero, a6, a7);
}

/*

static unsigned __int8 (*SpecialEventScript_ActivatePhaseOriginal)(UObject* SpecialEventScript, int NewPhase);

unsigned __int8 SpecialEventScript_ActivatePhaseHook(UObject* SpecialEventScript, int NewPhase)
{
    LOG_INFO(LogDev, "SpecialEventScript_ActivatePhaseHook {}!", NewPhase);

    static auto ReplicatedActivePhaseIndexOffset = SpecialEventScript->GetOffset("ReplicatedActivePhaseIndex");
    SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = NewPhase;

    static auto OnRep_ReplicatedActivePhaseIndexFn = FindObject<UFunction>("/Script/SpecialEventGameplayRuntime.SpecialEventScript.OnRep_ReplicatedActivePhaseIndex");
    SpecialEventScript->ProcessEvent(OnRep_ReplicatedActivePhaseIndexFn);

    return SpecialEventScript_ActivatePhaseOriginal(SpecialEventScript, NewPhase);
}

*/

static void (*ActivatePhaseAtIndexOriginal)(UObject* SpecialEventScript, int Index);

void ActivatePhaseAtIndexHook(UObject* SpecialEventScript, int Index)
{
    LOG_INFO(LogDev, "ActivatePhaseAtIndexHook {}!", Index);

    static auto ReplicatedActivePhaseIndexOffset = SpecialEventScript->GetOffset("ReplicatedActivePhaseIndex");
    SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = Index;

    static auto OnRep_ReplicatedActivePhaseIndexFn = FindObject<UFunction>("/Script/SpecialEventGameplayRuntime.SpecialEventScript.OnRep_ReplicatedActivePhaseIndex");
    SpecialEventScript->ProcessEvent(OnRep_ReplicatedActivePhaseIndexFn);

    auto& ClientConnections = GetWorld()->GetNetDriver()->GetClientConnections();

    // Rift Tour
    if (Fortnite_Version == 17.30)
    {
        auto EventPlaylist = GetEventPlaylist();

        struct { UObject* GameState; UObject* Playlist; FGameplayTagContainer PlaylistContextTags; } OnReadyParams{ Cast<AFortGameStateAthena>(GetWorld()->GetGameState()), EventPlaylist };

        {
            if (Index == 0) // Start of Event
            {
                auto AllSatelliteDishes = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FindObject<UClass>("/Game/Athena/Apollo/Environments/BuildingActors/Complex/Props/Apollo_Complex_Satellite_Dish_Top_01.Apollo_Complex_Satellite_Dish_Top_01_C"));

                for (int i = 0; i < AllSatelliteDishes.Num(); i++)
                {
                    AllSatelliteDishes.At(i)->K2_DestroyActor();
                }

                auto AllSpecialEventMutators = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FindObject<UClass>("/Script/SpecialEventGameplayRuntime.FortAthenaMutator_SpecialEvent"));

                for (int i = 0; i < AllSpecialEventMutators.Num(); i++)
                {
                    auto CurrentSpecialEventMutator = AllSpecialEventMutators.At(i);

                    auto ScriptActor = CurrentSpecialEventMutator->Get<AActor*>(CurrentSpecialEventMutator->GetOffset("ScriptActor"));

                    if (!ScriptActor)
                    {
                        ScriptActor = Cast<AActor>(GetEventScripting());
                        LOG_INFO(LogEvent, "ScriptActor set for mutator: {}", CurrentSpecialEventMutator->GetName());
                    }
                }
            }
            if (Index == 2) // Slide
            {
                auto Script = FindObject<UObject>(L"/Buffet/Levels/Buffet_Part_4.Buffet_Part_4.PersistentLevel.BP_Buffet_PhaseScripting_Paint_4");
                auto SplineActor = Script->Get<AActor*>(Script->GetOffset("SplineActor"));
                auto PawnLocation = Script->Get<AActor*>(Script->GetOffset("PawnLocation")) = SplineActor;

                auto AllWrapsSpawners = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FindObject<UClass>(L"/Script/SpecialEventGameplayRuntime.FortSpecialRelevancyActorSpawner"));

                for (int i = 0; i < AllWrapsSpawners.Num(); i++)
                {
                    auto CurrentWrapSpawner = AllWrapsSpawners.At(i);

                    if (CurrentWrapSpawner)
                    {
                        auto WrapSpawnerLocation = CurrentWrapSpawner->GetActorLocation();
                        auto WrapSpawnerRotation = CurrentWrapSpawner->GetActorRotation();
                        UClass* ClassToSpawn = nullptr;

                        if (CurrentWrapSpawner->GetName().contains("WrapChangePickup"))
                        {
                            ClassToSpawn = FindObject<UClass>(L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_WrapChangePickup.BP_Buffet_Paint_WrapChangePickup_C");
                        }
                        else if (CurrentWrapSpawner->GetName().contains("Paint_Pickup"))
                        {
                            ClassToSpawn = FindObject<UClass>(L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_Pickup.BP_Buffet_Paint_Pickup_C");
                        }
                        auto SpawnedWrap = GetWorld()->SpawnActor<AActor>(ClassToSpawn, WrapSpawnerLocation, WrapSpawnerRotation.Quaternion(), FVector(1, 1, 1), 
                            CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn, true, nullptr));

                        if (SpawnedWrap)
                        {
                            LOG_INFO(LogDev, "Spawner Spawned a Wrap: {}", SpawnedWrap->GetName());
                        }
                        else
                        {
                            LOG_INFO(LogDev, "Spawner Failed to spawn a Wrap {}!", CurrentWrapSpawner->GetName());
                        }
                    }
                    else
                    {
                        LOG_INFO(LogDev, "Spawner Invalid!");
                    }
                }

                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentController = ClientConnections.At(i)->GetPlayerController();
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();

                    if (!CurrentPawn)
                        continue;

                    auto PlayerComponent = CurrentPawn->AddComponentByClass(FindObject<UClass>("/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_PlayerComponent.BP_Buffet_Paint_PlayerComponent_C")); // WHY DOESnT IT WORK AUTO
                    auto MovementComponent = CurrentPawn->AddComponentByClass(FindObject<UClass>("/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_MovementComponent.BP_Buffet_Paint_MovementComponent_C"));

                    PlayerComponent->Get<AActor*>(PlayerComponent->GetOffset("OwningPlayerController")) = CurrentController;

                    struct
                    {
                        AActor* Pawn;
                        AActor* ReturnValue;
                    }wrapmanagerparams{};
                    wrapmanagerparams.Pawn = CurrentPawn;

                    Script->ProcessEvent(Script->FindFunction("GetWrapManagerForPlayer"), &wrapmanagerparams);

                    MovementComponent->Get<bool>(MovementComponent->GetOffset("bIsMovingAlongSpline")) = true;
                    MovementComponent->Get<float>(MovementComponent->GetOffset("ReplicatedTotalSplineTime")) = 59.793846f; // TotalSplineTime
                    MovementComponent->Get<AActor*>(MovementComponent->GetOffset("TargetSplineActor")) = SplineActor;
                    MovementComponent->ProcessEvent(MovementComponent->FindFunction("SetSplineActor"), &SplineActor);
                    bool IsMoving = true;
                    MovementComponent->ProcessEvent(MovementComponent->FindFunction("SetIsMovingAlongSpline"), &IsMoving);
                    MovementComponent->Get<float>(MovementComponent->GetOffset("ReplicatedSplineInterpStrength")) = 1.3f; // SplineInterpStrength
                    auto GameMode = (AFortGameMode*)GetWorld()->GetGameMode();
                    auto GameState = Cast<AFortGameStateAthena>(GameMode->GetGameState());
                    MovementComponent->Get<float>(MovementComponent->GetOffset("StartServerWorldTime")) = GameState->GetServerWorldTimeSeconds();

                    MovementComponent->ProcessEvent(MovementComponent->FindFunction("OnRep_TargetSplineActor"));

                    PlayerComponent->Get<AActor*>(PlayerComponent->GetOffset("WrapManager")) = wrapmanagerparams.ReturnValue;

                    PlayerComponent->Get<UObject*>(PlayerComponent->GetOffset("MovementComponent")) = MovementComponent;

                    int StasisMode = 3;
                    CurrentPawn->ProcessEvent(CurrentPawn->FindFunction("SetStasisMode"), &StasisMode); // ??
                }
            }
            if (Index == 3)
            {
                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();
                    auto CurrentController = ClientConnections.At(i)->GetPlayerController();
                    int StasisMode = 0;

                    CurrentPawn->ProcessEvent(CurrentPawn->FindFunction("SetStasisMode"), &StasisMode);
                }
            }
            if (Index == 4) // Storm King
            {
                auto StormScript = FindObject<AActor>("/Buffet/Levels/Buffet_Part_6.Buffet_Part_6:PersistentLevel.BP_Buffet_PhaseScripting_4");

                auto DefaultPlane = FindObject("/Buffet/Gameplay/Blueprints/WolfMother/BP_PlanePrototype.Default__BP_PlanePrototype_C");

                auto WID = Cast<UFortWorldItemDefinition>(FindObject("WID_Buffet_BeatMatchingBoomBox", nullptr, ANY_PACKAGE)); // Storm King weapon thing

                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();
                    auto CurrentController = (AFortPlayerControllerAthena*)ClientConnections.At(i)->GetPlayerController();

                    auto WorldInventory = CurrentController->GetWorldInventory();

                    bool bShouldUpdate = false;
                    WorldInventory->AddItem(WID, &bShouldUpdate, 1);

                    if (bShouldUpdate)
                        WorldInventory->Update();

                    SendMessageToConsole(CurrentController, L"Gave WID_Buffet_BeatMatchingBoomBox!");
                }

                auto AllWeps = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FindObject<UClass>(L"/Buffet/Gameplay/Blueprints/WolfMother/BeatmatchingWeapon/B_Buffet_BeatMatchingWeaponPrototype.B_Buffet_BeatMatchingWeaponPrototype_C"));

                for (int i = 0; i < AllWeps.Num(); i++)
                {
                    auto CurrentWep = AllWeps.At(i);

                    if (CurrentWep != nullptr)
                    {
                        CurrentWep->Get<AActor*>(CurrentWep->GetOffset("Phase4ScriptingRef")) = StormScript;
                    }
                }
            }
            if (Index == 6) // Ariana
            {
                auto ReflectScript = FindObject(L"/Buffet/Levels/Buffet_Reflect.Buffet_Reflect:PersistentLevel.BP_Buffet_PhaseScripting_Stars_2");

                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();

                    CurrentPawn->AddComponentByClass(FindObject<UClass>("/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_PlayerComponent.BP_Buffet_Stars_PlayerComponent_C"));
                    CurrentPawn->AddComponentByClass(FindObject<UClass>("/BuffetPlaylist/Playlist/BuffetCharacterMovementMutatorComponent.BuffetCharacterMovementMutatorComponent_C"));
                }
            }
            if (Index == 8) // Bubbles
            {
                //auto BubbleScript = FindObject("/Buffet/Levels/Buffet_Bubbles.Buffet_Bubbles:PersistentLevel.BP_Buffet_PhaseScripting_Bubble_4");
                //auto BubbleSpline = FindObject("/Buffet/Levels/Buffet_Bubbles.Buffet_Bubbles:PersistentLevel.BP_Buffet_SmallBubblePath_2.Spline");

                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();
                    auto ComptoRemove = CurrentPawn->GetComponentByClass(FindObject<UClass>("/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_PlayerComponent.BP_Buffet_Stars_PlayerComponent_C"));

                    auto BubbleMovementComponent = CurrentPawn->AddComponentByClass(FindObject<UClass>("/Buffet/Gameplay/Blueprints/Bubble/BP_BubblePlayerMovementComponent.BP_BubblePlayerMovementComponent_C"));

                    /*
                    struct
                    {
                        UObject* InSplineComponent;
                        float StartDistance;

                    }SetSplineComponent;
                    SetSplineComponent.StartDistance = 0;
                    SetSplineComponent.InSplineComponent = BubbleSpline;

                    // Works but too fast and player not in correct place on the spline.
                    BubbleMovementComponent->ProcessEvent(BubbleMovementComponent->FindFunction("SetSplineComponent"), &SetSplineComponent);
                    BubbleMovementComponent->Get<UObject*>(BubbleMovementComponent->GetOffset("SplineComponent")) = BubbleSpline;
                    BubbleMovementComponent->Get<bool>(BubbleMovementComponent->GetOffset("bIsMovingAlongSpline")) = true;
                    auto GameMode = (AFortGameMode*)GetWorld()->GetGameMode();
                    auto GameState = Cast<AFortGameStateAthena>(GameMode->GetGameState());
                    BubbleMovementComponent->Get<float>(BubbleMovementComponent->GetOffset("StartServerWorldTime")) = GameState->GetServerWorldTimeSeconds();
                    bool IsMoving = true;
                    BubbleMovementComponent->ProcessEvent(BubbleMovementComponent->FindFunction("SetIsMovingAlongSpline"), &IsMoving);
                    */

                    CurrentPawn->ProcessEvent(CurrentPawn->FindFunction("K2_DestroyComponent"), &ComptoRemove);
                }
            }
            if (Index == 10) // Escher
            {
                for (int i = 0; i < ClientConnections.Num(); i++)
                {
                    auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();

                    CurrentPawn->TeleportTo(FVector(36.34f, 1044.07f, 504.50f), CurrentPawn->GetActorRotation());
                }
            }
        }
    }

    if (Fortnite_Version == 18.40)
    {
        if (Index == 0)
        {
            for (int i = 0; i < ClientConnections.Num(); i++)
            {
                auto CurrentController = (AFortPlayerControllerAthena*)ClientConnections.At(i)->GetPlayerController();

                static auto StormEffectClass = FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C");
                auto PlayerState = CurrentController->GetPlayerStateAthena();
                PlayerState->GetAbilitySystemComponent()->RemoveActiveGameplayEffectBySourceEffect(StormEffectClass, 1, PlayerState->GetAbilitySystemComponent());
            }

            auto AssultWID = Cast<UFortWorldItemDefinition>(FindObject("WID_Guava_Assault_AutoHigh_Athena_SR_Ore_T03", nullptr, ANY_PACKAGE));
            auto RevolveWID = Cast<UFortWorldItemDefinition>(FindObject("WID_Guava_Pistol_SixShooter_Athena_SR_Ore_T03", nullptr, ANY_PACKAGE));

            for (int i = 0; i < ClientConnections.Num(); i++)
            {
                auto CurrentPawn = ClientConnections.At(i)->GetPlayerController()->GetPawn();
                auto CurrentController = (AFortPlayerControllerAthena*)ClientConnections.At(i)->GetPlayerController();

                auto WorldInventory = CurrentController->GetWorldInventory();

                bool bShouldUpdate = true;
                WorldInventory->AddItem(AssultWID, &bShouldUpdate, 1);
                WorldInventory->AddItem(RevolveWID, &bShouldUpdate, 1);


                if (bShouldUpdate)
                    WorldInventory->Update();

                SendMessageToConsole(CurrentController, L"Gave Event Weapons!");
            }

            auto AllSatelliteDishes = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FindObject<UClass>("/Game/Athena/Apollo/Environments/BuildingActors/Complex/Props/Apollo_Complex_Satellite_Dish_Top_01.Apollo_Complex_Satellite_Dish_Top_01_C"));

            for (int i = 0; i < AllSatelliteDishes.Num(); i++)
            {
                AllSatelliteDishes.At(i)->K2_DestroyActor();
            }
        }
    }

    return ActivatePhaseAtIndexOriginal(SpecialEventScript, Index);
}

static void (*AttemptFinishPhaseOriginal)(AActor* SpecialEventScript, FGameplayTag PhaseTag);
void AttemptFinishPhaseHook(AActor* SpecialEventScript, FGameplayTag PhaseTag)
{
    LOG_INFO(LogDev, "SpecialEventScript::AttemptFinishPhase PhaseTag: {}", PhaseTag.TagName.ToString());

    static auto ReplicatedActivePhaseIndexOffset = SpecialEventScript->GetOffset("ReplicatedActivePhaseIndex"); // Sets what Map/Phase were replicating to clients.
    auto LastTag = PhaseTag.TagName.ToString();

    static auto OnRep_ReplicatedActivePhaseIndexFn = FindObject<UFunction>("/Script/SpecialEventGameplayRuntime.SpecialEventScript.OnRep_ReplicatedActivePhaseIndex");

    auto& Connections = GetWorld()->GetNetDriver()->GetClientConnections();

    if (LastTag == "Kiwi.Phase.Islands") // PrisonBlocks (Event Start)
    {
        SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = 1;

        for (int i = 0; i < Connections.Num(); i++)
        {
            auto CurrentPawn = Cast<AFortPlayerPawn>(Connections.At(i)->GetPlayerController()->GetPawn());

            CurrentPawn->TeleportTo(FVector(34640.0f, 32350.0f, 100600.0f), CurrentPawn->GetActorRotation());

            CurrentPawn->ServerChoosePart(EFortCustomPartType::Backpack, FindObject("/Kiwi/Gameplay/Blueprints/Backpack/CP_Backpack_Kiwi.CP_Backpack_Kiwi"));
        }
    }
    if (LastTag == "Kiwi.Phase.Prison") // Kevin
    {
        SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = 2;
    }
    if (LastTag == "Kiwi.Phase.ObservationRoom") // Hangar
    {
        SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = 3;
    }
    if (LastTag == "Kiwi.Phase.Hanger") // Ending
    {
        SpecialEventScript->Get<int32>(ReplicatedActivePhaseIndexOffset) = 4;
    }
    SpecialEventScript->ProcessEvent(OnRep_ReplicatedActivePhaseIndexFn);

    return AttemptFinishPhaseOriginal(SpecialEventScript, PhaseTag);
}

// Used to teleport the pawn in some events, not used in all parts of rift tour for some reason.
static void (*TeleportPlayerPawnOriginal)(UObject* Context, FFrame& Stack, void* Ret);
void TeleportPlayerPawnHook(UObject* Context, FFrame& Stack, void* Ret)
{
    LOG_INFO(LogEvent, "Teleported PlayerPawn!");

    UObject* WorldContextObject;
    AFortPlayerPawn* PlayerPawn;                                                      //(Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
    FVector                DestLocation;                                                //(Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
    FRotator               DestRotation;                                                    //(Parm, ZeroConstructor, IsPlainOldData, NoDestructor, NativeAccessSpecifierPublic)
    bool                   bIgnoreCollision;                                             //(Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
    bool                   bIgnoreSupplementalKillVolumeSweep;                           //(Parm, ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)

    Stack.StepCompiledIn(&WorldContextObject);
    Stack.StepCompiledIn(&PlayerPawn);
    Stack.StepCompiledIn(&DestLocation);
    Stack.StepCompiledIn(&DestRotation);
    Stack.StepCompiledIn(&bIgnoreCollision);
    Stack.StepCompiledIn(&bIgnoreSupplementalKillVolumeSweep);

    PlayerPawn->TeleportTo(DestLocation, DestRotation);

    return TeleportPlayerPawnOriginal(Context, Stack, Ret);
}

/*

// Storm King Spawning, I might put this in its own class soon.
bool bStormKingSpawned = false;
static inline void (*DADBroOnGamephaseStepChangedOriginal)(UObject* Context, FFrame& Stack, void* Ret);
void DADBroOnGamephaseStepChangedHook(UObject* Context, FFrame& Stack, void* Ret)
{
    auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());

    if (!GameState)
        return DADBroOnGamephaseStepChangedOriginal(Context, Stack, Ret);

    FRotator Rotation;
    Context->ProcessEvent(Context->FindFunction("GetDesiredDadBroRotation"), &Rotation);

    if (GameState->GetGamePhaseStep() == EAthenaGamePhaseStep::StormShrinking && bStormKingSpawned == false)
    {
        auto StormKing = GetWorld()->SpawnActor<AActor>(FindObject<UClass>("/Game/Athena/DADBRO/DADBRO_Pawn.DADBRO_Pawn_C"), FVector{ 5150.0f, 4900.0f, -100.0f }, Rotation.Quaternion(), FVector{ 1,1,1 }, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn));

        if (StormKing)
        {
            bStormKingSpawned = true;
            Context->Get<AActor*>(Context->GetOffset("DadBroPawn")) = StormKing;
            Context->ProcessEvent(Context->FindFunction("OnRep_DadBroPawn"));
        }
        else
        {
            LOG_INFO(LogDev, "Failed to spawn StormKing!");
        }
    }

    return DADBroOnGamephaseStepChangedOriginal(Context, Stack, Ret);
}
*/

static void (*StartEventAtIndexOriginal)(UObject* Context, FFrame& Stack, void* Ret);
void StartEventAtIndexHook(UObject* Context, FFrame& Stack, void* Ret)
{
    int32 InStartingIndex;
    Stack.StepCompiledIn(&InStartingIndex);

    if (Fortnite_Version == 17.50)
    {
        auto Scripting = FindObject<UObject>("/Kiwi/Levels/Kiwi_P.Kiwi_P:PersistentLevel.BP_Kiwi_Master_Scripting_2");
        auto EventPlaylist = GetEventPlaylist();

        struct { UObject* GameState; UObject* Playlist; FGameplayTagContainer PlaylistContextTags; } OnReadyParams{ Cast<AFortGameStateAthena>(GetWorld()->GetGameState()), EventPlaylist };
        if (EventPlaylist)
        {
            static auto GameplayTagContainerOffset = EventPlaylist->GetOffset("GameplayTagContainer");
            OnReadyParams.PlaylistContextTags = EventPlaylist->Get<FGameplayTagContainer>(GameplayTagContainerOffset);
        }
        else
        {
            OnReadyParams.PlaylistContextTags = FGameplayTagContainer();
        }

        Scripting->ProcessEvent(Scripting->FindFunction("OnReady_F1A32853487CB7603278E6847A5F2625"), &OnReadyParams);
        Context->ProcessEvent(Context->FindFunction("LoadKiwiAssets"), &OnReadyParams);
        Context->ProcessEvent(Context->FindFunction("BP_OnScriptReady"), &OnReadyParams);
        Scripting->ProcessEvent(Scripting->FindFunction("startevent"), &InStartingIndex);
    }

    return StartEventAtIndexOriginal(Context, Stack, Ret);
}

static __int64 (*FlowStep_SetPhaseToActiveOriginal)(AActor* SpecialEventPhase);

__int64 FlowStep_SetPhaseToActiveHook(AActor* SpecialEventPhase)
{
    LOG_INFO(LogDev, "FlowStep_SetPhaseToActiveHook!");

    auto ret = FlowStep_SetPhaseToActiveOriginal(SpecialEventPhase); // idk if three actually is a ret

    static auto OnRep_PhaseState = FindObject<UFunction>(L"/Script/SpecialEventGameplayRuntime.SpecialEventPhase.OnRep_PhaseState");
    SpecialEventPhase->ProcessEvent(OnRep_PhaseState);

    SpecialEventPhase->ForceNetUpdate();

    return ret;
}

UObject* GetGoalManagerHook(UObject* WorldContextObject)
{
    auto GameMode = GetWorld() ? (UObject*)GetWorld()->GetGameMode() : nullptr;
    if (!GameMode) return nullptr;
    static auto AIGoalManagerOffset = GameMode->GetOffset("AIGoalManager", false);
    if (AIGoalManagerOffset == -1) return nullptr;
    return GameMode->Get<UObject*>(AIGoalManagerOffset);
}

UObject* GetAIDirectorHook()
{
    auto GameMode = GetWorld() ? (UObject*)GetWorld()->GetGameMode() : nullptr;
    if (!GameMode) return nullptr;
    static auto AIDirectorOffset = GameMode->GetOffset("AIDirector", false);
    if (AIDirectorOffset == -1) return nullptr;
    return GameMode->Get<UObject*>(AIDirectorOffset);
}

DWORD WINAPI ClientThread(LPVOID)
{
    while (true)
    {

        return 0;
    }
}

static bool (*Zone_ReadyToStartMatchOriginal)(UObject*);
static bool gZoneMatchStarted = false;
static bool gZoneMatchStartPending = false; // the outpost's match start, waiting for the owner's quest config (see the player setup)
static std::chrono::steady_clock::time_point gZoneMatchStartRequestedAt{};

static bool Zone_Objective_StartPlayingObjective(UObject* Objective)
{
    if (!Objective) return false;

    static int StatusOffset = Objective->GetOffset("ObjectiveStatus", false);
    if (StatusOffset == -1) { LOG_WARN(LogDev, "Zone Objective: FortObjectiveBase has no ObjectiveStatus"); return false; }

    uint8_t* StatusPtr = (uint8_t*)Objective + StatusOffset;
    LOG_INFO(LogDev, "Zone Objective StartPlayingObjective: {} (prev status {})", Objective->GetFullName(), (int)*StatusPtr);

    if (*StatusPtr != 1)
    {
        *StatusPtr = 1; // EFortObjectiveStatus::InProgress

        static auto OnRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.OnRep_ObjectiveStatus");
        if (OnRepFn) Objective->ProcessEvent(OnRepFn);

        static int OwnerOffset = Objective->GetOffset("Owner");
        auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset);
        if (Mission)
        {
            if (Mission->VFTable)
            {
                auto OnObjStatusChanged = (void (*)(UObject*, UObject*, uint8_t))(Mission->VFTable[ZoneFinder::Fixed730::MissionObjectiveStatusChangedSlot / 8]);
                OnObjStatusChanged(Mission, Objective, 1);
            }
            else
            {
                LOG_WARN(LogDev, "Zone Objective: mission has no vtable, InProgress not forwarded");
            }
        }

        static auto BpStartFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.BlueprintOnStartPlaying");
        if (BpStartFn) Objective->ProcessEvent(BpStartFn);

        ((AActor*)Objective)->ForceNetUpdate();

        if (Objective->GetName().contains("OBJ_HuskProblem"))
        {
            if (Mission)
            {
                Zone::PrepareMissionEncounters(Mission);
            }

            // the StartFirstEncounter call above is the game's own husk spawn (its encounters + the AI director); this only starts
            // watching for the pawns it produces (and falls back to hand-placed husks if it produces none)
            Zone::OnHuskObjectiveStarted(Objective);
        }
    }

    return true;
}

static bool Zone_Objective_ActivateObjective(UObject* Objective)
{
    if (!Objective) return false;

    LOG_INFO(LogDev, "Zone Objective ActivateObjective: {}", Objective->GetFullName());

    static auto BpActivateFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.BlueprintOnActivated");
    if (BpActivateFn) Objective->ProcessEvent(BpActivateFn);

    static int OwnerOffset = Objective->GetOffset("Owner");
    auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset);

    static int StartOnMissionStartOffset = Objective->GetOffset("bStartPlayingOnMissionStart");
    static uint8_t StartOnMissionStartMask = GetFieldMask(Objective->GetProperty("bStartPlayingOnMissionStart"));
    bool bStartOnMissionStart = Objective->ReadBitfieldValue(StartOnMissionStartOffset, StartOnMissionStartMask);
    if (Mission && bStartOnMissionStart)
    {
        static int MissionStatusOffset = Mission->GetOffset("MissionStatus");
        if (*((uint8_t*)Mission + MissionStatusOffset) == 1)
        {
            Zone_Objective_StartPlayingObjective(Objective);
        }
    }

    return true;
}

// A component's world location through K2_GetComponentLocation (what the mission scripts read).
static FVector Zone_ComponentWorldLocation(UObject* Component)
{
    static auto GetLocationFn = FindObject<UFunction>(L"/Script/Engine.SceneComponent.K2_GetComponentLocation");
    if (!Component || !GetLocationFn) return FVector{};
    std::vector<uint8_t> Params(GetLocationFn->GetPropertiesSize() + 16, 0);
    Component->ProcessEvent(GetLocationFn, Params.data());
    static int ReturnOffset = FindOffsetStruct("/Script/Engine.SceneComponent.K2_GetComponentLocation", "ReturnValue");
    return ReturnOffset >= 0 ? *(FVector*)(Params.data() + ReturnOffset) : FVector{};
}

// Where a component of Owner should be in the world: Owner's transform with the component's relative location and yaw (the outpost props
// are placed upright, so yaw is the only rotation that matters).
static void Zone_ExpectedComponentWorld(AActor* Owner, UObject* Component, FVector& OutLocation, FRotator& OutRotation)
{
    static int RelativeLocationOffset = Component->GetOffset("RelativeLocation", false);
    static int RelativeRotationOffset = Component->GetOffset("RelativeRotation", false);
    const FVector Relative = RelativeLocationOffset >= 0 ? Component->Get<FVector>(RelativeLocationOffset) : FVector{};
    const FRotator RelativeRotation = RelativeRotationOffset >= 0 ? Component->Get<FRotator>(RelativeRotationOffset) : FRotator{};
    const FVector At = Owner->GetActorLocation();
    const FRotator Rotation = Owner->GetActorRotation();
    const double Yaw = Rotation.Yaw * 3.14159265358979 / 180.0;
    OutLocation = FVector(At.X + (float)(Relative.X * cos(Yaw) - Relative.Y * sin(Yaw)), At.Y + (float)(Relative.X * sin(Yaw) + Relative.Y * cos(Yaw)), At.Z + Relative.Z);
    OutRotation = Rotation;
    OutRotation.Yaw += RelativeRotation.Yaw;
}

// Puts a component of Owner where it belongs in the world if its world location reads as something else. On this server the particle
// components of BP_StormShield_Core_Egg read as the world origin although the core stands elsewhere (P_ArrowInteraction), and
// the mission spawns the deployed core at another of them (TestSpawnLoc, SpawnCoreAndConsoles), so the core would land at the origin.
static void Zone_FixComponentWorld(AActor* Owner, const char* ComponentName)
{
    if (!Owner) return;
    const int ComponentOffset = Owner->GetOffset(ComponentName, false);
    UObject* Component = ComponentOffset >= 0 ? Owner->Get<UObject*>(ComponentOffset) : nullptr;
    if (!Component) { LOG_INFO(LogDev, "Zone: {} has no {} component", Owner->GetName(), ComponentName); return; }

    FVector Expected; FRotator ExpectedRotation;
    Zone_ExpectedComponentWorld(Owner, Component, Expected, ExpectedRotation);
    const FVector Before = Zone_ComponentWorldLocation(Component);
    const float Off = sqrtf((Before.X - Expected.X) * (Before.X - Expected.X) + (Before.Y - Expected.Y) * (Before.Y - Expected.Y) + (Before.Z - Expected.Z) * (Before.Z - Expected.Z));
    if (Off < 5.0f)
    {
        LOG_INFO(LogDev, "Zone: {}.{} is in place at ({:.0f}, {:.0f}, {:.0f})", Owner->GetName(), ComponentName, Before.X, Before.Y, Before.Z);
        return;
    }

    static auto SetWorldFn = FindObject<UFunction>(L"/Script/Engine.SceneComponent.K2_SetWorldLocationAndRotation");
    if (!SetWorldFn) return;
    static const char* SetWorldPath = "/Script/Engine.SceneComponent.K2_SetWorldLocationAndRotation";
    static int NewLocationOffset = FindOffsetStruct(SetWorldPath, "NewLocation");
    static int NewRotationOffset = FindOffsetStruct(SetWorldPath, "NewRotation");
    static int SweepOffset = FindOffsetStruct(SetWorldPath, "bSweep");
    static int TeleportOffset = FindOffsetStruct(SetWorldPath, "bTeleport");
    std::vector<uint8_t> Params(SetWorldFn->GetPropertiesSize() + 64, 0);
    *(FVector*)(Params.data() + NewLocationOffset) = Expected;
    *(FRotator*)(Params.data() + NewRotationOffset) = ExpectedRotation;
    Params[SweepOffset] = 0;
    Params[TeleportOffset] = 1;
    Component->ProcessEvent(SetWorldFn, Params.data());

    const FVector After = Zone_ComponentWorldLocation(Component);
    LOG_INFO(LogDev, "Zone: {}.{} read ({:.0f}, {:.0f}, {:.0f}) instead of ({:.0f}, {:.0f}, {:.0f}), set it there: now ({:.0f}, {:.0f}, {:.0f})", Owner->GetName(), ComponentName,
        Before.X, Before.Y, Before.Z, Expected.X, Expected.Y, Expected.Z, After.X, After.Y, After.Z);
}

// The mission's answer to a finished objective. Mission_Outpost deploys the storm shield in HandleMissionEvent_ObjectiveSucceeded: when the
// objective is its DeployCoreObjectiveHandle it starts SpawnCore four seconds later (SpawnCoreAndConsoles: the deployed core
// BP_StormShield_Core_New and its four BP_Outpost_ControlPanel_New consoles, then the first supply drop, the egg destroyed, building allowed).
// That event comes from the server's objective code, which this exe lacks: the deploy objectives succeeded on interacting with the core but
// nothing else happened, no consoles, the egg still there (2026-11-08). The game's own dispatch (FortMissionLibrary.ExecuteMissionEventEx)
// faulted when called from here before (ZoneSearch.cpp RaisePlayerBuilt), so the handler is called on the mission itself. Missions without
// such a handler (the tutorial's Mission_FarmsteadFort) are left alone.
static void Zone_RaiseObjectiveSucceeded(UObject* Mission, UObject* Objective)
{
    if (!Mission || !Objective) return;
    auto HandlerFn = Mission->FindFunction("HandleMissionEvent_ObjectiveSucceeded");
    if (!HandlerFn) return;

    // the deployed core is spawned at the egg's TestSpawnLoc
    static auto MissionOutpostClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Mission_Outpost.Mission_Outpost_C");
    if (MissionOutpostClass && Mission->IsA(MissionOutpostClass))
    {
        static int EggOffset = Mission->GetOffset("BP_StormShield_Egg", false);
        if (EggOffset >= 0) Zone_FixComponentWorld(Mission->Get<AActor*>(EggOffset), "TestSpawnLoc");
    }

    // parameter offsets straight from the function's properties (a blueprint function's path has a ':' that FindObject does not take)
    auto ParamOffset = [HandlerFn](const std::string& Name) -> int
    {
        for (void* Property = *(void**)(__int64(HandlerFn) + Offsets::Children); Property; Property = GetNext(Property))
            if (GetFNameOfProp(Property)->ToString() == Name) return *(int*)(__int64(Property) + Offsets::Offset_Internal);
        return -1;
    };
    const int HandleOffset = ParamOffset("ObjectiveHandle");
    const int GuidOffset = ParamOffset("InMissionGuid");
    const int EventOffset = ParamOffset("MissionEvent");
    const int TextOffset = ParamOffset("GenericText");
    static int ObjectiveHandleOffset = Objective->GetOffset("ObjectiveHandle", false);
    static int MissionGuidOffset = Mission->GetOffset("MissionGuid", false);
    if (HandleOffset < 0 || TextOffset < 0 || ObjectiveHandleOffset < 0)
    {
        LOG_WARN(LogDev, "Zone: cannot raise ObjectiveSucceeded on {} (handler handle {}, text {}, objective handle {})", Mission->GetName(), HandleOffset, TextOffset, ObjectiveHandleOffset);
        return;
    }

    // The handler copies every parameter onto its frame, the text too: it must be a real (empty) text, a zeroed one would be copied through a
    // null pointer. Containers are copied bit for bit: the call only reads them and the frame copies are the engine's own.
    static FText EmptyText = UKismetTextLibrary::Conv_StringToText(FString(L""));
    std::vector<uint8_t> Params(HandlerFn->GetPropertiesSize() + 64, 0);
    memcpy(Params.data() + HandleOffset, (uint8_t*)Objective + ObjectiveHandleOffset, 32); // FGameplayTagContainer: GameplayTags + ParentTags
    memcpy(Params.data() + TextOffset, &EmptyText, sizeof(FText));
    if (GuidOffset >= 0 && MissionGuidOffset >= 0) memcpy(Params.data() + GuidOffset, (uint8_t*)Mission + MissionGuidOffset, 16);
    if (EventOffset >= 0)
    {
        static int EventNameOffset = FindOffsetStruct("/Script/FortniteGame.FortEventName", "EventName", false);
        if (EventNameOffset >= 0) *(FName*)(Params.data() + EventOffset + EventNameOffset) = UKismetStringLibrary::Conv_StringToName(L"ObjectiveSucceeded");
    }

    LOG_INFO(LogDev, "Zone: raising ObjectiveSucceeded for {} on {}", Objective->GetName(), Mission->GetName());
    Mission->ProcessEvent(HandlerFn, Params.data());
}

static void Zone_Objective_StopObjective(UObject* Objective, uint8_t Status, void* Description, UObject* Badge)
{
    if (!Objective) return;

    static int StatusOffset = Objective->GetOffset("ObjectiveStatus", false);
    if (StatusOffset == -1) { LOG_WARN(LogDev, "Zone Objective: FortObjectiveBase has no ObjectiveStatus"); return; }

    uint8_t* StatusPtr = (uint8_t*)Objective + StatusOffset;
    LOG_INFO(LogDev, "Zone Objective StopObjective: {} -> status {}", Objective->GetFullName(), (int)Status);

    // the tutorial's last objective: the mission succeeds, and the zone end that should follow is server code this exe lacks
    if (Status == 2 && Objective->GetName().contains("OBJ_DefendFort"))
        Zone::BeginEndOfZone();

    if (*StatusPtr != Status)
    {
        *StatusPtr = Status;

        static auto OnRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.OnRep_ObjectiveStatus");
        if (OnRepFn) Objective->ProcessEvent(OnRepFn);

        static auto BpEndFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.BlueprintOnObjectiveEnd");
        if (BpEndFn) Objective->ProcessEvent(BpEndFn, &Status);

        static int OwnerOffset = Objective->GetOffset("Owner");
        if (auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset))
        {
            // the mission's OnObjectiveStatusChanged (a slot with no code to read it from, see ZoneFinder.h); a null vtable must not be called through
            if (Mission->VFTable)
            {
                auto OnObjStatusChanged = (void (*)(UObject*, UObject*, uint8_t))(Mission->VFTable[ZoneFinder::Fixed730::MissionObjectiveStatusChangedSlot / 8]);
                OnObjStatusChanged(Mission, Objective, Status);
            }
            else
            {
                LOG_WARN(LogDev, "Zone Objective: mission has no vtable, status change not forwarded");
            }

            if (Status == 2) // EFortObjectiveStatus::Succeeded
                Zone_RaiseObjectiveSucceeded(Mission, Objective);
        }

        ((AActor*)Objective)->ForceNetUpdate();
    }
}

// 7.30.1: exe+0x2b1cf0 is the server-only "world -> AIDirector" helper, compiled to `xor eax,eax; ret` on this client build (and identical-code-folded with other null stubs,
// so the function itself cannot be hooked). Every real caller does `call [rax+0x140] (GetWorld); mov rcx,rax; call 0x2b1cf0`, so those call sites alone are redirected to
// a stub that returns GameMode->AIDirector (found with fnmod/re/d730.py; this is what made "StartMissionAIEncounterFromGeneratedProfile: No AI Director!").
static UObject* Zone_GetAIDirectorStub(UObject*)
{
    auto World = (UObject*)GetWorld();
    auto GameMode = World ? World->Get<UObject*>("AuthorityGameMode") : nullptr;
    if (!GameMode) return nullptr;

    static auto AIDirectorOffset = GameMode->GetOffset("AIDirector", false);
    return AIDirectorOffset == -1 ? nullptr : GameMode->Get<UObject*>(AIDirectorOffset);
}

// The same folded stub is also the goal manager's getter, so some of the callers found below want the AIGoalManager instead (their callee reads a small object:
// b78fd0 looks up a pawn in a map at this+0x360, the director's own fields start far above that). Handing them the director made a husk pawn fault in that
// map lookup (exe+0xb6b92f) a few seconds after it spawned. Call sites told apart by what they call next (fnmod/re/d730.py, callee offsets).
static UObject* Zone_GetAIGoalManagerStub(UObject*)
{
    auto World = (UObject*)GetWorld();
    auto GameMode = World ? World->Get<UObject*>("AuthorityGameMode") : nullptr;
    if (!GameMode) return nullptr;

    static auto GoalManagerOffset = GameMode->GetOffset("AIGoalManager", false);
    auto GoalManager = GoalManagerOffset == -1 ? nullptr : GameMode->Get<UObject*>(GoalManagerOffset);

    static bool bLogged = false;
    if (!bLogged)
    {
        bLogged = true;
        LOG_INFO(LogDev, "Zone: first goal manager lookup returned {} (AIGoalManager offset {})", GoalManager ? GoalManager->GetFullName() : "NULL", GoalManagerOffset);
    }

    return GoalManager;
}

static void Zone_PatchAIDirectorCallers()
{
    if (Fortnite_Version < 7.3 || Fortnite_Version >= 8.0) return;

    static bool bDone = false;
    if (bDone) return;
    bDone = true;

    auto Base = (uint8_t*)GetModuleHandleW(0);
    auto Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    const uint64_t ImageSize = Nt->OptionalHeader.SizeOfImage;
    // the folded "return nullptr" getter, found through a goal manager call site (ZoneFinder.h)
    const uint64_t Target = ZoneFinder::AIManagerLookupStub();
    if (!Target)
    {
        LOG_WARN(LogDev, "Zone: the AI manager lookup stub was not found, AI director redirect is OFF");
        return;
    }

    // a stub within +-2GB of the exe so a rel32 call can reach it: mov rax, imm64; jmp rax
    uint8_t* Stub = nullptr;
    for (uint64_t Try = ((uint64_t)Base + ImageSize + 0xFFFF) & ~0xFFFFull; Try < (uint64_t)Base + 0x70000000ull && !Stub; Try += 0x10000)
        Stub = (uint8_t*)VirtualAlloc((void*)Try, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!Stub)
    {
        LOG_ERROR(LogDev, "Zone: could not allocate a stub near the exe for the AI director callers");
        return;
    }

    Stub[0] = 0x48; Stub[1] = 0xB8;
    *(uint64_t*)(Stub + 2) = (uint64_t)&Zone_GetAIDirectorStub;
    Stub[10] = 0xFF; Stub[11] = 0xE0;

    uint8_t* GoalStub = Stub + 16;
    GoalStub[0] = 0x48; GoalStub[1] = 0xB8;
    *(uint64_t*)(GoalStub + 2) = (uint64_t)&Zone_GetAIGoalManagerStub;
    GoalStub[10] = 0xFF; GoalStub[11] = 0xE0;

    // call sites (found by signature, ZoneFinder.h) whose callee works on the goal manager (the assignment registration, MakeGoalSelectionQuery, ...)
    const uint64_t* GoalManagerSites = nullptr;
    const int GoalManagerCount = ZoneFinder::GoalManagerSites(GoalManagerSites);

    // More callers pass a world-context object straight through (`mov rcx, rbx; call`) instead of the GetWorld() result, so the pattern below misses them. One of
    // them fed the encounter's target list: left null, an encounter that has target actors (the defend-the-fort ones) faulted and was left with no spawn
    // points. Same sorting: the callee reads the goal manager's fields (+0x338/+0x348/+0x360) or the director's (past 0xc80).
    const uint64_t* ContextGoalManagerSites = nullptr;
    const int ContextGoalManagerCount = ZoneFinder::ContextGoalManagerSites(ContextGoalManagerSites);
    const uint64_t* ContextDirectorSites = nullptr;
    const int ContextDirectorCount = ZoneFinder::ContextDirectorSites(ContextDirectorSites);
    int PatchedGoal = 0;

    int Patched = 0;
    auto Section = IMAGE_FIRST_SECTION(Nt);
    for (int s = 0; s < Nt->FileHeader.NumberOfSections; ++s, ++Section)
    {
        if (!(Section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        auto Start = Base + Section->VirtualAddress;
        const uint64_t Size = Section->Misc.VirtualSize;

        for (uint64_t i = 9; i + 5 <= Size; ++i)
        {
            if (Start[i] != 0xE8) continue;

            const uint64_t Dest = (uint64_t)(Start + i + 5) + *(int32_t*)(Start + i + 1);
            if (Dest != Target) continue;

            const uint64_t Site = (uint64_t)(Start + i);

            // call [rax+0x140]; mov rcx, rax; call
            static const uint8_t Pre[9] = { 0xFF, 0x90, 0x40, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xC8 };
            const bool bGetWorldForm = memcmp(Start + i - 9, Pre, 9) == 0;

            bool bGoalManager = false, bListedContextSite = false;
            for (int k = 0; k < GoalManagerCount; ++k) bGoalManager |= bGetWorldForm && GoalManagerSites[k] == Site;
            for (int k = 0; k < ContextGoalManagerCount; ++k) { if (ContextGoalManagerSites[k] == Site) bGoalManager = bListedContextSite = true; }
            for (int k = 0; k < ContextDirectorCount; ++k) bListedContextSite |= ContextDirectorSites[k] == Site;

            // the folded stub has thousands of unrelated callers: only the GetWorld() form and the listed context-object sites are managers
            if (!bGetWorldForm && !bListedContextSite) continue;

            DWORD Old = 0;
            if (VirtualProtect(Start + i + 1, 4, PAGE_EXECUTE_READWRITE, &Old))
            {
                *(int32_t*)(Start + i + 1) = (int32_t)((int64_t)(bGoalManager ? GoalStub : Stub) - (int64_t)(Start + i + 5));
                VirtualProtect(Start + i + 1, 4, Old, &Old);
                ++Patched;
                PatchedGoal += bGoalManager ? 1 : 0;
            }
        }
    }

    // zero matches means the byte pattern is not on this build: AI director lookups then stay on the stub-less path
    if (Patched == 0)
    {
        LOG_WARN(LogDev, "Zone: no AI director call sites matched the 7.30.1 pattern, AI director redirect is OFF");
        return;
    }

    LOG_INFO(LogDev, "Zone: redirected {} AI manager lookups ({} to GameMode->AIGoalManager, the rest to GameMode->AIDirector)", Patched, PatchedGoal);
}

// The Fort replication graph has no route for FortQuickBars (class policy 0, not routed; FortInventory is too but the graph collects the controller's WorldInventory by
// interface), so a quickbars actor would never reach the client and ClientRestart waits for it forever. Policy 1 = relevant to all connections. The graph's
// ClassRepNodePolicies map and its lookup are found by signature (ZoneFinder.h: the lookup's caller passes the map with lea rcx, [graph+offset]).
static void Zone_RouteQuickBars()
{
    auto GetPolicy = (int* (*)(void*, UObject*))ZoneFinder::ClassRepNodePolicyLookup();
    const int PoliciesOffset = ZoneFinder::ClassRepNodePoliciesOffset();
    if (!GetPolicy || PoliciesOffset < 0)
    {
        LOG_WARN(LogDev, "Zone: the replication graph's class policy lookup was not found, quickbars cannot be routed.");
        return;
    }

    auto World = (UObject*)GetWorld();
    auto Driver = World ? World->Get<UObject*>("NetDriver") : nullptr;
    auto Graph = Driver ? Driver->Get<UObject*>("ReplicationDriver") : nullptr;

    if (!Graph)
    {
        LOG_INFO(LogDev, "Zone: no replication graph on the net driver, quickbars cannot be routed.");
        return;
    }

    for (auto Path : { L"/Script/FortniteGame.FortQuickBars", L"/Script/FortniteGame.FortQuickBarsAthena" })
    {
        auto Class = FindObject<UClass>(Path);
        if (!Class) continue;

        auto Policy = GetPolicy((char*)Graph + PoliciesOffset, Class);
        if (!Policy) continue;

        LOG_INFO(LogDev, "Zone: replication policy of {} was {}, now relevant to all connections", Class->GetName(), *Policy);
        *Policy = 1;
    }
}
// STW build unlock: the tutorial's build steps are gated on the Zone GameState's layout-requirement
// flags (FortGameStateZone.SetBuildingAllowed keeps the client from placing the ghost pieces the
// mission script asks for), so the server flips both to allowed once the Zone world is up
static void Zone_AllowBuilding()
{
    auto GameState = ((UObject*)GetWorld())->Get<UObject*>("GameState");

    if (!GameState)
    {
        LOG_WARN(LogDev, "Zone: build unlock skipped, no GameState");
        return;
    }

    static auto SetBuildingAllowedFn = FindObject<UFunction>(L"/Script/FortniteGame.FortGameStateZone.SetBuildingAllowed");

    if (!SetBuildingAllowedFn)
    {
        LOG_WARN(LogDev, "Zone: build unlock skipped, SetBuildingAllowed not found");
        return;
    }

    struct { bool bBuildingWithoutLayoutRequirementsAllowed; bool bBuildingAtLayoutRequirementsAllowed; } Params{ true, true };
    GameState->ProcessEvent(SetBuildingAllowedFn, &Params);
    LOG_INFO(LogDev, "Zone: called FortGameStateZone.SetBuildingAllowed on {} (both requirements allowed)", GameState->GetFullName());
}

// The engine creates the AI system (behavior tree, perception and environment query managers) when a world is initialized, and the log shows it did so for the
// Frontend world only ("Creating AISystem for world Frontend"). Without it the AI director's rift manager has no environment query manager to find spawn rifts with:
// its encounters stay in EFortEncounterState::InitializingRiftManager for good, which is why the fort's waves never spawned. Make it when the world has none.
static void Zone_EnsureAISystem()
{
    auto World = (UObject*)GetWorld();
    if (!World) return;

    static int AISystemOffset = World->GetOffset("AISystem", false);
    if (AISystemOffset == -1)
    {
        LOG_WARN(LogDev, "Zone: UWorld has no AISystem property here");
        return;
    }

    auto Existing = World->Get<UObject*>(AISystemOffset);
    if (Existing)
    {
        auto Queries = Existing->Get<UObject*>("EnvironmentQueryManager");
        LOG_INFO(LogDev, "Zone: world already has AI system {} (environment query manager {})", Existing->GetFullName(), Queries ? "present" : "MISSING");
        return;
    }

    static auto SystemClass = FindObject<UClass>(L"/Script/FortniteGame.FortAISystem");
    if (!SystemClass)
    {
        LOG_WARN(LogDev, "Zone: FortAISystem class not found, no AI system created");
        return;
    }

    auto System = UGameplayStatics::SpawnObject(SystemClass, World);
    if (!System)
    {
        LOG_WARN(LogDev, "Zone: could not create the AI system");
        return;
    }

    World->Get<UObject*>(AISystemOffset) = System;
    auto Queries = System->Get<UObject*>("EnvironmentQueryManager");
    auto Trees = System->Get<UObject*>("BehaviorTreeManager");
    LOG_INFO(LogDev, "Zone: created AI system {} (environment query manager {}, behavior tree manager {})", System->GetFullName(), Queries ? "present" : "MISSING", Trees ? "present" : "MISSING");
}

// The storm shield core on the outpost. Its mission (MissionInfo_Outpost_PvE_01, MissionPlacementInfo) has the world manager place
// BP_StormShield_Core_Egg at the FortPlacementActor tagged Outpost.PlacementActor.Egg (OutpostPlacementQuery_CoreEgg), but that runs for the
// missions of the world record only, and this mission is loaded without a record ("MissionRecrod not found! Cannot initializea actors."):
// the player was told to activate a storm shield that was not there (2026-11-07). Mission_Outpost's InitBuildingData looks for an egg actor in
// the world when initialization gave none ("Found a StormShield Egg actor even though it was not returned by mission initialization ... we
// should be able to continue"), so spawning it at that placement actor before the mission starts is enough.
static void Zone_SpawnOutpostCoreEgg()
{
    static auto EggClass = LoadObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_StormShield_Core_Egg.BP_StormShield_Core_Egg_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
    static auto PlacementClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlacementActor");
    if (!EggClass || !PlacementClass) { LOG_WARN(LogDev, "Zone: storm shield core: class missing (egg {}, placement actor {})", (bool)EggClass, (bool)PlacementClass); return; }

    auto Eggs = UGameplayStatics::GetAllActorsOfClass(GetWorld(), EggClass);
    const int ExistingEggs = Eggs.Num();
    Eggs.Free();
    if (ExistingEggs > 0) { LOG_INFO(LogDev, "Zone: storm shield core already there"); return; }

    const auto EggTag = UKismetStringLibrary::Conv_StringToName(L"Outpost.PlacementActor.Egg");
    AActor* Placement = nullptr;
    auto Placements = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlacementClass);
    for (int i = 0; i < Placements.Num() && !Placement; ++i)
    {
        auto Candidate = Placements.at(i);
        static auto TagsOffset = Candidate->GetOffset("StaticGameplayTags", false);
        if (TagsOffset == -1) break;
        auto& Tags = Candidate->Get<TArray<FName>>(TagsOffset); // FGameplayTagContainer: GameplayTags (FGameplayTag = one FName) first
        for (int t = 0; t < Tags.Num(); ++t)
            if (Tags.at(t).ComparisonIndex.Value == EggTag.ComparisonIndex.Value) { Placement = Candidate; break; }
    }
    const int PlacementCount = Placements.Num();
    Placements.Free();

    if (!Placement) { LOG_WARN(LogDev, "Zone: storm shield core: none of the {} placement actors is tagged Outpost.PlacementActor.Egg", PlacementCount); return; }

    FTransform Transform = Placement->GetTransform();
    Transform.Scale3D = FVector(1, 1, 1);
    auto Egg = GetWorld()->SpawnActor<AActor>(EggClass, Transform);
    const FVector At = Transform.Translation;
    LOG_INFO(LogDev, "Zone: storm shield core {} spawned at {} ({:.0f}, {:.0f}, {:.0f})", Egg ? Egg->GetName() : "NOT", Placement->GetName(), At.X, At.Y, At.Z);
}

// Starts playing every mission the Zone's mission manager has (UFortMissionManager::StartPlayingMission); nothing on this build starts them by itself.
static void Zone_StartLoadedMissions()
{
    auto ZoneGameState = ((UObject*)GetWorld())->Get<UObject*>("GameState");
    auto MissionMgr = ZoneGameState ? ZoneGameState->Get<UObject*>("MissionManager") : nullptr;
    if (!MissionMgr) return;

    static int MissionsOffset = MissionMgr->GetOffset("Missions", false);
    if (MissionsOffset == -1) return;

    auto& Missions = MissionMgr->Get<TArray<UObject*>>(MissionsOffset);
    auto StartPlayingMission = (void (*)(UObject*, UObject*))(MissionMgr->VFTable[ZoneFinder::Fixed730::MissionManagerStartPlayingSlot / 8]);
    for (int i = 0; i < Missions.Num(); ++i)
    {
        if (auto Mission = Missions.at(i))
        {
            StartPlayingMission(MissionMgr, Mission);
            static int MissionStatusOffset = Mission->GetOffset("MissionStatus");
            LOG_INFO(LogDev, "Zone: started mission {} (status {})", Mission->GetFullName(), (int)*((uint8_t*)Mission + MissionStatusOffset));
        }
    }
}

// Listens once for the Zone world: with the replication graph switched on first (the Zone game mode ships with it off and only the graph delivers actors on this client exe),
// then the quickbars routing. Two Listen calls would make two net drivers with the same name in the world, one of them without the graph.
static bool Zone_ListenOnce()
{
    static UObject* ListenedWorld = nullptr;
    auto World = (UObject*)GetWorld();

    if (!World || World == ListenedWorld) return false;
    ListenedWorld = World;

    // a new world means the husks and tutorial state of the last one are gone with it
    Zone::ResetForNewWorld();

    // 1. Patch UWorld::SpawnActor client/server context check (CanCreateInCurrentContext -> NeedsLoadForClient) so server-only
    // classes like AFortAIDirector, AAIHotSpot, and AFortDynamicBuilder can spawn on the client binary acting as a server.
    if (Addresses::SpawnActor)
    {
        uint8_t* ScanStart = (uint8_t*)Addresses::SpawnActor + 0x480;
        for (int i = 0; i < 0x100; ++i)
        {
            if (ScanStart[i] == 0x84 && ScanStart[i + 1] == 0xC0 && ScanStart[i + 2] == 0x75 && ScanStart[i + 3] == 0x3C)
            {
                DWORD OldProtect = 0;
                if (VirtualProtect(&ScanStart[i + 2], 1, PAGE_EXECUTE_READWRITE, &OldProtect))
                {
                    ScanStart[i + 2] = 0xEB; // jmp +0x3c
                    VirtualProtect(&ScanStart[i + 2], 1, OldProtect, &OldProtect);
                    LOG_INFO(LogDev, "Zone: patched UWorld::SpawnActor CanCreateInCurrentContext check at +0x{:x}", 0x480 + i + 2);
                }
                break;
            }
        }
    }

    Zone_PatchAIDirectorCallers();
    Zone_EnsureAISystem();
    Zone::InstallAIControllerPossess();

    // AI crash patches for spawning AI pawns on client binary acting as dedicated server (each place found by signature, ZoneFinder.h):
    if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
    {
        // 1. Patch GetPawnGoalSelectionCriteria null criteria crash (cmp rbx, rdi -> test rbx, rbx)
        uint8_t* Target1 = (uint8_t*)ZoneFinder::GoalCriteriaCompareSite();
        if (Target1 && Target1[0] == 0x48 && Target1[1] == 0x3B && Target1[2] == 0xDF)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target1, 3, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target1[1] = 0x85; // test
                Target1[2] = 0xDB; // rbx, rbx
                VirtualProtect(Target1, 3, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: patched GetPawnGoalSelectionCriteria");
            }
        }

        // 2. Patch AFortAIPawn::BeginPlay AIDirector assignment call (je -> jmp)
        uint8_t* Target2 = (uint8_t*)ZoneFinder::AIPawnBeginPlayAssignSite();
        if (Target2 && Target2[0] == 0x74 && Target2[1] == 0x0B)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target2, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target2[0] = 0xEB; // jmp +0x0b (skip the assignment registration)
                VirtualProtect(Target2, 1, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: patched AFortAIPawn::BeginPlay");
            }
        }

        // 3. Stub the assignment registration to immediately ret
        uint8_t* Target3 = (uint8_t*)ZoneFinder::AssignmentRegistration();
        if (Target3 && Target3[0] == 0x40 && Target3[1] == 0x55)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target3, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target3[0] = 0xC3; // ret
                VirtualProtect(Target3, 1, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: stubbed the assignment registration");
            }
        }

        // 4. Stub MakeGoalSelectionQuery (xor eax, eax; ret)
        uint8_t* Target4 = (uint8_t*)ZoneFinder::MakeGoalSelectionQuery();
        if (Target4 && Target4[0] == 0x48 && Target4[1] == 0x89)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target4, 3, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target4[0] = 0x31; Target4[1] = 0xC0; Target4[2] = 0xC3; // xor eax, eax; ret
                VirtualProtect(Target4, 3, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: stubbed MakeGoalSelectionQuery");
            }
        }
    }

    // 2. Restore stripped WITH_SERVER_CODE virtual methods on AFortObjectiveBase (ActivateObjective, StartPlayingObjective, StopObjective).
    if (auto DefaultObjBase = FindObject<UObject>(L"/Script/FortniteGame.Default__FortObjectiveBase"))
    {
        VirtualSwap(DefaultObjBase->VFTable, ZoneFinder::Fixed730::ObjectiveActivateSlot / 8, Zone_Objective_ActivateObjective);
        VirtualSwap(DefaultObjBase->VFTable, ZoneFinder::Fixed730::ObjectiveStartPlayingSlot / 8, Zone_Objective_StartPlayingObjective);
        VirtualSwap(DefaultObjBase->VFTable, ZoneFinder::Fixed730::ObjectiveStopSlot / 8, Zone_Objective_StopObjective);
        LOG_INFO(LogDev, "Zone: hooked AFortObjectiveBase virtual methods (ActivateObjective, StartPlayingObjective, StopObjective)");
    }

    if (auto GameMode = World->Get<UObject*>("AuthorityGameMode"))
    {
        static auto EnableGraphOffset = GameMode->GetOffset("bEnableReplicationGraph", false);
        LOG_INFO(LogDev, "Zone game mode bEnableReplicationGraph offset {}, replication mode {}", EnableGraphOffset, RebootConfig::Get().Stw.Replication);
        if (EnableGraphOffset != -1 && RebootConfig::Get().Stw.Replication == "graph") GameMode->Get<bool>(EnableGraphOffset) = true;
    }

    GetWorld()->Listen();

    // the island start used to set this from its own driver; it is the Zone world's driver that matters now
    if (auto ZoneDriver = GetWorld()->GetNetDriver())
    {
        static auto ReplicationDriverOffset = ZoneDriver->GetOffset("ReplicationDriver", false);
        Globals::bShouldUseReplicationGraph = ReplicationDriverOffset != -1 && Fortnite_Version < 20 && Fortnite_Version != 3.3;
        LOG_INFO(LogDev, "Zone: bShouldUseReplicationGraph {}", Globals::bShouldUseReplicationGraph);
    }

    Globals::bStartedListening = true;

    // the reservation beacon and the backend registration the game's own matchmaking needs (ZoneMatchmaking.cpp)
    Zone::StartMatchmaking();

    if (RebootConfig::Get().Stw.Replication == "graph") Zone_RouteQuickBars();

    // AFortGameMode::FinishWorldInitialization is what sets bWorldIsReady, and no Zone game mode on this build calls it (its vtable slot is replaced). The controller's
    // loading tick waits for it before it lets the player finish loading (found with fnmod/re/d730.py, 7.30.1).
    if (auto GameMode = ((UObject*)GetWorld())->Get<UObject*>("AuthorityGameMode"))
    {
        static auto WorldIsReadyOffset = GameMode->GetOffset("bWorldIsReady", false);
        if (WorldIsReadyOffset != -1) SetBitfield(GameMode->GetPtr<PlaceholderBitfield>(WorldIsReadyOffset), 1, true);

        // Set GameDifficulty and create GameState->MissionManager BEFORE loading the mission.
        // Do NOT call StartMatch() here (before a player joins), because Zone_Onboarding_FarmsteadFort_C's OnMatchStarted script
        // expects GetAllFortPlayerControllers()[0] to exist!
        if (auto ZoneGameState = ((UObject*)GetWorld())->Get<UObject*>("GameState"))
        {
            static auto DifficultyOffset = ZoneGameState->GetOffset("GameDifficulty", false);
            if (DifficultyOffset != -1 && ZoneGameState->Get<float>(DifficultyOffset) <= 0.f)
            {
                ZoneGameState->Get<float>(DifficultyOffset) = 1.0f;
                static auto OnRepDiffFn = FindObject<UFunction>(L"/Script/FortniteGame.FortGameStateZone.OnRep_GameDifficulty");
                if (OnRepDiffFn) ZoneGameState->ProcessEvent(OnRepDiffFn);
            }

            auto MissionMgr = ZoneGameState->Get<UObject*>("MissionManager");
            if (!MissionMgr && Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
            {
                alignas(16) uint8_t Record[0x80] = {};
                static auto MgrClassOffset = GameMode->GetOffset("MissionManagerClass", false);
                UObject* MgrClass = MgrClassOffset != -1 ? GameMode->Get<UObject*>(MgrClassOffset) : nullptr;
                auto InitRecord = (void (*)(void*, UObject*))ZoneFinder::InitMissionManagerRecord();
                if (InitRecord)
                {
                    InitRecord(Record, MgrClass);
                    auto LoadMissionMgr = (void (*)(UObject*, void*))(ZoneGameState->VFTable[ZoneFinder::Fixed730::GameStateLoadMissionManagerSlot / 8]);
                    LoadMissionMgr(ZoneGameState, Record);
                }
                MissionMgr = ZoneGameState->Get<UObject*>("MissionManager");
            }
            LOG_INFO(LogDev, "Zone: GameState MissionManager is {}", MissionMgr ? MissionMgr->GetFullName() : "NULL");
        }

        static auto AIDirectorOffset = GameMode->GetOffset("AIDirector", false);
        if (AIDirectorOffset != -1 && !GameMode->Get<UObject*>(AIDirectorOffset))
        {
            // The Zone blueprint director carries the spawn caps, graphs and settings (the bare native class has MaxTotalActiveAliveAI 0, which is
            // what the log showed); another STW server (Limbo) runs the same blueprint. The native class is the fallback.
            static auto DirectorClass = []() -> UClass*
            {
                auto Blueprint = LoadObject<UClass>(L"/Game/AIDirector/AIDirector_Fortnite.AIDirector_Fortnite_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
                LOG_INFO(LogDev, "Zone: director blueprint class {}", Blueprint ? "loaded" : "NOT FOUND, using the native class");
                return Blueprint ? Blueprint : FindObject<UClass>(L"/Script/FortniteGame.FortAIDirector");
            }();
            auto Director = DirectorClass ? GetWorld()->SpawnActor<AActor>(DirectorClass, FVector(), FQuat()) : nullptr;
            if (Director)
            {
                GameMode->Get<UObject*>(AIDirectorOffset) = Director;
                Zone::SyncDirectorDefaults(Director);
                static auto UseProtoOffset = Director->GetOffset("bUsePrototypeEnemies", false);
                if (UseProtoOffset != -1) SetBitfield(Director->GetPtr<PlaceholderBitfield>(UseProtoOffset), 1, true);
                if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
                {
                    if (auto ActivateDirector = (void (*)(UObject*))ZoneFinder::ActivateAIDirector())
                        ActivateDirector(Director);
                }
            }
            LOG_INFO(LogDev, "Zone: AI director {}", Director ? Director->GetFullName() : "could not be created");
        }

        static auto AIGoalManagerOffset = GameMode->GetOffset("AIGoalManager", false);
        if (AIGoalManagerOffset != -1 && !GameMode->Get<UObject*>(AIGoalManagerOffset))
        {
            static auto GoalMgrClass = []() -> UClass*
            {
                auto Blueprint = LoadObject<UClass>(L"/Game/AI/GoalSelection/AIGoalManager.AIGoalManager_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
                return Blueprint ? Blueprint : FindObject<UClass>(L"/Script/FortniteGame.FortAIGoalManager");
            }();
            auto GoalMgr = GoalMgrClass ? GetWorld()->SpawnActor<AActor>(GoalMgrClass, FVector(), FQuat()) : nullptr;
            if (GoalMgr)
            {
                GameMode->Get<UObject*>(AIGoalManagerOffset) = GoalMgr;
                static auto SetActorTickEnabledFn = FindObject<UFunction>(L"/Script/Engine.Actor.SetActorTickEnabled");
                if (SetActorTickEnabledFn)
                {
                    struct { bool bEnabled; } Params{ false };
                    GoalMgr->ProcessEvent(SetActorTickEnabledFn, &Params);
                }
            }
            LOG_INFO(LogDev, "Zone: AI goal manager {}", GoalMgr ? GoalMgr->GetFullName() : "could not be created");
        }

        // The outpost's storm shield core has to be in the world before its mission loads: Mission_Outpost's InitMission looks for it then
        // (InitBuildingData -> StoreStormShieldEggAndPingLocation -> SaveLocationofOutpostEgg sets EggLocation, where the shield dome is spawned).
        // Spawned at the match start it came too late: EggLocation stayed (0, 0, 0), the dome stood at the map origin with the core near its
        // edge and looked small from there (2026-11-07).
        {
            static auto OutpostGameModeClass = FindObject<UClass>(L"/Script/FortniteGame.FortGameModeOutpost");
            if (OutpostGameModeClass && GameMode->IsA(OutpostGameModeClass)) Zone_SpawnOutpostCoreEgg();
        }

        // The primary mission comes from the mission generator of the mission's tile (MissionGen_TheOutpost_PARENT lists MissionInfo_Outpost_PvE_01
        // for Storm Shield Defense 1); the backend that starts this server passes it as SaveTheWorld.Mission. The tutorial map keeps its own mission.
        std::string MissionPath = RebootConfig::Get().Stw.Mission;
        if (MissionPath.empty() && RebootConfig::Get().Stw.Map.find("Onboarding") != std::string::npos)
            MissionPath = "/Game/Missions/Primary/OB_FarmsteadFort/OB_FarmsteadFort.OB_FarmsteadFort";

        if (!MissionPath.empty())
        {
            const std::wstring WideMissionPath = RebootConfig::Widen(MissionPath);
            auto MissionInfo = LoadObject<UObject>(WideMissionPath.c_str(), FindObject<UClass>(L"/Script/FortniteGame.FortMissionInfo"));
            static auto LoadMissionFn = FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary.LoadMission");
            static auto MissionLibrary = FindObject<UObject>(L"/Script/FortniteGame.Default__FortMissionLibrary");

            if (MissionInfo && LoadMissionFn && MissionLibrary)
            {
                Zone::SetMissionInfo(MissionInfo);
                alignas(16) unsigned char Params[64] = {};
                *(UObject**)(Params) = (UObject*)GetWorld();
                *(UObject**)(Params + 8) = MissionInfo;
                MissionLibrary->ProcessEvent(LoadMissionFn, Params);
                LOG_INFO(LogDev, "Zone: mission {} loaded", MissionInfo->GetFullName());
            }
            else LOG_ERROR(LogDev, "Zone: could not load the mission (info {}, function {}, library {})", (bool)MissionInfo, (bool)LoadMissionFn, (bool)MissionLibrary);
        }

        // Start playing all loaded missions now so OB_FarmsteadFort_C has MissionStatus == InProgress (1) before the player spawns. Not on the
        // outpost: Mission_Outpost chooses its leg when it starts (InitMission asks for the outpost owner's quest config). Started here, with
        // nobody in yet, it found no quest, put the storm shield up and skipped the intro movie and the tutorial dialogue (2026-11-07); it is
        // started with the match instead, once the owner's quests are on the server (Zone_TickPendingMatchStart).
        static auto OutpostGameModeClass = FindObject<UClass>(L"/Script/FortniteGame.FortGameModeOutpost");
        if (OutpostGameModeClass && GameMode->IsA(OutpostGameModeClass))
        {
            LOG_INFO(LogDev, "Zone: outpost missions wait for the match start");
        }
        else
        {
            Zone_StartLoadedMissions();
        }

        // Drain WorldSettings->WorldReadyActors so AFortAIDirector::WorldReady, AFortMission::OnWorldReady, and AFortObjectiveBase::OnWorldReady execute!
        if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
        {
            if (auto PersistentLevel = ((UObject*)GetWorld())->Get<UObject*>("PersistentLevel"))
            {
                if (auto WorldSettings = PersistentLevel->Get<UObject*>("WorldSettings"))
                {
                    if (auto NotifyWorldReady = (void (*)(UObject*))ZoneFinder::NotifyWorldReady())
                    {
                        NotifyWorldReady(WorldSettings);
                        LOG_INFO(LogDev, "Zone: called AFortWorldSettings::NotifyWorldReady on {}", WorldSettings->GetFullName());
                    }
                }
            }
        }
    }

    return true;
}



static bool Zone_ReadyToStartMatchHook(UObject* GameMode)
{
    if (Zone_ListenOnce())
        LOG_INFO(LogDev, "Zone game mode is waiting for the match: listening.");

    return Zone_ReadyToStartMatchOriginal(GameMode);
}

// Runs every frame on the game thread (the engine asks for the tick rate). When the server has travelled to a Zone (Save the World) map it starts listening
// a moment after the map came up, the same thing the Athena ReadyToStartMatch hook does for Battle Royale.
// Zone tracing (only when [SaveTheWorld] Map is set): logs calls of the world manager's load machine. The functions are found by signature (ZoneFinder.h).
static __int64 (*Trace_RetrieveZoneCompleteOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_RetrieveTheaterCompleteOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_CreateWorldRecordOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_InitWorldRecordCompleteOriginal)(__int64, __int64, __int64, __int64);

static __int64 Trace_RetrieveZoneCompleteHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace OnRetrieveZoneInformationComplete");
    return Trace_RetrieveZoneCompleteOriginal(A, B, C, D);
}

static __int64 Trace_RetrieveTheaterCompleteHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace OnRetrieveTheaterInformationComplete");
    return Trace_RetrieveTheaterCompleteOriginal(A, B, C, D);
}

static __int64 Trace_CreateWorldRecordHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    static int WorldIdOffset = FindOffsetStruct("/Script/FortniteGame.FortZoneInstanceInfo", "WorldId", false);
    LOG_INFO(LogDev, "ZoneTrace CreateNewWorldRecord (info WorldId.Num {})", WorldIdOffset >= 0 && B ? ((FString*)(B + WorldIdOffset))->Data.Num() : -1);

    // B is the zone instance info (FFortZoneInstanceInfo). Without a zone theme the world manager runs with no theme and places no player
    // spawn pad (Storm Shield Defense 1: "theme NONE", the player spawned over nothing). The theme cannot come in the URL (zoneThemeClass
    // there makes the Zone fetch the theater from the backend, which fails: "Unable to find service permissions for server"), so it is
    // written into the info this record is made from.
    // only the Zone's record: the island the server starts on makes one first
    const std::string& ZoneTheme = RebootConfig::Get().Stw.ZoneTheme;
    if (!ZoneTheme.empty() && B && GetWorld() && GetWorld()->GetName().starts_with("Zone_"))
    {
        static auto ThemeOffset = FindOffsetStruct("/Script/FortniteGame.FortZoneInstanceInfo", "ZoneThemeClass", false);
        if (ThemeOffset >= 0)
        {
            auto& Theme = *(TSoftObjectPtr<UObject>*)(B + ThemeOffset);
            if (!Theme.SoftObjectPtr.ObjectID.AssetPathName.ComparisonIndex.Value)
            {
                static const std::wstring WideTheme = RebootConfig::Widen(ZoneTheme);
                Theme.SoftObjectPtr.ObjectID.AssetPathName = UKismetStringLibrary::Conv_StringToName(WideTheme.c_str());
                LOG_INFO(LogDev, "ZoneTrace CreateNewWorldRecord: zone theme set to {} (at +0x{:x})", ZoneTheme, ThemeOffset);
            }
        }
        else LOG_WARN(LogDev, "ZoneTrace CreateNewWorldRecord: FortZoneInstanceInfo has no ZoneThemeClass, theme not set");

        // The record alone left the world manager's Theme empty (it is filled from the theater data a real server gets from the backend), and
        // the world manager builds the zone right after this. So the theme object goes onto the world manager and the zone game state too:
        // the theme class's default object.
        static auto ThemeClass = LoadObject<UClass>(RebootConfig::Widen(ZoneTheme).c_str(), FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
        UObject* ThemeObject = ThemeClass ? ThemeClass->CreateDefaultObject() : nullptr;

        static auto WorldManagerClass = FindObject<UClass>(L"/Script/FortniteGame.FortWorldManager");
        auto WorldManagers = WorldManagerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), WorldManagerClass) : TArray<AActor*>();
        UObject* WorldManager = WorldManagers.Num() > 0 ? WorldManagers.at(0) : nullptr;
        WorldManagers.Free();

        if (ThemeObject && WorldManager)
        {
            static auto WorldManagerThemeOffset = WorldManager->GetOffset("Theme");
            if (!WorldManager->Get<UObject*>(WorldManagerThemeOffset)) WorldManager->Get<UObject*>(WorldManagerThemeOffset) = ThemeObject;
        }

        UObject* GameState = ((UObject*)GetWorld())->Get<UObject*>("GameState");
        static auto GameStateThemeOffset = GameState ? GameState->GetOffset("ZoneTheme", false) : -1;
        if (ThemeObject && GameState && GameStateThemeOffset >= 0 && !GameState->Get<UObject*>(GameStateThemeOffset)) GameState->Get<UObject*>(GameStateThemeOffset) = ThemeObject;

        LOG_INFO(LogDev, "ZoneTrace CreateNewWorldRecord: theme object {} given to world manager {} and game state {}", ThemeObject ? ThemeObject->GetName() : "NONE (class did not load)",
            WorldManager ? WorldManager->GetName() : "NONE", GameState && GameStateThemeOffset >= 0 ? GameState->GetName() : "NONE");
    }
    return Trace_CreateWorldRecordOriginal(A, B, C, D);
}

static __int64 Trace_InitWorldRecordCompleteHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace GameModeZone::OnInitWorldRecordComplete");
    return Trace_InitWorldRecordCompleteOriginal(A, B, C, D);
}

static void* (*GetPawnGoalSelectionCriteriaOriginal)(void* FortAIPawn, void* CriteriaList) = nullptr;
static void* GetPawnGoalSelectionCriteriaHook(void* FortAIPawn, void* CriteriaList)
{
    static uint8_t FallbackCriteria[0x100] = { 0 };
    if (!FortAIPawn || !CriteriaList)
        return FallbackCriteria;

    void* Data = *(void**)CriteriaList;
    int Num = *(int*)((char*)CriteriaList + 8);
    if (!Data || Num <= 0)
        return FallbackCriteria;

    return GetPawnGoalSelectionCriteriaOriginal(FortAIPawn, CriteriaList);
}

static void InstallZoneTrace();
static void Zone_InstallStwFixHooks(); // defined right below InstallZoneTrace

static void InstallZoneTrace()
{
    // the world record callbacks, found by signature (ZoneFinder.h; the signatures are 7.30's, and ZoneFinder answers 0 on other builds)
    if (auto A = ZoneFinder::OnRetrieveZoneInformationComplete()) Hooking::MinHook::Hook((PVOID)A, Trace_RetrieveZoneCompleteHook, (PVOID*)&Trace_RetrieveZoneCompleteOriginal);
    if (auto A = ZoneFinder::OnRetrieveTheaterInformationComplete()) Hooking::MinHook::Hook((PVOID)A, Trace_RetrieveTheaterCompleteHook, (PVOID*)&Trace_RetrieveTheaterCompleteOriginal);
    if (auto A = ZoneFinder::CreateNewWorldRecord()) Hooking::MinHook::Hook((PVOID)A, Trace_CreateWorldRecordHook, (PVOID*)&Trace_CreateWorldRecordOriginal);
    if (auto A = ZoneFinder::OnInitWorldRecordComplete()) Hooking::MinHook::Hook((PVOID)A, Trace_InitWorldRecordCompleteHook, (PVOID*)&Trace_InitWorldRecordCompleteOriginal);
    if (auto A = ZoneFinder::GetPawnGoalSelectionCriteria())
    {
        Hooking::MinHook::Hook((PVOID)A, (PVOID)GetPawnGoalSelectionCriteriaHook, (PVOID*)&GetPawnGoalSelectionCriteriaOriginal);
        LOG_INFO(LogDev, "Zone: GetPawnGoalSelectionCriteria hooked.");
    }
    LOG_INFO(LogDev, "Zone trace hooks installed. Engine_Version={}", Engine_Version);
    Zone_InstallStwFixHooks();
}

// ---------------- Save the World: crafted-item quickbar repair ----------------
// AFortPlayerController::AddItemToQuickBars (native 0x13d4eb0, byte-ported from 7.20 and confirmed against the
// 7.30.1 exe) resolves its argument through the world inventory and answers "could not find specified item"
// when the item is not there. The param dump at install read: ItemDefinition@0 QuickBarType@8 SlotIndex@12 —
// the first argument is the item DEFINITION object, not a guid. The mission uses this call to hand the player
// the crafting schematic (searching the tutorial pit chest pushed SID_Edged_Sword_Medium_C_Ore_T00 here), and
// since the schematic is never in the world inventory on this server, it never lands: no schematic in the
// inventory means the crafting subobjectives never spawn and crafting produces nothing.
// Repair: when the definition is missing from the world inventory, grant it (with the ShouldShowItemToast
// state so the client shows the pickup feed); otherwise place the existing instance's guid into its quickbar.
// The ExecuteInventoryItem hook is deliberately NOT installed: the direct-equip bypass crashed (access
// violation at dll+0x318770) because the crafted item's definition-class layout was assumed, not known.
static void (*Zone_AddItemToQuickBarsOriginal)(UObject* PlayerController, void* Item, bool bUpdate, int Slot) = nullptr;

static void Zone_AddItemToQuickBarsHook(UObject* PlayerController, void* Item, bool bUpdate, int Slot)
{
    Zone_AddItemToQuickBarsOriginal(PlayerController, Item, bUpdate, Slot);
    if (!Item || !PlayerController) return;

    auto MaybeObject = (UObject*)Item;
    static auto DefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
    if (!MaybeObject->IsValidLowLevel() || !MaybeObject->ClassPrivate || !DefinitionClass || !MaybeObject->IsA(DefinitionClass))
        return;

    auto PlayerControllerFort = (AFortPlayerController*)PlayerController;
    auto WorldInventory = PlayerControllerFort->GetWorldInventory();
    if (!WorldInventory) return;

    auto Definition = (UFortItemDefinition*)MaybeObject;

    // Schematics (FortSchematicItemDefinition) are not FortWorldItemDefinitions — they go through a
    // different inventory system and GrantItemWithToast would crash on them. Skip granting schematics
    // here; the mission will need its own schematic-grant path.
    static auto WorldItemDefClass = FindObject<UClass>(L"/Script/FortniteGame.FortWorldItemDefinition");
    if (!Definition->IsA(WorldItemDefClass))
    {
        LOG_WARN(LogDev, "Zone: AddItemToQuickBars skipped non-world-item definition (likely a schematic): {}", Definition->GetPathName());
        return;
    }

    // the native cannot place what the inventory does not hold: grant it (toast + quickbar) and stop
    auto ItemInstance = WorldInventory->FindItemInstance(Definition);
    if (!ItemInstance)
    {
        LOG_INFO(LogDev, "Zone: AddItemToQuickBars got a definition that is not in the world inventory, granting it: {}", Definition->GetPathName());
        Zone::GrantItemWithToast(PlayerControllerFort, Definition, 1);
        return;
    }

    auto Entry = ItemInstance->GetItemEntry();
    if (!Entry) return;
    auto ItemGuid = Entry->GetItemGuid();

    auto QuickBarsOffset = PlayerController->GetOffset("QuickBars", false);
    auto QuickBarsActor = QuickBarsOffset != -1 ? PlayerController->Get<AActor*>(QuickBarsOffset) : nullptr;
    if (!QuickBarsActor || !QuickBarsActor->IsValidLowLevel()) return;
    if (((AFortQuickBars*)QuickBarsActor)->GetSlotIndex(ItemGuid, EFortQuickBars::Primary) >= 0)
        return; // the native did place it (or our earlier repair did); nothing to do

    static auto ServerAddItemInternalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");
    if (ServerAddItemInternalFn)
    {
        struct { FGuid Item; uint8_t InQuickBar; int Slot; } AddParams{ ItemGuid, (uint8_t)(IsPrimaryQuickbar(Definition) ? 0 : 1), -1 };
        QuickBarsActor->ProcessEvent(ServerAddItemInternalFn, &AddParams);
        LOG_INFO(LogDev, "Zone: placed {} into the quickbar after the native add failed (mission requested slot {})", Definition->GetPathName(), Slot);
    }
}

static void Zone_InstallStwFixHooks()
{
    // AFortPlayerController::AddItemToQuickBars (found by signature, ZoneFinder.h)
    auto AddItemToQuickBars = ZoneFinder::AddItemToQuickBars();
    if (!AddItemToQuickBars)
    {
        LOG_WARN(LogDev, "Zone: craft hooks skipped, AddItemToQuickBars was not found (Engine_Version={})", Engine_Version);
        return;
    }

    Hooking::MinHook::Hook((PVOID)AddItemToQuickBars, Zone_AddItemToQuickBarsHook, (PVOID*)&Zone_AddItemToQuickBarsOriginal);
    LOG_INFO(LogDev, "Zone: AddItemToQuickBars repair hook installed.");
}

static void Zone_ApplyHeroSkin(AFortPlayerPawn* Pawn, UObject* Hero)
{
    if (!Pawn || !Hero) return;

    static auto PartsOffset = Hero->GetOffset("CharacterParts", false);
    if (PartsOffset == -1)
    {
        // without this the player keeps the default character: say so, instead of returning silently
        LOG_WARN(LogDev, "Zone: hero {} has no CharacterParts, no STW skin applied", Hero->GetFullName());
        return;
    }

    auto& Parts = Hero->Get<TArray<UObject*>>(PartsOffset);
    TArray<UObject*> CosmeticParts;
    int Counts[(int)EFortCustomPartType::NumTypes] = {};
    int Unknown = 0;
    std::string Names;

    // Each part says what it is (CustomCharacterPart.CharacterPartType, EFortCustomPartType). Sorting by the AdditionalData class missed every
    // body: that class is CustomCharacterBodyPartData on this build, not CustomCharacterBodyData, so the hero's body was never chosen and the
    // pawn kept its default one under the hero's head and hat (legs clipping through, 2026-11-07); face and charm parts were dropped the same way.
    for (int i = 0; i < Parts.Num(); ++i)
    {
        auto Part = Parts.at(i);
        if (!Part) continue;

        CosmeticParts.Add(Part);

        static auto PartTypeOffset = Part->GetOffset("CharacterPartType", false);
        const int Type = PartTypeOffset != -1 ? Part->Get<uint8>(PartTypeOffset) : -1;

        Names += (Names.empty() ? "" : ", ") + Part->GetName() + "=" + std::to_string(Type);

        if (Type >= 0 && Type < (int)EFortCustomPartType::NumTypes)
        {
            Pawn->ServerChoosePart((EFortCustomPartType)Type, Part);
            ++Counts[Type];
        }
        else ++Unknown;
    }

    auto PlayerState = Cast<AFortPlayerState>(Pawn->GetPlayerState());
    if (PlayerState && CosmeticParts.Num() > 0)
    {
        bool bDummy = false;
        UFortKismetLibrary::ApplyCharacterCosmetics(GetWorld(), CosmeticParts, PlayerState, &bDummy);
    }
    else
    {
        LOG_WARN(LogDev, "Zone: skin not applied to cosmetics: player state {} parts {}", PlayerState ? "ok" : "missing", CosmeticParts.Num());
    }
    CosmeticParts.Free();
    // the counts say which slots were actually chosen; "unknown" parts were skipped by type
    LOG_INFO(LogDev, "Zone: applied STW hero skin: head {} body {} hat {} backpack {} charm {} face {} unknown {} (of {} parts: {})", Counts[0], Counts[1], Counts[2], Counts[3],
        Counts[4], Counts[5], Unknown, Parts.Num(), Names);
}

// Where a player's pawn goes: the player start the game mode itself picks (GameModeBase.FindPlayerStart, which the mission's game mode can
// override), the first PlayerStart of the level when it picks none. The first PlayerStart alone was right on the tutorial map, but on Storm
// Shield Defense 1 (Zone_Outpost_Stonewood) it had no ground under it: the player fell through the map at once and again after every respawn
// (2026-11-07). The starts are logged once so a wrong one can be told from the log.
static FVector Zone_StartLocationFor(AActor* Controller)
{
    static auto StartClass = FindObject<UClass>(L"/Script/Engine.PlayerStart");
    auto Starts = StartClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), StartClass) : TArray<AActor*>();

    static bool bListed = false;
    if (!bListed)
    {
        bListed = true;
        for (int i = 0; i < Starts.Num() && i < 24; ++i)
        {
            auto Start = Starts.at(i);
            const FVector Location = Start->GetActorLocation();
            LOG_INFO(LogDev, "Zone: player start {} of {}: {} at ({:.0f}, {:.0f}, {:.0f})", i, Starts.Num(), Start->GetName(), Location.X, Location.Y, Location.Z);
        }
    }

    AActor* Chosen = nullptr;
    UObject* GameMode = GetWorld()->Get<UObject*>("AuthorityGameMode");
    static auto FindPlayerStartFn = FindObject<UFunction>(L"/Script/Engine.GameModeBase.FindPlayerStart");
    if (GameMode && FindPlayerStartFn)
    {
        struct { AActor* Player; FString IncomingName; AActor* ReturnValue; } Params{ Controller, FString(), nullptr };
        GameMode->ProcessEvent(FindPlayerStartFn, &Params);
        Chosen = Params.ReturnValue;
    }

    const char* Source = "the game mode's choice";
    if (!Chosen && Starts.Num() > 0)
    {
        Chosen = Starts.at(0);
        Source = "the first PlayerStart (the game mode chose none)";
    }
    Starts.Free();

    FVector Location{};
    if (Chosen)
    {
        Location = Chosen->GetActorLocation();
        Location.Z += 150;
    }

    // On the outpost the only start is a FortPlayerStart left at the origin with nothing under it: a real server's world generation puts the
    // player spawn (the outpost's teleport pad) where the map's PlayerSpawnPlacementActor stands, (-3584, 0, 768) on Zone_Outpost_Stonewood,
    // and that generation does not run here. So a start at the origin gives way to the placement actor.
    const FVector At = Chosen ? Chosen->GetActorLocation() : FVector{};
    if (!Chosen || (fabs(At.X) < 1 && fabs(At.Y) < 1 && fabs(At.Z) < 1))
    {
        static auto PlacementClass = LoadObject<UClass>(L"/Game/Building/ObjectivePlacementActors/PlayerSpawnPlacementActor.PlayerSpawnPlacementActor_C", FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass"));
        auto Placements = PlacementClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlacementClass) : TArray<AActor*>();
        if (Placements.Num() > 0)
        {
            Location = Placements.at(0)->GetActorLocation();
            Location.Z += 150;
            Source = "the map's player spawn placement (the start was at the origin)";
            Chosen = Placements.at(0);
        }
        Placements.Free();
    }
    LOG_INFO(LogDev, "Zone: pawn start for {}: {} {} at ({:.0f}, {:.0f}, {:.0f})", Controller->GetName(), Source, Chosen ? Chosen->GetName() : "NONE", Location.X, Location.Y, Location.Z);
    return Location;
}

// Save the World: what the Zone game mode does not do for a player on this build. The player needs quickbars and a world inventory (the client waits for them in
// ClientRestart), the match has to be started (the zone info is not valid, so the game mode never starts it) and a pawn has to be spawned and possessed.
static void (*Zone_HandleStartingNewPlayerOriginal)(UObject*, AActor*);

// The possession handshake the server sends when the pawn is possessed can reach the client before it has finished loading and is then dropped (the client keeps no
// AcknowledgedPawn, movement mode None, floats). Repeat ClientRestart a few times, a second or so apart, after the player is set up.
static UObject* gZoneRestartController = nullptr;
static int gZoneRestartFrames = 0;
static int gZoneRestartsSent = 0;
static UFortWeaponItemDefinition* gZonePickaxeDef = nullptr;
static FGuid gZonePickaxeGuid{};
static bool gZoneHeroInitialized = false;

static void Zone_SetUpPlayer(UObject* GameMode, AActor* NewPlayer)
{
    // Players are remembered by address AND object name: a leaving player's controller is freed, and the next player's controller can be handed the same
    // address before this ever runs again (it only runs while a controller exists), so the address alone made a rejoin look already set up and it hung on loading.
    static std::vector<std::pair<AActor*, std::string>> SetUp;

    SetUp.erase(std::remove_if(SetUp.begin(), SetUp.end(), [](const std::pair<AActor*, std::string>& Entry)
    {
        return !Entry.first || !Entry.first->IsValidLowLevel() || Entry.first->GetName() != Entry.second;
    }), SetUp.end());

    if (!NewPlayer || NewPlayer == GetLocalPlayerController()
        || std::find_if(SetUp.begin(), SetUp.end(), [&](const std::pair<AActor*, std::string>& Entry) { return Entry.first == NewPlayer; }) != SetUp.end())
        return;

    SetUp.push_back({ NewPlayer, NewPlayer->GetName() });
    LOG_INFO(LogDev, "Zone: setting up the player {}", NewPlayer->GetName());

    // Enable building for this player immediately (STW building is on from the start, not gated by mission). This used to also set the
    // controller's bBuildFree, but that is the free-build cheat flag: nothing ever cost materials (wood stuck at 30). Pieces are paid for
    // in ServerCreateBuildingActorHook's Zone branch now.
    Zone_AllowBuilding();

    auto SpawnOwned = [&](const wchar_t* ClassPath, const char* Property) -> AActor*
    {
        const int Offset = NewPlayer->GetOffset(Property, false);
        if (Offset == -1) return nullptr;
        if (auto Existing = NewPlayer->Get<AActor*>(Offset)) return Existing;

        auto Class = FindObject<UClass>(ClassPath);
        if (!Class) { LOG_ERROR(LogDev, "Zone: class {} not found", Property); return nullptr; }

        FTransform Transform;
        Transform.Rotation = FQuat(0, 0, 0, 1);
        Transform.Scale3D = FVector(1, 1, 1);
        auto Actor = GetWorld()->SpawnActor<AActor>(Class, Transform);
        if (!Actor) { LOG_ERROR(LogDev, "Zone: could not spawn {}", Property); return nullptr; }

        Actor->SetOwner(NewPlayer);
        NewPlayer->Get<AActor*>(Offset) = Actor;

        static auto RemoteRoleOffset = Actor->GetOffset("RemoteRole", false);
        LOG_INFO(LogDev, "Zone: spawned {} (replicates {}, remote role {})", Property, Actor->DoesReplicate(), RemoteRoleOffset == -1 ? -1 : (int)Actor->Get<uint8_t>(RemoteRoleOffset));
        return Actor;
    };

    // on this build the game creates the quickbars and sets up the inventory in ServerReadyToStartMatch (Reboot only spawns the quickbars itself for 2.5 and older)
    {
        static auto ReadyFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerReadyToStartMatch");
        if (ReadyFn) NewPlayer->ProcessEvent(ReadyFn);
        // GetOffset returns -1 for a missing field; reading through that offset would read before the object
        const int QuickBarsOffset = NewPlayer->GetOffset("QuickBars", false);
        AActor* QuickBarsNow = QuickBarsOffset != -1 ? NewPlayer->Get<AActor*>(QuickBarsOffset) : nullptr;
        LOG_INFO(LogDev, "Zone: ServerReadyToStartMatch called, quickbars {}", (__int64)QuickBarsNow);
    }

    SpawnOwned(L"/Script/FortniteGame.FortQuickBars", "QuickBars");
    SpawnOwned(L"/Script/FortniteGame.FortInventory", "WorldInventory");

    {
        static auto InitializedOffset = NewPlayer->GetOffset("bHasInitializedWorldInventory", false);
        if (InitializedOffset != -1) NewPlayer->Get<bool>(InitializedOffset) = true;
    }

    if (auto PlayerState = NewPlayer->Get<UObject*>("PlayerState"))
    {
        static auto OwnerOffset = PlayerState->GetOffset("bIsGameSessionOwner", false);
        if (OwnerOffset != -1) PlayerState->Get<bool>(OwnerOffset) = true;

        static auto OnRepOwner = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerState.OnRep_SessionOwner");
        if (OnRepOwner) PlayerState->ProcessEvent(OnRepOwner);
    }

    // a pawn at the player start the game mode picks
    static auto PawnClass = LoadObject<UClass>(L"/Game/Abilities/Player/Pawns/PlayerPawn_Generic.PlayerPawn_Generic_C", FindObject<UClass>(L"/Script/CoreUObject.Class"));

    FTransform Transform;
    Transform.Rotation = FQuat(0, 0, 0, 1);
    Transform.Scale3D = FVector(1, 1, 1);
    Transform.Translation = Zone_StartLocationFor(NewPlayer);

    if (PawnClass)
    {
        auto Pawn = GetWorld()->SpawnActor<AActor>(PawnClass, Transform);
        if (Pawn)
        {
            ((AController*)NewPlayer)->Possess((APawn*)Pawn);
            LOG_INFO(LogDev, "Zone: pawn spawned and possessed");

            // Jump, interact, sprint and so on are gameplay abilities on this build: the default player ability set goes on the player state's ability system (Reboot does this for
            // Athena players in its ServerAcknowledgePossession hook; the Zone has none, so the list had 3 abilities). Give this BEFORE equipping the pickaxe!
            {
                auto AbilityPlayerState = Cast<AFortPlayerState>(NewPlayer->Get<UObject*>("PlayerState"));
                auto AbilitySet = GetPlayerAbilitySet();
                auto AbilitySystem = AbilityPlayerState ? AbilityPlayerState->GetAbilitySystemComponent() : nullptr;

                if (AbilitySet && AbilitySystem)
                {
                    AbilitySet->GiveToAbilitySystem(AbilitySystem);
                    LOG_INFO(LogDev, "Zone: the default player abilities are given");
                }
                else
                {
                    LOG_ERROR(LogDev, "Zone: could not give the default player abilities");
                }
            }

            // The starter kit. The hero loadout is not set up on this server (no hero, no hero inventory), so the player would have an empty inventory.
            // The pickaxe comes from the mission's ManageInventory() in real STW. Building tools + edit tool are needed for the tutorial.
            // Materials are granted by the mission script via ManageInventory().
            {
                auto Controller = (AFortPlayerController*)NewPlayer;
                auto Inventory = Controller->GetWorldInventory();
                static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");

                if (Inventory)
                {
                    struct FKitItem { const wchar_t* Path; int Count; int QuickBar; int Slot; };
                    // Building tools + edit tool for the tutorial
                    // Pickaxe comes from hero loadout AFTER GetOrInitializeHero runs (~700ms post-join, after cutscene)
                    const FKitItem Kit[] = {
                        { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall", 1, 1, 0 },
                        { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor", 1, 1, 1 },
                        { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W", 1, 1, 2 },
                        { L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS", 1, 1, 3 },
                        { L"/Game/Items/Weapons/BuildingTools/EditTool.EditTool", 1, -1, -1 },
                    };
                    const int KitCount = sizeof(Kit) / sizeof(Kit[0]);

                    for (int k = 0; k < KitCount; k++)
                    {
                        const auto& Entry = Kit[k];
                        auto Definition = LoadObject<UFortItemDefinition>(Entry.Path, ItemDefinitionClass);
                        if (!Definition) { { const std::wstring Wide = Entry.Path; LOG_ERROR(LogDev, "Zone: starter item {} not found", std::string(Wide.begin(), Wide.end())); } continue; }

                        bool bShouldUpdate = false;
                        auto Added = Inventory->AddItem(Definition, &bShouldUpdate, Entry.Count);

                        auto QuickBarsActor = Controller->GetOffset("QuickBars", false) != -1 ? Controller->Get<AActor*>(Controller->GetOffset("QuickBars", false)) : nullptr;
                        static auto ServerAddItemInternalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");

                        if (QuickBarsActor && ServerAddItemInternalFn)
                        {
                            for (auto NewItem : Added.first)
                            {
                                if (Entry.QuickBar >= 0)
                                {
                                    struct { FGuid Item; uint8_t InQuickBar; int Slot; } AddParams{ NewItem->GetItemEntry()->GetItemGuid(), (uint8_t)Entry.QuickBar, Entry.Slot };
                                    QuickBarsActor->ProcessEvent(ServerAddItemInternalFn, &AddParams);
                                }
                            }
                        }

                        if (bShouldUpdate)
                            Inventory->Update();
                    }

                    LOG_INFO(LogDev, "Zone: starter kit given (building tools + edit tool)");

                    // and what the player carried out of their last mission
                    Zone::LoadBackpack(Controller);
                }

                // In STW, do not apply Athena Ramirez (CID_001_Athena_Commando_F_Default) because its BR body/pants mesh conflicts with the STW hero.
                // The STW hero skin is applied via Zone_ApplyHeroSkin once GetOrInitializeHero resolves the campaign profile hero.
            }

            // the server only accepts a client's movement (ServerMove) once the controller's AcknowledgedPawn is the pawn; the client's ServerAcknowledgePossession never gets through
            // here, and the client then piles up saved moves ("Hit limit of 96 saved moves") and cannot walk
            {
                static auto AcknowledgedPawnOffset = NewPlayer->GetOffset("AcknowledgedPawn", false);
                if (AcknowledgedPawnOffset != -1) NewPlayer->Get<AActor*>(AcknowledgedPawnOffset) = Pawn;
            }

            // The Zone loading screen stays up until the server marks the player as having finished loading (bHasServerFinishedLoading, replicated; the client then answers with its
            // own flag and the player state's). The native loading tick never gets there because there is no zone data, so do what it ends with. 7.30.1: AFortPlayerController::SetHasFinishedLoading.
            if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
            {
                if (auto SetHasFinishedLoading = (void (*)(UObject*, bool))ZoneFinder::SetHasFinishedLoading())
                {
                    SetHasFinishedLoading(NewPlayer, true);
                    LOG_INFO(LogDev, "Zone: the player is marked as having finished loading");
                }
            }

            if (!gZoneMatchStarted)
            {
                gZoneMatchStarted = true;

                // Storm Shield Defense chooses its leg when the match starts (Mission_Outpost OnMatchStarted asks for the outpost owner's quest
                // config). Started here the owner's quests were not on the server yet: the mission saw no active quest, put the storm shield
                // up and skipped the intro movie and the tutorial dialogue (2026-11-07; the config came ~4 s later, after the profile loaded).
                // So on the outpost the start waits for that config (Zone_TickPendingMatchStart); the tutorial starts at once as before.
                static auto OutpostGameModeClass = FindObject<UClass>(L"/Script/FortniteGame.FortGameModeOutpost");
                if (OutpostGameModeClass && GameMode->IsA(OutpostGameModeClass))
                {
                    gZoneMatchStartPending = true;
                    gZoneMatchStartRequestedAt = std::chrono::steady_clock::now();
                    LOG_INFO(LogDev, "Zone: outpost match start waits for the owner's quest config");
                }
                else
                {
                    static auto StartMatchFn = FindObject<UFunction>(L"/Script/Engine.GameMode.StartMatch");
                    if (StartMatchFn) GameMode->ProcessEvent(StartMatchFn);
                    LOG_INFO(LogDev, "Zone: match started after player setup");
                }
            }

            gZoneRestartController = NewPlayer;
            gZoneRestartFrames = 0;
            gZoneRestartsSent = 0;
            gZoneHeroInitialized = false; // each joining player needs the hero, skin and abilities, not only the first
        }
        else LOG_ERROR(LogDev, "Zone: could not spawn the pawn");
    }
    else LOG_ERROR(LogDev, "Zone: pawn class not found");
}

static void Zone_HandleStartingNewPlayerHook(UObject* GameMode, AActor* NewPlayer)
{
    Zone_HandleStartingNewPlayerOriginal(GameMode, NewPlayer);
    Zone_SetUpPlayer(GameMode, NewPlayer);
}

// A player's pawn after a death. The Zone game mode respawns a dead player by itself (RestartPlayerAtPlayerStart about a second after
// PlayerDied), but everything Zone_SetUpPlayer does for the first pawn (AcknowledgedPawn, the default abilities, "finished loading", the
// repeated ClientRestart, the hero with its skin and the pickaxe) was missing for that one, and the client sat on the respawn loading bar
// (2026-11-07). When the game mode does not respawn the player within RespawnTimeoutSeconds, a pawn is spawned here like at the join.
static void Zone_SetUpRespawnedPawn(AActor* Controller, AActor* Pawn)
{
    static auto AcknowledgedPawnOffset = Controller->GetOffset("AcknowledgedPawn", false);
    if (AcknowledgedPawnOffset != -1) Controller->Get<AActor*>(AcknowledgedPawnOffset) = Pawn;

    auto AbilityPlayerState = Cast<AFortPlayerState>(Controller->Get<UObject*>("PlayerState"));
    auto AbilitySet = GetPlayerAbilitySet();
    auto AbilitySystem = AbilityPlayerState ? AbilityPlayerState->GetAbilitySystemComponent() : nullptr;
    if (AbilitySet && AbilitySystem) AbilitySet->GiveToAbilitySystem(AbilitySystem);

    if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
    {
        if (auto SetHasFinishedLoading = (void (*)(UObject*, bool))ZoneFinder::SetHasFinishedLoading())
            SetHasFinishedLoading(Controller, true);
    }

    // the restart loop in Zone_MaxTickRateHook sends ClientRestart again and re-runs the hero (skin, abilities, pickaxe)
    gZoneRestartController = Controller;
    gZoneRestartFrames = 0;
    gZoneRestartsSent = 0;
    gZoneHeroInitialized = false;

    LOG_INFO(LogDev, "Zone: {} respawned as {}: possession, abilities and hero set up again", Controller->GetName(), Pawn->GetName());
}

static AActor* Zone_SpawnPlayerPawnAtStart(AActor* Controller)
{
    static auto PawnClass = LoadObject<UClass>(L"/Game/Abilities/Player/Pawns/PlayerPawn_Generic.PlayerPawn_Generic_C", FindObject<UClass>(L"/Script/CoreUObject.Class"));
    if (!PawnClass) return nullptr;

    FTransform Transform;
    Transform.Rotation = FQuat(0, 0, 0, 1);
    Transform.Scale3D = FVector(1, 1, 1);
    Transform.Translation = Zone_StartLocationFor(Controller);

    auto Pawn = GetWorld()->SpawnActor<AActor>(PawnClass, Transform);
    if (Pawn) ((AController*)Controller)->Possess((APawn*)Pawn);
    return Pawn;
}

static void Zone_WatchRespawn(AActor* Controller)
{
    constexpr double RespawnTimeoutSeconds = 10.0;
    struct FWatch { std::string Name; AActor* Pawn = nullptr; std::chrono::steady_clock::time_point LostAt{}; bool bLost = false; };
    static std::unordered_map<AActor*, FWatch> Watched;

    for (auto It = Watched.begin(); It != Watched.end();)
        It = (!It->first->IsValidLowLevel() || It->first->GetName() != It->second.Name) ? Watched.erase(It) : std::next(It);

    static auto IsDeadFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.IsDead");
    auto Pawn = Controller->Get<AActor*>("Pawn");
    bool bDead = !Pawn;
    if (Pawn && IsDeadFn) { bool bIsDead = false; Pawn->ProcessEvent(IsDeadFn, &bIsDead); bDead = bIsDead; }

    auto Found = Watched.find(Controller);
    if (Found == Watched.end())
    {
        // the first pawn is set up by Zone_SetUpPlayer
        if (Pawn && !bDead) Watched[Controller] = { Controller->GetName(), Pawn };
        return;
    }

    auto& Watch = Found->second;
    const auto Now = std::chrono::steady_clock::now();

    if (Pawn && !bDead && Pawn != Watch.Pawn)
    {
        Watch.Pawn = Pawn;
        Watch.bLost = false;
        Zone_SetUpRespawnedPawn(Controller, Pawn);
        return;
    }

    if (bDead)
    {
        if (!Watch.bLost) { Watch.bLost = true; Watch.LostAt = Now; LOG_INFO(LogDev, "Zone: {} died, waiting for the respawn", Controller->GetName()); }
        else if (std::chrono::duration<double>(Now - Watch.LostAt).count() > RespawnTimeoutSeconds && !Pawn)
        {
            if (auto NewPawn = Zone_SpawnPlayerPawnAtStart(Controller))
            {
                LOG_INFO(LogDev, "Zone: the game mode did not respawn {} within {:.0f}s, spawned a pawn", Controller->GetName(), RespawnTimeoutSeconds);
                Watch.Pawn = NewPawn;
                Watch.bLost = false;
                Zone_SetUpRespawnedPawn(Controller, NewPawn);
            }
            else Watch.LostAt = Now;
        }
    }
}

// the hook above is not always called (the event can go through Blueprint), so the controllers are also looked for now and then
static void Zone_ScanPlayers()
{
    static int Frames = 0;
    if (++Frames % 45 != 0) return;

    auto World = GetWorld();
    auto GameMode = World ? World->Get<UObject*>("AuthorityGameMode") : nullptr;
    static auto ControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
    if (!GameMode || !ControllerClass) return;

    auto Controllers = UGameplayStatics::GetAllActorsOfClass(World, ControllerClass);
    for (int i = 0; i < Controllers.Num(); ++i)
    {
        auto Controller = Controllers.at(i);
        Zone_SetUpPlayer(GameMode, Controller);
        if (Controller && Controller != GetLocalPlayerController()) Zone_WatchRespawn(Controller);
    }
    Controllers.Free();
}

static float Zone_MaxTickRateHook()
{
    if (!RebootConfig::Get().Stw.bEnabled)
        return GetMaxTickRateHook();

    static UObject* ListenedWorld = nullptr;
    static UObject* SeenWorld = nullptr;
    static int FramesSeen = 0;

    auto World = (UObject*)GetWorld();

    if (World && World != ListenedWorld)
    {
        if (World != SeenWorld)
        {
            SeenWorld = World;
            FramesSeen = 0;
            Globals::bZoneMode = World->GetName().starts_with("Zone_");
            if (Globals::bZoneMode && RebootConfig::Get().Stw.bMcp) Globals::bNoMCP = false; // the players' STW profiles are fetched from the backend as dedicated server commands
        }
        else if (World->GetName().starts_with("Zone_") && ++FramesSeen == RebootConfig::Get().Stw.ListenDelayFrames)
        {
            ListenedWorld = World;
            LOG_INFO(LogDev, "Zone world {} is up: listening.", World->GetName());
            Zone_ListenOnce();
        }
    }


    // reboot.ini [SaveTheWorld] Map: once the island is up and the server listens (Reboot's whole Athena start has run), go to the Zone map. Opening the Zone map
    // straight at start skips that start and the server dies in the net tick.
    {
        const auto& Stw = RebootConfig::Get().Stw;
        static int AthenaFrames = 0;
        static bool bTravelled = false;

        if (!bTravelled && !Stw.Map.empty() && World && Globals::bStartedListening && !Globals::bZoneMode && ++AthenaFrames == Stw.TravelDelayFrames)
        {
            bTravelled = true;
            auto Url = Stw.Map + "?game=" + Stw.GameMode + (Stw.Options.empty() ? "" : "?" + Stw.Options);
            LOG_INFO(LogDev, "reboot.ini: travelling to the Zone map {}", Url);

            // the FStrings keep raw pointers, so the wide strings must outlive the call
            const std::wstring WideMap = RebootConfig::Widen(Stw.Map);
            const std::wstring WideOptions = RebootConfig::Widen(Url.substr(Url.find('?') + 1));
            struct { UObject* WorldContext; FName LevelName; bool bAbsolute; FString Options; } Params{ World, UKismetStringLibrary::Conv_StringToName(WideMap.c_str()), true, WideOptions.c_str() };
            static auto OpenLevelFn = FindObject<UFunction>(L"/Script/Engine.GameplayStatics.OpenLevel");
            static auto DefaultStatics = FindObject<UObject>(L"/Script/Engine.Default__GameplayStatics");
            DefaultStatics->ProcessEvent(OpenLevelFn, &Params);
        }
    }

    if (Globals::bZoneMode && Globals::bStartedListening) Zone_ScanPlayers();

    // The outpost's deferred match start: once the outpost owner's quest config is there (its profile reached the server), or after 15 s so a
    // player without an outpost quest still gets a match.
    if (gZoneMatchStartPending && World)
    {
        static auto LibraryDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortOutpostFunctionLibrary");
        static auto GetConfigFn = FindObject<UFunction>(L"/Script/FortniteGame.FortOutpostFunctionLibrary.GetOutpostQuestMissionConfigData");
        struct { UObject* WorldContextObject; FName ConfigTag; UObject* ReturnValue; } ConfigParams{ (UObject*)World, UKismetStringLibrary::Conv_StringToName(L"Quest.Metadata.Mission.Primary.Outpost"), nullptr };
        if (LibraryDefault && GetConfigFn) LibraryDefault->ProcessEvent(GetConfigFn, &ConfigParams);

        const double Waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - gZoneMatchStartRequestedAt).count();
        if (ConfigParams.ReturnValue || Waited > 15.0)
        {
            gZoneMatchStartPending = false;
            Zone_SpawnOutpostCoreEgg();  // before the mission looks for it
            Zone_StartLoadedMissions(); // held back at load on the outpost, see Zone_ListenOnce
            UObject* GameMode = ((UObject*)World)->Get<UObject*>("AuthorityGameMode");
            static auto StartMatchFn = FindObject<UFunction>(L"/Script/Engine.GameMode.StartMatch");
            if (GameMode && StartMatchFn) GameMode->ProcessEvent(StartMatchFn);
            LOG_INFO(LogDev, "Zone: outpost match started after {:.1f}s ({})", Waited, ConfigParams.ReturnValue ? "quest config " + ConfigParams.ReturnValue->GetName() : std::string("no quest config, timed out"));
        }
    }

    // the restart controller belongs to a player who may have left since the last frame: forget it rather than call through it
    if (gZoneRestartController && !gZoneRestartController->IsValidLowLevel())
        gZoneRestartController = nullptr;

    if (gZoneRestartController && ++gZoneRestartFrames % 45 == 0)
    {
        auto RestartPawn = gZoneRestartController->Get<UObject*>("Pawn");
        static auto ClientRestartFn = FindObject<UFunction>(L"/Script/Engine.PlayerController.ClientRestart");

        // When MCP campaign profile completes (~700ms after login), UFortRegisteredPlayerInfo::UpdateAfterAccountChanged clears the server's
        // AbilitySystemComponent and never re-runs GetOrInitializeHero (which ran too early when the player first joined). Call GetOrInitializeHero
        // now so PlayerState->HeroId & HeroType replicate to the client (initializing the client's ASC via BindToState), then re-grant default abilities
        // and re-equip the pickaxe so its weapon abilities are present on the freshly-initialized ASC.
        // GetOrInitializeHero is found by signature (ZoneFinder.h)
        auto GetOrInitializeHero = (UObject* (*)(UObject*))ZoneFinder::GetOrInitializeHero();
        if (!gZoneHeroInitialized && GetOrInitializeHero)
        {
            if (auto Hero = GetOrInitializeHero(gZoneRestartController))
            {
                gZoneHeroInitialized = true;
                LOG_INFO(LogDev, "Zone: GetOrInitializeHero succeeded ({})", Hero->GetFullName());

                if (RestartPawn)
                    Zone_ApplyHeroSkin((AFortPlayerPawn*)RestartPawn, Hero);

                auto AbilityPlayerState = Cast<AFortPlayerState>(gZoneRestartController->Get<UObject*>("PlayerState"));
                auto AbilitySet = GetPlayerAbilitySet();
                auto AbilitySystem = AbilityPlayerState ? AbilityPlayerState->GetAbilitySystemComponent() : nullptr;
                if (AbilitySet && AbilitySystem)
                {
                    AbilitySet->GiveToAbilitySystem(AbilitySystem);
                    LOG_INFO(LogDev, "Zone: re-granted default player abilities after hero init");
                }

                if (AbilityPlayerState)
                {
                    static auto OnRepHeroTypeFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerState.OnRep_HeroType");
                    if (OnRepHeroTypeFn) AbilityPlayerState->ProcessEvent(OnRepHeroTypeFn);
                    ((AActor*)AbilityPlayerState)->ForceNetUpdate();
                }

                if (RestartPawn)
                {
                    // Grant pickaxe from hero loadout: explicitly grant the known pickaxe definition
                    // The pickaxe is a gadget in the hero loadout (slot 0). Since GetGadgetDefinitionBySlotIndex
                    // may not work reliably via ProcessEvent, we directly load the known pickaxe definition.
                    auto Controller = (AFortPlayerController*)gZoneRestartController;
                    auto Inventory = Controller->GetWorldInventory();
                    UFortWeaponItemDefinition* PickaxeDef = nullptr;
                    FGuid PickaxeGuid;

                    // Directly load the STW pickaxe definition (bypassing hero loadout lookup which may not work via ProcessEvent)
                    static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
                    auto PickaxeDefinition = LoadObject<UFortItemDefinition>(L"/Game/Items/Weapons/Melee/Harvest/WID_Harvest_Pickaxe_C_T01.WID_Harvest_Pickaxe_C_T01", ItemDefinitionClass);

                    // after a respawn the player still has the pickaxe from the first life: equip that one instead of adding a second
                    if (Inventory && PickaxeDefinition)
                    {
                        if (auto Existing = Inventory->FindItemInstance(PickaxeDefinition))
                        {
                            if (auto ExistingEntry = Existing->GetItemEntry())
                            {
                                PickaxeDef = (UFortWeaponItemDefinition*)PickaxeDefinition;
                                PickaxeGuid = ExistingEntry->GetItemGuid();
                            }
                        }
                    }

                    if (PickaxeDefinition && !PickaxeDef)
                    {
                        PickaxeDef = (UFortWeaponItemDefinition*)PickaxeDefinition;

                        // Grant to world inventory
                        if (Inventory)
                        {
                            bool bShouldUpdate = false;
                            auto Added = Inventory->AddItem(PickaxeDef, &bShouldUpdate, 1);
                            if (!Added.first.empty() && Added.first[0] && Added.first[0]->GetItemEntry())
                            {
                                PickaxeGuid = Added.first[0]->GetItemEntry()->GetItemGuid();
                                if (bShouldUpdate)
                                    Inventory->Update();
                            }
                        }
                    }

                    // Fallback: search inventory for any harvest pickaxe
                    if (!PickaxeDef && Inventory)
                    {
                        auto& ItemList = Inventory->GetItemList();
                        auto& Instances = ItemList.GetItemInstances();
                        for (int i = 0; i < (int)Instances.size(); ++i)
                        {
                            auto Instance = Instances.at(i);
                            if (!Instance) continue;
                            auto ItemEntry = Instance->GetItemEntry();
                            if (!ItemEntry) continue;
                            auto Def = ItemEntry->GetItemDefinition();
                            if (Def && Def->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
                            {
                                if (Def->GetName().contains("Harvest_Pickaxe"))
                                {
                                    PickaxeDef = (UFortWeaponItemDefinition*)Def;
                                    PickaxeGuid = ItemEntry->GetItemGuid();
                                    break;
                                }
                            }
                        }
                    }

                    if (PickaxeDef)
                    {
                        gZonePickaxeDef = PickaxeDef;
                        gZonePickaxeGuid = PickaxeGuid;
                        ((AFortPlayerPawn*)RestartPawn)->EquipWeaponDefinition(PickaxeDef, PickaxeGuid);
                        Controller->ClientEquipItem(PickaxeGuid, true);

                        // Also add to quickbar slot 0
                        auto QuickBarsActor = Controller->GetOffset("QuickBars", false) != -1 ? Controller->Get<AActor*>(Controller->GetOffset("QuickBars", false)) : nullptr;
                        static auto ServerAddItemInternalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");
                        if (QuickBarsActor && ServerAddItemInternalFn)
                        {
                            struct { FGuid Item; uint8_t InQuickBar; int Slot; } AddParams{ PickaxeGuid, 0, 0 };
                            QuickBarsActor->ProcessEvent(ServerAddItemInternalFn, &AddParams);
                        }

                        LOG_INFO(LogDev, "Zone: equipped pickaxe from hero loadout after hero init");
                    }
                    else
                    {
                        LOG_WARN(LogDev, "Zone: no pickaxe found in hero loadout");
                    }
                }
            }
        }

        if (RestartPawn && ClientRestartFn)
        {
            struct { UObject* NewPawn; } RestartParams{ RestartPawn };
            gZoneRestartController->ProcessEvent(ClientRestartFn, &RestartParams);
        }

        if (++gZoneRestartsSent >= 10)
            gZoneRestartController = nullptr;
    }

    return GetMaxTickRateHook();
}

void ChangeLevels()
{
    constexpr bool bUseRemovePlayer = false;
    constexpr bool bUseSwitchLevel = false;
    constexpr bool bShouldRemoveLocalPlayer = true;

    LOG_INFO(LogDev, "FindGIsClient(): 0x{:x}", FindGIsClient() - __int64(GetModuleHandleW(0)));

    // auto bruh = std::wstring(CustomMapName.begin(), CustomMapName.end());
    // auto CustomMapOpenWStr = (L"open " + bruh);

    bool bShouldUseMoleMap = PlaylistName == "/MoleGame/Playlists/Playlist_MoleGame.Playlist_MoleGame";

    FString LevelB = /* bUseCustomMap ? CustomMapOpenWStr.c_str() : */ (Engine_Version < 424
        ? L"open Athena_Terrain" : Engine_Version >= 500 ? Fortnite_Version >= 23
        ? L"open Asteria_Terrain"
        : Globals::bCreative ? L"open Creative_NoApollo_Terrain"
        : L"open Artemis_Terrain"
        : Globals::bCreative ? L"open Creative_NoApollo_Terrain"
        : L"open Apollo_Terrain");

    FString Level = /* bUseCustomMap ? bruh.c_str() : */ (Engine_Version < 424
        ? L"Athena_Terrain" : Engine_Version >= 500 ? Fortnite_Version >= 23
        ? L"Asteria_Terrain"
        : Globals::bCreative ? L"Creative_NoApollo_Terrain"
        : L"Artemis_Terrain"
        : Globals::bCreative ? L"Creative_NoApollo_Terrain"
        : L"Apollo_Terrain");

    if (bShouldUseMoleMap) // erms
    {
        // FString MoleMap = L"/MoleGame/Maps/MoleGame_Layout.MoleGame_Layout";
        LevelB = L"open MoleGame_Layout";
        Level = L"MoleGame_Layout";
    }

    LOG_INFO(LogDev, "Using \"{}\" as our map.", bUseSwitchLevel ? Level.ToString() : LevelB.ToString());

    auto LocalPC = GetLocalPlayerController();

    LOG_INFO(LogDev, "Got PC: {}", __int64(LocalPC));

    if (bUseSwitchLevel)
    {
        static auto SwitchLevelFn = FindObject<UFunction>(L"/Script/Engine.PlayerController.SwitchLevel");

        LocalPC->ProcessEvent(SwitchLevelFn, &Level);

        if (FindGIsServer())
        {
            *(bool*)FindGIsServer() = true;
        }

        if (FindGIsClient())
        {
            *(bool*)FindGIsClient() = false;
        }
    }
    else
    {
        if (Fortnite_Version != 18.10)
        {
            if (FindGIsServer())
            {
                *(bool*)FindGIsServer() = true;
            }

            if (FindGIsClient())
            {
                *(bool*)FindGIsClient() = false;
            }
        }

        if (bShouldRemoveLocalPlayer)
        {
            if (!bUseRemovePlayer)
            {
                auto& LocalPlayers = GetLocalPlayers();

                if (LocalPlayers.Num() && LocalPlayers.Data)
                {
                    LocalPlayers.Remove(0);
                }
            }
            else if (bUseRemovePlayer)
            {
                UGameplayStatics::RemovePlayer((APlayerController*)LocalPC, true);
            }
        }

        UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), LevelB, nullptr);
    }

    LOG_INFO(LogPlayer, "Switched level.");

    if (bUseSwitchLevel && bShouldRemoveLocalPlayer)
    {
        if (!bUseRemovePlayer)
        {
            auto& LocalPlayers = GetLocalPlayers();

            if (LocalPlayers.Num() && LocalPlayers.Data)
            {
                LocalPlayers.Remove(0);
            }
        }
        else if (bUseRemovePlayer)
        {
            UGameplayStatics::RemovePlayer((APlayerController*)GetLocalPlayerController(), true);
        }
    }
}

static inline char (*oFunc)(__int64) = nullptr;
static inline __int64 (*func2)(__int64) = nullptr;
static inline int persi = 0x0;
char Func(__int64 a1)
{
    __int64 r = 0;
    if (Fortnite_Version >= 24)
    {
        if (a1)
        {
            auto v1 = a1;

            while ((*(uint8_t*)(__int64(v1) + 8) & 0x30) == 0)
            {
                v1 = *(__int64*)(v1 + 32);
                if (!v1)
                    goto LABEL_4;
            }
            r = 0LL;
        }
        else
        {
        LABEL_4:
            r = *(__int64*)(a1 + 32);
        }
    }
    else
    {
        r = func2(a1);
    }

    if (r)
    {
        if (auto v5 = *(__int64*)(__int64(r) + 0x38))
        {
            auto persisntelevle = *(__int64*)(__int64(v5) + persi);
            if (!persisntelevle)
            {
                LOG_INFO(LogDev, "tralaleo trallala");
                return 0;
            }
        }
    }

    return oFunc(a1);
}

void ApplyNullAndRetTrues()
{
    if (Fortnite_Version >= 23)
    {
        persi = 0x98;

        __int64 sig = 0;

        if (Fortnite_Version >= 24)
        {
            persi = 0xA0;
            sig = Memcury::Scanner::FindPattern("48 89 5C 24 ? 57 48 83 EC 20 48 8B C1 48 85 C9 74 0F F6 40 08 30 75 61 48 8B 40 20 48 85 C0 75 F1 48 8B 59 20 48 8B 81").Get(); // 24.40
        }
        else
        {
            func2 = decltype(func2)(Memcury::Scanner::FindPattern("48 83 EC 28 BA ? ? ? ? 4C 8B C1 E8 ? ? ? ? 84 C0 74 04 33 C0 EB 04 49 8B 40 20").Get());
            sig = Memcury::Scanner::FindPattern("48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 8B F8 48 8B 83 ? ? ? ? 48 85 C0").Get(); // 23.40
        }

        Hooking::MinHook::Hook((PVOID)sig, Func, (void**)&oFunc);
    }

    static auto FortPlayerControllerAthenaDefault = FindObject<AFortPlayerControllerAthena>(L"/Script/FortniteGame.Default__FortPlayerControllerAthena"); // FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Default__Athena_PlayerController_C");

    auto AddressesToNull = Addresses::GetFunctionsToNull();
    const auto AddressesToReturnTrue = Addresses::GetFunctionsToReturnTrue();

    auto ServerCheatAllIndex = GetFunctionIdxOrPtr(FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerCheatAll"));

    if (ServerCheatAllIndex)
        AddressesToNull.push_back(__int64(FortPlayerControllerAthenaDefault->VFTable[ServerCheatAllIndex / 8]));

    for (auto func : AddressesToNull)
    {
        if (func == 0)
            continue;

        LOG_INFO(LogDev, "Nulling 0x{:x}", func - __int64(GetModuleHandleW(0)));

        DWORD dwProtection;
        VirtualProtect((PVOID)func, 1, PAGE_EXECUTE_READWRITE, &dwProtection);

        *(uint8_t*)func = 0xC3;

        DWORD dwTemp;
        VirtualProtect((PVOID)func, 1, dwProtection, &dwTemp);
    }

    for (auto func : AddressesToReturnTrue)
    {
        if (func == 0)
            continue;

        LOG_INFO(LogDev, "Forcing return true on 0x{:x}", func - __int64(GetModuleHandleW(0)));

        MH_CreateHook((PVOID)func, ReturnTrueHook, nullptr);
        MH_EnableHook((PVOID)func);
    }
}

bool (*ReplicateActorOriginal)(UActorChannel* Channel) = nullptr;
bool ReplicateActorHook(UActorChannel* Channel)
{
    LOG_INFO(LogDev, "[ReplicateActorHook] Replicating: {}", Channel->GetActor()->GetFullName());
    return ReplicateActorOriginal(Channel);
}

// #define CLIENT_ONLY // only console reboot

DWORD WINAPI Main(LPVOID)
{
    Timing::Begin();
    Timing::Mark("Main entered");

#ifndef CLIENT_ONLY
    InitLogger();

    std::cin.tie(0);
    std::cout.tie(0);
    std::ios_base::sync_with_stdio(false);

    auto MH_InitCode = MH_Initialize();
    
    if (MH_InitCode != MH_OK)
    {
        LOG_ERROR(LogInit, "Failed to initialize MinHook {}!", MH_StatusToString(MH_InitCode));
        return 1;
    }
    Timing::Mark("MinHook initialized");

    std::cout << std::format("Base Address: 0x{:x}\n", __int64(GetModuleHandleW(0)));

    if (ProdMatch::Get().bEnabled)
    {
        Globals::bNoMCP = false; // the account's profile (cosmetics) is fetched from the backend, as for a real match
    }

    LOG_INFO(LogInit, "Initializing Project Reboot!");

    {
        const auto& Config = RebootConfig::Get();
        LOG_INFO(LogDev, "reboot.ini: {}", std::string(Config.Path.begin(), Config.Path.end()));
        LOG_INFO(LogDev, "reboot.ini: bots {} (count {}), Save the World {} (map '{}')", Config.Bots.bEnabled ? "on" : "off", Config.Bots.Count, Config.Stw.bEnabled ? "on" : "off", Config.Stw.Map);

        bEnableBotTick = Config.Bots.bEnabled;
        AmountOfBotsToSpawn = Config.Bots.bEnabled ? Config.Bots.Count : 0;
    }
    LOG_INFO(LogDev, "Built on {} {}", __DATE__, __TIME__);

#endif 

    Addresses::SetupVersion();
    Timing::Mark("version detected");

    NumToSubtractFromSquadId = Engine_Version >= 424 ? 2 : Engine_Version >= 423 ? 3 : 0; // TODO: check this
    NumElementsPerChunk = std::floor(Fortnite_Version) >= 5 && Fortnite_Version <= 6 ? 0x10400 : 0x10000; // Idk what version tbh

    Offsets::FindAll(); // We have to do this before because FindCantBuild uses FortAIController.CreateBuildingActor
    Offsets::Print();
    Timing::Mark("offsets found");

    Addresses::FindAll();
    Addresses::Init();
    Addresses::Print();
    Timing::Mark("addresses found");
    Memcury::ScanCache::Save();

    bEnableRebooting = Addresses::RebootingDelegate && Addresses::FinishResurrection && Addresses::GetSquadIdForCurrentPlayer && false;

#ifdef CLIENT_ONLY // CONSOLE ONLY (FOR CLIENT)
    SetConsoleTitleA("Console");

    // Spawn Console unreal engine Gameviewport
    static UObject* Engine = GetEngine();
    auto GameViewport = Engine->Get("GameViewport");

    if (!GameViewport) return 0;

    auto ViewportConsolePtr = GameViewport->GetPtr("ViewportConsole");
	*ViewportConsolePtr = UGameplayStatics::SpawnObject(FindObject<UClass>(L"/Script/Engine.Console"), GameViewport);

    return 0;
#endif

    LOG_INFO(LogDev, "Fortnite_CL: {}", Fortnite_CL);
    LOG_INFO(LogDev, "Fortnite_Version: {}", Fortnite_Version);
    LOG_INFO(LogDev, "Engine_Version: {}", Engine_Version);

#ifdef ABOVE_S20
    if (Fortnite_Version < 20)
    {
        MessageBoxA(0, "Please undefine ABOVE_S20", "Project Reboot 3.0", MB_ICONERROR);
        LOG_ERROR(LogDev, "Aborting!");
        return 0;
    }
#else
    if (Fortnite_Version > 20)
    {
        MessageBoxA(0, "Please define ABOVE_S20 (compile it yourself and change inc.h)", "Project Reboot 3.0", MB_ICONERROR);
        LOG_ERROR(LogDev, "Aborting!");
        return 0;
    }
#endif

    Timing::Mark("version guard passed");

    if (!ProdMatch::Get().bEnabled) // a backend-run server has no display to draw on
    {
        CreateThread(0, 0, GuiThread, 0, 0, 0);
    }
    else
    {
        LOG_INFO(LogInit, "Prod: no GUI (match '{}')", ProdMatch::Get().MatchId);
    }

    // Nobody is at the keyboard on a backend-run server or a bots-only test match, so the GUI's
    // "Seconds until load into map" wait would only delay when the server becomes joinable.
    if (!ProdMatch::Get().bEnabled && !Bots::TestOptions().bEnabled)
    {
        while (SecondsUntilTravel > 0)
        {
            SecondsUntilTravel -= 1;

            Sleep(1000);
        }
    }

    bSwitchedInitialLevel = true;

    // Globals::bAutoRestart = IsRestartingSupported();

    bool isPlayspaceGamemode = PlaylistName == "/MoleGame/Playlists/Playlist_MoleGame.Playlist_MoleGame";
    static auto GameModeDefault = isPlayspaceGamemode ? FindObject<AFortGameModeAthena>(L"/Script/FortniteGame.Default__FortPlayspaceGameMode")
        : FindObject<AFortGameModeAthena>(L"/Script/FortniteGame.Default__FortGameModeBR")
        ? FindObject<AFortGameModeAthena>(L"/Script/FortniteGame.Default__FortGameModeBR") // 22.30 atleast
        : FindObject<AFortGameModeAthena>(L"/Script/FortniteGame.Default__FortGameModeAthena");
    static auto FortPlayerControllerZoneDefault = FindObject<AFortPlayerController>(L"/Script/FortniteGame.Default__FortPlayerControllerZone");
    static auto FortPlayerControllerDefault = FindObject<AFortPlayerController>(L"/Script/FortniteGame.Default__FortPlayerController");
    static auto FortPlayerPawnAthenaDefault = FindObject<AFortPlayerPawn>(L"/Script/FortniteGame.Default__FortPlayerPawnAthena"); // FindObject<AFortPlayerPawn>(L"/Game/Athena/PlayerPawn_Athena.Default__PlayerPawn_Athena_C");
    static auto FortAbilitySystemComponentAthenaDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortAbilitySystemComponentAthena");
    static auto FortPlayerStateAthenaDefault = FindObject<AFortPlayerStateAthena>(L"/Script/FortniteGame.Default__FortPlayerStateAthena");
    static auto FortKismetLibraryDefault = FindObject<UFortKismetLibrary>(L"/Script/FortniteGame.Default__FortKismetLibrary");
    static auto AthenaMarkerComponentDefault = FindObject<UAthenaMarkerComponent>(L"/Script/FortniteGame.Default__AthenaMarkerComponent");
    static auto FortWeaponDefault = FindObject<AFortWeapon>(L"/Script/FortniteGame.Default__FortWeapon");
    static auto FortOctopusVehicleDefault = FindObject<AFortOctopusVehicle>(L"/Script/FortniteGame.Default__FortOctopusVehicle");
    static auto FortPlayerControllerAthenaDefault = FindObject<AFortPlayerControllerAthena>(L"/Script/FortniteGame.Default__FortPlayerControllerAthena"); // FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Default__Athena_PlayerController_C");

    if (Fortnite_Version >= 20 || Fortnite_Version == 12.00)
        ApplyNullAndRetTrues();

    // ObjectsDump.txt is now written only from the GUI's "Dump Objects" button, not on every startup.

    // Engine-side VeryVerbose categories used to be forced on every boot. That is a lot of log I/O (slow under
    // Wine) and none of it is needed for normal operation; enable individual categories from the console when debugging.

    Hooking::MinHook::Hook((PVOID)Addresses::NoMCP, (PVOID)NoMCPHook, nullptr);
    Hooking::MinHook::Hook((PVOID)Addresses::GetNetMode, (PVOID)GetNetModeHook, nullptr);
    Hooking::MinHook::Hook((PVOID)Addresses::DispatchRequest, (PVOID)DispatchRequestHook, (PVOID*)&DispatchRequestOriginal);

    // Hooking::MinHook::Hook((PVOID)Addresses::ReplicateActor, (PVOID)ReplicateActorHook, (PVOID*)&ReplicateActorOriginal);

    GSRandSeed = FGenericPlatformTime::Cycles();
    ReplicationRandStream = FRandomStream(FGenericPlatformTime::Cycles());

    Hooking::MinHook::Hook((PVOID)Addresses::KickPlayer, (PVOID)AGameSession::KickPlayerHook, (PVOID*)&AGameSession::KickPlayerOriginal);

    LOG_INFO(LogDev, "Size: 0x{:x}", sizeof(TMap<FName, void*>));

    Hooking::MinHook::Hook((PVOID)Addresses::ActorGetNetMode, (PVOID)GetNetModeHook2, nullptr);

    if (Fortnite_Version > 13 && // ermm
        Fortnite_Version < 20
        ) 
    {
        Hooking::MinHook::Hook(FindObject<ABuildingFoundation>(L"/Script/FortniteGame.Default__BuildingFoundation"),
            FindObject<UFunction>(L"/Script/FortniteGame.BuildingFoundation.SetDynamicFoundationTransform"),
            ABuildingFoundation::SetDynamicFoundationTransformHook, (PVOID*)&ABuildingFoundation::SetDynamicFoundationTransformOriginal, false, true);

        Hooking::MinHook::Hook(FindObject<ABuildingFoundation>(L"/Script/FortniteGame.Default__BuildingFoundation"),
            FindObject<UFunction>(L"/Script/FortniteGame.BuildingFoundation.SetDynamicFoundationEnabled"),
            ABuildingFoundation::SetDynamicFoundationEnabledHook, (PVOID*)&ABuildingFoundation::SetDynamicFoundationEnabledOriginal, false, true);
    }


    /*
    if (Fortnite_Version == 6.21) // ur trolling
    {
        std::string AIDirectorFuncName = "/Script/Engine.PlayerController.FOV"; // "/Script/Engine.PlayerController.ClientVoiceHandshakeComplete";
        std::string GoalManagerFuncName = "/Script/Engine.PlayerController.EnableCheats";

        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0xAADD50), (PVOID)ConstructEmptyQueryInfoHook, (PVOID*)&ConstructEmptyQueryInfoOriginal); // 7FF7E556D158  

        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB10480, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xABBAB9, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB1E2BC, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB25EAA, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xABFDC1, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xAEC76D, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xA9C62C, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xAA1165, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xA9F5C0, (PVOID)GetGoalManagerHook, GoalManagerFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x10975EE, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xAC1C15, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB2096E, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x107B6A5, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB213DC, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xAB51D2, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB21BB8, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xABE6AC, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0xB2247D, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x10988B7, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x107C7B6, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x1096D21, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
        HookInstruction(__int64(GetModuleHandleW(0)) + 0x1097982, (PVOID)GetAIDirectorHook, AIDirectorFuncName, ERelativeOffsets::CALL, FortPlayerControllerAthenaDefault);
    }
    */

    if (Fortnite_Version == 8.40)/*Fortnite_Version >= 8.40 && Fortnite_Version <= 8.51*/
    {
        auto TheFunc = (uint8*)FindObject<UFunction>("/Script/MeshNetwork.MeshNetworkSubsystem:GetMeshNetworkNodeType")->GetFunc();
        for (int i = 0; i < 75; i++)
        {
            if (TheFunc[i] == 0xE8)
            {
                Hooking::MinHook::Hook((PVOID)(Memcury::Scanner(__int64(TheFunc) + i).RelativeOffset(1).Get()), (PVOID)GetMeshNetworkNodeTypeHook, nullptr);
            }
        }

        //Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x8BC450), (PVOID)GetMeshNetworkNodeTypeHook, nullptr);
    }

    if (Fortnite_Version >= 16 && Fortnite_Version < 19)
    {
        // Bus crash (only needed if we are calling StartAircraftPhase on separate thread I THINK) (sometimes)
        Hooking::MinHook::Hook(Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 54 41 56 41 57 48 83 EC 40 48 8B 59 28 45 33 E4").GetAs<PVOID>(), (PVOID)EmptyHook); // also on 16.50
    }

    if (Fortnite_Version == 17.30) // Rift Tour stuff
    {
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E07910), (PVOID)GetMeshNetworkNodeTypeHook, nullptr);
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3DED158), (PVOID)ReturnTrueHook, nullptr); // 7FF7E556D158  
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3DECFC8), (PVOID)ReturnTrueHook, nullptr); // 7FF7E556CFC8
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3DED050), (PVOID)ReturnTrueHook, nullptr); // 7FF7E556D050
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3DECF40), (PVOID)ReturnFalseHook, nullptr); // 7FF7E556CF40
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3DE5CE8), (PVOID)ActivatePhaseAtIndexHook, (PVOID*)&ActivatePhaseAtIndexOriginal); // 7FF7E5565CE8
    }
    else if (Fortnite_Version == 17.50) // Sky fire stuff
    {
        // Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + ), (PVOID)GetMeshNetworkNodeTypeHook, nullptr);
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E54BCC), (PVOID)ReturnTrueHook, nullptr); // 7FF638A04BCC    
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E5496C), (PVOID)ReturnTrueHook, nullptr); // 7FF638A0496C   
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E54A68), (PVOID)ReturnTrueHook, nullptr); // 7FF638A04A68 
        // Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + ), (PVOID)ReturnFalseHook, nullptr);
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E4D768), (PVOID)ActivatePhaseAtIndexHook, (PVOID*)&ActivatePhaseAtIndexOriginal); // 07FF6389FD768
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x3E4DFA4), (PVOID)AttemptFinishPhaseHook, (PVOID*)&AttemptFinishPhaseOriginal);
    }
    else if (Fortnite_Version == 18.40)
    {
        // Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + ), (PVOID)GetMeshNetworkNodeTypeHook, nullptr);
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x416AAB8), (PVOID)ReturnTrueHook, nullptr); // 7FF79E3EAAB8  
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x416A840), (PVOID)ReturnTrueHook, nullptr); // 7FF79E3EA840
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x416A93C), (PVOID)ReturnTrueHook, nullptr); // 7FF79E3EA93C
        // Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + ), (PVOID)ReturnFalseHook, nullptr);
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x41624C8), (PVOID)ActivatePhaseAtIndexHook, (PVOID*)&ActivatePhaseAtIndexOriginal); // 7FF79E3E24C8  
    }

    if (std::floor(Fortnite_Version) == 4)
    {
        auto RetrieveCharacterPartsAddr = Memcury::Scanner::FindPattern("48 89 5C 24 ? 57 48 83 EC 20 48 8B 01 0F B6 FA 48 8B D9 FF 90 ? ? ? ? 48 8B C8 E8 ? ? ? ? 84 C0 74 0D 33 C0 48 8B 5C 24 ? 48 83 C4 20 5F", false).Get();
        
        if (!RetrieveCharacterPartsAddr)
            RetrieveCharacterPartsAddr = Memcury::Scanner::FindPattern("40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 ? ? ? ? 48 8B C8 E8 ? ? ? ? 84 C0 74 08 33 C0 48 83 C4 20 5B C3 48 8B CB").Get(); // 4.0

        LOG_INFO(LogDev, "RetrieveCharacterPartsAddr: {}", RetrieveCharacterPartsAddr);

        if (RetrieveCharacterPartsAddr)
        {
            for (int i = 0; i < 400; i++)
            {
                if (*(uint8_t*)(RetrieveCharacterPartsAddr + i) == 0x74) // jz
                {
                    DWORD dwProtection;
                    VirtualProtect((PVOID)(RetrieveCharacterPartsAddr + i), 1, PAGE_EXECUTE_READWRITE, &dwProtection);

                    *(uint8_t*)(RetrieveCharacterPartsAddr + i) = 0x75; // jnz

                    DWORD dwTemp;
                    VirtualProtect((PVOID)(RetrieveCharacterPartsAddr + i), 1, dwProtection, &dwTemp);

                    LOG_INFO(LogDev, "Applied RetrieveCharacterParts patch!");
                    break;
                }
            }
        }
    }

    if (Fortnite_Version >= 17.30)
    {
        Hooking::MinHook::Hook(FindObject<UObject>("/Script/FortniteGame.Default__FortMissionLibrary"), FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary:TeleportPlayerPawn"), TeleportPlayerPawnHook,
            (PVOID*)&TeleportPlayerPawnOriginal, false, true);
    }

    if (Globals::bGoingToPlayEvent)
    {
        // Todo hook UBuffetRacePlayerMovementComponent::ServerTeleportPlayer?

        if (Fortnite_Version == 17.50)
        {
            Hooking::MinHook::Hook(FindObject<UObject>("/Script/SpecialEventGameplayRuntime.Default__SpecialEventScript"), FindObject<UFunction>(L"/Script/SpecialEventGameplayRuntime.SpecialEventScript:StartEventAtIndex"), StartEventAtIndexHook,
                (PVOID*)&StartEventAtIndexOriginal, false, true);
        }
    }

    /*

    if (Fortnite_Version == 6.21)
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x191D2E0), (PVOID)CanCreateInCurrentContextHook, (PVOID*)&CanCreateInCurrentContextOriginal);
    else if (Fortnite_Version == 10.40)
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x22A30C0), (PVOID)CanCreateInCurrentContextHook, (PVOID*)&CanCreateInCurrentContextOriginal);
    else if (Fortnite_Version == 12.41)
        Hooking::MinHook::Hook((PVOID)(__int64(GetModuleHandleW(0)) + 0x2DBCBA0), (PVOID)CanCreateInCurrentContextHook, (PVOID*)&CanCreateInCurrentContextOriginal);

    */

    Timing::Mark("hooks installed");
    Memcury::ScanCache::Save();

    ChangeLevels();

    Timing::Mark("ChangeLevels issued");
    LOG_INFO(LogDev, "Switched levels.");

    if (Fortnite_Version < 20 && Fortnite_Version != 12)
        ApplyNullAndRetTrues();

    if (Fortnite_Version != 22.4)
    {
        auto matchmaking = Memcury::Scanner::FindPattern("83 BD ? ? ? ? 01 7F 18 49 8D 4D D8 48 8B D6 E8 ? ? ? ? 48", false).Get(); // 1.11

        if (!matchmaking)
            matchmaking = Memcury::Scanner::FindPattern("83 7D 88 01 7F 0D 48 8B CE E8", false).Get();
        if (!matchmaking)
            matchmaking = Memcury::Scanner::FindPattern("83 BD ? ? ? ? ? 7F 18 49 8D 4D D8 48 8B D7 E8").Get(); // 4.20
	if (!matchmaking)
	    matchmaking = Memcury::Scanner::FindPattern("83 7C 24 ?? 01 7F 0D 48 8B CF E8").Get();
	    
        bool bMatchmakingSupported = matchmaking;
        int idx = 0;

        if (bMatchmakingSupported) // now check if it leads to the right place and where the jg is at
        {
            for (int i = 0; i < 9; i++)
            {
                auto byte = (uint8_t*)(matchmaking + i);

                if (IsBadReadPtr(byte))
                    continue;

                // std::cout << std::format("[{}] 0x{:x}\n", i, (int)*byte);

                if (*byte == 0x7F) // jump if greater
                {
                    bMatchmakingSupported = true;
                    idx = i;
                    break;
                }

                bMatchmakingSupported = false;
            }
        }

        LOG_INFO(LogMatchmaker, "Matchmaking will {}", (Engine_Version >= 420 && bMatchmakingSupported // since tcp for most isnt supported we wont say its supported
            ? "be supported" : "not be supported"));

        if (bMatchmakingSupported)
        {
            std::cout << "idx: " << idx << '\n';

            auto before = (uint8_t*)(matchmaking + idx);

            std::cout << "before byte: " << (int)*before << '\n';

            DWORD dwProtection;
            VirtualProtect((PVOID)before, 1, PAGE_EXECUTE_READWRITE, &dwProtection);

            *before = 0x74; // jump if zero

            DWORD dwTemp;
            VirtualProtect((PVOID)before, 1, dwProtection, &dwTemp);
        }
    }

    // return false;

    // UNetDriver::ReplicationDriverOffset = FindOffsetStruct("/Script/Engine.NetDriver", "ReplicationDriver"); // NetDriver->GetOffset("ReplicationDriver");

    // Globals::bAbilitiesEnabled = Engine_Version < 500;

    InitBotNames();

    if (Engine_Version < 420)
    {
        auto ApplyHomebaseEffectsOnPlayerSetupAddr = Memcury::Scanner::FindPattern("40 55 53 57 41 54 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 00 4C 8B").Get();

        Hooking::MinHook::Hook((PVOID)ApplyHomebaseEffectsOnPlayerSetupAddr, ApplyHomebaseEffectsOnPlayerSetupHook, (PVOID*)&ApplyHomebaseEffectsOnPlayerSetupOriginal);
    }

    Hooking::MinHook::Hook(FindObject(L"/Script/FortniteGame.Default__FortAthenaVehicleSpawner"), FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaVehicleSpawner.SpawnVehicle"),
        AFortAthenaVehicleSpawner::SpawnVehicleHook, nullptr, false);

    // Save the World: Reboot has no Zone game mode logic. The Zone game mode only gets what it needs to take players: the server listens once it waits for the match.
    auto ZoneGameModeDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortGameModeZone");
    LOG_INFO(LogDev, "Zone game mode default object: {}", __int64(ZoneGameModeDefault));
    if (RebootConfig::Get().Stw.bEnabled && !RebootConfig::Get().Stw.Map.empty()) InstallZoneTrace();

    if (ZoneGameModeDefault && RebootConfig::Get().Stw.bEnabled)
    {
        Hooking::MinHook::Hook(ZoneGameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameModeBase.HandleStartingNewPlayer"), Zone_HandleStartingNewPlayerHook,
            (PVOID*)&Zone_HandleStartingNewPlayerOriginal, false);
        Hooking::MinHook::Hook(ZoneGameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameMode.ReadyToStartMatch"), Zone_ReadyToStartMatchHook,
            (PVOID*)&Zone_ReadyToStartMatchOriginal, false, false, true);
        LOG_INFO(LogDev, "Zone ReadyToStartMatch hooked: {}", __int64(Zone_ReadyToStartMatchOriginal));
    }

    Hooking::MinHook::Hook(GameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameMode.ReadyToStartMatch"), AFortGameModeAthena::Athena_ReadyToStartMatchHook,
       (PVOID*)&AFortGameModeAthena::Athena_ReadyToStartMatchOriginal, false, false, true);

    if (Fortnite_Version > 3.3) // 0xE9 on 3.3 (assumed every build below)
    {
        Hooking::MinHook::Hook(GameModeDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortGameModeAthena.OnAircraftEnteredDropZone"), AFortGameModeAthena::OnAircraftEnteredDropZoneHook,
            (PVOID*)&AFortGameModeAthena::OnAircraftEnteredDropZoneOriginal, false, false, true, true);
    }

    if (Fortnite_Version >= 12.00 && Fortnite_Version <= 13.40) // you cant spawn bosses using spawnbot on s14 and above
    {
        if (Fortnite_Version >= 12.00 && Fortnite_Version <= 13.40 && Fortnite_Version != 12.61)
        {
            Hooking::MinHook::Hook(FindObject<UFortServerBotManagerAthena>(L"/Script/FortniteGame.Default__FortServerBotManagerAthena"), FindObject<UFunction>(L"/Script/FortniteGame.FortServerBotManagerAthena.SpawnBot"),
                UFortServerBotManagerAthena::SpawnBotHook, (PVOID*)&UFortServerBotManagerAthena::SpawnBotOriginal, false);
        }
        else
        {
            Hooking::MinHook::Hook((PVOID)SpawnBot(), (PVOID)UFortServerBotManagerAthena::SpawnBotHook, (PVOID*)&UFortServerBotManagerAthena::SpawnBotOriginal);
        }
    }

    Hooking::MinHook::Hook(GameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameModeBase.SpawnDefaultPawnFor"),
        AGameModeBase::SpawnDefaultPawnForHook, nullptr, false);
    // Hooking::MinHook::Hook(GameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameModeBase.PlayerCanRestart"),
       // AGameModeBase::PlayerCanRestartHook, (PVOID*)&AGameModeBase::PlayerCanRestartOriginal, false, true);
    Hooking::MinHook::Hook(GameModeDefault, FindObject<UFunction>(L"/Script/Engine.GameModeBase.HandleStartingNewPlayer"), AFortGameModeAthena::Athena_HandleStartingNewPlayerHook,
        (PVOID*)&AFortGameModeAthena::Athena_HandleStartingNewPlayerOriginal, false);

    static auto ControllerServerAttemptInteractFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerAttemptInteract");

    if (ControllerServerAttemptInteractFn)
    {
        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, ControllerServerAttemptInteractFn, AFortPlayerController::ServerAttemptInteractHook,
            (PVOID*)&AFortPlayerController::ServerAttemptInteractOriginal, false, true);
    }
    else
    {
        Hooking::MinHook::Hook(FindObject(L"/Script/FortniteGame.Default__FortControllerComponent_Interaction"),
            FindObject<UFunction>(L"/Script/FortniteGame.FortControllerComponent_Interaction.ServerAttemptInteract"),
            AFortPlayerController::ServerAttemptInteractHook, (PVOID*)&AFortPlayerController::ServerAttemptInteractOriginal, false, true);
    }

    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/Engine.PlayerController.ServerAcknowledgePossession"),
        AFortPlayerControllerAthena::ServerAcknowledgePossessionHook, nullptr, false);

    if (Engine_Version >= 424)
    {
        static auto ServerRestartPlayerFn = FindObject<UFunction>(L"/Script/Engine.PlayerController.ServerRestartPlayer");
        auto ZoneServerRestartPlayer = FortPlayerControllerZoneDefault->VFTable[GetFunctionIdxOrPtr(ServerRestartPlayerFn) / 8];

        LOG_INFO(LogDev, "ZoneServerRestartPlayer: 0x{:x}", __int64(ZoneServerRestartPlayer) - __int64(GetModuleHandleW(0)));

        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, ServerRestartPlayerFn,
            // ZoneServerRestartPlayer,
            AFortPlayerControllerAthena::ServerRestartPlayerHook,
            nullptr, false);
    }

    auto OnRep_EditActorFn = FindObject<UFunction>(L"/Script/FortniteGame.FortWeap_EditingTool.OnRep_EditActor");

    if (OnRep_EditActorFn)
    {
        auto OnRep_EditActorExec = (uint64)OnRep_EditActorFn->GetFunc();
        uint64 OnRep_EditActorOriginal = 0;

        for (int i = 0; i < 400; i++)
        {
            if (*(uint8_t*)(OnRep_EditActorExec + i) == 0xE8 || *(uint8_t*)(OnRep_EditActorExec + i) == 0xE9)
            {
                OnRep_EditActorOriginal = Memcury::Scanner(OnRep_EditActorExec + i).RelativeOffset(1).Get();
                break;
            }
        }

        LOG_INFO(LogDev, "OnRep_EditActor Offset: 0x{:x}", OnRep_EditActorOriginal - __int64(GetModuleHandleW(0)));
        AFortWeap_EditingTool::originalOnRep_EditActor = decltype(AFortWeap_EditingTool::originalOnRep_EditActor)(OnRep_EditActorOriginal);
    }

    static auto ServerReturnToMainMenuFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerReturnToMainMenu");
    static auto ServerReturnToMainMenuIdx = GetFunctionIdxOrPtr(ServerReturnToMainMenuFn) / 8;
    auto ServerReturnToMainMenu = FortPlayerControllerDefault->VFTable[ServerReturnToMainMenuIdx];
    VirtualSwap(FortPlayerControllerAthenaDefault->VFTable, ServerReturnToMainMenuIdx, ServerReturnToMainMenu);

    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerSuicide"),
        AFortPlayerController::ServerSuicideHook, nullptr, false);

    // HookInstruction(Addresses::UpdateTrackedAttributesLea, (PVOID)AFortPlayerControllerAthena::UpdateTrackedAttributesHook, "/Script/Engine.PlayerController.EnableCheats", ERelativeOffsets::LEA, FortPlayerControllerAthenaDefault);
    // HookInstruction(Addresses::CombinePickupLea, (PVOID)AFortPickup::CombinePickupHook, "/Script/Engine.PlayerController.SetVirtualJoystickVisibility", ERelativeOffsets::LEA, FortPlayerControllerAthenaDefault);
   
    if (Fortnite_Version == 13.40)
    {
        // HookInstruction(__int64(GetModuleHandleW(0)) + 0x1FC835D, (PVOID)UFortAthenaAIBotCustomizationData::ApplyOverrideCharacterCustomizationHook, "/Script/Engine.PlayerController.SetVirtualJoystickVisibility", ERelativeOffsets::CALL, nullptr);
    }

    Hooking::MinHook::Hook(FortWeaponDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortWeapon.ServerReleaseWeaponAbility"),
        AFortWeapon::ServerReleaseWeaponAbilityHook, (PVOID*)&AFortWeapon::ServerReleaseWeaponAbilityOriginal, false, true);

    Hooking::MinHook::Hook(FindObject<UKismetSystemLibrary>(L"/Script/Engine.Default__KismetSystemLibrary"), FindObject<UFunction>(L"/Script/Engine.KismetSystemLibrary.PrintString"),
        UKismetSystemLibrary::PrintStringHook, (PVOID*)&UKismetSystemLibrary::PrintStringOriginal, false, true); // todo FortShippingLog

    Hooking::MinHook::Hook((PVOID)Addresses::GetSquadIdForCurrentPlayer, (PVOID)AFortGameSessionDedicatedAthena::GetSquadIdForCurrentPlayerHook);

    auto OnPlayImpactFXStringRef = Memcury::Scanner::FindStringRef(L"OnPlayImpactFX", true, 0);
    __int64 OnPlayImpactFXAddr = 0;

    if (OnPlayImpactFXStringRef.Get())
    {
        auto OnPlayImpactFXFunctionPtr = OnPlayImpactFXStringRef.ScanFor({ 0x48, 0x8D, 0x0D }).RelativeOffset(3).GetAs<void*>();
        auto OnPlayImpactFXPtrRef = Memcury::Scanner::FindPointerRef(OnPlayImpactFXFunctionPtr).Get();

        for (int i = 0; i < 2000; i++)
        {
            if (*(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i) == 0x48 && *(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i + 1) == 0x89 && *(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i + 2) == 0x5C)
            {
                OnPlayImpactFXAddr = OnPlayImpactFXPtrRef - i;
                break;
            }

            if (*(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i) == 0x4C && *(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i + 1) == 0x8B && *(uint8_t*)(uint8_t*)(OnPlayImpactFXPtrRef - i + 2) == 0xDC)
            {
                OnPlayImpactFXAddr = OnPlayImpactFXPtrRef - i;
                break;
            }
        }
    }

    LOG_INFO(LogDev, "OnPlayImpactFX: 0x{:x}", OnPlayImpactFXAddr - __int64(GetModuleHandleW(0)));
    Hooking::MinHook::Hook((PVOID)OnPlayImpactFXAddr, AFortWeapon::OnPlayImpactFXHook, (PVOID*)&AFortWeapon::OnPlayImpactFXOriginal);

    /* Hooking::MinHook::Hook(FindObject<AFortVolumeManager>("/Script/FortniteGame.Default__FortVolumeManager"), FindObject<UFunction>(L"/Script/FortniteGame.FortVolumeManager.SpawnVolume"),
        AFortVolumeManager::SpawnVolumeHook, (PVOID*)&AFortVolumeManager::SpawnVolumeOriginal, false);
    Hooking::MinHook::Hook((PVOID)GetFunctionIdxOrPtr(FindObject<UFunction>("/Script/FortniteGame.PlaysetLevelStreamComponent.SetPlayset"), true), 
        UPlaysetLevelStreamComponent::SetPlaysetHook, (PVOID*)&UPlaysetLevelStreamComponent::SetPlaysetOriginal); */

    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerDropAllItems"),
        AFortPlayerController::ServerDropAllItemsHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault,
        FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerSpawnInventoryDrop") 
        ? FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerSpawnInventoryDrop") : FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerAttemptInventoryDrop"),
        AFortPlayerController::ServerAttemptInventoryDropHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerCheat"),
        ServerCheatHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerExecuteInventoryItem"),
       AFortPlayerController::ServerExecuteInventoryItemHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerPlayEmoteItem"),
       AFortPlayerController::ServerPlayEmoteItemHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerPlaySprayItem"),
        AFortPlayerController::ServerPlaySprayItemHook, nullptr, false); // S4 explain yourself
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerRepairBuildingActor"),
        AFortPlayerController::ServerRepairBuildingActorHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerCreateBuildingActor"), 
        AFortPlayerController::ServerCreateBuildingActorHook, (PVOID*)&AFortPlayerController::ServerCreateBuildingActorOriginal, false, true);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerBeginEditingBuildingActor"),
        AFortPlayerController::ServerBeginEditingBuildingActorHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerEditBuildingActor"),
        AFortPlayerController::ServerEditBuildingActorHook, (PVOID*)&AFortPlayerController::ServerEditBuildingActorOriginal, false, true);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerEndEditingBuildingActor"),
        AFortPlayerController::ServerEndEditingBuildingActorHook, nullptr, false);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerLoadingScreenDropped"),
        AFortPlayerController::ServerLoadingScreenDroppedHook, (PVOID*)&AFortPlayerController::ServerLoadingScreenDroppedOriginal, false, true);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerReadyToStartMatch"),
        AFortPlayerControllerAthena::ServerReadyToStartMatchHook, (PVOID*)&AFortPlayerControllerAthena::ServerReadyToStartMatchOriginal, false);

    if (Fortnite_Version >= 14)
    {
        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerClientIsReadyToRespawn"),
            AFortPlayerControllerAthena::ServerClientIsReadyToRespawnHook, nullptr, false);
    }
    

    auto ServerRequestSeatChangeFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerZone.ServerRequestSeatChange");

    if (ServerRequestSeatChangeFn)
        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerZone.ServerRequestSeatChange"),
            AFortPlayerControllerAthena::ServerRequestSeatChangeHook, (PVOID*)&AFortPlayerControllerAthena::ServerRequestSeatChangeOriginal, false);

    // if (false)
    if (Fortnite_Version > 6.20) // so on 6.20 & below there is a param and our little finder dont work for that so
    {
        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerGameplay.StartGhostMode"), // (Milxnor) TODO: This changes to a component in later seasons.
            AFortPlayerControllerAthena::StartGhostModeHook, (PVOID*)&AFortPlayerControllerAthena::StartGhostModeOriginal, false, true); // We can exec hook since it only gets called via blueprint.
        
        auto EndGhostModeFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerGameplay.EndGhostMode");

        if (EndGhostModeFn)
        {
            auto EndGhostModeExec = (uint64)EndGhostModeFn->GetFunc();

            for (int i = 0; i < 400; i++)
            {
                if (*(uint8_t*)(EndGhostModeExec + i) == 0xE9 // thanks 6.21
                    || *(uint8_t*)(EndGhostModeExec + i) == 0xE8)
                {
                    Hooking::MinHook::Hook((PVOID)Memcury::Scanner(EndGhostModeExec + i).RelativeOffset(1).Get(), AFortPlayerControllerAthena::EndGhostModeHook, (PVOID*)&AFortPlayerControllerAthena::EndGhostModeOriginal);
                    break;
                }
            }
        }
    }

    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerGiveCreativeItem"),
        AFortPlayerControllerAthena::ServerGiveCreativeItemHook, nullptr, true);

    static auto ServerCreativeSetFlightSpeedIndexFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerGameplay.ServerCreativeSetFlightSpeedIndex");

    if (ServerCreativeSetFlightSpeedIndexFn)
        Hooking::MinHook::Hook(FindObject<UObject>("/Script/FortniteGame.Default__FortPlayerControllerGameplay"), ServerCreativeSetFlightSpeedIndexFn,
            AFortPlayerControllerAthena::ServerCreativeSetFlightSpeedIndexHook, nullptr, false, true);

    if (Fortnite_Version < 19) // its all screwed up idk
    {
        Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerPlaySquadQuickChatMessage"),
            AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessageHook, nullptr, false);
    }

    if (Fortnite_Version < 19) // jmp qword ptr idk why our dtc dont wrok
    {
        auto ServerTeleportToPlaygroundIslandFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerTeleportToPlaygroundLobbyIsland");

        if (ServerTeleportToPlaygroundIslandFn)
        {
            Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, ServerTeleportToPlaygroundIslandFn,
                AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIslandHook, nullptr, false);
        }
    }

    // Hooking::MinHook::Hook(FortPlayerStateAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerStateAthena.ServerSetInAircraft"),
        // AFortPlayerStateAthena::ServerSetInAircraftHook, (PVOID*)&AFortPlayerStateAthena::ServerSetInAircraftOriginal, false, true); // We could use second method but eh

    if (false && FortOctopusVehicleDefault) // hooking broken on 19.10 i cant figure it out for the life of me
    {
        static auto ServerUpdateTowhookFn = FindObject<UFunction>(L"/Script/FortniteGame.FortOctopusVehicle.ServerUpdateTowhook");
        Hooking::MinHook::Hook(FortOctopusVehicleDefault, ServerUpdateTowhookFn, AFortOctopusVehicle::ServerUpdateTowhookHook, nullptr, false);
    }

    Hooking::MinHook::Hook(FindObject<AFortWeaponRangedMountedCannon>(L"/Script/FortniteGame.Default__FortWeaponRangedMountedCannon"),
        FindObject<UFunction>(L"/Script/FortniteGame.FortWeaponRangedMountedCannon.ServerFireActorInCannon"), AFortWeaponRangedMountedCannon::ServerFireActorInCannonHook, nullptr, false);

    Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerSendZiplineState"),
        AFortPlayerPawn::ServerSendZiplineStateHook, nullptr, false);

    static auto NetMulticast_Athena_BatchedDamageCuesFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.NetMulticast_Athena_BatchedDamageCues") ? FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.NetMulticast_Athena_BatchedDamageCues") : FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawnAthena.NetMulticast_Athena_BatchedDamageCues");

    if (Fortnite_Version < 21)
    {
        Hooking::MinHook::Hook((PVOID)GetFunctionIdxOrPtr(FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerOnExitVehicle"), true), AFortPlayerPawn::ServerOnExitVehicleHook, (PVOID*)&AFortPlayerPawn::ServerOnExitVehicleOriginal);
    }
    
    if (Fortnite_Version == 1.11 || Fortnite_Version > 1.8)
    {
        Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerReviveFromDBNO"),
            AFortPlayerPawn::ServerReviveFromDBNOHook, nullptr, false);
    }

    static auto FortGameplayAbilityAthena_PeriodicItemGrantDefault = FindObject<UFortGameplayAbilityAthena_PeriodicItemGrant>(L"/Script/FortniteGame.Default__FortGameplayAbilityAthena_PeriodicItemGrant");

    if (FortGameplayAbilityAthena_PeriodicItemGrantDefault)
    {
        Hooking::MinHook::Hook(FortGameplayAbilityAthena_PeriodicItemGrantDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortGameplayAbilityAthena_PeriodicItemGrant.StopItemAwardTimers"),
            UFortGameplayAbilityAthena_PeriodicItemGrant::StopItemAwardTimersHook, (PVOID*)&UFortGameplayAbilityAthena_PeriodicItemGrant::StopItemAwardTimersOriginal, false, true);
        Hooking::MinHook::Hook(FortGameplayAbilityAthena_PeriodicItemGrantDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortGameplayAbilityAthena_PeriodicItemGrant.StartItemAwardTimers"),
            UFortGameplayAbilityAthena_PeriodicItemGrant::StartItemAwardTimersHook, (PVOID*)&UFortGameplayAbilityAthena_PeriodicItemGrant::StartItemAwardTimersOriginal, false, true);
    }

    Hooking::MinHook::Hook(FindObject<AFortAthenaMutator_Barrier>(L"/Script/FortniteGame.Default__FortAthenaMutator_Barrier"), FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaMutator_Barrier.OnGamePhaseStepChanged"),
        AFortAthenaMutator_Barrier::OnGamePhaseStepChangedHook, (PVOID*)&AFortAthenaMutator_Barrier::OnGamePhaseStepChangedOriginal, false, true);
    Hooking::MinHook::Hook(FindObject<AFortAthenaMutator_Disco>(L"/Script/FortniteGame.Default__FortAthenaMutator_Disco"), FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaMutator_Disco.OnGamePhaseStepChanged"),
        AFortAthenaMutator_Disco::OnGamePhaseStepChangedHook, (PVOID*)&AFortAthenaMutator_Disco::OnGamePhaseStepChangedOriginal, false, true);
    Hooking::MinHook::Hook(FindObject<AFortAthenaMutator_GiveItemsAtGamePhaseStep>(L"/Script/FortniteGame.Default__FortAthenaMutator_GiveItemsAtGamePhaseStep"), FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaMutator_GiveItemsAtGamePhaseStep.OnGamePhaseStepChanged"),
        AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChangedHook, (PVOID*)&AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChangedOriginal, false, true);

    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.GetAIDirector"),
        UFortKismetLibrary::GetAIDirectorHook, (PVOID*)&UFortKismetLibrary::GetAIDirectorOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.GetAIGoalManager"),
        UFortKismetLibrary::GetAIGoalManagerHook, (PVOID*)&UFortKismetLibrary::GetAIGoalManagerOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_GiveItemToPlayer"),
        UFortKismetLibrary::K2_GiveItemToPlayerHook, (PVOID*)&UFortKismetLibrary::K2_GiveItemToPlayerOriginal, false, true);
    // tracing of the mission's encounter starts (does the mission script reach them, and what comes back)
    if (auto MissionDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortMission"))
    {
        Hooking::MinHook::Hook(MissionDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortMission.StartMissionAIEncounterFromGeneratedProfile"),
            Zone::StartEncounterTraceHook, (PVOID*)&Zone::StartEncounterTraceOriginal, false, true);
    }

    if (auto DecoToolDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortDecoTool"))
    {
        Hooking::MinHook::Hook(DecoToolDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortDecoTool.ServerSpawnDeco"),
            Zone::ServerSpawnDecoHook, (PVOID*)&Zone::ServerSpawnDecoOriginal, false, true);
    }

    // Save the World mission item functions (the hooks pass through to the game's own outside the Zone)
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_GiveItemToAllPlayers"),
        UFortKismetLibrary::K2_GiveItemToAllPlayersHook, (PVOID*)&UFortKismetLibrary::K2_GiveItemToAllPlayersOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_RemoveItemFromAllPlayers"),
        UFortKismetLibrary::K2_RemoveItemFromAllPlayersHook, (PVOID*)&UFortKismetLibrary::K2_RemoveItemFromAllPlayersOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_GetItemQuantityOnPlayer"),
        UFortKismetLibrary::K2_GetItemQuantityOnPlayerHook, (PVOID*)&UFortKismetLibrary::K2_GetItemQuantityOnPlayerOriginal, false, true);
    if (auto MissionLibraryDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortMissionLibrary"))
        Hooking::MinHook::Hook(MissionLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary.SpawnAndCollectPlayerPickups"),
            Zone::SpawnAndCollectPlayerPickupsHook, (PVOID*)&Zone::SpawnAndCollectPlayerPickupsOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_GiveBuildingResource"),
        UFortKismetLibrary::K2_GiveBuildingResourceHook, (PVOID*)&UFortKismetLibrary::K2_GiveBuildingResourceOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.GiveItemToInventoryOwner"),
        UFortKismetLibrary::GiveItemToInventoryOwnerHook, (PVOID*)&UFortKismetLibrary::GiveItemToInventoryOwnerOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_RemoveItemFromPlayerByGuid"),
        UFortKismetLibrary::K2_RemoveItemFromPlayerByGuidHook, (PVOID*)&UFortKismetLibrary::K2_RemoveItemFromPlayerByGuidOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_RemoveItemFromPlayer"),
        UFortKismetLibrary::K2_RemoveItemFromPlayerHook, (PVOID*)&UFortKismetLibrary::K2_RemoveItemFromPlayerOriginal, false, true);

    Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, NetMulticast_Athena_BatchedDamageCuesFn,
        AFortPawn::NetMulticast_Athena_BatchedDamageCuesHook, (PVOID*)&AFortPawn::NetMulticast_Athena_BatchedDamageCuesOriginal, false, true);
    Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.MovingEmoteStopped"),
        AFortPawn::MovingEmoteStoppedHook, (PVOID*)&AFortPawn::MovingEmoteStoppedOriginal, false, true);
    if (Fortnite_Version < 20) // todo
    {
        // Hook the base FortPlayerPawn class which both Athena and Zone pawns inherit from
            // OnCapsuleBeginOverlap hook disabled - causes crashes on Zone pawns (FortPlayerPawn/PlayerPawn_Generic)
            // The hook was installed on FortPlayerPawnAthena but VirtualSwap also hooks the parent FortPlayerPawn
            // vtable entry, and the hook function signature doesn't match what Zone pawns expect.
            // Zone uses native pickup handling which works correctly without this hook.
            // Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.OnCapsuleBeginOverlap"),
            //     AFortPlayerPawnAthena::OnCapsuleBeginOverlapHook, (PVOID*)&AFortPlayerPawnAthena::OnCapsuleBeginOverlapOriginal, false, true);
    }


    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_RemoveFortItemFromPlayer"),
        UFortKismetLibrary::K2_RemoveFortItemFromPlayerHook, (PVOID*)&UFortKismetLibrary::K2_RemoveFortItemFromPlayerOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_SpawnPickupInWorld"),
        UFortKismetLibrary::K2_SpawnPickupInWorldHook, (PVOID*)&UFortKismetLibrary::K2_SpawnPickupInWorldOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_SpawnPickupInWorldWithLootTier"),
        UFortKismetLibrary::K2_SpawnPickupInWorldWithLootTierHook, (PVOID*)&UFortKismetLibrary::K2_SpawnPickupInWorldWithLootTierOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.K2_SpawnPickupInWorldWithClass"),
        UFortKismetLibrary::K2_SpawnPickupInWorldWithClassHook, (PVOID*)&UFortKismetLibrary::K2_SpawnPickupInWorldWithClassOriginal, false, true);

    // if (Addresses::FreeArrayOfEntries || Addresses::FreeEntry)
    {
        Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.PickLootDrops"),
            UFortKismetLibrary::PickLootDropsHook, (PVOID*)&UFortKismetLibrary::PickLootDropsOriginal, false, true);
    }

    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.CreateTossAmmoPickupForWeaponItemDefinitionAtLocation"),
        UFortKismetLibrary::CreateTossAmmoPickupForWeaponItemDefinitionAtLocationHook, (PVOID*)&UFortKismetLibrary::CreateTossAmmoPickupForWeaponItemDefinitionAtLocationOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.SpawnInstancedPickupInWorld"),
        UFortKismetLibrary::SpawnInstancedPickupInWorldHook, (PVOID*)&UFortKismetLibrary::SpawnInstancedPickupInWorldOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.SpawnItemVariantPickupInWorld"),
        UFortKismetLibrary::SpawnItemVariantPickupInWorldHook, (PVOID*)&UFortKismetLibrary::SpawnItemVariantPickupInWorldOriginal, false, true);
    Hooking::MinHook::Hook(FortKismetLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortKismetLibrary.PickLootDropsWithNamedWeights"),
        UFortKismetLibrary::PickLootDropsWithNamedWeightsHook, (PVOID*)&UFortKismetLibrary::PickLootDropsWithNamedWeightsOriginal, false, true);

    // TODO Add RemoveItemFromInventoryOwner

    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.SpawnToyInstance"),
        AFortPlayerController::SpawnToyInstanceHook, (PVOID*)&AFortPlayerController::SpawnToyInstanceOriginal, false, true);
    Hooking::MinHook::Hook(FortPlayerControllerAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.DropSpecificItem"),
        AFortPlayerController::DropSpecificItemHook, (PVOID*)&AFortPlayerController::DropSpecificItemOriginal, false, true);

    static auto FortAthenaSupplyDropDefault = FindObject(L"/Script/FortniteGame.Default__FortAthenaSupplyDrop");

    Hooking::MinHook::Hook(FortAthenaSupplyDropDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaSupplyDrop.SpawnPickup"),
        AFortAthenaSupplyDrop::SpawnPickupHook, (PVOID*)&AFortAthenaSupplyDrop::SpawnPickupOriginal, false, true);
    Hooking::MinHook::Hook(FortAthenaSupplyDropDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaSupplyDrop.SpawnGameModePickup"),
        AFortAthenaSupplyDrop::SpawnGameModePickupHook, (PVOID*)&AFortAthenaSupplyDrop::SpawnGameModePickupOriginal, false, true);
    Hooking::MinHook::Hook(FortAthenaSupplyDropDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaSupplyDrop.SpawnPickupFromItemEntry"),
        AFortAthenaSupplyDrop::SpawnPickupFromItemEntryHook, (PVOID*)&AFortAthenaSupplyDrop::SpawnPickupFromItemEntryOriginal, false, true);

    static auto FortAthenaCreativePortalDefault = FindObject(L"/Script/FortniteGame.Default__FortAthenaCreativePortal");

    Hooking::MinHook::Hook(FortAthenaCreativePortalDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaCreativePortal.TeleportPlayerToLinkedVolume"),
        AFortAthenaCreativePortal::TeleportPlayerToLinkedVolumeHook, (PVOID*)&AFortAthenaCreativePortal::TeleportPlayerToLinkedVolumeOriginal, false, true);
    Hooking::MinHook::Hook(FortAthenaCreativePortalDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortAthenaCreativePortal.TeleportPlayer"),
        AFortAthenaCreativePortal::TeleportPlayerHook, (PVOID*)&AFortAthenaCreativePortal::TeleportPlayerOriginal, false, true);

    static auto FortMinigameDefault = FindObject(L"/Script/FortniteGame.Default__FortMinigame");

    Hooking::MinHook::Hook(FortMinigameDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortMinigame.ClearPlayerInventory"),
        AFortMinigame::ClearPlayerInventoryHook, (PVOID*)&AFortMinigame::ClearPlayerInventoryOriginal, false, true);

    static auto InventoryManagementLibraryDefault = FindObject<UInventoryManagementLibrary>(L"/Script/FortniteGame.Default__InventoryManagementLibrary");

    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.AddItem"),
        UInventoryManagementLibrary::AddItemHook, (PVOID*)&UInventoryManagementLibrary::AddItemOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.AddItems"),
        UInventoryManagementLibrary::AddItemsHook, (PVOID*)&UInventoryManagementLibrary::AddItemsOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.GiveItemEntryToInventoryOwner"),
        UInventoryManagementLibrary::GiveItemEntryToInventoryOwnerHook, (PVOID*)&UInventoryManagementLibrary::GiveItemEntryToInventoryOwnerOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.RemoveItem"),
        UInventoryManagementLibrary::RemoveItemHook, (PVOID*)&UInventoryManagementLibrary::RemoveItemOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.RemoveItems"),
        UInventoryManagementLibrary::RemoveItemsHook, (PVOID*)&UInventoryManagementLibrary::RemoveItemsOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.SwapItem"),
        UInventoryManagementLibrary::SwapItemHook, (PVOID*)&UInventoryManagementLibrary::SwapItemOriginal, false, true);
    Hooking::MinHook::Hook(InventoryManagementLibraryDefault, FindObject<UFunction>(L"/Script/FortniteGame.InventoryManagementLibrary.SwapItems"),
        UInventoryManagementLibrary::SwapItemsHook, (PVOID*)&UInventoryManagementLibrary::SwapItemsOriginal, false, true);

    // Pickup hooks for both BR and Zone: weapons require ServerHandlePickupWithRequestedSwap
    // (which was previously disabled for Zone mode, causing weapons to not enter inventory)
    static auto ServerHandlePickupInfoFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerHandlePickupInfo");

    if (ServerHandlePickupInfoFn)
    {
        Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, ServerHandlePickupInfoFn, AFortPlayerPawn::ServerHandlePickupInfoHook, nullptr, false);
    }
    else
    {
        Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerHandlePickup"),
            AFortPlayerPawn::ServerHandlePickupHook, nullptr, false);
        Hooking::MinHook::Hook(FortPlayerPawnAthenaDefault, FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerPawn.ServerHandlePickupWithRequestedSwap"),
            AFortPlayerPawn::ServerHandlePickupWithRequestedSwapHook, (PVOID*)&AFortPlayerPawn::ServerHandlePickupWithRequestedSwapOriginal, false, true);
    }

    static auto PredictionKeyStruct = FindObject<UStruct>(L"/Script/GameplayAbilities.PredictionKey");
    static auto PredictionKeySize = PredictionKeyStruct->GetPropertiesSize();

    if (Addresses::InternalTryActivateAbility)
    {
        int InternalServerTryActivateAbilityIndex = 0;

        if (Engine_Version > 420)
        {
            static auto OnRep_ReplicatedAnimMontageFn = FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.OnRep_ReplicatedAnimMontage");
            InternalServerTryActivateAbilityIndex = (GetFunctionIdxOrPtr(OnRep_ReplicatedAnimMontageFn) - 8) / 8;
        }
        else
        {
            static auto ServerTryActivateAbilityWithEventDataFn = FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.ServerTryActivateAbilityWithEventData");
            auto ServerTryActivateAbilityWithEventDataNativeAddr = __int64(FortAbilitySystemComponentAthenaDefault->VFTable[GetFunctionIdxOrPtr(ServerTryActivateAbilityWithEventDataFn) / 8]);

            for (int i = 0; i < 400; i++)
            {
                if ((*(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i) == 0xFF && *(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i + 1) == 0x90) || // call qword ptr
                    (*(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i) == 0xFF && *(uint8_t*)(ServerTryActivateAbilityWithEventDataNativeAddr + i + 1) == 0x93)) // call qword ptr
                {
                    InternalServerTryActivateAbilityIndex = GetIndexFromVirtualFunctionCall(ServerTryActivateAbilityWithEventDataNativeAddr + i) / 8;
                    break;
                }
            }
        }

        LOG_INFO(LogDev, "InternalServerTryActivateAbilityIndex: 0x{:x}", InternalServerTryActivateAbilityIndex);

        VirtualSwap(FortAbilitySystemComponentAthenaDefault->VFTable, InternalServerTryActivateAbilityIndex, UAbilitySystemComponent::InternalServerTryActivateAbilityHook);
        if (auto FortASCDefault = FindObject(L"/Script/FortniteGame.Default__FortAbilitySystemComponent"))
            VirtualSwap(FortASCDefault->VFTable, InternalServerTryActivateAbilityIndex, UAbilitySystemComponent::InternalServerTryActivateAbilityHook);
        if (auto BaseASCDefault = FindObject(L"/Script/GameplayAbilities.Default__AbilitySystemComponent"))
            VirtualSwap(BaseASCDefault->VFTable, InternalServerTryActivateAbilityIndex, UAbilitySystemComponent::InternalServerTryActivateAbilityHook);
    }

    // if (Engine_Version >= 424)
    {
        static auto FortControllerComponent_AircraftDefault = FindObject<AActor>(L"/Script/FortniteGame.Default__FortControllerComponent_Aircraft");
        static auto ServerAttemptAircraftJumpFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerAttemptAircraftJump") ? FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerAttemptAircraftJump")
            : FindObject<UFunction>(L"/Script/FortniteGame.FortControllerComponent_Aircraft.ServerAttemptAircraftJump");

        Hooking::MinHook::Hook(FortControllerComponent_AircraftDefault ? FortControllerComponent_AircraftDefault : FortPlayerControllerAthenaDefault, ServerAttemptAircraftJumpFn,
            AFortPlayerController::ServerAttemptAircraftJumpHook, (PVOID*)&AFortPlayerController::ServerAttemptAircraftJumpOriginal, false);
    }

    // if (false)
    {
        if (Fortnite_Version >= 8.3 && Engine_Version < 424) // I can't remember, so ServerAddMapMarker existed on like 8.0 or 8.1 or 8.2 but it didn't have the same params.
        {
            Hooking::MinHook::Hook(AthenaMarkerComponentDefault, FindObject<UFunction>(L"/Script/FortniteGame.AthenaMarkerComponent.ServerAddMapMarker"),
                UAthenaMarkerComponent::ServerAddMapMarkerHook, nullptr, false);
            Hooking::MinHook::Hook(AthenaMarkerComponentDefault, FindObject<UFunction>(L"/Script/FortniteGame.AthenaMarkerComponent.ServerRemoveMapMarker"),
                UAthenaMarkerComponent::ServerRemoveMapMarkerHook, nullptr, false);
        }
    }

    Hooking::MinHook::Hook((PVOID)Addresses::GetPlayerViewpoint, (PVOID)AFortPlayerControllerAthena::GetPlayerViewPointHook, (PVOID*)&AFortPlayerControllerAthena::GetPlayerViewPointOriginal);

    Hooking::MinHook::Hook((PVOID)Addresses::TickFlush, (PVOID)UNetDriver::TickFlushHook, (PVOID*)&UNetDriver::TickFlushOriginal);
    Hooking::MinHook::Hook((PVOID)Addresses::OnDamageServer, (PVOID)ABuildingActor::OnDamageServerHook, (PVOID*)&ABuildingActor::OnDamageServerOriginal);
    
    Hooking::MinHook::Hook((PVOID)Addresses::GetMaxTickRate, Zone_MaxTickRateHook);
    // Hooking::MinHook::Hook((PVOID)Addresses::CollectGarbage, (PVOID)CollectGarbageHook, nullptr);
    Hooking::MinHook::Hook((PVOID)Addresses::PickTeam, (PVOID)AFortGameModeAthena::Athena_PickTeamHook);
    Hooking::MinHook::Hook((PVOID)Addresses::CompletePickupAnimation, (PVOID)AFortPickup::CompletePickupAnimationHook, (PVOID*)&AFortPickup::CompletePickupAnimationOriginal);
    Hooking::MinHook::Hook((PVOID)Addresses::CanActivateAbility, ReturnTrueHook); // ahhh wtf

    uint64 ServerRemoveInventoryItemFunctionCallBeginFunctionAddr = 0;

    // if (Engine_Version >= 419)
    if (Fortnite_Version < 20)
    {
        std::vector<uint8_t> ServerRemoveInventoryItemCallFunctionStarts = Engine_Version == 416 
            ? std::vector<uint8_t>{ 0x44, 0x88, 0x4C } 
            : Fortnite_Version >= 16 
                ? std::vector<uint8_t>{ 0x48, 0x8B, 0xC4 }
            : std::vector<uint8_t>{ 0x48, 0x89, 0x5C };

        auto ServerRemoveInventoryItemCallFunctionCall = FindFunctionCall(L"ServerRemoveInventoryItem", ServerRemoveInventoryItemCallFunctionStarts);
        auto ServerRemoveInventoryItemFunctionCallRef = Memcury::Scanner::FindPointerRef((PVOID)ServerRemoveInventoryItemCallFunctionCall, 0, true);

        LOG_INFO(LogDev, "ServerRemoveInventoryItemFunctionCallRef: 0x{:x}", ServerRemoveInventoryItemFunctionCallRef.Get() - __int64(GetModuleHandleW(0)));

        for (int i = 0; i < 400; i++)
        {
            // LOG_INFO(LogDev, "[{}] Bugha: 0x{:x}", i, (int)(*(uint8_t*)ServerRemoveInventoryItemFunctionCallRef.Get() - i));
            if (*(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i) == 0x48 && *(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i + 1) == 0x89 && *(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i + 2) == 0x5C)
            {
                ServerRemoveInventoryItemFunctionCallBeginFunctionAddr = ServerRemoveInventoryItemFunctionCallRef.Get() - i;
                break;
            }

            if (*(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i) == 0x48 && *(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i + 1) == 0x83 && *(uint8_t*)(uint8_t*)(ServerRemoveInventoryItemFunctionCallRef.Get() - i + 2) == 0xEC)
            {
                ServerRemoveInventoryItemFunctionCallBeginFunctionAddr = ServerRemoveInventoryItemFunctionCallRef.Get() - i;
                break;
            }
        }
    }

    Hooking::MinHook::Hook(Memcury::Scanner(ServerRemoveInventoryItemFunctionCallBeginFunctionAddr).GetAs<PVOID>(), UFortInventoryInterface::RemoveInventoryItemHook);
   
    // if (Fortnite_Version >= 13)
    Hooking::MinHook::Hook((PVOID)Addresses::SetZoneToIndex, (PVOID)SetZoneToIndexHook, (PVOID*)&SetZoneToIndexOriginal);
    Hooking::MinHook::Hook((PVOID)Addresses::EnterAircraft, (PVOID)AFortPlayerControllerAthena::EnterAircraftHook, (PVOID*)&AFortPlayerControllerAthena::EnterAircraftOriginal);

    AddVehicleHook();

    auto ClientOnPawnDiedCallAddr = FindFunctionCall(L"ClientOnPawnDied", Engine_Version == 416 ? std::vector<uint8_t>{ 0x48, 0x89, 0x54 } : std::vector<uint8_t>{ 0x48, 0x89, 0x5C });
    LOG_INFO(LogDev, "ClientOnPawnDiedCallAddr: 0x{:x}", ClientOnPawnDiedCallAddr - __int64(GetModuleHandleW(0)));
    Hooking::MinHook::Hook((PVOID)ClientOnPawnDiedCallAddr, AFortPlayerController::ClientOnPawnDiedHook, (PVOID*)&AFortPlayerController::ClientOnPawnDiedOriginal);

#if 0
    auto OnSafeZoneStateChangeAddr = FindFunctionCall(L"OnSafeZoneStateChange", Engine_Version == 416 ? std::vector<uint8_t>{ 0x48, 0x89, 0x54 } : std::vector<uint8_t>{ 0x48, 0x89, 0x5C });
    LOG_INFO(LogDev, "OnSafeZoneStateChangeAddr: 0x{:x}", OnSafeZoneStateChangeAddr - __int64(GetModuleHandleW(0)));
    Hooking::MinHook::Hook((PVOID)OnSafeZoneStateChangeAddr, AFortSafeZoneIndicator::OnSafeZoneStateChangeHook, (PVOID*)&AFortSafeZoneIndicator::OnSafeZoneStateChangeOriginal);
#endif

    LOG_INFO(LogDev, "PredictionKeySize: 0x{:x} {}", PredictionKeySize, PredictionKeySize);

    static auto GameplayEventDataSize = FindObject<UStruct>(L"/Script/GameplayAbilities.GameplayEventData")->GetPropertiesSize();
    LOG_INFO(LogDev, "GameplayEventDataSize: 0x{:x} {}", GameplayEventDataSize, GameplayEventDataSize);

    {
        int increaseOffset = 0x10;

        if (Fortnite_Version > 11.00 && std::floor(Fortnite_Version) < 18) // checked on 11.00, 11.31, 12.41, 14.60, 15.10, 16.40, 17.30 and 18.40
            increaseOffset += 0x8;

        auto MoveSoundStimulusBroadcastIntervalOffset = FindOffsetStruct("/Script/FortniteGame.FortPlayerPawn", "MoveSoundStimulusBroadcastInterval");
        MemberOffsets::FortPlayerPawn::CorrectTags = MoveSoundStimulusBroadcastIntervalOffset + increaseOffset;
        LOG_INFO(LogDev, "CorrectTags: 0x{:x}", MemberOffsets::FortPlayerPawn::CorrectTags);
        MemberOffsets::FortPlayerState::PawnDeathLocation = FindOffsetStruct("/Script/FortniteGame.FortPlayerState", "PawnDeathLocation", false);

        MemberOffsets::FortPlayerPawnAthena::LastFallDistance = FindOffsetStruct("/Script/FortniteGame.FortPlayerPawnAthena", "LastFallDistance", false);

        MemberOffsets::FortPlayerStateAthena::DeathInfo = FindOffsetStruct("/Script/FortniteGame.FortPlayerStateAthena", "DeathInfo");
        MemberOffsets::FortPlayerStateAthena::KillScore = FindOffsetStruct("/Script/FortniteGame.FortPlayerStateAthena", "KillScore", false);
        MemberOffsets::FortPlayerStateAthena::TeamKillScore = FindOffsetStruct("/Script/FortniteGame.FortPlayerStateAthena", "TeamKillScore", false);

        MemberOffsets::DeathInfo::bDBNO = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "bDBNO");
        MemberOffsets::DeathInfo::DeathCause = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "DeathCause");
        MemberOffsets::DeathInfo::bInitialized = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "bInitialized", false);
        MemberOffsets::DeathInfo::Distance = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "Distance", false);
        MemberOffsets::DeathInfo::DeathTags = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "DeathTags", false);
        MemberOffsets::DeathInfo::DeathLocation = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "DeathLocation", false);
        MemberOffsets::DeathInfo::bIsWeakFinisherOrDowner = false;

        MemberOffsets::DeathReport::Tags = FindOffsetStruct("/Script/FortniteGame.FortPlayerDeathReport", "Tags");
        MemberOffsets::DeathReport::KillerPawn = FindOffsetStruct("/Script/FortniteGame.FortPlayerDeathReport", "KillerPawn");
        MemberOffsets::DeathReport::KillerPlayerState = FindOffsetStruct("/Script/FortniteGame.FortPlayerDeathReport", "KillerPlayerState");
        MemberOffsets::DeathReport::DamageCauser = FindOffsetStruct("/Script/FortniteGame.FortPlayerDeathReport", "DamageCauser");
    }

    srand(time(0));

    Timing::Mark("Main finished");
    LOG_INFO(LogHook, "Finished initialization!");

    if (false)
    {
        while (true)
        {
            Sleep(10000);
        }
    }
    else
    {
        Sleep(-1);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(0, 0, Main, 0, 0, 0);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}

