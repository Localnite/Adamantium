#pragma once

// STW Zone searchables: the Athena loot picker does not fit the STW container's records, so Save the World
// containers hand out loot through the STW side of the search hook. One chunk = one deliverable feature.
class ABuildingContainer;
class AFortPlayerController;

namespace Zone
{
	// Drops loot for one searched STW container. Returns true when something was granted; the container keeps its
	// BounceContainer/bAlreadySearched client behaviour in the caller. The prop name decides the pool; deliberate
	// no-look props ("OB_MineCartNoLoot" and friends) grant nothing on purpose.
	bool SearchContainerSpawnLoot(ABuildingContainer* Container, AFortPlayerController* PlayerController);

	// shared xorshift roll for all Zone chunks (loot picks, harvest yields, husk stats ...)
	int RandRange(int Min, int Max);
}
