#pragma once

// Chunk 3: real STW husks. The pawn is the game's own HuskPawn blueprint; movement is the server-side characte
// movement write that the BR bots already prove out (BotBrain Apply()), minus the navmesh - the onboarding farm
// is open ground, so a straight shamble at the nearest player covers it.

class AFortPawn;
class UObject;

namespace Zone
{
	// every TickFlush frame of a Zone world (NetDriver.cpp): walk spawned husks toward the nearest player
	void HusksTick();

	// first spawn wave for a joining player, then the tick trickles new husks up to the cap
	void SpawnInitialWave(AFortPawn* PlayerPawn);

	// spawn tutorial husks when OBJ_HuskProblem starts playing
	void SpawnTutorialHusks(UObject* Objective);

	int HuskCount();

	// forget every husk, corpse and tutorial flag from the previous world; called when a new Zone world starts
	void ResetForNewWorld();
}
