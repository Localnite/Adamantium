#pragma once

// Settings for a server run by the backend (LawinServer's match manager) with no one at the keyboard. Read once from the command line:
//   -PROD_MATCH               turns it all on: no GUI window, "No MCP" off (so cosmetics load), and the match flow in prodmatch.h
//   -PROD_MATCH_ID=<id>       the backend's name for this match, sent back with every report
//   -PROD_CALLBACK=<url>      where to report ("ready", "joined", "filled", ... plus a heartbeat); without it nothing is reported
//   -PROD_TOKEN=<secret>      sent with every report so the backend can tell it is really us
//   -PROD_JOIN_WINDOW=S       seconds after the latest player joined before bots are added (default 15, a new player starts it again)
//   -PROD_BUS_PAUSE=S         seconds between the last bot being added and the bus countdown (default 3.5)
//   -PROD_COUNTDOWN=S         length of the bus countdown (default 10, the same as the GUI button)
//   -PROD_FILL=N              players in the match once the bots are in (default: everything the playlist holds)
//   -PROD_DRAIN_GRACE=S       seconds with nobody connected, after the bots were added, before the server closes (default 10)
//   -PROD_IDLE_TIMEOUT=S      close if nobody has joined this long after the server was ready (default 600, 0 = never)
//   -PROD_END_GRACE=S         close this long after the match has a winner, even if players are still connected (default 90)
//   -PROD_ORPHAN_LIMIT=N      close (if nobody is connected) after this many reports in a row could not be delivered (default 6)

#include <Windows.h>
#include <string>
#include <atomic>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace ProdMatch
{
	struct Options
	{
		bool bEnabled = false;
		std::string MatchId;
		std::string Callback;
		std::string Token;
		float JoinWindow = 15.0f;
		float BusPause = 3.5f;
		float Countdown = 10.0f;
		int FillTotal = -1;
		float DrainGrace = 10.0f;
		float IdleTimeout = 600.0f;
		float EndGrace = 90.0f;
		int OrphanLimit = 6;
	};

	inline Options Parse()
	{
		Options Out;
		const std::wstring Original = GetCommandLineW();
		std::wstring Lower = Original;

		for (auto& Letter : Lower)
			Letter = (wchar_t)towlower(Letter);

		auto IsBoundary = [&](size_t At) { return At >= Lower.size() || Lower[At] == L' ' || Lower[At] == L'"' || Lower[At] == L'\t'; };

		// "-prod_match" must not be mistaken for the start of "-prod_match_id="
		{
			const std::wstring Flag = L"-prod_match";
			size_t At = 0;

			while ((At = Lower.find(Flag, At)) != std::wstring::npos)
			{
				if (IsBoundary(At + Flag.size()))
				{
					Out.bEnabled = true;
					break;
				}

				At += Flag.size();
			}
		}

		auto Text = [&](const wchar_t* Name) -> std::string
		{
			const size_t At = Lower.find(Name);

			if (At == std::wstring::npos)
				return {};

			const size_t Start = At + wcslen(Name);
			size_t End = Start;

			while (End < Original.size() && Original[End] != L' ' && Original[End] != L'"' && Original[End] != L'\t')
				++End;

			std::string Value;

			for (size_t i = Start; i < End; ++i)
				Value += (char)Original[i];

			return Value;
		};

		auto Number = [&](const wchar_t* Name, float& Value)
		{
			const auto Raw = Text(Name);

			if (Raw.empty())
				return;

			char* EndPtr = nullptr;
			const double Parsed = strtod(Raw.c_str(), &EndPtr);

			if (EndPtr != Raw.c_str() && Parsed >= 0.0)
				Value = (float)Parsed;
		};

		Out.MatchId = Text(L"-prod_match_id=");
		Out.Callback = Text(L"-prod_callback=");
		Out.Token = Text(L"-prod_token=");

		float Fill = -1.0f, Orphan = (float)Out.OrphanLimit;
		Number(L"-prod_join_window=", Out.JoinWindow);
		Number(L"-prod_bus_pause=", Out.BusPause);
		Number(L"-prod_countdown=", Out.Countdown);
		Number(L"-prod_drain_grace=", Out.DrainGrace);
		Number(L"-prod_idle_timeout=", Out.IdleTimeout);
		Number(L"-prod_end_grace=", Out.EndGrace);
		Number(L"-prod_fill=", Fill);
		Number(L"-prod_orphan_limit=", Orphan);
		Out.FillTotal = (int)Fill;
		Out.OrphanLimit = (std::max)(1, (int)Orphan);

		return Out;
	}

	inline const Options& Get()
	{
		static const Options Cached = Parse();
		return Cached;
	}

	// set when the join window is over; ReadyToStartMatch then lets the match start (so the warm-up phase exists for the bots and the countdown)
	inline std::atomic<bool> gAllowStart{ false };
}
