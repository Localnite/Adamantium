#pragma once

// The match flow of a server run by the backend (-PROD_MATCH, see prodopts.h): wait for a player, give others a few seconds to arrive, add the bots,
// run the bus countdown, and close the server when the last real player has left. Everything is driven from Tick(), once per network tick.
//
//   Booting -> Waiting (ready, nobody yet) -> JoinWindow (a player is in; every new player starts the timer again)
//           -> Filling (the match may start; waits for the warm-up phase) -> BusPause (the bots are in) -> Countdown -> InMatch -> closed
//
// While nobody has joined (or everybody left before the bots were added) the server just goes back to waiting. Once the bots are in, an empty server closes.

#include <winhttp.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include "prodopts.h"
#include "bots.h"
#include "gui.h"

#pragma comment(lib, "winhttp.lib")

namespace ProdMatch
{
	enum class State { Booting, Waiting, JoinWindow, Filling, BusPause, Countdown, InMatch, Closing };

	inline const char* StateName(State S)
	{
		switch (S)
		{
		case State::Booting: return "booting";
		case State::Waiting: return "waiting";
		case State::JoinWindow: return "join_window";
		case State::Filling: return "filling";
		case State::BusPause: return "bus_pause";
		case State::Countdown: return "countdown";
		case State::InMatch: return "in_match";
		case State::Closing: return "closing";
		}

		return "?";
	}

