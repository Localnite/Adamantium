#pragma once

// ---- challenge progress (Battle Royale quests) ----
// In 7.30 the game server's quest manager only tracks the quests it has for a player (Reboot never gives it any), and nothing sends the
// progress of a server-side objective to the backend (UpdateQuestClientObjectives is only used for client-tracked objectives,
// UFortQuestManager::SendClientStatEvent, exe 0x1495ec0; the server's ServerQuestLogin/UpdateQuests are never called). So the server
// reports what real players do as "stat facts", the way the game's own objective rows describe them (AthenaObjectiveStatTable: a type
// such as Interact or Damage, and target/source gameplay tags), and the backend (LawinServerV2 structs/athenaStats.js) matches them
// against the player's active quests.
//
// The hooks only queue facts (cheap, any thread that runs game code); NetDriver.cpp's tick hands them to the match report thread
// (prodmatch.h, "stats" events) every couple of seconds. Without -PROD_MATCH there is nowhere to send them and they are dropped.

#include <mutex>
#include <string>
#include <vector>
#include <format>

#include <map>
#include <set>
#include <unordered_map>

#include "reboot.h"
#include "GameplayTagContainer.h"
#include "GameplayStatics.h"
#include "Actor.h"
#include "FortPawn.h"
#include "FortWeapon.h"
#include "Class.h"
#include "BotRegistry.h"
#include "Map.h"
#include "globals.h"
#include "log.h"

namespace QuestStats
{
	struct Fact
	{
		std::string AccountId;
		std::string Type;                  // EFortQuestObjectiveStatEvent name: "Interact", "Damage", ...
		std::vector<std::string> Target;   // gameplay tags of what it was done to
		std::vector<std::string> Source;   // gameplay tags of who/what did it
		double Amount = 1.0;
	};

	inline std::mutex gLock;
	inline std::vector<Fact> gPending;
	inline int gQueued = 0;

	// The account id of a real player: their UniqueId holds it as text. FUniqueNetIdRepl [8] = the id object; the object holds an FString at
	// [24] (layout seen in the logs, see Bots::AssignBotUniqueIds). Empty for bots (they have a copied or no id, and no player controller).
	inline std::string AccountIdOf(UObject* PlayerState)
	{
		static int UniqueIdOffset = PlayerState ? PlayerState->GetOffset("UniqueId", false) : -1;

		if (!PlayerState || UniqueIdOffset == -1)
			return "";

		// bots carry a copy of a real player's id (so the kill feed tells them apart): whatever a bot does must never count for that player
		if (BotRegistry::BotLabelByPlayerState.count(PlayerState))
			return "";

		uint8_t* Object = *(uint8_t**)((uint8_t*)PlayerState + UniqueIdOffset + 8);

		if (!Object || IsBadReadPtr(Object, 40) || *(uint8_t**)(Object + 8) != Object)
			return "";

		const wchar_t* Text = *(wchar_t**)(Object + 24);
		const int Length = *(int*)(Object + 32);

		if (!Text || Length < 8 || Length > 64 || IsBadReadPtr(Text, Length * 2))
			return "";

		std::string Out;

		for (int i = 0; i < Length - 1; ++i)
		{
			if (Text[i] < 0x20 || Text[i] >= 0x7f)
				return "";

			Out += (char)Text[i];
		}

		return Out;
	}

	inline std::vector<std::string> TagsOf(FGameplayTagContainer* Container)
	{
		std::vector<std::string> Out;

		if (!Container)
			return Out;

		for (int i = 0; i < Container->GameplayTags.Num(); ++i)
			Out.push_back(Container->GameplayTags.at(i).TagName.ToString());

		return Out;
	}

	// ---- where something is: the map's POI volumes (FortPoiVolume LocationTags, e.g. Athena.Location.POI.TiltedTowers, and the quest
	// spots such as Athena.Quests.DanceLocation.*). A point has the tags of every volume whose bounds hold it (bounds are axis-aligned boxes,
	// so a rotated volume reaches a little past its corners).
	struct PoiBox { FVector Min, Max; std::vector<std::string> Tags; };
	inline std::vector<PoiBox> gPoiBoxes;
	inline bool gPoiLoaded = false;
	inline ULONGLONG gPoiNextTry = 0;

