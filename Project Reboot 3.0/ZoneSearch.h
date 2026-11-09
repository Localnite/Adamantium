#pragma once

// STW Zone searchables: the Athena loot picker does not fit the STW container's records, so Save the World
// containers hand out loot through the STW side of the search hook. One chunk = one deliverable feature.
class ABuildingContainer;
class AFortPlayerController;
class UFortItemDefinition;
class UObject;
struct FFrame;

namespace Zone
{
	// Drops loot for one searched STW container. Returns true when something was granted; the container keeps its
	// BounceContainer/bAlreadySearched client behaviour in the caller. The prop name decides the pool; deliberate
	// no-look props ("OB_MineCartNoLoot" and friends) grant nothing on purpose.
	bool SearchContainerSpawnLoot(ABuildingContainer* Container, AFortPlayerController* PlayerController);

	// Adds the item to the player's world inventory with the ShouldShowItemToast entry state (the client HUD
	// renders that as the STW pickup feed "x<amount> [Crafting Item] <Name>") and places weapons, traps and
	// schematics on the quickbar. Used by the search pools and the harvest chunk.
	// bShowToast = false is for silent mission grants (K2_GiveItemToAllPlayers with bNotifyPlayer false). Stacking onto an
	// existing stack is handled too (the toast flag is raised on the stack that grew).
	void GrantItemWithToast(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, int Count, bool bShowToast = true);

	// Every TickFlush frame of a Zone world: pickups spawned by a search wait a moment (so the player sees them pop out of
	// the container) and are then sent flying into the player like the real STW loot animation; the item is added to the
	// inventory when that animation completes (CompletePickupAnimationHook)
	void SearchTick();

	// The player's backpack (the backend's theater0: weapons, traps, ammo, resources, ingredients) is read when they join and given to them;
	// SearchTick saves it back every minute and BeginEndOfZone at the end. Call once the player's world inventory exists. The Zone server
	// of this build never reads theater0 itself, so without this nothing carried over from one mission to the next.
	void LoadBackpack(AFortPlayerController* PlayerController);

	// pending search pickups hold raw pointers; a new world frees them
	void ResetSearchState();

	// FortDecoTool.ServerSpawnDeco (the client's trap placement). Runs the game's own code, logs what happened, and takes the trap
	// out of the inventory if the game placed one but did not use the item up. Installed on the tool's UFunction.
	void ServerSpawnDecoHook(UObject* Tool, FFrame& Stack, void* Ret);
	extern void (*ServerSpawnDecoOriginal)(UObject* Tool, FFrame& Stack, void* Ret);

	// FortMissionLibrary.SpawnAndCollectPlayerPickups (the storm shield supply drop's loot), filled from the Xbox build's STW loot tables
	void SpawnAndCollectPlayerPickupsHook(UObject* Context, FFrame& Stack, void* Ret);
	extern void (*SpawnAndCollectPlayerPickupsOriginal)(UObject* Context, FFrame& Stack, void* Ret);

	// how many of this item the player holds (all stacks), 0 for no inventory
	int CountItemOnPlayer(AFortPlayerController* PlayerController, UFortItemDefinition* Definition);

	// removes up to Amount of this item across stacks, returns how many were removed
	int RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortItemDefinition* Definition, int Amount);

	// The tutorial mission has succeeded: a few seconds later the Zone sends the players back to the main menu, where the frontend plays the
	// satellite movie and the homebase naming. The game's own zone end is server code that is compiled out of this client exe, and without it the
	// client stays on a black screen. Driven from SearchTick.
	void BeginEndOfZone();

	// shared xorshift roll for all Zone chunks (loot picks, harvest yields, husk stats ...)
	int RandRange(int Min, int Max);
}
