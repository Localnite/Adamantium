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

static bool Zone_Objective_StartPlayingObjective(UObject* Objective)
{
    if (!Objective) return false;

    static int StatusOffset = Objective->GetOffset("ObjectiveStatus", false);
    if (StatusOffset == -1) { StatusOffset = 0x3bf; LOG_WARN(LogDev, "Zone Objective: ObjectiveStatus reflection offset missing, using 0x3bf (7.30.1 value)"); }

    uint8_t* StatusPtr = (uint8_t*)Objective + StatusOffset;
    LOG_INFO(LogDev, "Zone Objective StartPlayingObjective: {} (prev status {})", Objective->GetFullName(), (int)*StatusPtr);

    if (*StatusPtr != 1)
    {
        *StatusPtr = 1; // EFortObjectiveStatus::InProgress

        static auto OnRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.OnRep_ObjectiveStatus");
        if (OnRepFn) Objective->ProcessEvent(OnRepFn);

        static int OwnerOffset = Objective->GetOffset("Owner", false);
        if (OwnerOffset == -1) OwnerOffset = 0x108;
        auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset);
        if (Mission)
        {
            if (Mission->VFTable)
            {
                auto OnObjStatusChanged = (void (*)(UObject*, UObject*, uint8_t))(Mission->VFTable[0x6b8 / 8]);
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
                static auto ManageInvFn = Mission->FindFunction("ManageInventory");
                if (ManageInvFn)
                {
                    LOG_INFO(LogDev, "Zone: Invoking Mission->ManageInventory()");
                    Mission->ProcessEvent(ManageInvFn, nullptr);
                }
                else
                {
                    LOG_ERROR(LogDev, "Zone: ManageInventory function not found on Mission!");
                }

                static auto StartEncounterFn = Mission->FindFunction("StartFirstEncounter");
                if (StartEncounterFn)
                {
                    LOG_INFO(LogDev, "Zone: Invoking Mission->StartFirstEncounter()");
                    Mission->ProcessEvent(StartEncounterFn, nullptr);
                }
                else
                {
                    LOG_ERROR(LogDev, "Zone: StartFirstEncounter function not found on Mission!");
                }
            }

            // Verify the cutscene handgun and slot 1 activation
            auto World = GetWorld();
            static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
            if (World && PlayerControllerClass)
            {
                auto PCs = UGameplayStatics::GetAllActorsOfClass(World, PlayerControllerClass);
                for (int i = 0; i < PCs.Num(); i++)
                {
                    auto PC = Cast<AFortPlayerController>(PCs.at(i));
                    if (!PC) continue;
                    auto Inv = PC->GetWorldInventory();
                    if (!Inv) continue;

                    static auto HandgunDef = LoadObject<UFortItemDefinition>(L"/Game/Items/Weapons/Ranged/Pistol/SemiAuto/WID_Pistol_SemiAuto_C_Ore_T00.WID_Pistol_SemiAuto_C_Ore_T00");
                    if (HandgunDef)
                    {
                        bool bFound = false;
                        for (int j = 0; j < Inv->GetItemList().GetItemInstances().Num(); j++)
                        {
                            auto ItemInst = Inv->GetItemList().GetItemInstances().at(j);
                            if (ItemInst && ItemInst->GetItemEntry() && ItemInst->GetItemEntry()->GetItemDefinition() == HandgunDef)
                            {
                                bFound = true;
                                break;
                            }
                        }
                        if (!bFound)
                        {
                            LOG_INFO(LogDev, "Zone: Handgun not found after ManageInventory, adding manually");
                            bool bShouldUpdate = false;
                            auto Added = Inv->AddItem(HandgunDef, &bShouldUpdate, 1);
                            auto QuickBarsActor = PC->GetOffset("QuickBars", false) != -1 ? PC->Get<AActor*>(PC->GetOffset("QuickBars", false)) : nullptr;
                            static auto ServerAddItemInternalFn = FindObject<UFunction>(L"/Script/FortniteGame.FortQuickBars.ServerAddItemInternal");
                            if (QuickBarsActor && ServerAddItemInternalFn && Added.first.size() > 0)
                            {
                                struct { FGuid Item; uint8 Bar; int Slot; } SlotParams{ Added.first[0]->GetItemEntry()->GetItemGuid(), 0, 1 };
                                QuickBarsActor->ProcessEvent(ServerAddItemInternalFn, &SlotParams);
                            }
                            if (bShouldUpdate) Inv->Update();
                        }

                        static auto ServerActivateSlotFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerController.ServerActivateSlot");
                        if (ServerActivateSlotFn)
                        {
                            struct { uint8 Bar; int Slot; float Duration; bool bForce; bool bFromServer; } ActParams{ 0, 1, 0.0f, true, false };
                            PC->ProcessEvent(ServerActivateSlotFn, &ActParams);
                        }
                    }
                }
                PCs.Free();
            }

            Zone::SpawnTutorialHusks(Objective);
            
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

    static int OwnerOffset = Objective->GetOffset("Owner", false);
    if (OwnerOffset == -1) OwnerOffset = 0x108;
    auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset);

    bool bStartOnMissionStart = (*((uint8_t*)Objective + 0x380) & 1) != 0;
    if (Mission && bStartOnMissionStart)
    {
        static int MissionStatusOffset = Mission->GetOffset("MissionStatus", false);
        if (MissionStatusOffset == -1) MissionStatusOffset = 0x488;
        if (*((uint8_t*)Mission + MissionStatusOffset) == 1)
        {
            Zone_Objective_StartPlayingObjective(Objective);
        }
    }

    return true;
}

