#pragma once

// reboot.ini: the settings of our additions to Reboot (the bots and Save the World), kept out of the code so each can be switched off.
//
// The file lives next to the game exe (FortniteGame/Binaries/Win64/reboot.ini, where reboot.log is too). -REBOOT_INI=<path> names another one. When it does not exist yet it is
// written with the defaults below, so there is something to edit. A command line flag of the same setting (-BOT_COUNT=5 ...) wins over the file.
// Read once, when the DLL starts.
//
// Servers that run the same build share one reboot.ini, so every setting can also be given on the command line of the server that needs it differently:
//   -reboot.<Section>.<Key>=<value>      e.g.  -reboot.Bots.Enabled=false  -reboot.SaveTheWorld.Map=Zone_Onboarding_FarmsteadFort  -reboot.Bots.Playlist="a b"
// (case does not matter; a value with spaces goes in quotes). These win over the file and over the older -BOT_* flags' defaults from the file.

#include <Windows.h>
#include <string>
#include <fstream>
#include <cwctype>
#include <cstdlib>
#include <algorithm>

namespace RebootConfig
{
	struct Bots
	{
		bool bEnabled = true;       // master switch: false = no bot is spawned, filled or ticked
		int Count = 1;              // bots added at match start when nobody asks for more (the "Amount of bots to spawn" field of the window)
		int Fill = -1;              // add bots just before the bus up to this many players in all (like a real match), -1 = off
		float Skill = -1.0f;        // aim skill of every bot, -1 = each bot rolls its own
		bool bTest = false;         // bots-only match: the bus starts on its own
		float BusDelay = 20.0f;     // test: seconds between the bots appearing and the bus
		float MaxSeconds = 900.0f;  // test: it ends after this long
		bool bExit = false;         // test: close the server when it ends
		bool bNavBuild = false;     // explore the navigation map, save it and close
		std::string Playlist;       // playlist to play, empty = the default
	};

	struct SaveTheWorld
	{
		bool bEnabled = true;       // false = Reboot ignores the Zone (STW) game mode completely
		std::string Map;            // Zone map the server opens at start instead of the Battle Royale island, e.g. Zone_Onboarding_FarmsteadFort (empty = Athena_Terrain)
		std::string GameMode = "/Game/Blueprints/GameModeZone.GameModeZone_C";
		std::string Options;        // more URL options for that map, without the leading ?, e.g. worldId=...?theaterId=...
		int ListenDelayFrames = 150; // frames after a Zone map is up before the server starts listening
		int TravelDelayFrames = 400; // frames after the island is up before the server travels to Map
		bool bMcp = true;           // fetch the players' profiles from the backend as dedicated server commands (needed for STW)
		bool bSpawnHusks = false;   // spawn HuskPawn_C manually on player join (off by default so server doesn't crash on join; encounters handle native husks)
		std::string Replication = "graph"; // how the Zone world replicates: graph = the replication graph is switched on and Reboot drives it, native = the game's own setting and the engine replicates (Reboot does not), reboot = Reboot's own port of ServerReplicateActors without the graph
	};

	struct Config
	{
		std::wstring Path;
		bool bLoaded = false;
		Bots Bots;
		SaveTheWorld Stw;
	};

