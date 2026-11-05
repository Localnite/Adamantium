#include "ZoneSearch.h"
#include "reboot.h"
#include "BuildingContainer.h"
#include "BuildingSMActor.h"
#include "Actor.h"
#include "FortPlayerController.h"
#include "FortInventory.h"
#include "FortItemDefinition.h"
#include "FortItem.h"
#include "FortPickup.h"
#include "log.h"

// STW Zone searchables (chunk 1: loot for a search, spawning as world pickups like real STW). The Athena loot
// picker does not fit STW containers (their tier groups live in the zone-runtime tables that 7.30 dedicated never
// loads), so the pools are hand-picked item definitions verified present in the installed STW paks. Loot is spawned
// as tossed AFortPickups at the container so the player walks over them like in STW (direct inventory grants felt
// wrong in testing - 2026-11-03).

namespace
{
	struct FPoolItem { const wchar_t* Path; int Min; int Max; };

	// LoadObject is cheap after the first load of a package; every definition here is verified present in the 7.30 paks
	UFortItemDefinition* ZoneFindItem(const wchar_t* Path)
	{
		static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
		return LoadObject<UFortItemDefinition>(Path, ItemDefinitionClass);
	}

	// one pickup per item, tossed out of the container like the Athena chest path in BuildingContainer.cpp does
	// the log names the definition that failed: a missing item used to mean silently missing loot
	std::string NarrowPath(const wchar_t* Path)
	{
		std::wstring Wide = Path;
		return std::string(Wide.begin(), Wide.end());
	}

	void ZoneSpawnPickup(const wchar_t* Path, int Count, const FVector& Location)
	{
		auto Definition = ZoneFindItem(Path);

		if (!Definition)
		{
			LOG_WARN(LogDev, "Zone: search loot definition did not load, nothing dropped: {}", NarrowPath(Path));
			return;
		}

		auto ItemEntry = FFortItemEntry::MakeItemEntry(Definition, Count, 0, MAX_DURABILITY, 0);

		if (!ItemEntry)
		{
			LOG_WARN(LogDev, "Zone: could not make an item entry for {} x{}", NarrowPath(Path), Count);
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

		if (NewPickup)
		{
			LOG_INFO(LogDev, "Zone: spawned search pickup {} x{}", NarrowPath(Path), Count);
		}
	}

	struct FZoneWeapon { const wchar_t* Weapon; const wchar_t* Ammo; int AmmoCount; };

	// one verified gray-tier weapon per family, with the ammo the family uses in STW
	const FZoneWeapon ZoneWeapons[] = {
		{ L"/Game/Items/Weapons/Ranged/Assault/Auto/WID_Assault_Auto_C_Ore_T01.WID_Assault_Auto_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 20 },
		{ L"/Game/Items/Weapons/Ranged/Shotgun/Auto/WID_Shotgun_Auto_UC_Ore_T01.WID_Shotgun_Auto_UC_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 10 },
		{ L"/Game/Items/Weapons/Ranged/Shotgun/Break/WID_Shotgun_Break_C_Ore_T01.WID_Shotgun_Break_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium", 10 },
		{ L"/Game/Items/Weapons/Ranged/Sniper/BoltAction/WID_Sniper_BoltAction_C_Ore_T01.WID_Sniper_BoltAction_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy", 6 },
		{ L"/Game/Items/Weapons/Ranged/Pistol/Auto/WID_Pistol_Auto_C_Ore_T01.WID_Pistol_Auto_C_Ore_T01", L"/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight", 24 },
	};
	const int ZoneWeaponCount = sizeof(ZoneWeapons) / sizeof(ZoneWeapons[0]);

	const FPoolItem ZoneResourcePool[] = {
		{ L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData", 30, 90 },
		{ L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData", 30, 90 },
		{ L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData", 25, 75 },
	};
	const int ZoneResourceCount = sizeof(ZoneResourcePool) / sizeof(ZoneResourcePool[0]);

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

	// the weapon goes into one pickup, its ammo into another, both at the container
	bool ZoneSpawnWeaponPickups(FVector Location)
	{
		auto Pick = &ZoneWeapons[ZoneRandRange(0, ZoneWeaponCount - 1)];
		int Offset = ZoneRandRange(-40, 40);

		Location.X += (float)Offset;
		ZoneSpawnPickup(Pick->Weapon, 1, Location);

		Location.X -= 2.f * (float)Offset;
		Location.Z += 10;
		ZoneSpawnPickup(Pick->Ammo, Pick->AmmoCount, Location);

		return true;
	}
}

namespace Zone
{
	// public so the other Zone chunks (harvest, husks) share the pick rolls
	int RandRange(int Min, int Max)
	{
		return ZoneRandRange(Min, Max);
	}

	bool SearchContainerSpawnLoot(ABuildingContainer* Container, AFortPlayerController* PlayerController)
{
	if (!Container || !PlayerController)
		return false;

	// Mine carts marked "NoLoot" are genuine parts of the onboarding layout: STW leaves them empty on purpose, so do we
	auto ContainerName = Container->GetName();
	auto Location = Container->GetActorLocation() + FVector{ 0, 0, 50 };

	if (ContainerName.contains("NoLoot"))
	{
		LOG_INFO(LogDev, "Zone: searched prop is a deliberate no-loot prop");
		return false;
	}

	// Tutorial chest (BP_SpecialChest_Pit_Child_C) should NOT give weapons - only resources/ammo
	bool bIsTutorialChest = ContainerName.contains("BP_SpecialChest_Pit_Child_C");
	// Only actual chests (with "Chest" in name) give weapons - not bushes, plants, mine carts, etc.
	bool bIsActualChest = ContainerName.contains("Chest");

	// every searchable prop: resources as pickups
	auto Resource = ZoneResourcePool[ZoneRandRange(0, ZoneResourceCount - 1)];
	ZoneSpawnPickup(Resource.Path, ZoneRandRange(Resource.Min, Resource.Max), Location);

	// Weapons only for actual chests (not tutorial chest, not bushes/plants/mine carts)
	if (!bIsTutorialChest && bIsActualChest)
		ZoneSpawnWeaponPickups(Location);

	return true;
}
}
