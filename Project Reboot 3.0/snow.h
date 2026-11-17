#pragma once

// ---- holiday snow (Season 7) ----
//   -SNOW_LEVEL=X     how much holiday snow covers the map, 0 (none) to 1 (all of it); the backend works it out from the date
//
// The map holds one BP_SnowSetup actor (/Game/Athena/Environments/Landscape/Blueprints). It replicates a float, Snow_Amount (0..1), and every
// client turns that into the landscape's Holiday_SnowAmount material parameter (MPC_Landscape, through SnowCurve) in OnRep_Snow_Amount, and also
// shows the snowed-in places on the minimap. On a real server its authority-only "RefreshCalender" sets the amount from the calendar events;
// here the server sets it with the blueprint's own SetSnow(NewParam) event (it flushes the actor's net dormancy, so clients get it at once).
// The value is checked again now and then and put back if the blueprint's calendar logic changed it.

#include <Windows.h>
#include <string>
#include <cwctype>
#include <cstdlib>
#include <cmath>

#include "reboot.h"
#include "GameplayStatics.h"
#include "log.h"

namespace Snow
{
	inline float ParseLevel()
	{
		std::wstring Line = GetCommandLineW();

		for (auto& Letter : Line)
			Letter = (wchar_t)towlower(Letter);

		const std::wstring Flag = L"-snow_level=";
		const size_t At = Line.find(Flag);

		if (At == std::wstring::npos)
			return -1.0f;

		const float Value = (float)_wtof(Line.c_str() + At + Flag.size());
		return Value < 0.0f ? 0.0f : (Value > 1.0f ? 1.0f : Value);
	}

	inline float Level()
	{
		static float Value = ParseLevel();
		return Value;
	}

	inline void Tick()
	{
		if (Level() < 0.0f)
			return; // not asked for: the map keeps whatever the game does by itself

		static double NextCheck = 0.0;
		static int Applied = 0;
		static int Missing = 0;
		const double Now = (double)GetTickCount64() / 1000.0;

		if (Now < NextCheck)
			return;

		NextCheck = Now + (Applied > 0 ? 10.0 : 2.0);

		auto World = GetWorld();

		if (!World || !World->GetGameState())
			return;

		static auto SnowClass = FindObject<UClass>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C");

		if (!SnowClass)
		{
			if (++Missing == 15)
				LOG_INFO(LogDev, "Snow: BP_SnowSetup is not loaded in this map; -SNOW_LEVEL={:.2f} has nothing to act on", Level());

			return;
		}

		auto Actors = UGameplayStatics::GetAllActorsOfClass(World, SnowClass);

		if (Actors.Num() == 0)
		{
			Actors.Free();

			if (++Missing == 15)
				LOG_INFO(LogDev, "Snow: no BP_SnowSetup actor in the world yet; -SNOW_LEVEL={:.2f} waits", Level());

			return;
		}

		static auto SetSnowFn = FindObject<UFunction>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C.SetSnow");

		for (int i = 0; i < Actors.Num(); ++i)
		{
			auto Actor = Actors.at(i);

			if (!Actor)
				continue;

			static int AmountOffset = Actor->GetOffset("Snow_Amount", false);
			const float Current = AmountOffset != -1 ? Actor->Get<float>(AmountOffset) : -1.0f;

			if (AmountOffset != -1 && std::fabs(Current - Level()) < 0.001f)
				continue;

			if (SetSnowFn)
			{
				struct { float NewParam; } Params{ Level() };
				Actor->ProcessEvent(SetSnowFn, &Params);
			}
			else if (AmountOffset != -1)
			{
				// no event to call: set the replicated value; the actor is net-dormant until flushed, so flush it as the event would
				Actor->Get<float>(AmountOffset) = Level();
				static auto FlushFn = FindObject<UFunction>(L"/Script/Engine.Actor.FlushNetDormancy");

				if (FlushFn)
					Actor->ProcessEvent(FlushFn, nullptr);
			}

			const float After = AmountOffset != -1 ? Actor->Get<float>(AmountOffset) : -1.0f;
			++Applied;

			if (Applied <= 5)
				LOG_INFO(LogDev, "Snow: set the holiday snow to {:.3f} (was {:.3f}, now reads {:.3f}, through {})", Level(), Current, After, SetSnowFn ? "SetSnow" : "the property");
		}

		Actors.Free();
	}
}
