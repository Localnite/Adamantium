#pragma once

// Bot pawns run without a controller (see bots.h), but a few Reboot hooks look the controller up through the pawn
// (e.g. to add a picked up item to its inventory). They use this to find the bot's controller instead.

#include <unordered_map>
#include <string>
#include <unordered_set>

#include "Pawn.h"

class AController;

namespace BotRegistry
{
	inline std::unordered_map<void*, AController*> ControllerByPawn;
	inline std::unordered_set<void*> BotControllers;
	inline std::unordered_map<void*, std::string> BotLabelByPlayerState; // "#14 SneakyDragon40", for logs

	inline std::string LabelFor(void* PlayerState)
	{
		auto Found = BotLabelByPlayerState.find(PlayerState);
		return Found != BotLabelByPlayerState.end() ? Found->second : std::string("(not a bot)");
	}

	inline bool IsBotController(void* Controller)
	{
		return BotControllers.find(Controller) != BotControllers.end();
	}

	// The pawn's own controller if it has one, otherwise the controller of the bot that owns the pawn.
	inline AController* ControllerFor(APawn* Pawn)
	{
		if (!Pawn)
			return nullptr;

		if (auto Controller = (AController*)Pawn->GetController())
			return Controller;

		auto Found = ControllerByPawn.find(Pawn);
		return Found != ControllerByPawn.end() ? Found->second : nullptr;
	}
}
