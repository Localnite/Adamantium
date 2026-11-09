#pragma once

// Chunk 3: real STW husks. The pawn is the game's own HuskPawn blueprint; movement is the server-side characte
// movement write that the BR bots already prove out (BotBrain Apply()), minus the navmesh - the onboarding farm
// is open ground, so a straight shamble at the nearest player covers it.

class AFortPawn;
class UObject;
struct FFrame;

namespace Zone
{
	// every TickFlush frame of a Zone world (NetDriver.cpp): walk spawned husks toward the nearest player
	void HusksTick();

	// Once per Zone world, from the tick: gives the navigation system real bounds and releases its initial build lock (see ZoneHusks.cpp). The AI director's
	// rift manager does not start looking for spawn rifts until then, so the fort's encounters sit in InitializingRiftManager forever.
	void NavigationTick();

	// AFortAIController::Possess is a `ret` stub on this exe, so enemy AI pawns are never driven by their behaviour tree. Once per world: when the slot is a stub,
	// replace it with AAIController::Possess followed by RunBehaviorTree on the pawn's tree. The husks then get their goal from HusksTick (SetGoalActor) and move and
	// attack with the game's own abilities and animations instead of our velocity writes.
	void InstallAIControllerPossess();

	// The Zone's AI director normally comes from the Blueprint /Game/AIDirector/AIDirector_Fortnite, which the PC client paks do not contain (it is in the Xbox
	// build's). The blueprint has no logic, only default property values. With the Blueprint present (a one-time setup with the Xbox pak installed) the values are
	// exported through the engine's own property text export into Win64/zone_director_defaults.txt; without it the native director gets them from that file
	// through the property text import, before anything reads them.
	void SyncDirectorDefaults(UObject* Director);

	// OBJ_HuskProblem started playing: the mission script starts its AI encounters and the game's AI director spawns the husks.
	// From here the tick adopts those pawns, logs what the director is doing, and only if no husk appears within
	// [SaveTheWorld] HuskFallbackSeconds spawns the old hand-placed ones (switch off with HuskFallback=false).
	void OnHuskObjectiveStarted(UObject* Objective);

	// the FortMissionInfo that LoadMission was given (remembered for PrepareMissionEncounters)
	void SetMissionInfo(UObject* Info);

	// before the mission script starts its encounters: gives the mission its info and an entry per generated encounter profile
	void PrepareMissionEncounters(UObject* Mission);

	// FortMission.StartMissionAIEncounterFromGeneratedProfile traced: logs each call (mission role, the director's state) and what
	// it returned, then runs the game's own code unchanged
	void StartEncounterTraceHook(UObject* Mission, struct FFrame& Stack, void* Ret);
	extern void (*StartEncounterTraceOriginal)(UObject* Mission, struct FFrame& Stack, void* Ret);

	int HuskCount();

	// forget every husk, corpse and tutorial flag from the previous world; called when a new Zone world starts
	void ResetForNewWorld();
}