static void Zone_Objective_StopObjective(UObject* Objective, uint8_t Status, void* Description, UObject* Badge)
{
    if (!Objective) return;

    static int StatusOffset = Objective->GetOffset("ObjectiveStatus", false);
    if (StatusOffset == -1) { StatusOffset = 0x3bf; LOG_WARN(LogDev, "Zone Objective: ObjectiveStatus reflection offset missing, using 0x3bf (7.30.1 value)"); }

    uint8_t* StatusPtr = (uint8_t*)Objective + StatusOffset;
    LOG_INFO(LogDev, "Zone Objective StopObjective: {} -> status {}", Objective->GetFullName(), (int)Status);

    if (*StatusPtr != Status)
    {
        *StatusPtr = Status;

        static auto OnRepFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.OnRep_ObjectiveStatus");
        if (OnRepFn) Objective->ProcessEvent(OnRepFn);

        static auto BpEndFn = FindObject<UFunction>(L"/Script/FortniteGame.FortObjectiveBase.BlueprintOnObjectiveEnd");
        if (BpEndFn) Objective->ProcessEvent(BpEndFn, &Status);

        static int OwnerOffset = Objective->GetOffset("Owner", false);
        if (OwnerOffset == -1) OwnerOffset = 0x108;
        if (auto Mission = *(UObject**)((uint8_t*)Objective + OwnerOffset))
        {
            // 0x6b8 is the mission's OnObjectiveStatusChanged slot on 7.30.1; a null vtable must not be called through
            if (Mission->VFTable)
            {
                auto OnObjStatusChanged = (void (*)(UObject*, UObject*, uint8_t))(Mission->VFTable[0x6b8 / 8]);
                OnObjStatusChanged(Mission, Objective, Status);
            }
            else
            {
                LOG_WARN(LogDev, "Zone Objective: mission has no vtable, status change not forwarded");
            }
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

static void Zone_PatchAIDirectorCallers()
{
    if (Fortnite_Version < 7.3 || Fortnite_Version >= 8.0) return;

    static bool bDone = false;
    if (bDone) return;
    bDone = true;

    auto Base = (uint8_t*)GetModuleHandleW(0);
    auto Nt = (PIMAGE_NT_HEADERS)(Base + ((PIMAGE_DOS_HEADER)Base)->e_lfanew);
    const uint64_t ImageSize = Nt->OptionalHeader.SizeOfImage;
    const uint64_t Target = (uint64_t)Base + 0x2b1cf0;

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
            // call [rax+0x140]; mov rcx, rax; call
            static const uint8_t Pre[9] = { 0xFF, 0x90, 0x40, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xC8 };
            if (memcmp(Start + i - 9, Pre, 9) != 0) continue;

            const uint64_t Dest = (uint64_t)(Start + i + 5) + *(int32_t*)(Start + i + 1);
            if (Dest != Target) continue;

            DWORD Old = 0;
            if (VirtualProtect(Start + i + 1, 4, PAGE_EXECUTE_READWRITE, &Old))
            {
                *(int32_t*)(Start + i + 1) = (int32_t)((int64_t)Stub - (int64_t)(Start + i + 5));
                VirtualProtect(Start + i + 1, 4, Old, &Old);
                ++Patched;
            }
        }
    }

    // zero matches means the byte pattern or the 0x2b1cf0 target is not on this build: AI director lookups then stay on the stub-less path
    if (Patched == 0)
    {
        LOG_WARN(LogDev, "Zone: no AI director call sites matched the 7.30.1 pattern, AI director redirect is OFF");
        return;
    }

    LOG_INFO(LogDev, "Zone: redirected {} AI director lookups to GameMode->AIDirector", Patched);
}

// The Fort replication graph has no route for FortQuickBars (class policy 0, not routed; FortInventory is too but the graph collects the controller's WorldInventory by
// interface), so a quickbars actor would never reach the client and ClientRestart waits for it forever. Policy 1 = relevant to all connections. 7.30.1 (CL 4834550) addresses
// found with fnmod/re/d730.py: ClassRepNodePolicies is at graph+0x4e0 and its lookup is exe+0x133c8d0.
static void Zone_RouteQuickBars()
{
    // the GetPolicy RVA (0x133c8d0) and the graph member offset (0x4e0) were read from the 7.30.1 build only; any other
    // build would call an unrelated function, so this is held to 7.30 until those are re-checked for each new build
    if (Fortnite_Version < 7.3 || Fortnite_Version >= 7.31) return;

    auto World = (UObject*)GetWorld();
    auto Driver = World ? World->Get<UObject*>("NetDriver") : nullptr;
    auto Graph = Driver ? Driver->Get<UObject*>("ReplicationDriver") : nullptr;

    if (!Graph)
    {
        LOG_INFO(LogDev, "Zone: no replication graph on the net driver, quickbars cannot be routed.");
        return;
    }

    auto GetPolicy = (int* (*)(void*, UObject*))(__int64(GetModuleHandleW(0)) + 0x133c8d0);

    for (auto Path : { L"/Script/FortniteGame.FortQuickBars", L"/Script/FortniteGame.FortQuickBarsAthena" })
    {
        auto Class = FindObject<UClass>(Path);
        if (!Class) continue;

        auto Policy = GetPolicy((char*)Graph + 0x4e0, Class);
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

    // 7.30.1 AI crash patches for spawning AI pawns on client binary acting as dedicated server:
    if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
    {
        uint8_t* Base = (uint8_t*)GetModuleHandleW(0);

        // 1. Patch GetPawnGoalSelectionCriteria null criteria crash (+0xbd964d: cmp rbx, rdi -> test rbx, rbx)
        uint8_t* Target1 = Base + 0xbd964d;
        if (Target1[0] == 0x48 && Target1[1] == 0x3B && Target1[2] == 0xDF)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target1, 3, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target1[1] = 0x85; // test
                Target1[2] = 0xDB; // rbx, rbx
                VirtualProtect(Target1, 3, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: patched GetPawnGoalSelectionCriteria at +0xbd964d");
            }
        }

        // 2. Patch AFortAIPawn::BeginPlay AIDirector assignment call (+0xb27ce8: je -> jmp)
        uint8_t* Target2 = Base + 0xb27ce8;
        if (Target2[0] == 0x74 && Target2[1] == 0x0B)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target2, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target2[0] = 0xEB; // jmp +0x0b (skip 0xb7f0c0 assignment registration)
                VirtualProtect(Target2, 1, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: patched AFortAIPawn::BeginPlay at +0xb27ce8");
            }
        }

        // 3. Stub 0xb7f0c0 (assignment registration) to immediately ret
        uint8_t* Target3 = Base + 0xb7f0c0;
        if (Target3[0] == 0x40 && Target3[1] == 0x55)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target3, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target3[0] = 0xC3; // ret
                VirtualProtect(Target3, 1, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: stubbed assignment registration at +0xb7f0c0");
            }
        }

        // 4. Stub MakeGoalSelectionQuery (+0xb84a10: xor eax, eax; ret)
        uint8_t* Target4 = Base + 0xb84a10;
        if (Target4[0] == 0x48 && Target4[1] == 0x89)
        {
            DWORD OldProtect = 0;
            if (VirtualProtect(Target4, 3, PAGE_EXECUTE_READWRITE, &OldProtect))
            {
                Target4[0] = 0x31; Target4[1] = 0xC0; Target4[2] = 0xC3; // xor eax, eax; ret
                VirtualProtect(Target4, 3, OldProtect, &OldProtect);
                LOG_INFO(LogDev, "Zone: stubbed MakeGoalSelectionQuery at +0xb84a10");
            }
        }
    }

    // 2. Restore stripped WITH_SERVER_CODE virtual methods on AFortObjectiveBase (ActivateObjective, StartPlayingObjective, StopObjective).
    if (auto DefaultObjBase = FindObject<UObject>(L"/Script/FortniteGame.Default__FortObjectiveBase"))
    {
        VirtualSwap(DefaultObjBase->VFTable, 0x610 / 8, Zone_Objective_ActivateObjective);
        VirtualSwap(DefaultObjBase->VFTable, 0x618 / 8, Zone_Objective_StartPlayingObjective);
        VirtualSwap(DefaultObjBase->VFTable, 0x660 / 8, Zone_Objective_StopObjective);
        LOG_INFO(LogDev, "Zone: hooked AFortObjectiveBase virtual methods (ActivateObjective, StartPlayingObjective, StopObjective)");
    }

    if (auto GameMode = World->Get<UObject*>("AuthorityGameMode"))
    {
        static auto EnableGraphOffset = GameMode->GetOffset("bEnableReplicationGraph", false);
        LOG_INFO(LogDev, "Zone game mode bEnableReplicationGraph offset {}, replication mode {}", EnableGraphOffset, RebootConfig::Get().Stw.Replication);
        if (EnableGraphOffset != -1 && RebootConfig::Get().Stw.Replication == "graph") GameMode->Get<bool>(EnableGraphOffset) = true;
    }

    GetWorld()->Listen();
    Globals::bStartedListening = true;

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
                auto InitRecord = (void (*)(void*, UObject*))(__int64(GetModuleHandleW(0)) + 0x1205c90);
                InitRecord(Record, MgrClass);
                auto LoadMissionMgr = (void (*)(UObject*, void*))(ZoneGameState->VFTable[0x770 / 8]);
                LoadMissionMgr(ZoneGameState, Record);
                MissionMgr = ZoneGameState->Get<UObject*>("MissionManager");
            }
            LOG_INFO(LogDev, "Zone: GameState MissionManager is {}", MissionMgr ? MissionMgr->GetFullName() : "NULL");
        }

        static auto AIDirectorOffset = GameMode->GetOffset("AIDirector", false);
        if (AIDirectorOffset != -1 && !GameMode->Get<UObject*>(AIDirectorOffset))
        {
            static auto DirectorClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIDirector");
            auto Director = DirectorClass ? GetWorld()->SpawnActor<AActor>(DirectorClass, FVector(), FQuat()) : nullptr;
            if (Director)
            {
                GameMode->Get<UObject*>(AIDirectorOffset) = Director;
                static auto UseProtoOffset = Director->GetOffset("bUsePrototypeEnemies", false);
                if (UseProtoOffset != -1) SetBitfield(Director->GetPtr<PlaceholderBitfield>(UseProtoOffset), 1, true);
                if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
                {
                    auto ActivateDirector = (void (*)(UObject*))(__int64(GetModuleHandleW(0)) + 0xb39370);
                    ActivateDirector(Director);
                }
            }
            LOG_INFO(LogDev, "Zone: AI director {}", Director ? Director->GetFullName() : "could not be created");
        }

        static auto AIGoalManagerOffset = GameMode->GetOffset("AIGoalManager", false);
        if (AIGoalManagerOffset != -1 && !GameMode->Get<UObject*>(AIGoalManagerOffset))
        {
            static auto GoalMgrClass = FindObject<UClass>(L"/Script/FortniteGame.FortAIGoalManager");
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

        if (RebootConfig::Get().Stw.Map.find("Onboarding") != std::string::npos)
        {
            auto MissionInfo = LoadObject<UObject>(L"/Game/Missions/Primary/OB_FarmsteadFort/OB_FarmsteadFort.OB_FarmsteadFort", FindObject<UClass>(L"/Script/FortniteGame.FortMissionInfo"));
            static auto LoadMissionFn = FindObject<UFunction>(L"/Script/FortniteGame.FortMissionLibrary.LoadMission");
            static auto MissionLibrary = FindObject<UObject>(L"/Script/FortniteGame.Default__FortMissionLibrary");

            if (MissionInfo && LoadMissionFn && MissionLibrary)
            {
                alignas(16) unsigned char Params[64] = {};
                *(UObject**)(Params) = (UObject*)GetWorld();
                *(UObject**)(Params + 8) = MissionInfo;
                MissionLibrary->ProcessEvent(LoadMissionFn, Params);
                LOG_INFO(LogDev, "Zone: mission {} loaded", MissionInfo->GetFullName());
            }
            else LOG_ERROR(LogDev, "Zone: could not load the mission (info {}, function {}, library {})", (bool)MissionInfo, (bool)LoadMissionFn, (bool)MissionLibrary);
        }

        // Start playing all loaded missions now so OB_FarmsteadFort_C has MissionStatus == InProgress (1) before the player spawns.
        if (auto ZoneGameState = ((UObject*)GetWorld())->Get<UObject*>("GameState"))
        {
            if (auto MissionMgr = ZoneGameState->Get<UObject*>("MissionManager"))
            {
                static int MissionsOffset = MissionMgr->GetOffset("Missions", false);
                if (MissionsOffset != -1)
                {
                    auto& Missions = MissionMgr->Get<TArray<UObject*>>(MissionsOffset);
                    auto StartPlayingMission = (void (*)(UObject*, UObject*))(MissionMgr->VFTable[0x600 / 8]);
                    for (int i = 0; i < Missions.Num(); ++i)
                    {
                        if (auto Mission = Missions.at(i))
                        {
                            StartPlayingMission(MissionMgr, Mission);
                            LOG_INFO(LogDev, "Zone: started mission {} (status {})", Mission->GetFullName(), (int)*((uint8_t*)Mission + 0x488));
                        }
                    }
                }
            }
        }

        // Drain WorldSettings->WorldReadyActors so AFortAIDirector::WorldReady, AFortMission::OnWorldReady, and AFortObjectiveBase::OnWorldReady execute!
        if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
        {
            if (auto PersistentLevel = ((UObject*)GetWorld())->Get<UObject*>("PersistentLevel"))
            {
                if (auto WorldSettings = PersistentLevel->Get<UObject*>("WorldSettings"))
                {
                    auto NotifyWorldReady = (void (*)(UObject*))(__int64(GetModuleHandleW(0)) + 0x11282c0);
                    NotifyWorldReady(WorldSettings);
                    LOG_INFO(LogDev, "Zone: called AFortWorldSettings::NotifyWorldReady on {}", WorldSettings->GetFullName());
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
// Zone tracing (only when [SaveTheWorld] Map is set): logs calls of the world manager's load machine. Addresses are RVAs of 7.20 (names from the Xbox 7.30 symbols, see fnmod/re).
static __int64 (*Trace_HandleLoadingOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_RetrieveZoneCompleteOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_RetrieveTheaterCompleteOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_CreateWorldRecordOriginal)(__int64, __int64, __int64, __int64);
static __int64 (*Trace_InitWorldRecordCompleteOriginal)(__int64, __int64, __int64, __int64);

static __int64 Trace_HandleLoadingHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    static unsigned char LastState[4] = { 255, 255, 255, 255 };
    auto State = *(__int64*)(A + 0x7d8);
    unsigned char Now[4] = { 0, 0, 0, 0 };
    if (State) for (int i = 0; i < 4; ++i) Now[i] = *(unsigned char*)(State + i);

    if (State && memcmp(Now, LastState, 4) != 0)
    {
        memcpy(LastState, Now, 4);
        LOG_INFO(LogDev, "ZoneTrace HandleLoadingWorldRecordState: state bytes {} {} {} {} (count {})", Now[0], Now[1], Now[2], Now[3], *(int*)(A + 0x7e0));
    }

    return Trace_HandleLoadingOriginal(A, B, C, D);
}

static __int64 Trace_RetrieveZoneCompleteHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace OnRetrieveZoneInformationComplete: result bytes {} {}", *(unsigned char*)B, *(unsigned char*)(B + 0x40));
    return Trace_RetrieveZoneCompleteOriginal(A, B, C, D);
}

static __int64 Trace_RetrieveTheaterCompleteHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace OnRetrieveTheaterInformationComplete");
    return Trace_RetrieveTheaterCompleteOriginal(A, B, C, D);
}

static __int64 Trace_CreateWorldRecordHook(__int64 A, __int64 B, __int64 C, __int64 D)
{
    LOG_INFO(LogDev, "ZoneTrace CreateNewWorldRecord (info WorldId.Num {})", *(int*)(B + 8));
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

static void InstallZoneTrace()
{
    const auto Base = __int64(GetModuleHandleW(0));
    // exe addresses of the world record callbacks; 7.30.1 (CL 4834550) values were ported from 7.20 by byte matching (fnmod/re/port730c.py)
    const bool b730 = Fortnite_Version >= 7.3;
    Hooking::MinHook::Hook((PVOID)(Base + (b730 ? 0x10fe6c0 : 0x10d54e0)), Trace_RetrieveZoneCompleteHook, (PVOID*)&Trace_RetrieveZoneCompleteOriginal);
    Hooking::MinHook::Hook((PVOID)(Base + (b730 ? 0x10fe600 : 0x10d5420)), Trace_RetrieveTheaterCompleteHook, (PVOID*)&Trace_RetrieveTheaterCompleteOriginal);
    Hooking::MinHook::Hook((PVOID)(Base + (b730 ? 0x10f0750 : 0x10c7820)), Trace_CreateWorldRecordHook, (PVOID*)&Trace_CreateWorldRecordOriginal);
    Hooking::MinHook::Hook((PVOID)(Base + (b730 ? 0x1008700 : 0xfe4110)), Trace_InitWorldRecordCompleteHook, (PVOID*)&Trace_InitWorldRecordCompleteOriginal);
    if (b730)
    {
        Hooking::MinHook::Hook((PVOID)(Base + 0xbd95e0), (PVOID)GetPawnGoalSelectionCriteriaHook, (PVOID*)&GetPawnGoalSelectionCriteriaOriginal);
        LOG_INFO(LogDev, "Zone: GetPawnGoalSelectionCriteria hooked at 0xbd95e0.");
    }
    LOG_INFO(LogDev, "Zone trace hooks installed.");
}

static void Zone_ApplyHeroSkin(AFortPlayerPawn* Pawn, UObject* Hero)
{
    if (!Pawn || !Hero) return;

    static auto HeadClass = FindObject<UClass>(L"/Script/FortniteGame.CustomCharacterHeadData");
    static auto BodyClass = FindObject<UClass>(L"/Script/FortniteGame.CustomCharacterBodyData");
    static auto HatClass = FindObject<UClass>(L"/Script/FortniteGame.CustomCharacterHatData");
    static auto BackpackClass = FindObject<UClass>(L"/Script/FortniteGame.CustomCharacterBackpackData");

    static auto PartsOffset = Hero->GetOffset("CharacterParts", false);
    if (PartsOffset == -1)
    {
        // without this the player keeps the default character: say so, instead of returning silently
        LOG_WARN(LogDev, "Zone: hero {} has no CharacterParts, no STW skin applied", Hero->GetFullName());
        return;
    }

    auto& Parts = Hero->Get<TArray<UObject*>>(PartsOffset);
    TArray<UObject*> CosmeticParts;
    int Heads = 0, Bodies = 0, Hats = 0, Backpacks = 0, Unknown = 0;

    for (int i = 0; i < Parts.Num(); ++i)
    {
        auto Part = Parts.at(i);
        if (!Part) continue;

        CosmeticParts.Add(Part);

        static auto AdditionalDataOffset = Part->GetOffset("AdditionalData", false);
        auto AddData = AdditionalDataOffset != -1 ? Part->Get<UObject*>(AdditionalDataOffset) : nullptr;
        if (!AddData) { ++Unknown; continue; }

        if (HeadClass && AddData->IsA(HeadClass)) { Pawn->ServerChoosePart(EFortCustomPartType::Head, Part); ++Heads; }
        else if (BodyClass && AddData->IsA(BodyClass)) { Pawn->ServerChoosePart(EFortCustomPartType::Body, Part); ++Bodies; }
        else if (HatClass && AddData->IsA(HatClass)) { Pawn->ServerChoosePart(EFortCustomPartType::Hat, Part); ++Hats; }
        else if (BackpackClass && AddData->IsA(BackpackClass)) { Pawn->ServerChoosePart(EFortCustomPartType::Backpack, Part); ++Backpacks; }
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
    LOG_INFO(LogDev, "Zone: applied STW hero skin: head {} body {} hat {} backpack {} unknown {} (of {} parts)", Heads, Bodies, Hats, Backpacks, Unknown, Parts.Num());
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
    static std::vector<AActor*> SetUp;

    // drop players that have left: a freed actor's address can be handed to a new player, which must still be set up
    SetUp.erase(std::remove_if(SetUp.begin(), SetUp.end(), [](AActor* Actor) { return !Actor || !Actor->IsValidLowLevel(); }), SetUp.end());

    if (!NewPlayer || NewPlayer == GetLocalPlayerController() || std::find(SetUp.begin(), SetUp.end(), NewPlayer) != SetUp.end())
        return;

    SetUp.push_back(NewPlayer);
    LOG_INFO(LogDev, "Zone: setting up the player {}", NewPlayer->GetName());

    // Enable building for this player immediately (STW building is on from the start, not gated by mission)
    {
        // Per-player build free flag
        int bBuildFreeOffset = NewPlayer->GetOffset("bBuildFree", false);
        if (bBuildFreeOffset != -1)
        {
            void* Prop = NewPlayer->GetProperty("bBuildFree", false);
            uint8_t Mask = GetFieldMask(Prop);
            NewPlayer->SetBitfieldValue(bBuildFreeOffset, Mask, true);
            LOG_INFO(LogDev, "Zone: enabled bBuildFree on {}", NewPlayer->GetName());
        }

        // Also call the GameState's SetBuildingAllowed (idempotent)
        Zone_AllowBuilding();
    }

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

    // a pawn at the first player start of the level
    static auto PawnClass = LoadObject<UClass>(L"/Game/Abilities/Player/Pawns/PlayerPawn_Generic.PlayerPawn_Generic_C", FindObject<UClass>(L"/Script/CoreUObject.Class"));
    static auto StartClass = FindObject<UClass>(L"/Script/Engine.PlayerStart");

    FTransform Transform;
    Transform.Rotation = FQuat(0, 0, 0, 1);
    Transform.Scale3D = FVector(1, 1, 1);

    if (StartClass)
    {
        auto Starts = UGameplayStatics::GetAllActorsOfClass(GetWorld(), StartClass);
        if (Starts.Num() > 0)
        {
            Transform.Translation = Starts.at(0)->GetActorLocation();
            Transform.Translation.Z += 150;
        }
        Starts.Free();
    }

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

            // chunk 3: the onboarding mission wants husks around the farm (Build60+ test)
            // Off by default: spawning HuskPawn_C crashes the server (engine +0xbd9660). Enable with -reboot.SaveTheWorld.SpawnHusks=true or [SaveTheWorld] SpawnHusks=true to debug it.
            if (RebootConfig::Get().Stw.bSpawnHusks)
                Zone::SpawnInitialWave((AFortPlayerPawn*)Pawn);

            // The Zone loading screen stays up until the server marks the player as having finished loading (bHasServerFinishedLoading, replicated; the client then answers with its
            // own flag and the player state's). The native loading tick never gets there because there is no zone data, so do what it ends with. 7.30.1: AFortPlayerController::SetHasFinishedLoading.
            if (Fortnite_Version >= 7.3 && Fortnite_Version < 8.0)
            {
                auto SetHasFinishedLoading = (void (*)(UObject*, bool))(__int64(GetModuleHandleW(0)) + 0x1418ba0);
                SetHasFinishedLoading(NewPlayer, true);
                LOG_INFO(LogDev, "Zone: the player is marked as having finished loading");
            }

            if (!gZoneMatchStarted)
            {
                gZoneMatchStarted = true;
                static auto StartMatchFn = FindObject<UFunction>(L"/Script/Engine.GameMode.StartMatch");
                if (StartMatchFn) GameMode->ProcessEvent(StartMatchFn);
                LOG_INFO(LogDev, "Zone: match started after player setup");
            }

            gZoneRestartController = NewPlayer;
            gZoneRestartFrames = 0;
            gZoneRestartsSent = 0;
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
    for (int i = 0; i < Controllers.Num(); ++i) Zone_SetUpPlayer(GameMode, Controllers.at(i));
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
        // the 0x13efdf0 RVA is the 7.30.1 GetOrInitializeHero; held to 7.30 like the other hard-coded RVAs
        if (!gZoneHeroInitialized && Fortnite_Version >= 7.3 && Fortnite_Version < 7.31)
        {
            auto GetOrInitializeHero = (UObject* (*)(UObject*))(__int64(GetModuleHandleW(0)) + 0x13efdf0);
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

                    if (PickaxeDefinition)
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

    NumToSubtractFromSquadId = Engine_Version >= 424 ? 2 : Engine_Version >= 423 ? 3 : 0; // TODO: check this
    NumElementsPerChunk = std::floor(Fortnite_Version) >= 5 && Fortnite_Version <= 6 ? 0x10400 : 0x10000; // Idk what version tbh

    Offsets::FindAll(); // We have to do this before because FindCantBuild uses FortAIController.CreateBuildingActor
    Offsets::Print();

    Addresses::FindAll();
    Addresses::Init();
    Addresses::Print();

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

    if (!ProdMatch::Get().bEnabled) // a backend-run server has no display to draw on
    {
        CreateThread(0, 0, GuiThread, 0, 0, 0);
    }
    else
    {
        LOG_INFO(LogInit, "Prod: no GUI (match '{}')", ProdMatch::Get().MatchId);
    }

    while (SecondsUntilTravel > 0)
    {
        SecondsUntilTravel -= 1;

        Sleep(1000);
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

    auto ObjectNum = ChunkedObjects ? ChunkedObjects->Num() : UnchunkedObjects ? UnchunkedObjects->Num() : 0;

    std::ofstream obj("ObjectsDump.txt");

    for (int i = 0; i < ObjectNum; ++i)
    {
        auto CurrentObject = GetObjectByIndex(i);

        if (!CurrentObject)
            continue;

        obj << CurrentObject->GetFullName() << '\n';
    }

    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogNetPackageMap VeryVerbose", nullptr);
    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogNetTraffic VeryVerbose", nullptr);
    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogNet VeryVerbose", nullptr);
    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogNavigation VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortMission VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortAIGoalSelection VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortWorld VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortPlayerRegistration VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogBuilding VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortTeams VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortAI VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortAIDirector VeryVerbose", nullptr);
	UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortUIDirector Fatal", nullptr); // Temporary
    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortQuest VeryVerbose", nullptr);
    // UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogAbilitySystem VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogDataTable VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogMeshNetwork VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogEQS VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFort VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogGameMode VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogSpecialEvent VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogPlayerController VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogSpecialEventPhase VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogFortCustomization VeryVerbose", nullptr);
    UKismetSystemLibrary::ExecuteConsoleCommand(GetWorld(), L"log LogSpecialEventScriptMeshActor VeryVerbose", nullptr);

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

    ChangeLevels();

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