	inline bool BoundsOf(AActor* Actor, FVector& Origin, FVector& Extent)
	{
		static auto Fn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorBounds");
		static int CollidingOffset = FindOffsetStruct("/Script/Engine.Actor.GetActorBounds", "bOnlyCollidingComponents", false);
		static int OriginOffset = FindOffsetStruct("/Script/Engine.Actor.GetActorBounds", "Origin", false);
		static int ExtentOffset = FindOffsetStruct("/Script/Engine.Actor.GetActorBounds", "BoxExtent", false);

		if (!Fn || !Actor || OriginOffset == -1 || ExtentOffset == -1)
			return false;

		std::vector<uint8_t> Params(Fn->GetPropertiesSize() + 32, 0);

		if (CollidingOffset != -1)
			Params[CollidingOffset] = 0; // all components: a POI volume's shape does not collide

		Actor->ProcessEvent(Fn, Params.data());
		Origin = *(FVector*)(Params.data() + OriginOffset);
		Extent = *(FVector*)(Params.data() + ExtentOffset);
		return Extent.X > 1.0f && Extent.Y > 1.0f;
	}

	inline void LoadPois()
	{
		if (gPoiLoaded || GetTickCount64() < gPoiNextTry)
			return;

		gPoiNextTry = GetTickCount64() + 10000;
		static auto PoiClass = FindObject<UClass>(L"/Script/FortniteGame.FortPoiVolume");

		if (!PoiClass || !GetWorld())
			return;

		auto Volumes = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PoiClass);
		int Tagged = 0;

		for (int i = 0; i < Volumes.Num(); ++i)
		{
			auto Volume = Volumes.at(i);
			static int TagsOffset = Volume ? Volume->GetOffset("LocationTags", false) : -1;
			FVector Origin{}, Extent{};

			if (!Volume || TagsOffset == -1 || !BoundsOf(Volume, Origin, Extent))
				continue;

			auto Tags = TagsOf(Volume->GetPtr<FGameplayTagContainer>(TagsOffset));

			if (Tags.empty())
				continue;

			gPoiBoxes.push_back({ FVector{ Origin.X - Extent.X, Origin.Y - Extent.Y, Origin.Z - Extent.Z }, FVector{ Origin.X + Extent.X, Origin.Y + Extent.Y, Origin.Z + Extent.Z }, std::move(Tags) });
			++Tagged;
		}

		LOG_INFO(LogDev, "QuestStats: {} POI volumes, {} with location tags", Volumes.Num(), Tagged);
		Volumes.Free();