	inline double Clock()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count(); // real time, not the game's: that one jumps when the level changes
	}

	// ---- reports to the backend, sent from a thread of their own so a slow or dead backend never stalls the game ----
	namespace Report
	{
		struct Message
		{
			std::string Event;
			std::string Body;
			bool bCritical = false; // retried a few times
		};

		inline std::mutex gLock;
		inline std::condition_variable gWake;
		inline std::deque<Message> gQueue;
		inline std::atomic<bool> gThreadStarted{ false };
		inline std::atomic<int> gFailures{ 0 };       // reports in a row that could not be delivered
		inline std::atomic<bool> gBackendGone{ false }; // the backend answered "I don't know this match"
		inline std::atomic<bool> gClosingSent{ false };

		inline std::string Escape(const std::string& In)
		{
			std::string Out;

			for (char c : In)
			{
				if (c == '"' || c == '\\') { Out += '\\'; Out += c; }
				else if ((unsigned char)c < 0x20) Out += ' ';
				else Out += c;
			}

			return Out;
		}

		// 0 = could not be sent; otherwise the HTTP status
		inline int Post(const std::string& Url, const std::string& Body, const std::string& Token)
		{
			URL_COMPONENTS Parts{};
			Parts.dwStructSize = sizeof(Parts);
			wchar_t Host[256] = {}, Path[1024] = {};
			Parts.lpszHostName = Host; Parts.dwHostNameLength = 256;
			Parts.lpszUrlPath = Path; Parts.dwUrlPathLength = 1024;

			const std::wstring WideUrl(Url.begin(), Url.end());

			if (!WinHttpCrackUrl(WideUrl.c_str(), 0, 0, &Parts))
				return 0;

			int Status = 0;
			HINTERNET Session = WinHttpOpen(L"RebootProd/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

			if (!Session)
				return 0;

			WinHttpSetTimeouts(Session, 2000, 2000, 3000, 3000);

			if (HINTERNET Connection = WinHttpConnect(Session, Host, Parts.nPort, 0))
			{
				const DWORD Flags = Parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

				if (HINTERNET Request = WinHttpOpenRequest(Connection, L"POST", Path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Flags))
				{
					std::wstring Headers = L"Content-Type: application/json\r\nX-Prod-Token: " + std::wstring(Token.begin(), Token.end()) + L"\r\n";

					if (WinHttpSendRequest(Request, Headers.c_str(), (DWORD)-1, (LPVOID)Body.data(), (DWORD)Body.size(), (DWORD)Body.size(), 0)
						&& WinHttpReceiveResponse(Request, nullptr))
					{
						DWORD Code = 0, Size = sizeof(Code);

						if (WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX))
							Status = (int)Code;
					}

					WinHttpCloseHandle(Request);
				}

				WinHttpCloseHandle(Connection);
			}

			WinHttpCloseHandle(Session);
			return Status;
		}

		inline void Worker()
		{
			const auto& Opts = Get();

			for (;;)
			{
				Message Next;

				{
					std::unique_lock<std::mutex> Lock(gLock);
					gWake.wait(Lock, [] { return !gQueue.empty(); });
					Next = std::move(gQueue.front());
					gQueue.pop_front();
				}

				const int Attempts = Next.bCritical ? 5 : 1;
				int Status = 0;

				for (int Try = 0; Try < Attempts; ++Try)
				{
					if (Try > 0)
						Sleep(1000);

					Status = Post(Opts.Callback, Next.Body, Opts.Token);

					if (Status != 0)
						break; // the backend answered; whatever it said, the report arrived
				}

				if (Status == 410)
				{
					gBackendGone = true; // it has no record of this match (it was restarted?)
					gFailures = 0;
				}
				else if (Status >= 200 && Status < 300)
				{
					gFailures = 0;
				}
				else if (Status == 0)
				{
					++gFailures;
				}
				else
				{
					LOG_WARN(LogMatchmaker, "Prod: the backend answered {} to '{}'", Status, Next.Event);
					gFailures = 0; // it is there, it just did not like the report
				}

				if (Next.Event == "closing")
					gClosingSent = true;
			}
		}

		inline void Send(const std::string& Event, const std::string& Body, bool bCritical)
		{
			const auto& Opts = Get();

			if (Opts.Callback.empty())
			{
				if (Event == "closing")
					gClosingSent = true;

				return;
			}

			if (!gThreadStarted.exchange(true))
				std::thread(Worker).detach();

			{
				std::lock_guard<std::mutex> Lock(gLock);

				if (Event == "heartbeat")
				{
					for (auto& Queued : gQueue)
					{
						if (Queued.Event == "heartbeat")
							return; // one is already waiting
					}
				}

				gQueue.push_back({ Event, Body, bCritical });
			}

			gWake.notify_one();
		}
	}

	// ---- the flow ----
	struct Runtime
	{
		State Current = State::Booting;
		double Created = Clock();
		double ReadyAt = 0, Deadline = 0, FillingAt = 0, FilledAt = 0, CountdownAt = 0, BusAt = 0, EndedAt = 0, EmptySince = 0;
		double NextEval = 0, NextBeat = 0, NextLog = 0;
		int LastHumans = 0;
		bool bHadHuman = false;
		bool bEnded = false;
	};

	inline Runtime gRt;

	// people who are really connected: a network connection that has a player controller. Bots have none, and a dead or spectating player still counts.
	inline int CountConnectedHumans()
	{
		auto World = GetWorld();

		if (!World)
			return 0;

		auto Driver = World->GetNetDriver();

		if (!Driver)
			return 0;

		auto& Connections = Driver->GetClientConnections();
		int Count = 0;

		for (int i = 0; i < Connections.Num(); ++i)
		{
			auto Connection = Connections.At(i);

			if (Connection && Connection->GetPlayerController())
				++Count;
		}

		return Count;
	}

	inline std::string Json(const char* Event, int Humans, const char* Reason = nullptr)
	{
		const auto& Opts = Get();
		int Players = -1, Phase = -1;

		if (auto World = GetWorld())
		{
			if (auto GameState = Cast<AFortGameStateAthena>(World->GetGameState()))
			{
				Players = GameState->GetPlayersLeft();
				Phase = (int)GameState->GetGamePhase();
			}
		}

		std::string Body = std::format("{{\"matchId\":\"{}\",\"event\":\"{}\",\"state\":\"{}\",\"humans\":{},\"players\":{},\"phase\":{},\"uptime\":{:.0f}",
			Report::Escape(Opts.MatchId), Event, StateName(gRt.Current), Humans, Players, Phase, Clock() - gRt.Created);

		if (Reason)
			Body += std::format(",\"reason\":\"{}\"", Report::Escape(Reason));

		return Body + "}";
	}

	inline void Say(const char* Event, int Humans, bool bCritical = true, const char* Reason = nullptr)
	{
		Report::Send(Event, Json(Event, Humans, Reason), bCritical);
	}

	inline void CloseServer(const char* Reason, int Humans)
	{
		if (gRt.Current == State::Closing)
			return;

		LOG_INFO(LogMatchmaker, "Prod: closing the server: {} ({} players connected)", Reason, Humans);
		gRt.Current = State::Closing;
		Say("closing", Humans, true, Reason);

		// give the report a moment to get out (the backend then stops the launcher); it will not wait forever
		for (int Waited = 0; Waited < 40 && !Report::gClosingSent; ++Waited)
			Sleep(100);

		if (BotBrain::gNavDirty)
			BotBrain::SaveNavCache();

		spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });
		ExitProcess(0);
	}

	// what the "Start Bus Countdown" button of the GUI does (the "Start Bus" button, which skips the countdown, on the versions that need it)
	inline void StartBusCountdown(AFortGameModeAthena* GameMode, AFortGameStateAthena* GameState)
	{
		bStartedBus = true;
		AmountOfPlayersWhenBusStart = GameState->GetPlayersLeft();

		if (Globals::bLateGame.load() || Fortnite_Version >= 11)
		{
			GameMode->StartAircraftPhase();
			return;
		}

		static auto WarmupCountdownEndTimeOffset = GameState->GetOffset("WarmupCountdownEndTime");
		static auto WarmupCountdownDurationOffset = GameMode->GetOffset("WarmupCountdownDuration");
		static auto WarmupEarlyCountdownDurationOffset = GameMode->GetOffset("WarmupEarlyCountdownDuration");

		const float TimeSeconds = GameState->GetServerWorldTimeSeconds();
		const float Duration = Get().Countdown;

		GameState->Get<float>(WarmupCountdownEndTimeOffset) = TimeSeconds + Duration;
		GameMode->Get<float>(WarmupCountdownDurationOffset) = Duration;
		GameMode->Get<float>(WarmupEarlyCountdownDurationOffset) = Duration;
	}

	inline void Tick()
	{
		const auto& Opts = Get();

		if (!Opts.bEnabled || gRt.Current == State::Closing)
			return;

		const double Now = Clock();

		if (Now < gRt.NextEval)
			return;

		gRt.NextEval = Now + 0.25;

		auto World = GetWorld();

		if (!World)
			return;

		auto GameState = Cast<AFortGameStateAthena>(World->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(World->GetGameMode());
		const bool bUp = Globals::bStartedListening && World->GetNetDriver() && GameState && GameMode;

		if (!bUp)
			return; // still loading (or the level is changing): nothing to look at yet

		const int Humans = CountConnectedHumans();
		const auto Phase = GameState->GetGamePhase();

		if (Humans > 0)
		{
			gRt.bHadHuman = true;
			gRt.EmptySince = 0;
		}
		else if (gRt.EmptySince == 0)
		{
			gRt.EmptySince = Now;
		}

		// a heartbeat tells the backend we are alive and who is here
		if (Now >= gRt.NextBeat)
		{
			gRt.NextBeat = Now + 10.0;
			Say("heartbeat", Humans, false);
		}

		// the backend was restarted and no longer knows us, or it has been unreachable for a while: do not run forever with nobody here
		if (Humans == 0 && gRt.Current != State::Booting)
		{
			if (Report::gBackendGone)
				return CloseServer("the backend has no record of this match", Humans);

			if (!Opts.Callback.empty() && Report::gFailures >= Opts.OrphanLimit)
				return CloseServer("the backend cannot be reached", Humans);
		}

		switch (gRt.Current)
		{
		case State::Booting:
		{
			gRt.Current = State::Waiting;
			gRt.ReadyAt = Now;
			LOG_INFO(LogMatchmaker, "Prod: ready for players (match '{}')", Opts.MatchId);
			Say("ready", Humans);
			break;
		}

		case State::Waiting:
		{
			if (Humans > 0)
			{
				gRt.Current = State::JoinWindow;
				gRt.Deadline = Now + Opts.JoinWindow;
				gRt.LastHumans = Humans;
				LOG_INFO(LogMatchmaker, "Prod: first player joined, waiting {:.0f}s for others", Opts.JoinWindow);
				Say("first_join", Humans);
			}
			else if (Opts.IdleTimeout > 0.0f && Now - gRt.ReadyAt > Opts.IdleTimeout)
			{
				return CloseServer("nobody joined in time", Humans);
			}

			break;
		}

		case State::JoinWindow:
		{
			if (Humans == 0)
			{
				// they left again before the bots came: nothing to start, back to waiting (also covers a quick reconnect)
				gRt.Current = State::Waiting;
				gRt.ReadyAt = Now;
				LOG_INFO(LogMatchmaker, "Prod: everybody left during the join window, waiting again");
				Say("window_cancelled", Humans);
				break;
			}

			if (Humans > gRt.LastHumans)
			{
				gRt.Deadline = Now + Opts.JoinWindow; // somebody new: the timer starts again
				LOG_INFO(LogMatchmaker, "Prod: {} players now, the join timer starts again", Humans);
				Say("joined", Humans);
			}

			gRt.LastHumans = Humans;

			// a full match does not need to wait (a playlist that could not be read gives a target of 0: then there is no such thing as full)
			const int Target = Bots::FillTarget();

			if (Target > 1 && Humans >= Target)
				gRt.Deadline = Now;

			if (Now >= gRt.Deadline)
			{
				gRt.Current = State::Filling;
				gRt.FillingAt = Now;
				gAllowStart = true; // the match may leave the lobby now; the warm-up phase has to exist before the bots and the countdown
				LOG_INFO(LogMatchmaker, "Prod: join window over with {} players, starting the match", Humans);
				Say("filling", Humans);
			}

			break;
		}

		case State::Filling:
		{
			// the bots are spawned in the warm-up phase, as in the other test and fill modes; if it never comes, go on anyway
			if (Phase != EAthenaGamePhase::Warmup && Now - gRt.FillingAt < 30.0)
				break;

			if (Phase != EAthenaGamePhase::Warmup)
				LOG_WARN(LogMatchmaker, "Prod: the warm-up phase did not start in 30s (phase {}), adding the bots anyway", (int)Phase);

			Bots::FillBots("prod match");
			gRt.Current = State::BusPause;
			gRt.FilledAt = Now;
			LOG_INFO(LogMatchmaker, "Prod: bots added ({} players), the countdown starts in {:.1f}s", GameState->GetPlayersLeft(), Opts.BusPause);
			Say("filled", Humans);
			break;
		}

		case State::BusPause:
		{
			if (Now - gRt.FilledAt < Opts.BusPause)
				break;

			// the countdown fields only mean something in the warm-up phase
			if (Phase != EAthenaGamePhase::Warmup && Now - gRt.FilledAt < Opts.BusPause + 30.0)
				break;

			StartBusCountdown(GameMode, GameState);
			gRt.Current = State::Countdown;
			gRt.CountdownAt = Now;
			LOG_INFO(LogMatchmaker, "Prod: bus countdown started ({:.0f}s)", Opts.Countdown);
			Say("countdown", Humans);
			break;
		}

		case State::Countdown:
		{
			if (Phase == EAthenaGamePhase::Aircraft || Phase == EAthenaGamePhase::SafeZones || Phase == EAthenaGamePhase::EndGame)
			{
				gRt.Current = State::InMatch;
				gRt.BusAt = Now;
				LOG_INFO(LogMatchmaker, "Prod: the bus left");
				Say("bus", Humans);
			}
			else if (Now - gRt.CountdownAt > Opts.Countdown + 25.0)
			{
				// the countdown ran out and nothing happened: start the bus ourselves (the "Start Bus" button)
				LOG_WARN(LogMatchmaker, "Prod: the bus did not leave after the countdown (phase {}), starting it", (int)Phase);
				GameMode->StartAircraftPhase();
				gRt.CountdownAt = Now;
			}

			break;
		}

		case State::InMatch:
		{
			if (!gRt.bEnded && Now - gRt.BusAt > 5.0 && GameState->GetPlayersLeft() <= 1)
			{
				gRt.bEnded = true;
				gRt.EndedAt = Now;
				LOG_INFO(LogMatchmaker, "Prod: the match has a winner");
				Say("ended", Humans);
			}

			if (gRt.bEnded && Now - gRt.EndedAt > Opts.EndGrace)
				return CloseServer("the match is over", Humans);

			break;
		}

		default:
			break;
		}

		// the bots are in: the match no longer waits for anybody, and with nobody left there is nothing to serve
		if (gRt.Current >= State::BusPause && gRt.Current != State::Closing && Humans == 0 && gRt.EmptySince != 0 && Now - gRt.EmptySince >= Opts.DrainGrace)
			return CloseServer("all players left", Humans);

		if (Now >= gRt.NextLog)
		{
			gRt.NextLog = Now + 30.0;
			LOG_INFO(LogMatchmaker, "Prod: {} ({} connected, {} in the match, phase {})", StateName(gRt.Current), Humans, GameState ? GameState->GetPlayersLeft() : -1, (int)Phase);
		}
	}
}