	inline const char* DefaultText()
	{
		return
			"; Settings of the additions to Project Reboot (bots, Save the World). Read when the DLL starts.\r\n"
			"; A command line flag of the same name (-BOT_COUNT=5, -BOT_TEST ...) overrides the file.\r\n"
			"\r\n"
			"[Bots]\r\n"
			"; false = no bots at all (none spawned, none filled, none ticked)\r\n"
			"Enabled=true\r\n"
			"; bots added at match start (the 'Amount of bots to spawn' field of the window)\r\n"
			"Count=1\r\n"
			"; fill the match with bots just before the bus up to this many players in all, -1 = off\r\n"
			"Fill=-1\r\n"
			"; aim skill of every bot, -1 = every bot rolls its own\r\n"
			"Skill=-1\r\n"
			"; bots-only test match: the bus leaves on its own\r\n"
			"Test=false\r\n"
			"BusDelay=20\r\n"
			"MaxSeconds=900\r\n"
			"Exit=false\r\n"
			"NavBuild=false\r\n"
			"; playlist to play, empty = the default\r\n"
			"Playlist=\r\n"
			"\r\n"
			"[SaveTheWorld]\r\n"
			"; false = Reboot leaves the Zone (Save the World) game mode alone\r\n"
			"Enabled=true\r\n"
			"; Zone map to open at start instead of Athena_Terrain, e.g. Zone_Onboarding_FarmsteadFort. Empty = Battle Royale.\r\n"
			"Map=\r\n"
			"GameMode=/Game/Blueprints/GameModeZone.GameModeZone_C\r\n"
			"; more URL options for the map, without the leading ?  (worldId=...?theaterId=...?theaterMissionId=...?zoneThemeClass=...)\r\n"
			"Options=\r\n"
			"; frames after the Zone map is up before the server listens\r\n"
			"ListenDelayFrames=150\r\n"
			"; frames after the island is up before the server travels to Map\r\n"
			"TravelDelayFrames=400\r\n"
			"; fetch the players' profiles from the backend as dedicated server commands\r\n"
			"Mcp=true\r\n";
	}

	inline std::wstring Widen(const std::string& In) { return std::wstring(In.begin(), In.end()); }