		if (Tagged > 0)
			gPoiLoaded = true; // (the streaming sublevels may not be in yet: try again in a while)
	}

	inline std::vector<std::string> LocationTagsAt(const FVector& At)
	{
		LoadPois();
		std::set<std::string> Out;

		for (auto& Box : gPoiBoxes)
		{
			if (At.X >= Box.Min.X && At.X <= Box.Max.X && At.Y >= Box.Min.Y && At.Y <= Box.Max.Y && At.Z >= Box.Min.Z && At.Z <= Box.Max.Z)
				Out.insert(Box.Tags.begin(), Box.Tags.end());
		}

		return std::vector<std::string>(Out.begin(), Out.end());
	}

	// an item definition's own gameplay tags (a weapon's: Weapon.Ranged.Shotgun..., Weapon.Melee.Impact.Pickaxe)
	inline std::vector<std::string> ItemTags(UObject* Definition)
	{
		if (!Definition)
			return {};

		static int TagsOffset = Definition->GetOffset("GameplayTags", false);
		return TagsOffset != -1 ? TagsOf(Definition->GetPtr<FGameplayTagContainer>(TagsOffset)) : std::vector<std::string>{};
	}

	// Rarity.Common ... Rarity.SuperRare from the item's EFortRarity (the enum's own names are mapped, whatever their spelling in this build)
	inline std::string RarityTag(UObject* Definition)
	{
		static auto RarityEnum = FindObject<UEnum>(L"/Script/FortniteGame.EFortRarity");
		static int RarityOffset = Definition ? Definition->GetOffset("Rarity", false) : -1;

		if (!Definition || !RarityEnum || RarityOffset == -1)
			return "";

		const int64 Value = *(uint8_t*)((uint8_t*)Definition + RarityOffset);
		auto Names = (TArray<TPair<FName, int64>>*)(__int64(RarityEnum) + sizeof(UField) + sizeof(FString));
		std::string Name;

		for (int i = 0; i < Names->Num(); ++i)
		{
			if (Names->At(i).Value() == Value)
				Name = Names->At(i).Key().ToString();
		}

		static int Logged = 0;

		if (++Logged <= 5)
			LOG_INFO(LogDev, "QuestStats: rarity {} of {} is '{}'", Value, Definition->GetName(), Name);

		auto Has = [&](const char* Part) { return Name.find(Part) != std::string::npos; };

		if (Has("Uncommon") || Has("Sturdy")) return "Rarity.Uncommon";
		if (Has("VeryRare") || Has("Epic") || Has("Fine")) return "Rarity.VeryRare";
		if (Has("SuperRare") || Has("Legendary") || Has("Elegant")) return "Rarity.SuperRare";
		if (Has("Rare") || Has("Quality")) return "Rarity.Rare";
		if (Has("Common") || Has("Handmade") || Has("Ordinary")) return "Rarity.Common";
		return "";
	}

	inline void Queue(Fact&& F)
	{
		if (F.AccountId.empty())
			return;

		std::lock_guard<std::mutex> Lock(gLock);

		if (gPending.size() < 2000)
			gPending.push_back(std::move(F));

		if (++gQueued <= 20)
			LOG_INFO(LogDev, "QuestStats: {} by {} amount {:.0f} target [{}] source [{}]", gPending.back().Type, gPending.back().AccountId, gPending.back().Amount,
				gPending.back().Target.empty() ? "" : gPending.back().Target[0], gPending.back().Source.empty() ? "" : gPending.back().Source.back());
	}

	inline std::vector<std::string> Join(std::vector<std::string> A, const std::vector<std::string>& B)
	{
		A.insert(A.end(), B.begin(), B.end());
		return A;
	}

	inline AActor* PawnOf(UObject* PlayerState)
	{
		static int PawnOffset = PlayerState ? PlayerState->GetOffset("PawnPrivate", false) : -1;
		return PlayerState && PawnOffset != -1 ? *(AActor**)((uint8_t*)PlayerState + PawnOffset) : nullptr;
	}

	// a real player searched or used something: chests, ammo boxes, and quest objects (gnomes, ...), by the actor's own tags
	inline void ReportInteract(UObject* PlayerState, AActor* Actor)
	{
		if (!Actor || Globals::bZoneMode)
			return;

		static auto BuildingActorClass = FindObject<UClass>(L"/Script/FortniteGame.BuildingActor");

		if (!BuildingActorClass || !Actor->IsA(BuildingActorClass))
			return;

		static int StaticTagsOffset = Actor->GetOffset("StaticGameplayTags", false); // a BuildingActor member: the same for every building

		if (StaticTagsOffset == -1)
			return;

		auto Pawn = PawnOf(PlayerState);
		Fact F;
		F.AccountId = AccountIdOf(PlayerState);
		F.Type = "Interact";
		F.Target = TagsOf(Actor->GetPtr<FGameplayTagContainer>(StaticTagsOffset));
		F.Source = Join({ "Homebase.Class" }, Pawn ? LocationTagsAt(Pawn->GetActorLocation()) : std::vector<std::string>{});

		if (!F.Target.empty())
			Queue(std::move(F));
	}

	// a real player eliminated somebody: the weapon in their hands and where both of them were
	inline void ReportKill(UObject* KillerState, AActor* KillerPawn, const FVector& VictimAt)
	{
		if (!KillerState || Globals::bZoneMode)
			return;

		if (!KillerPawn)
			KillerPawn = PawnOf(KillerState);

		Fact F;
		F.AccountId = AccountIdOf(KillerState);
		F.Type = "Kill";
		F.Target = Join({ "Homebase.Class", "Athena.Player" }, LocationTagsAt(VictimAt));
		F.Source = { "Homebase.Class" };

		if (KillerPawn && !IsBadReadPtr(KillerPawn, 8))
		{
			F.Source = Join(F.Source, LocationTagsAt(KillerPawn->GetActorLocation()));

			if (auto Weapon = ((AFortPawn*)KillerPawn)->GetCurrentWeapon())
				F.Source = Join(F.Source, ItemTags((UObject*)Weapon->GetWeaponData()));
		}

		Queue(std::move(F));
	}

	// a real player picked an item up (the Week 1 "collect an item of every rarity")
	inline void ReportCollect(UObject* PlayerState, UObject* Definition)
	{
		if (!Definition || Globals::bZoneMode)
			return;

		auto Rarity = RarityTag(Definition);
		Fact F;
		F.AccountId = AccountIdOf(PlayerState);
		F.Type = "Collect";
		F.Target = Join(Rarity.empty() ? std::vector<std::string>{} : std::vector<std::string>{ Rarity }, ItemTags(Definition));
		F.Source = { "Homebase.Class" };
		Queue(std::move(F));
	}

	// a real player emoted: dance at a place (the location's tags) and the emote's own tags
	inline void ReportEmote(UObject* PlayerState, AActor* Pawn, UObject* Emote)
	{
		if (!Pawn || Globals::bZoneMode)
			return;

		Fact F;
		F.AccountId = AccountIdOf(PlayerState);
		F.Type = "Emote";
		F.Source = Join(Join({ "Homebase.Class" }, LocationTagsAt(Pawn->GetActorLocation())), ItemTags(Emote));
		Queue(std::move(F));
	}

	// Once a second for every real player: entering a new place ("Visit", with the place's tags) and the first landing after the bus
	// ("Land", where it happened).
	struct Watch { std::set<std::string> Tags; bool bWasAirborne = false; bool bLanded = false; };
	inline std::unordered_map<void*, Watch> gWatch;

	inline void TickPlayer(UObject* PlayerState, AActor* Pawn)
	{
		if (!PlayerState || !Pawn || Globals::bZoneMode || BotRegistry::BotLabelByPlayerState.count(PlayerState))
			return;

		auto& W = gWatch[PlayerState];
		const auto Tags = LocationTagsAt(Pawn->GetActorLocation());
		std::vector<std::string> Entered;

		for (auto& Tag : Tags)
		{
			if (!W.Tags.count(Tag))
				Entered.push_back(Tag);
		}

		W.Tags = std::set<std::string>(Tags.begin(), Tags.end());

		static int SkydivingOffset = Pawn->GetOffset("bIsSkydiving", false);
		static int ParachuteOffset = Pawn->GetOffset("bIsParachuteOpen", false);
		static auto SkydivingProp = Pawn->GetProperty("bIsSkydiving", false);
		static auto ParachuteProp = Pawn->GetProperty("bIsParachuteOpen", false);
		const bool bAirborne = (SkydivingOffset != -1 && SkydivingProp && Pawn->ReadBitfieldValue(SkydivingOffset, GetFieldMask(SkydivingProp)))
			|| (ParachuteOffset != -1 && ParachuteProp && Pawn->ReadBitfieldValue(ParachuteOffset, GetFieldMask(ParachuteProp)));

		if (!Entered.empty())
		{
			Fact F;
			F.AccountId = AccountIdOf(PlayerState);
			F.Type = "Visit";
			F.Source = Join({ "Homebase.Class" }, Entered);
			Queue(std::move(F));
		}

		if (W.bWasAirborne && !bAirborne && !W.bLanded)
		{
			W.bLanded = true;
			Fact F;
			F.AccountId = AccountIdOf(PlayerState);
			F.Type = "Land";
			F.Source = Join({ "Homebase.Class" }, Tags);
			Queue(std::move(F));
		}

		W.bWasAirborne = bAirborne;
	}

	inline std::string Escape(const std::string& Text)
	{
		std::string Out;

		for (char C : Text)
		{
			if (C == '"' || C == '\\') Out += '\\';
			if ((unsigned char)C >= 0x20) Out += C;
		}

		return Out;
	}

	// the queued facts as a JSON array (and the queue emptied); "" when there are none
	inline std::string Drain()
	{
		std::vector<Fact> Taken;

		{
			std::lock_guard<std::mutex> Lock(gLock);
			Taken.swap(gPending);
		}

		if (Taken.empty())
			return "";

		auto List = [](const std::vector<std::string>& Tags)
		{
			std::string Out = "[";

			for (size_t i = 0; i < Tags.size(); ++i)
				Out += std::format("{}\"{}\"", i ? "," : "", Escape(Tags[i]));

			return Out + "]";
		};

		std::string Out = "[";

		for (size_t i = 0; i < Taken.size(); ++i)
		{
			const auto& F = Taken[i];
			Out += std::format("{}{{\"accountId\":\"{}\",\"type\":\"{}\",\"amount\":{:.1f},\"target\":{},\"source\":{}}}", i ? "," : "", Escape(F.AccountId), F.Type, F.Amount, List(F.Target), List(F.Source));
		}

		return Out + "]";
	}
}
