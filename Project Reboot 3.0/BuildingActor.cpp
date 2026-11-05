#include "BuildingActor.h"

#include "FortWeapon.h"
#include "BuildingSMActor.h"
#include "FortPlayerControllerAthena.h"
#include "FortPawn.h"
#include "FortWeaponMeleeItemDefinition.h"
#include "CurveTable.h"
#include "DataTable.h"
#include "FortResourceItemDefinition.h"
#include "FortKismetLibrary.h"
#include "DataTableFunctionLibrary.h"
#include "globals.h"
#include "FortPlayerController.h"
#include "ZoneSearch.h"
#include <unordered_map>
#include <algorithm>

void ABuildingActor::OnDamageServerHook(ABuildingActor* BuildingActor, float Damage, FGameplayTagContainer DamageTags,
	FVector Momentum, /* FHitResult */ __int64 HitInfo, APlayerController* InstigatedBy, AActor* DamageCauser,
	/* FGameplayEffectContextHandle */ __int64 EffectContext)
{
	// LOG_INFO(LogDev, "Befor3e");

	auto BuildingSMActor = Cast<ABuildingSMActor>(BuildingActor);
	auto PlayerController = Cast<AFortPlayerControllerAthena>(InstigatedBy);
	// zone: the STW player controller is not Athena, so the generic cast feeds the Zone harvest chunk below
	auto ZoneController = Globals::bZoneMode ? Cast<AFortPlayerController>(InstigatedBy) : nullptr;
	// auto Pawn = PlayerController ? PlayerController->GetMyFortPawn() : nullptr;
	auto Weapon = Cast<AFortWeapon>(DamageCauser);

	// STW diagnostic (once per session): a pickaxe hit that lands on something that is not a building prop gets no
	// harvest yield at all. If the mission cars are vehicles rather than building props, this is where they show up.
	static bool bLoggedNonBuildingHit = false;
	if (Globals::bZoneMode && !BuildingSMActor && !bLoggedNonBuildingHit && Cast<AFortWeapon>(DamageCauser))
	{
		bLoggedNonBuildingHit = true;
		LOG_INFO(LogDev, "Zone: weapon hit reached a non-building actor, no harvest yield: {}", BuildingActor->GetFullName());
	}

	if (!BuildingSMActor)
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	if (BuildingSMActor->IsDestroyed())
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	/*

	static auto LastDamageAmountOffset = BuildingSMActor->GetOffset("LastDamageAmount");
	static auto LastDamageHitOffset = BuildingSMActor->GetOffset("LastDamageHit", false) != -1 ? BuildingSMActor->GetOffset("LastDamageHit") : BuildingSMActor->GetOffset("LastDamageHitImpulseDir"); // idc

	const float PreviousLastDamageAmount = BuildingSMActor->Get<float>(LastDamageAmountOffset);
	const float PreviousLastDamageHit = BuildingSMActor->Get<float>(LastDamageHitOffset);
	const float CurrentBuildingHealth = BuildingActor->GetHealth();

	BuildingSMActor->Get<float>(LastDamageAmountOffset) = Damage;
	BuildingSMActor->Get<float>(LastDamageHitOffset) = CurrentBuildingHealth;

	*/

	if ((!PlayerController && !ZoneController) || !Weapon)
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	// if (!Pawn)
		// return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	auto WorldInventory = PlayerController ? PlayerController->GetWorldInventory() : ZoneController->GetWorldInventory();

	if (!WorldInventory)
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	auto WeaponData = Cast<UFortWeaponMeleeItemDefinition>(Weapon->GetWeaponData());

	if (!WeaponData)
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	// Zone (chunk 2: harvest): STW auto-adds the resource you mined to the inventory on every pickaxe hit. The
	// Athena grant below needs BuildingResourceAmountOverride + the AthenaResourceRates curve table, none of which
	// STW world props carry, so the Zone chunk hand-rolls the per-hit yields here.
	if (ZoneController)
	{
		// one yield budget per hit prop: STW props are finite, so cap what one prop can give before it breaks
		static std::unordered_map<void*, int> GrantedPerBuilding;
		auto& Granted = GrantedPerBuilding[BuildingSMActor];
		constexpr int MaxGrantPerProp = 60;

		if (Granted >= MaxGrantPerProp || !WorldInventory)
			return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

		// what one pickaxe hit gives, decided by what the prop is. The mission's harvest props are the
		// UniqueHarvestingActors in the paks (Car_*, OB_Tree_Special_*). Cars are tagged Metal in the level, so the level's
		// resource type is not trusted for them: that is why destroying cars never produced the nuts and bolts the
		// harvesting objectives count. Everything else keeps the resource-type path.
		struct FZoneYield { const wchar_t* Path; int Min; int Max; int ChancePct; };
		FZoneYield Yields[2] = {};
		int YieldCount = 0;

		auto PropName = BuildingSMActor->GetName();
		bool bCarProp = PropName.contains("Car_") || PropName.contains("Truck") || PropName.contains("Tractor") || PropName.contains("Forklift") || PropName.contains("Pickup") || PropName.contains("KCar");
		bool bTreeProp = PropName.contains("Tree") || PropName.contains("tree");

		if (bCarProp)
		{
			Yields[YieldCount++] = { L"/Game/Items/Ingredients/Ingredient_Nuts_Bolts.Ingredient_Nuts_Bolts", 3, 5, 100 };
			Yields[YieldCount++] = { L"/Game/Items/Ingredients/Ingredient_Mechanical_Parts_T01.Ingredient_Mechanical_Parts_T01", 1, 2, 40 };
		}
		else if (bTreeProp)
		{
			// wood as before; twine is the guessed tree source for the twine objective (not verified in the paks)
			Yields[YieldCount++] = { L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData", 4, 7, 100 };
			Yields[YieldCount++] = { L"/Game/Items/Ingredients/Ingredient_Twine_T01.Ingredient_Twine_T01", 1, 2, 50 };
		}
		else
		{
			auto Resource = BuildingSMActor->GetResourceType();

			if (Resource == EFortResourceType::None)
			{
				// zone props often carry no resource type; the prop name names its material
				if (PropName.contains("Tree") || PropName.contains("tree") || PropName.contains("Wood") || PropName.contains("wood"))
					Resource = EFortResourceType::Wood;
				else if (PropName.contains("Rock") || PropName.contains("rock") || PropName.contains("Stone") || PropName.contains("stone"))
					Resource = EFortResourceType::Stone;
				else if (PropName.contains("Permanite") || PropName.contains("Quartz") || PropName.contains("quartz"))
					Resource = EFortResourceType::Permanite;
				else if (PropName.contains("Metal") || PropName.contains("metal"))
					Resource = EFortResourceType::Metal;
				else
					Resource = EFortResourceType::Wood; // the onboarding world is mostly wooden farmstead props
			}

			bool bPermanite = Resource == EFortResourceType::Permanite;
			Yields[YieldCount++] = {
				Resource == EFortResourceType::Wood ? L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData"
				: Resource == EFortResourceType::Stone ? L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData"
				: Resource == EFortResourceType::Metal ? L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData"
				: L"/Game/Items/ResourcePickups/PermaniteItemData.PermaniteItemData",
				bPermanite ? 2 : 4, bPermanite ? 5 : 7, 100 };
		}

		// final blow on the prop: the yields are multiplied, so a broken prop pays out in bulk
		bool bFinalBlow = BuildingActor->GetHealth() - Damage <= 0;
		static auto ItemDefinitionClass = FindObject<UClass>(L"/Script/FortniteGame.FortItemDefinition");
		bool bGrantedAny = false;

		for (int y = 0; y < YieldCount; ++y)
		{
			const FZoneYield& Y = Yields[y];

			if (Zone::RandRange(1, 100) > Y.ChancePct)
				continue;

			int Count = Zone::RandRange(Y.Min, Y.Max) * (bFinalBlow ? 3 : 1);
			int Remaining = MaxGrantPerProp - Granted;

			if (Count > Remaining)
				Count = Remaining;

			if (Count <= 0)
				break;

			Granted += Count;

			auto ItemDef = LoadObject<UFortItemDefinition>(Y.Path, ItemDefinitionClass);

			if (!ItemDef)
			{
				LOG_WARN(LogDev, "Zone: harvest item definition did not load: {}", std::to_string(y));
				continue;
			}

			bool bShouldUpdate = false;
			WorldInventory->AddItem(ItemDef, &bShouldUpdate, Count);

			if (bShouldUpdate || bFinalBlow)
				WorldInventory->Update();

			bGrantedAny = true;
		}

		if (bGrantedAny && Granted <= MaxGrantPerProp && !bFinalBlow && Granted > 0)
			LOG_INFO(LogDev, "Zone: harvest grants on {} (budget {}/{})", BuildingSMActor->GetFullName(), Granted, MaxGrantPerProp);

		// the prop breaks on its final blow: drop its budget entry, or the address could be reused by a new prop
		// that inherits a spent budget (and the map would grow for the whole match)
		if (bFinalBlow)
			GrantedPerBuilding.erase(BuildingSMActor);

		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
	}


	UFortResourceItemDefinition* ItemDef = UFortKismetLibrary::K2_GetResourceItemDefinition(BuildingSMActor->GetResourceType());

	if (!ItemDef)
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	static auto BuildingResourceAmountOverrideOffset = BuildingSMActor->GetOffset("BuildingResourceAmountOverride");
	auto& BuildingResourceAmountOverride = BuildingSMActor->Get<FCurveTableRowHandle>(BuildingResourceAmountOverrideOffset);

	int ResourceCount = 0;

	if (BuildingResourceAmountOverride.RowName.IsValid())
	{
		// auto AssetManager = Cast<UFortAssetManager>(GEngine->AssetManager);
		// auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameStateAthena);
		UCurveTable* CurveTable = nullptr; // GameState->CurrentPlaylistInfo.BasePlaylist ? GameState->CurrentPlaylistInfo.BasePlaylist->ResourceRates.Get() : nullptr;

		// LOG_INFO(LogDev, "Before1");

		if (!CurveTable)
			CurveTable = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaResourceRates.AthenaResourceRates");

		{
			// auto curveMap = ((UDataTable*)CurveTable)->GetRowMap();

			// LOG_INFO(LogDev, "Before {}", __int64(CurveTable));

			float Out = UDataTableFunctionLibrary::EvaluateCurveTableRow(CurveTable, BuildingResourceAmountOverride.RowName, 0.f);

			// LOG_INFO(LogDev, "Out: {}", Out);

			const float DamageThatWillAffect = /* PreviousLastDamageHit > 0 && Damage > PreviousLastDamageHit ? PreviousLastDamageHit : */ Damage;

			float skid = Out / (BuildingActor->GetMaxHealth() / DamageThatWillAffect);

			ResourceCount = round(skid);
		}
	}

	if (ResourceCount <= 0)
	{
		return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
	}

	bool bIsWeakspot = Damage == 100.0f;
	PlayerController->ClientReportDamagedResourceBuilding(BuildingSMActor, BuildingSMActor->GetResourceType(), ResourceCount, false, bIsWeakspot);

	bool bShouldUpdate = false;
	WorldInventory->AddItem(ItemDef, &bShouldUpdate, ResourceCount);

	if (bShouldUpdate)
		WorldInventory->Update();

	return OnDamageServerOriginal(BuildingActor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
}

UClass* ABuildingActor::StaticClass()
{
	static auto Class = FindObject<UClass>(L"/Script/FortniteGame.BuildingActor");
	return Class;
}
