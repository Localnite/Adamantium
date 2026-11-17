#include "FortPawn.h"
#include "questStats.h"

#include "reboot.h"
#include "FortPlayerControllerAthena.h"

AFortWeapon* AFortPawn::EquipWeaponDefinition(UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid)
{
	static auto EquipWeaponDefinitionFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.EquipWeaponDefinition");

	FGuid TrackerGuid{};

	if (Fortnite_Version < 15.50)
	{
		struct { UObject* Def; FGuid Guid; AFortWeapon* Wep; } params{ WeaponData, ItemEntryGuid };
		this->ProcessEvent(EquipWeaponDefinitionFn, &params);
		return params.Wep;
	}
	else if (Fortnite_Version >= 15.50 && Fortnite_Version < 17)
	{
		struct { UObject* Def; FGuid Guid; FGuid TrackerGuid; AFortWeapon* Wep; } S16_params{ WeaponData, ItemEntryGuid, TrackerGuid };
		this->ProcessEvent(EquipWeaponDefinitionFn, &S16_params);
		return S16_params.Wep;
	}
	else
	{
		struct { UObject* Def; FGuid Guid; FGuid TrackerGuid; bool bDisableEquipAnimation; AFortWeapon* Wep; } S17_params{ WeaponData, ItemEntryGuid, TrackerGuid, false };
		this->ProcessEvent(EquipWeaponDefinitionFn, &S17_params);
		return S17_params.Wep;
	}

	return nullptr;
}

bool AFortPawn::PickUpActor(AActor* PickupTarget, UFortDecoItemDefinition* PlacementDecoItemDefinition)
{
	static auto fn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.PickUpActor");
	struct { AActor* PickupTarget; UFortDecoItemDefinition* PlacementDecoItemDefinition; bool ReturnValue; } AFortPawn_PickUpActor_Params{ PickupTarget, PlacementDecoItemDefinition };
	this->ProcessEvent(fn, &AFortPawn_PickUpActor_Params);

	return AFortPawn_PickUpActor_Params.ReturnValue;
}

void AFortPawn::OnRep_IsDBNO()
{
	static auto OnRep_IsDBNOFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.OnRep_IsDBNO");
	this->ProcessEvent(OnRep_IsDBNOFn);
}

float AFortPawn::GetShield()
{
	float Shield = 0;
	static auto GetShieldFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.GetShield");

	if (GetShieldFn)
		this->ProcessEvent(GetShieldFn, &Shield);
	
	return Shield;
}

float AFortPawn::GetHealth()
{
	float Health = 0;
	static auto GetHealthFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.GetHealth");

	if (GetHealthFn)
		this->ProcessEvent(GetHealthFn, &Health);

	return Health;
}

void AFortPawn::SetHealth(float NewHealth)
{
	static auto SetHealthFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.SetHealth");

	if (SetHealthFn)
		this->ProcessEvent(SetHealthFn, &NewHealth);
}

void AFortPawn::SetMaxHealth(float NewHealthVal)
{
	static auto SetMaxHealthFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPawn.SetMaxHealth");

	if (!SetMaxHealthFn)
		return;

	this->ProcessEvent(SetMaxHealthFn, &NewHealthVal);
}

void AFortPawn::SetShield(float NewShield)
{
	static auto SetShieldFn = FindObject<UFunction>("/Script/FortniteGame.FortPawn.SetShield");

	if (SetShieldFn)
		this->ProcessEvent(SetShieldFn, &NewShield);
}

void AFortPawn::SetMaxShield(float NewShieldVal)
{
	static auto SetMaxShieldFn = FindObject<UFunction>("/Script/FortniteGame.FortPawn.SetMaxShield");

	if (SetMaxShieldFn)
		this->ProcessEvent(SetMaxShieldFn, &NewShieldVal);
}