	inline Config Load()
	{
		Config Out;

		wchar_t Module[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, Module, MAX_PATH);
		std::wstring Dir = Module;
		Dir = Dir.substr(0, Dir.find_last_of(L"\\/") + 1);
		Out.Path = Dir + L"reboot.ini";

		// -REBOOT_INI=<path>
		{
			const std::wstring Original = GetCommandLineW();
			std::wstring Lower = Original;
			for (auto& Letter : Lower) Letter = (wchar_t)towlower(Letter);
			const size_t At = Lower.find(L"-reboot_ini=");

			if (At != std::wstring::npos)
			{
				size_t Start = At + 12, End = Start;
				while (End < Original.size() && Original[End] != L' ' && Original[End] != L'"') ++End;
				Out.Path = Original.substr(Start, End - Start);
			}
		}

		if (GetFileAttributesW(Out.Path.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			std::ofstream File(Out.Path, std::ios::binary);
			if (File) File << DefaultText();
		}

		const std::wstring CommandLine = GetCommandLineW();
		std::wstring LowerCommandLine = CommandLine;
		for (auto& Letter : LowerCommandLine) Letter = (wchar_t)towlower(Letter);

		auto Text = [&](const wchar_t* Section, const wchar_t* Key, const std::string& Default) -> std::string
		{
			// -reboot.<section>.<key>=value on the command line
			{
				std::wstring Name = std::wstring(L"-reboot.") + Section + L"." + Key + L"=";
				for (auto& Letter : Name) Letter = (wchar_t)towlower(Letter);

				size_t At = 0;
				while ((At = LowerCommandLine.find(Name, At)) != std::wstring::npos)
				{
					if (At == 0 || LowerCommandLine[At - 1] == L' ' || LowerCommandLine[At - 1] == L'\t' || LowerCommandLine[At - 1] == L'"') break;
					At += Name.size();
				}

				if (At != std::wstring::npos)
				{
					size_t Start = At + Name.size(), End = Start;
					std::wstring Value;

					if (Start < CommandLine.size() && CommandLine[Start] == L'"')
					{
						End = CommandLine.find(L'"', Start + 1);
						if (End == std::wstring::npos) End = CommandLine.size();
						Value = CommandLine.substr(Start + 1, End - Start - 1);
					}
					else
					{
						while (End < CommandLine.size() && CommandLine[End] != L' ' && CommandLine[End] != L'\t' && CommandLine[End] != L'"') ++End;
						Value = CommandLine.substr(Start, End - Start);
					}

					std::string Narrow;
					for (auto Letter : Value) Narrow += (char)Letter;
					return Narrow;
				}
			}

			wchar_t Buffer[1024] = {};
			GetPrivateProfileStringW(Section, Key, Widen(Default).c_str(), Buffer, 1024, Out.Path.c_str());

			std::wstring Value = Buffer;
			// trailing "; comment" and spaces
			if (const size_t Semi = Value.find(L" ;"); Semi != std::wstring::npos) Value = Value.substr(0, Semi);
			while (!Value.empty() && iswspace(Value.back())) Value.pop_back();

			std::string Narrow;
			for (auto Letter : Value) Narrow += (char)Letter;
			return Narrow;
		};
		auto Number = [&](const wchar_t* Section, const wchar_t* Key, double Default) -> double
		{
			const auto Raw = Text(Section, Key, "");
			if (Raw.empty()) return Default;
			char* EndPtr = nullptr;
			const double Parsed = strtod(Raw.c_str(), &EndPtr);
			return EndPtr != Raw.c_str() ? Parsed : Default;
		};
		auto Flag = [&](const wchar_t* Section, const wchar_t* Key, bool Default) -> bool
		{
			auto Raw = Text(Section, Key, "");
			if (Raw.empty()) return Default;
			for (auto& Letter : Raw) Letter = (char)tolower(Letter);
			return Raw == "1" || Raw == "true" || Raw == "yes" || Raw == "on";
		};

		Out.Bots.bEnabled = Flag(L"Bots", L"Enabled", Out.Bots.bEnabled);
		Out.Bots.Count = (int)Number(L"Bots", L"Count", Out.Bots.Count);
		Out.Bots.Fill = (int)Number(L"Bots", L"Fill", Out.Bots.Fill);
		Out.Bots.Skill = (float)Number(L"Bots", L"Skill", Out.Bots.Skill);
		Out.Bots.bTest = Flag(L"Bots", L"Test", Out.Bots.bTest);
		Out.Bots.BusDelay = (float)Number(L"Bots", L"BusDelay", Out.Bots.BusDelay);
		Out.Bots.MaxSeconds = (float)Number(L"Bots", L"MaxSeconds", Out.Bots.MaxSeconds);
		Out.Bots.bExit = Flag(L"Bots", L"Exit", Out.Bots.bExit);
		Out.Bots.bNavBuild = Flag(L"Bots", L"NavBuild", Out.Bots.bNavBuild);
		Out.Bots.Playlist = Text(L"Bots", L"Playlist", "");

		Out.Stw.bEnabled = Flag(L"SaveTheWorld", L"Enabled", Out.Stw.bEnabled);
		Out.Stw.Map = Text(L"SaveTheWorld", L"Map", "");
		Out.Stw.GameMode = Text(L"SaveTheWorld", L"GameMode", Out.Stw.GameMode);
		Out.Stw.Options = Text(L"SaveTheWorld", L"Options", "");
		Out.Stw.ListenDelayFrames = (std::max)(1, (int)Number(L"SaveTheWorld", L"ListenDelayFrames", Out.Stw.ListenDelayFrames));
		Out.Stw.TravelDelayFrames = (std::max)(1, (int)Number(L"SaveTheWorld", L"TravelDelayFrames", Out.Stw.TravelDelayFrames));
		Out.Stw.bMcp = Flag(L"SaveTheWorld", L"Mcp", Out.Stw.bMcp);
		Out.Stw.bSpawnHusks = Flag(L"SaveTheWorld", L"SpawnHusks", Out.Stw.bSpawnHusks);
		Out.Stw.Replication = Text(L"SaveTheWorld", L"Replication", Out.Stw.Replication);
		for (auto& Letter : Out.Stw.Replication) Letter = (char)tolower(Letter);

		Out.bLoaded = true;
		return Out;
	}

	inline const Config& Get()
	{
		static const Config Cached = Load();
		return Cached;
	}
}
