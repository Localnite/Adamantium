#pragma once

#include <cstdint>

// Save the World (Zone) addresses, found the way the rest of Reboot finds its addresses (finder.h): Memcury signatures over the
// loaded exe instead of fixed RVAs. Each one is resolved the first time it is asked for, logged once, and is 0 when its signature
// did not match (callers check and skip what needs it). The signatures were made from the 7.30.1 exe (CL 4834550) with
// ~/Documents/FortniteLinux/stw-tools/re/sig730.py, which grows each pattern until it matches exactly once; rip-relative and
// call displacements are wildcarded like Reboot's own patterns. Offsets the game's own code uses are read back out of that code.
//
// What is NOT found here and why: the vtable slots of server functions the client exe was compiled without (the objective's
// Activate/StartPlaying/StopObjective, the mission's OnObjectiveStatusChanged, ...). Their entries point at shared one-instruction
// stubs (ret / xor eax,eax; ret) that nothing in the exe calls, so there is no code to read the slot from; like the vtable offsets
// upstream Reboot keeps per version (NetDriver.cpp), they are the named constants at the bottom, held to 7.30.
namespace ZoneFinder
{
	bool Is730(); // the build these signatures and constants were made from (Engine_Version 422-424; Fortnite_Version reads as the CL here)

	// ---- matchmaking / lobby beacons (ZoneMatchmaking.cpp)
	uint64_t DoesSessionMatch();             // AOnlineBeaconHost session check (party/qos beacons)
	uint64_t AddEmptyServerReservation();    // APartyBeaconHost::AddEmptyServerReservationRequest
	uint64_t AllowedToProceed();             // ALobbyBeaconState::AllowedToProceed (... OnAllowedToProceed)
	uint64_t ReceivedNetworkEncryptionToken();
	uint64_t LobbyDoesSessionMatch();        // ALobbyBeaconHost::DoesSessionMatch
	uint64_t LobbyHandlePlayerLogin();       // ALobbyBeaconHost::HandlePlayerLogin
	uint64_t StartLobby();                   // ALobbyBeaconState::StartLobby

	// ---- zone world, missions, player (dllmain.cpp)
	uint64_t InitMissionManagerRecord();     // FFortMissionManagerRecord init (record handed to the game state's LoadMissionManager)
	uint64_t ActivateAIDirector();           // AFortAIDirector activation
	uint64_t NotifyWorldReady();             // AFortWorldSettings::NotifyWorldReady
	uint64_t OnRetrieveZoneInformationComplete();
	uint64_t OnRetrieveTheaterInformationComplete();
	uint64_t CreateNewWorldRecord();
	uint64_t OnInitWorldRecordComplete();    // AFortGameModeZone
	uint64_t GetPawnGoalSelectionCriteria();
	uint64_t AddItemToQuickBars();           // AFortPlayerController
	uint64_t SetHasFinishedLoading();        // AFortPlayerController
	uint64_t GetOrInitializeHero();          // AFortPlayerController

	// ---- AI crash patches (dllmain.cpp): the instruction or function each patch rewrites
	uint64_t GoalCriteriaCompareSite();      // GetPawnGoalSelectionCriteria: cmp rbx, rdi (made test rbx, rbx)
	uint64_t AIPawnBeginPlayAssignSite();    // AFortAIPawn::BeginPlay: je over the assignment registration (made jmp)
	uint64_t AssignmentRegistration();       // stubbed to ret
	uint64_t MakeGoalSelectionQuery();       // stubbed to xor eax, eax; ret

	// ---- AI manager lookups (dllmain.cpp Zone_PatchAIDirectorCallers)
	uint64_t AIManagerLookupStub();          // the folded "return nullptr" the AI director/goal manager getters became in the client exe
	// call sites (the call instruction itself) whose callee works on the goal manager, or that pass a world-context object straight through
	int GoalManagerSites(const uint64_t*& Out);
	int ContextGoalManagerSites(const uint64_t*& Out);
	int ContextDirectorSites(const uint64_t*& Out);

	// ---- replication graph (dllmain.cpp Zone_RouteQuickBars, ZoneSearch.cpp)
	uint64_t ClassRepNodePolicyLookup();     // UFortReplicationGraph class policy map lookup
	int ClassRepNodePoliciesOffset();        // the map's offset in the graph, read from the caller's lea; -1 if not found

	// ---- AI director curve warning (ZoneHusks.cpp)
	uint8_t* CurveTableWarningVerbosity();   // the log category verbosity byte FCurveTableRowHandle::Eval checks before warning

	// ---- navigation (ZoneHusks.cpp)
	uint64_t PrepareBoundsAndUnlock();       // UFortNavSystem::PrepareBoundsAndUnlock
	uint64_t AthenaPrepareBoundsAndUnlock(); // the Athena nav system's override (jumps to it)
	int NavSystemBoundsOffset();             // the nav system's own FNavigationBounds copy, read from PrepareBoundsAndUnlock; -1 if not found

	// ---- net driver fallbacks (NetDriver.cpp), when Reboot's own finders miss on this build
	uint64_t CreateChannelByName();
	uint64_t SetChannelActor();
	uint64_t ReplicateActor();

	// ---- vtable slots and member offsets with no code to read them from (see the top); byte offsets into the vtable / object
	namespace Fixed730
	{
		constexpr int ObjectiveActivateSlot = 0x610;      // AFortObjectiveBase::ActivateObjective
		constexpr int ObjectiveStartPlayingSlot = 0x618;  // AFortObjectiveBase::StartPlayingObjective
		constexpr int ObjectiveStopSlot = 0x660;          // AFortObjectiveBase::StopObjective
		constexpr int MissionObjectiveStatusChangedSlot = 0x6b8; // AFortMission::OnObjectiveStatusChanged
		constexpr int MissionManagerStartPlayingSlot = 0x600;    // UFortMissionManager::StartPlayingMission
		constexpr int GameStateLoadMissionManagerSlot = 0x770;   // AFortGameStateZone::LoadMissionManager
		constexpr int BeaconHostObjectInitSlot = 0x620;   // APartyBeaconHost::InitHostBeacon / ALobbyBeaconHost::Init
		constexpr int BeaconRegisterHostSlot = 0x628;     // AOnlineBeaconHost::RegisterHost
		constexpr int LobbySetupLobbyStateSlot = 0x628;   // ALobbyBeaconHost::SetupLobbyState
		constexpr int PropertyExportTextItemSlot = 0x298; // UProperty::ExportTextItem
		constexpr int PropertyImportTextSlot = 0x2a0;     // UProperty::ImportText_Internal
		constexpr int NavReleaseInitialBuildingLockSlot = 0x2c0; // UNavigationSystemV1::ReleaseInitialBuildingLock

		constexpr int MissionInfoPointer = 0x378;         // AFortMission's FortMissionInfo (not reflected)
		constexpr int DirectorEncounterClass = 0xca0;     // AFortAIDirector's encounter class (not reflected)
		constexpr int NavBuildingLockFlags = 0x398;       // UNavigationSystemV1 NavBuildingLockFlags (not reflected)
		constexpr int PrimitiveNavRelevancyFlags = 0xae;  // UPrimitiveComponent nav relevancy cache bits (not reflected)
		constexpr int FieldArrayDim = 0x30, FieldElementSize = 0x34; // UProperty ArrayDim / ElementSize
	}
}