// Challenge progress: damage a real player deals to another player. The server multicasts the hit from the shooter's pawn (the same
// cue the bots send, see BotBrain::SendShotCue): bIsCritical is a headshot, Magnitude the damage, HitActor who was hit. Reported as a
// "Damage" fact with the source tags the game's objective rows look for (Homebase.Class = a player, Gameplay.Damage.Critical = a
// headshot, plus the weapon's own item tags, e.g. Weapon.Melee.Impact.Pickaxe), see questStats.h.
static void ReportDamageForChallenges(AFortPawn* Pawn, AFortPlayerController* Controller, AFortWeapon* Weapon, FFrame* Stack)
{
	static const char* FnPath = "/Script/FortniteGame.FortPawn.NetMulticast_Athena_BatchedDamageCues";
	static const char* Shared = "/Script/FortniteGame.AthenaBatchedDamageGameplayCues_Shared";
	static const char* NonShared = "/Script/FortniteGame.AthenaBatchedDamageGameplayCues_NonShared";
	static int SharedOffset = FindOffsetStruct(FnPath, "SharedData", false);
	static int NonSharedOffset = FindOffsetStruct(FnPath, "NonSharedData", false);
	static int HitActorOffset = FindOffsetStruct(NonShared, "HitActor", false);
	static int MagnitudeOffset = FindOffsetStruct(Shared, "Magnitude", false);
	static int ValidOffset = FindOffsetStruct(Shared, "bIsValid", false);
	static int CriticalOffset = FindOffsetStruct(Shared, "bIsCritical", false);
	static auto ValidProp = FindPropertyStruct(Shared, "bIsValid", false);
	static auto CriticalProp = FindPropertyStruct(Shared, "bIsCritical", false);
	static auto PlayerPawnClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerPawnAthena");

	if (Globals::bZoneMode || !Controller || !Stack || !Stack->Locals || SharedOffset == -1 || NonSharedOffset == -1 || HitActorOffset == -1 || MagnitudeOffset == -1 || CriticalOffset == -1)
		return;

	auto ReadBool = [](uint8_t* Base, int Offset, void* Prop)
	{
		const uint8_t Mask = Prop ? GetFieldMask(Prop) : 0;
		const uint8_t Byte = *(Base + Offset);
		return (Mask == 0 || Mask == 0xFF) ? Byte != 0 : (Byte & Mask) != 0;
	};

	uint8_t* S = (uint8_t*)Stack->Locals + SharedOffset;
	uint8_t* N = (uint8_t*)Stack->Locals + NonSharedOffset;
	auto HitActor = *(UObject**)(N + HitActorOffset);
	const float Magnitude = *(float*)(S + MagnitudeOffset);

	if ((ValidOffset != -1 && !ReadBool(S, ValidOffset, ValidProp)) || !HitActor || HitActor == Pawn || Magnitude <= 0.0f || !PlayerPawnClass || !HitActor->IsA(PlayerPawnClass))
		return;

	QuestStats::Fact Fact;
	Fact.AccountId = QuestStats::AccountIdOf(Controller->GetPlayerState());
	Fact.Type = "Damage";
	Fact.Amount = Magnitude;
	Fact.Target = { "Athena.Player" };
	Fact.Source = { "Homebase.Class" };

	if (ReadBool(S, CriticalOffset, CriticalProp))
		Fact.Source.push_back("Gameplay.Damage.Critical");

	if (auto Definition = Weapon ? (UObject*)Weapon->GetWeaponData() : nullptr)
	{
		static int ItemTagsOffset = Definition->GetOffset("GameplayTags", false);

		if (ItemTagsOffset != -1)
		{
			for (auto& Tag : QuestStats::TagsOf(Definition->GetPtr<FGameplayTagContainer>(ItemTagsOffset)))
				Fact.Source.push_back(Tag);
		}
	}

	QuestStats::Queue(std::move(Fact));
}

void AFortPawn::NetMulticast_Athena_BatchedDamageCuesHook(UObject* Context, FFrame* Stack, void* Ret)
{
	auto Pawn = (AFortPawn*)Context;
	auto Controller = Cast<AFortPlayerController>(Pawn->GetController());
	auto CurrentWeapon = Pawn->GetCurrentWeapon();
	auto WorldInventory = Controller ? Controller->GetWorldInventory() : nullptr;

	ReportDamageForChallenges(Pawn, Controller, CurrentWeapon, Stack);

	if (!WorldInventory || !CurrentWeapon)
		return NetMulticast_Athena_BatchedDamageCuesOriginal(Context, Stack, Ret);

	auto AmmoCount = CurrentWeapon->GetAmmoCount();

	WorldInventory->CorrectLoadedAmmo(CurrentWeapon->GetItemEntryGuid(), AmmoCount);

	return NetMulticast_Athena_BatchedDamageCuesOriginal(Context, Stack, Ret);
}

void AFortPawn::MovingEmoteStoppedHook(UObject* Context, FFrame* Stack, void* Ret)
{
	// LOG_INFO(LogDev, "MovingEmoteStoppedHook!");

	auto Pawn = (AFortPawn*)Context;

	static auto bMovingEmoteOffset = Pawn->GetOffset("bMovingEmote", false);

	if (bMovingEmoteOffset != -1)
	{
		static auto bMovingEmoteFieldMask = GetFieldMask(Pawn->GetProperty("bMovingEmote"));
		Pawn->SetBitfieldValue(bMovingEmoteOffset, bMovingEmoteFieldMask, false);
	}

	static auto bMovingEmoteForwardOnlyOffset = Pawn->GetOffset("bMovingEmoteForwardOnly", false);

	if (bMovingEmoteForwardOnlyOffset != -1)
	{
		static auto bMovingEmoteForwardOnlyFieldMask = GetFieldMask(Pawn->GetProperty("bMovingEmoteForwardOnly"));
		Pawn->SetBitfieldValue(bMovingEmoteOffset, bMovingEmoteForwardOnlyFieldMask, false);
	}

	return MovingEmoteStoppedOriginal(Context, Stack, Ret);
}

UClass* AFortPawn::StaticClass()
{
	static auto Class = FindObject<UClass>("/Script/FortniteGame.FortPawn");
	return Class;
}