#include "ZoneMatchmaking.h"
#include "reboot.h"
#include "World.h"
#include "Actor.h"
#include "GameplayStatics.h"
#include "KismetStringLibrary.h"
#include "addresses.h"
#include "hooking.h"
#include "globals.h"
#include "log.h"
#include "ZoneFinder.h"
#include "RebootConfig.h"
#include "NetDriver.h"
#include "NetConnection.h"
#include <vector>
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#pragma comment(lib, "winhttp.lib")

// How the client gets into a mission (7.30 client log of the failed tutorial start, 2026-11-07):
//   matchMakingRequest (session search, "empty server") -> the backend answers one session (id, serverAddress/Port, BEACONPORT_i)
//   -> the client connects a FortPartyBeaconClient to serverAddress:BEACONPORT and sends ServerEmptyServerReservationRequest(sessionId, ...)
//   -> AFortPartyBeaconHost::ProcessEmptyServerReservationRequest: DoesSessionMatch(sessionId) must hold (else BadSessionId),
//      then AddEmptyServerReservationRequest, and the reservation result goes back to the client
//   -> the client waits for ClientAllowedToProceedFromReservation (a real server sends it once the requested mission is loaded),
//      then joins the session and connects to serverAddress:serverPort.
// With no beacon on this server the reservation timed out and the client showed "There was a network problem starting the tutorial".
//
// Addresses (7.30.1 PC, CL 4834550), found by the strings the functions log, checked against the symbolised Xbox 7.30 build
// (scratchpad re/xd.py; fnmod/re/d730.py on PC):
//   APartyBeaconHost::DoesSessionMatch                              exe+0x44cf80   (called by ProcessEmptyServerReservationRequest; false -> result 14 BadSessionId)
//   AFortPartyBeaconHost::AddEmptyServerReservationRequest          exe+0x12f2cf0  (returns EPartyReservationResult, 8 = ReservationAccepted)
//   AFortPartyBeaconHost::ProcessClientsAllowedToProceedFromReservation exe+0x1314460 ("Alerting current clients they are allowed to proceed")
//   vtable slots (AFortGameSession::InitHostBeacon uses the same ones on Xbox and PC): beacon host 0x618 InitHost, 0x628 RegisterHost;
//   party beacon host 0x620 InitHostBeacon(TeamCount, TeamSize, MaxReservations, FName SessionName, ForceTeamNum).
//
// DoesSessionMatch normally compares with the server's own online session; this server has none (it never runs CreateDedicatedSession), so
// the session is the one the backend made for it when it registered (POST /fortnite/api/game/v2/stwserver/register) and the hook compares
// with that id.
//
// Encryption: the client's NMT_Hello (beacon and game port alike) carries an EncryptionToken, and the server hands it to
// FNetDelegates::OnReceivedNetworkEncryptionToken, the game's handler at exe+0xfe5110 (strings "Unable to get user or session.",
// "Client's session ID (%s) doesn't match server's session ID (%s)."). That one looks up the server's own online session (none here), so it
// answered InvalidParams and the beacon closed the connection with NMT_Failure before the reservation was ever sent (2026-11-07 logs:
// "OnlineBeaconHost::SendWelcomeControlMessage: encryption failure [InvalidParams] Unable to get user or session."). A real server would
// then fetch the session key from GET /fortnite/api/game/v2/matchmaking/account/<id>/session/<id>, the same call the client makes. Both ends
// turn AES on after the handshake whatever the key, and with no key every packet fails to encrypt ("Key size 0 is not the expected size
// 32", Build143), so a key is required: the backend makes one per Zone session, answers it (hex) to this server's registration and (base64)
// to the client's key query, and the hook answers Success with it.

bool ZoneReplicateBeaconActor(AActor* Actor, UNetConnection* Connection); // NetDriver.cpp

namespace Zone
{
	namespace
	{
		// the beacon functions below are found by signature (ZoneFinder.h)
		constexpr int ReservationAccepted = 8;
		// ALobbyBeaconHost::DoesSessionMatch (shared with AQosBeaconHost on Xbox 0x21fb0ec): the first call ALobbyBeaconHost::ProcessLogin
		// (exe+0x40d150, string "ProcessLogin %s SessionId %s %s %s from (%s)") makes after its log line; it also asks the server's own session
		constexpr uint8 EncryptionSuccess = 1; // EEncryptionResponse: Failure, Success, InvalidToken, NoKey, SessionIdMismatch (4), InvalidParams (5)

		// FEncryptionKeyResponse as the handler builds it on its stack (Response byte at +0, ErrorMsg at +8, key at +0x18)
		struct FEncryptionKeyResponse
		{
			uint8 Response = 0;
			FString ErrorMsg;
			TArray<uint8> EncryptionKey;
		};

		// FOnEncryptionKeyResponse: the delegate instance pointer and its size; the handler runs ExecuteIfBound as
		// "Size != 0 && Instance && Instance->vtable[0x20]() then Instance->vtable[0x48](Instance, &Response)"
		struct FRawDelegate
		{
			void** Instance;
			int32 Size;
		};

		UObject* gBeaconHost = nullptr;     // AFortOnlineBeaconHost
		UObject* gPartyHost = nullptr;      // AFortPartyBeaconHost
		UObject* gLobbyHost = nullptr;      // AFortLobbyBeaconHostPvE
		bool gStarted = false, gClosed = false;
		bool gReserved = false;            // a party holds the reservation: the backend stops offering this server
		std::atomic<int> gGamePort = 0;     // what the world and the beacon really bound (0 = not known, use the config)
		std::atomic<int> gBeaconPort = 0;

		std::mutex gSessionMutex;
		std::string gSessionId;             // given by the backend when this server registered
		uint8 gSessionKey[32] = {};         // the session's AES key, from the same answer
		bool gHasSessionKey = false;
		std::atomic<bool> gHeartbeatBusy = false;
		std::chrono::steady_clock::time_point gNextHeartbeat{};
		std::chrono::steady_clock::time_point gProceedAt{};
		bool gProceedPending = false;

		bool (*DoesSessionMatchOriginal)(UObject*, FString*) = nullptr;
		bool (*LobbyDoesSessionMatchOriginal)(UObject*, FString*) = nullptr;
		int (*AddEmptyServerReservationOriginal)(UObject*, void*, void*) = nullptr;

		std::string Lower(std::string Text) { for (auto& c : Text) c = (char)tolower((unsigned char)c); return Text; }

		std::string SessionId()
		{
			std::lock_guard Lock(gSessionMutex);
			return gSessionId;
		}

		// POST <Backend><Path> with a JSON body; returns the HTTP status (0 = no answer) and the response body
		int PostJson(const std::wstring& Path, const std::string& Body, std::string& Response)
		{
			const std::string& Backend = RebootConfig::Get().Stw.Backend;
			const std::wstring Url = std::wstring(Backend.begin(), Backend.end()) + Path;

			URL_COMPONENTS Parts{};
			Parts.dwStructSize = sizeof(Parts);
			wchar_t Host[256] = {}, UrlPath[1024] = {};
			Parts.lpszHostName = Host; Parts.dwHostNameLength = 256;
			Parts.lpszUrlPath = UrlPath; Parts.dwUrlPathLength = 1024;
			if (!WinHttpCrackUrl(Url.c_str(), 0, 0, &Parts)) return 0;

			int Status = 0;
			if (HINTERNET Session = WinHttpOpen(L"RebootZone/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
			{
				WinHttpSetTimeouts(Session, 2000, 2000, 5000, 5000);
				if (HINTERNET Connection = WinHttpConnect(Session, Host, Parts.nPort, 0))
				{
					const DWORD Flags = Parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
					if (HINTERNET Request = WinHttpOpenRequest(Connection, L"POST", UrlPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Flags))
					{
						if (WinHttpSendRequest(Request, L"Content-Type: application/json\r\n", (DWORD)-1, (LPVOID)Body.data(), (DWORD)Body.size(), (DWORD)Body.size(), 0)
							&& WinHttpReceiveResponse(Request, nullptr))
						{
							DWORD Code = 0, Size = sizeof(Code);
							if (WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX))
								Status = (int)Code;

							DWORD Available = 0;
							while (WinHttpQueryDataAvailable(Request, &Available) && Available > 0 && Response.size() < 65536)
							{
								std::string Chunk(Available, '\0');
								DWORD Read = 0;
								if (!WinHttpReadData(Request, Chunk.data(), Available, &Read) || Read == 0) break;
								Response.append(Chunk.data(), Read);
							}
						}
						WinHttpCloseHandle(Request);
					}
					WinHttpCloseHandle(Connection);
				}
				WinHttpCloseHandle(Session);
			}
			return Status;
		}

		int CountPlayers()
		{
			static auto PlayerControllerClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerController");
			if (!PlayerControllerClass || !GetWorld()) return 0;
			auto Controllers = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PlayerControllerClass);
			const int Count = Controllers.Num();
			Controllers.Free();
			return Count;
		}

		// One value of the map's URL options (SaveTheWorld.Options, "a=1?b=2"), as text safe inside a JSON string; empty when it is not there.
		std::string OptionValue(const std::string& Options, const std::string& Name)
		{
			const std::string Wanted = Name + "=";
			size_t At = 0;
			while (At < Options.size())
			{
				const size_t End = Options.find('?', At);
				const std::string Pair = Options.substr(At, End == std::string::npos ? std::string::npos : End - At);
				if (Pair.rfind(Wanted, 0) == 0)
				{
					std::string Value;
					for (char c : Pair.substr(Wanted.size())) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) Value += c;
					return Value;
				}
				if (End == std::string::npos) break;
				At = End + 1;
			}
			return "";
		}

		// Registers (first call) or refreshes this server on the backend, on a thread of its own. The backend drops a server that stops sending these.
		// The mission it hosts goes along (theater and theater mission from the map options, and launchId when the backend started this server), so
		// the backend offers it only to the players who asked for that mission.
		void SendHeartbeat(const char* State)
		{
			if (gHeartbeatBusy.exchange(true)) return;

			const auto& Cfg = RebootConfig::Get().Stw;
			const std::string Body = "{\"sessionId\":\"" + SessionId() + "\",\"state\":\"" + State + "\",\"port\":" + std::to_string(gGamePort ? gGamePort.load() : Cfg.GamePort)
				+ ",\"beaconPort\":" + std::to_string(gBeaconPort ? gBeaconPort.load() : Cfg.BeaconPort) + ",\"maxPlayers\":" + std::to_string(Cfg.MaxPlayers)
				+ ",\"players\":" + std::to_string(CountPlayers()) + ",\"map\":\"" + Cfg.Map + "\""
				+ ",\"theaterId\":\"" + OptionValue(Cfg.Options, "theaterId") + "\",\"theaterMissionId\":\"" + OptionValue(Cfg.Options, "theaterMissionId") + "\""
				+ ",\"launchId\":\"" + OptionValue(Cfg.Options, "launchId") + "\"}";

			std::thread([Body]()
			{
				std::string Response;
				const int Status = PostJson(L"/fortnite/api/game/v2/stwserver/register", Body, Response);

				const std::string Key = "\"sessionId\":\"";
				const size_t At = Response.find(Key);
				std::string Id;
				if (Status == 200 && At != std::string::npos) Id = Response.substr(At + Key.size(), Response.find('"', At + Key.size()) - (At + Key.size()));

				// "key":"<64 hex digits>"
				const std::string KeyField = "\"key\":\"";
				const size_t KeyAt = Response.find(KeyField);
				std::string KeyHex = KeyAt != std::string::npos ? Response.substr(KeyAt + KeyField.size(), 64) : std::string();
				uint8 NewKey[32] = {};
				bool bKey = KeyHex.size() == 64;
				for (int i = 0; bKey && i < 32; ++i)
				{
					char* End = nullptr;
					const std::string Byte = KeyHex.substr(i * 2, 2);
					NewKey[i] = (uint8)strtoul(Byte.c_str(), &End, 16);
					bKey = End == Byte.c_str() + 2;
				}

				bool bNew = false;
				{
					std::lock_guard Lock(gSessionMutex);
					bNew = !Id.empty() && Id != gSessionId;
					if (!Id.empty()) gSessionId = Id;
					if (bKey) { memcpy(gSessionKey, NewKey, 32); gHasSessionKey = true; }
				}

				if (bNew)
				{
					LOG_INFO(LogDev, "Zone matchmaking: the backend registered this server as session {} ({})", Id, bKey ? "with its key" : "WITHOUT a key, joins will fail");
				}
				else if (Id.empty())
				{
					static int Warnings = 5;
					if (Warnings > 0) { --Warnings; LOG_WARN(LogDev, "Zone matchmaking: registering with the backend failed (HTTP {}), retrying", Status); }
				}

				gHeartbeatBusy = false;
			}).detach();
		}

		bool DoesSessionMatchHook(UObject* Host, FString* RequestedId)
		{
			std::string Requested = RequestedId && RequestedId->Data.Data && RequestedId->Data.Num() > 1 ? RequestedId->ToString() : std::string();
			const std::string Ours = SessionId();
			const bool bMatch = !Ours.empty() && Lower(Requested) == Lower(Ours);

			LOG_INFO(LogDev, "Zone matchmaking: reservation for session '{}', this server is '{}' -> {}", Requested, Ours, bMatch ? "match" : "NO match");

			// After joining the session the party leader opens a second beacon connection with a normal reservation request for the party it
			// already reserved: the answer is ReservationDuplicate ("OnReconnectResponseReceived Reservation Duplicate" in the client log) and the
			// client then waits for "allowed to proceed" again on that connection (the first one is gone). So it is sent again.
			if (bMatch && gReserved && !gClosed)
			{
				gProceedPending = true;
				gProceedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			}

			return bMatch;
		}

		bool LobbyDoesSessionMatchHook(UObject* Host, FString* RequestedId)
		{
			std::string Requested = RequestedId && RequestedId->Data.Data && RequestedId->Data.Num() > 1 ? RequestedId->ToString() : std::string();
			const std::string Ours = SessionId();
			const bool bMatch = !Ours.empty() && Lower(Requested) == Lower(Ours);

			LOG_INFO(LogDev, "Zone matchmaking: lobby login for session '{}', this server is '{}' -> {}", Requested, Ours, bMatch ? "match" : "NO match");
			return bMatch;
		}

		// Lobby login (ALobbyBeaconHost::ProcessLogin): DoesSessionMatch, then PreLogin, then HandlePlayerLogin (RegisterPlayer on the session,
		// then the lobby state's CreateNewPlayer when the state has authority, Role 3), and ClientLoginComplete(success). The first try failed after
		// "MCP: No game present to join for session (GameSession)"; these say which step. HandlePlayerLogin's signature is its real start:
		// .pdata splits the function, and hooking the second chunk patched the middle of it and crashed the server (Build148/149).
		// ALobbyBeaconState::StartLobby (ProcessLogin calls it when the lobby is full): sets bLobbyStarted (replicated) and broadcasts
		// OnLobbyStarted, which is what sends the logged in party on to the game port
		bool gLobbyStartPending = false;
		std::chrono::steady_clock::time_point gLobbyStartAt{};
		// After StartLobby the client stays in the lobby (Build153: bLobbyStarted 1, nothing else) until the server calls
		// ALobbyBeaconClient::ClientJoinGame, which a real server sends once the mission is ready; the Zone is up, so it follows the start.
		bool gJoinGamePending = false;
		std::chrono::steady_clock::time_point gJoinGameAt{};

		// What the lobby needs on the client: the lobby state (it holds bWorldRecordLoaded, which the client checks before following
		// ClientJoinGame), the lobby player states, and the beacon client actor again (its LobbyState pointer is set at login, after the
		// beacon host replicated the actor once at join). See ZoneReplicateBeaconActor in NetDriver.cpp.
		std::vector<UObject*> gLobbyPlayerStates;
		std::chrono::steady_clock::time_point gNextLobbyReplication{};
		UObject* (*LobbyHandlePlayerLoginOriginal)(UObject*, UObject*, void*, FString*) = nullptr;

		// the reflected members the lobby code reads (they were raw +0x388 / +0x118 / +0x348 reads)
		UObject* LobbyStateOf(UObject* Host)
		{
			static int Offset = Host ? Host->GetOffset("LobbyState", false) : -1;
			return Host && Offset >= 0 ? Host->Get<UObject*>(Offset) : nullptr;
		}

		int RoleOf(UObject* Actor)
		{
			static int Offset = Actor ? Actor->GetOffset("Role", false) : -1;
			return Actor && Offset >= 0 ? (int)Actor->Get<uint8>(Offset) : -1;
		}

		bool IsLobbyStarted(UObject* LobbyState)
		{
			static int Offset = LobbyState ? LobbyState->GetOffset("bLobbyStarted", false) : -1;
			static uint8 Mask = Offset >= 0 ? GetFieldMask(LobbyState->GetProperty("bLobbyStarted")) : 0;
			return LobbyState && Offset >= 0 && LobbyState->ReadBitfieldValue(Offset, Mask);
		}

		// Makes the lobby host's lobby state (ALobbyBeaconHost::SetupLobbyState(MaxPlayers), it lands in LobbyState) and sets what a real server has in it.
		void SetUpLobbyState(UObject* LobbyHost)
		{
			using SetupLobbyStateFn = void (*)(UObject*, int);
			((SetupLobbyStateFn)LobbyHost->VFTable[ZoneFinder::Fixed730::LobbySetupLobbyStateSlot / 8])(LobbyHost, RebootConfig::Get().Stw.MaxPlayers);

			UObject* LobbyState = LobbyStateOf(LobbyHost);
			LOG_INFO(LogDev, "Zone matchmaking: lobby state {} (Role {})", LobbyState ? LobbyState->GetName() : "NONE", RoleOf(LobbyState));
			if (!LobbyState) return;

			// HandlePlayerLogin got no player state from the lobby state's CreateNewPlayer (Build150), which spawns LobbyBeaconPlayerStateClass:
			// a real server has it from config; set the PvE one when it is missing
			static auto PlayerStateClassOffset = LobbyState->GetOffset("LobbyBeaconPlayerStateClass");
			static auto PvEPlayerStateClass = FindObject<UClass>(L"/Script/FortniteGame.FortLobbyBeaconPlayerStatePvE");
			UClass*& PlayerStateClass = LobbyState->Get<UClass*>(PlayerStateClassOffset);
			LOG_INFO(LogDev, "Zone matchmaking: lobby player state class {}{}", PlayerStateClass ? PlayerStateClass->GetName() : "NONE",
				!PlayerStateClass && PvEPlayerStateClass ? ", set to FortLobbyBeaconPlayerStatePvE" : "");
			if (!PlayerStateClass && PvEPlayerStateClass) PlayerStateClass = PvEPlayerStateClass;

			// the client only follows ClientJoinGame once the lobby state says the server's world record is loaded
			// ("Attempting JoinFromLobby before the lobby beacon says the server world record is loaded", Build154); a real server sets
			// it when the mission's world record arrives from the MCP. The Zone is up, so it is loaded.
			static auto WorldRecordProperty = LobbyState->GetProperty("bWorldRecordLoaded");
			if (WorldRecordProperty)
			{
				static auto WorldRecordOffset = LobbyState->GetOffset("bWorldRecordLoaded");
				static auto WorldRecordMask = GetFieldMask(WorldRecordProperty);
				LobbyState->SetBitfieldValue(WorldRecordOffset, WorldRecordMask, true);
				LOG_INFO(LogDev, "Zone matchmaking: lobby state bWorldRecordLoaded set ({})", LobbyState->ReadBitfieldValue(WorldRecordOffset, WorldRecordMask));
			}

			// bWorldRecordLoaded is not replicated: the client works it out in AFortLobbyBeaconState::UpdateWorldRecordLoadedState
			// (Xbox 0x2d63324, on OnRep_MissionGeneratorDetailsRequirement) as "requirement == 1, or requirement == 2 with a mission
			// generator class path". So the replicated MissionGeneratorDetailsRequirement goes to 1 (Build160 had the flag set on the
			// server and the client still said the world record was not loaded).
			static auto RequirementOffset = LobbyState->GetOffset("MissionGeneratorDetailsRequirement");
			if (RequirementOffset >= 0)
			{
				LobbyState->Get<uint8>(RequirementOffset) = 1;
				LOG_INFO(LogDev, "Zone matchmaking: lobby state MissionGeneratorDetailsRequirement (at {:x}) set to 1", RequirementOffset);
			}
		}

		UObject* LobbyHandlePlayerLoginHook(UObject* Host, UObject* Client, void* UniqueId, FString* Options)
		{
			// On Storm Shield Defense 1 the lobby state was gone by the time the player logged in (it was made at start; "lobby state NONE", the login
			// failed and the client said "no matches", 2026-11-07). The outpost game mode starts its match at once, unlike the tutorial, and
			// the lobby state seems to go with that. A login needs one, so it is made again.
			if (!LobbyStateOf(Host))
			{
				LOG_WARN(LogDev, "Zone matchmaking: lobby host {} ({}) has no lobby state at login, making a new one", Host->GetName(), Host == gLobbyHost ? "ours" : "NOT ours");
				SetUpLobbyState(Host);
			}

			UObject* LobbyState = LobbyStateOf(Host);
			const int StateRole = RoleOf(LobbyState);
			UObject* PlayerState = LobbyHandlePlayerLoginOriginal(Host, Client, UniqueId, Options);

			// a real PvE server starts the lobby after its wait timer (StartWaiting) or at once with a full lobby; the Zone is up already, so
			// start it right after the login answer has gone out
			if (PlayerState)
			{
				gLobbyPlayerStates.push_back(PlayerState);
				gLobbyStartPending = true;
				gLobbyStartAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			}
			LOG_INFO(LogDev, "Zone matchmaking: lobby HandlePlayerLogin -> player state {} (lobby state {}, Role {})", PlayerState ? PlayerState->GetName() : "NONE",
				LobbyState ? LobbyState->GetName() : "NONE", StateRole);
			return PlayerState;
		}

		void (*ReceivedEncryptionTokenOriginal)(void*, FString*, FRawDelegate*) = nullptr;

		void ReceivedEncryptionTokenHook(void* GameInstance, FString* Token, FRawDelegate* Delegate)
		{
			// the key stays put while the engine copies it out of the response (the AES handler keeps its own copy)
			static uint8 Key[32] = {};
			bool bKey = false;
			{
				std::lock_guard Lock(gSessionMutex);
				bKey = gHasSessionKey;
				memcpy(Key, gSessionKey, 32);
			}

			static int Logged = 0;
			if (Logged < 20)
			{
				++Logged;
				const std::string Text = Token && Token->Data.Data && Token->Data.Num() > 1 ? Token->ToString() : std::string();
				LOG_INFO(LogDev, "Zone matchmaking: connection with encryption token '{}' accepted {}", Text, bKey ? "with the session key" : "with NO key (not registered yet)");
			}

			if (!Delegate || Delegate->Size == 0 || !Delegate->Instance) return;

			auto VTable = *(void***)Delegate->Instance;
			if (!((bool (*)(void*))VTable[0x20 / 8])(Delegate->Instance)) return;

			// Reboot's TArray leaves its counts uninitialised: zero everything (empty ErrorMsg, empty key)
			FEncryptionKeyResponse Response;
			memset((void*)&Response, 0, sizeof(Response));
			Response.Response = EncryptionSuccess;
			if (bKey)
			{
				Response.EncryptionKey.Data = Key;
				Response.EncryptionKey.ArrayNum = 32;
				Response.EncryptionKey.ArrayMax = 32;
			}
			((void (*)(void*, FEncryptionKeyResponse*))VTable[0x48 / 8])(Delegate->Instance, &Response);
		}

		int AddEmptyServerReservationHook(UObject* Host, void* Reservation, void* EmptyReservation)
		{
			const int Result = AddEmptyServerReservationOriginal(Host, Reservation, EmptyReservation);
			LOG_INFO(LogDev, "Zone matchmaking: empty server reservation -> result {}{}", Result, Result == ReservationAccepted ? " (accepted)" : "");

			// a real server sends "allowed to proceed" once the requested mission is loaded; the Zone is up already, so right after the
			// reservation result has gone out (the next frames: the result RPC must reach the client first)
			if (Result == ReservationAccepted && !gClosed)
			{
				gProceedPending = true;
				gProceedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
				gReserved = true;
				gNextHeartbeat = {}; // tell the backend right away
			}

			return Result;
		}
	}

	void StartMatchmaking()
	{
		const auto& Cfg = RebootConfig::Get().Stw;
		if (gStarted || !Cfg.bMatchmaking) return;
		gStarted = true;

		if (Engine_Version < 422 || Engine_Version > 424)
		{
			LOG_WARN(LogDev, "Zone matchmaking: the reservation beacon addresses are for 7.30 only, not opening it");
			return;
		}

		const uint64 Base = (uint64)GetModuleHandleW(0);

		// Which interface the player controller has at +0x680, and its first slots: on Storm Shield Defense 1 the game mode's post-login
		// (exe+0x100a110) calls slot 0x28 of the interface there and crashed on what came back (2026-11-07). The controller class the
		// game mode spawns is the one to look at (BP_PlayerControllerOutpost_C on the outpost).
		{
			UObject* GameMode = GetWorld()->Get<UObject*>("AuthorityGameMode");
			static auto PlayerControllerClassOffset = GameMode ? GameMode->GetOffset("PlayerControllerClass", false) : -1;
			UClass* ControllerClass = GameMode && PlayerControllerClassOffset >= 0 ? GameMode->Get<UClass*>(PlayerControllerClassOffset) : nullptr;
			UObject* ControllerDefault = ControllerClass ? ControllerClass->CreateDefaultObject() : nullptr;
			const wchar_t* Interfaces[] = { L"/Script/AIModule.GenericTeamAgentInterface", L"/Script/AIModule.CrowdAgentInterface", L"/Script/FortniteGame.FortTeamActorInterface",
				L"/Script/FortniteGame.FortInventoryOwnerInterface", L"/Script/FortniteGame.FortInventoryInterface", L"/Script/FortniteGame.FortAbilitySystemInterface",
				L"/Script/FortniteGame.CustomCharacterPartOwnerInterface", L"/Script/FortniteGame.FortGameplayTagVolumeInterface", L"/Script/GameplayAbilities.AbilitySystemInterface",
				L"/Script/GameplayTags.GameplayTagAssetInterface", L"/Script/GameplayTasks.GameplayTaskOwnerInterface", L"/Script/FortniteGame.FortAIEncounterInfoOwnerInterface" };
			for (auto InterfacePath : Interfaces)
			{
				auto InterfaceClass = FindObject<UClass>(InterfacePath);
				void* Address = ControllerDefault && InterfaceClass ? ControllerDefault->GetInterfaceAddress(InterfaceClass) : nullptr;
				if (!Address) continue;
				uint64* VTable = *(uint64**)Address;
				LOG_INFO(LogDev, "Zone: {} has {} at +0x{:x}, slots 0..6: {:x} {:x} {:x} {:x} {:x} {:x} {:x}", ControllerClass->GetName(), InterfaceClass->GetName(),
					(uint64)Address - (uint64)ControllerDefault, VTable[0] - Base, VTable[1] - Base, VTable[2] - Base, VTable[3] - Base, VTable[4] - Base, VTable[5] - Base, VTable[6] - Base);
			}
		}
		static bool bHooked = false;
		if (!bHooked)
		{
			bHooked = true;
			// a signature that did not match leaves that step to the game (ZoneFinder logs which)
			if (auto A = ZoneFinder::DoesSessionMatch()) Hooking::MinHook::Hook((PVOID)A, (PVOID)DoesSessionMatchHook, (PVOID*)&DoesSessionMatchOriginal, "DoesSessionMatch");
			if (auto A = ZoneFinder::AddEmptyServerReservation()) Hooking::MinHook::Hook((PVOID)A, (PVOID)AddEmptyServerReservationHook, (PVOID*)&AddEmptyServerReservationOriginal, "AddEmptyServerReservationRequest");
			if (auto A = ZoneFinder::LobbyDoesSessionMatch()) Hooking::MinHook::Hook((PVOID)A, (PVOID)LobbyDoesSessionMatchHook, (PVOID*)&LobbyDoesSessionMatchOriginal, "LobbyDoesSessionMatch");
			if (auto A = ZoneFinder::ReceivedNetworkEncryptionToken()) Hooking::MinHook::Hook((PVOID)A, (PVOID)ReceivedEncryptionTokenHook, (PVOID*)&ReceivedEncryptionTokenOriginal, "ReceivedNetworkEncryptionToken");
		}

		static auto BeaconHostClass = FindObject<UClass>(L"/Script/FortniteGame.FortOnlineBeaconHost");
		static auto PartyHostClass = FindObject<UClass>(L"/Script/FortniteGame.FortPartyBeaconHost");
		static auto LobbyHostClass = FindObject<UClass>(L"/Script/FortniteGame.FortLobbyBeaconHostPvE");
		if (!BeaconHostClass || !PartyHostClass || !Addresses::InitHost || !Addresses::PauseBeaconRequests)
		{
			LOG_WARN(LogDev, "Zone matchmaking: missing beacon classes or addresses (host {}, party {}, InitHost {:x}, Pause {:x})", BeaconHostClass != nullptr,
				PartyHostClass != nullptr, Addresses::InitHost, Addresses::PauseBeaconRequests);
			return;
		}

		gBeaconHost = GetWorld()->SpawnActor<AActor>(BeaconHostClass);
		if (!gBeaconHost) { LOG_WARN(LogDev, "Zone matchmaking: could not spawn the beacon host"); return; }

		static auto ListenPortOffset = gBeaconHost->GetOffset("ListenPort");
		gBeaconHost->Get<int>(ListenPortOffset) = Cfg.BeaconPort;

		static bool (*InitHost)(UObject*) = decltype(InitHost)(Addresses::InitHost);
		if (!InitHost(gBeaconHost)) { LOG_WARN(LogDev, "Zone matchmaking: the beacon host could not listen on {}", Cfg.BeaconPort); return; }

		// InitHost writes back the port it bound, the next free one when BeaconPort is taken (e.g. a crashed server's socket still held by
		// the shared wineserver): advertise that one, or clients reserve on a dead port and time out
		gBeaconPort = gBeaconHost->Get<int>(ListenPortOffset);

		gPartyHost = GetWorld()->SpawnActor<AActor>(PartyHostClass);
		if (!gPartyHost) { LOG_WARN(LogDev, "Zone matchmaking: could not spawn the party beacon host"); return; }

		// one team of MaxPlayers, one party per mission; NAME_GameSession like the game's own
		using InitHostBeaconFn = bool (*)(UObject*, int, int, int, FName, int);
		const bool bInit = ((InitHostBeaconFn)gPartyHost->VFTable[ZoneFinder::Fixed730::BeaconHostObjectInitSlot / 8])(gPartyHost, 1, Cfg.MaxPlayers, Cfg.MaxPlayers, UKismetStringLibrary::Conv_StringToName(L"GameSession"), 0);

		using RegisterHostFn = void (*)(UObject*, UObject*);
		((RegisterHostFn)gBeaconHost->VFTable[ZoneFinder::Fixed730::BeaconRegisterHostSlot / 8])(gBeaconHost, gPartyHost);

		// The PvE lobby beacon: once allowed to proceed, the client connects a FortLobbyBeaconClientPvE to the same port and logs in to the
		// mission's lobby (without a host for it the beacon answered "Join failure, Couldn't spawn beacon", 2026-11-07). Set up like
		// AFortGameSession::InitHostBeacon does it (Xbox): spawn, vtable 0x620 Init(SessionName), register, vtable 0x628 SetupLobbyState(MaxPlayers).
		bool bLobby = false;
		if (LobbyHostClass)
		{
			gLobbyHost = GetWorld()->SpawnActor<AActor>(LobbyHostClass);
			if (gLobbyHost)
			{
				using LobbyInitFn = bool (*)(UObject*, FName);
				bLobby = ((LobbyInitFn)gLobbyHost->VFTable[ZoneFinder::Fixed730::BeaconHostObjectInitSlot / 8])(gLobbyHost, UKismetStringLibrary::Conv_StringToName(L"GameSession"));
				((RegisterHostFn)gBeaconHost->VFTable[ZoneFinder::Fixed730::BeaconRegisterHostSlot / 8])(gBeaconHost, gLobbyHost);

				SetUpLobbyState(gLobbyHost);

				static bool bLobbyHooked = false;
				if (!bLobbyHooked)
				{
					bLobbyHooked = true;
					// not PreLogin (vtable 0x640): it is a "return true" stub the linker shares with many other functions, hooking it crashed
					// the server (Build147); it answered true anyway
					if (auto A = ZoneFinder::LobbyHandlePlayerLogin())
						Hooking::MinHook::Hook((PVOID)A, (PVOID)LobbyHandlePlayerLoginHook, (PVOID*)&LobbyHandlePlayerLoginOriginal, "LobbyHandlePlayerLogin");
				}
			}
		}

		// The game's own code finds these through the game session: AFortLobbyBeaconState::CreateNewPlayer asks
		// AFortGameSession::GetTeamForCurrentPlayer, which reads the reservation from GameSession.ReservationBeaconHost and answers team 103
		// ("none", the new lobby player is destroyed and the login fails) while that is null (Build150: "player state NONE").
		{
			static auto AuthorityGameModeOffset = GetWorld()->GetOffset("AuthorityGameMode");
			UObject* GameMode = GetWorld()->Get<UObject*>(AuthorityGameModeOffset);
			static auto GameSessionOffset = GameMode ? GameMode->GetOffset("GameSession") : -1;
			UObject* GameSession = GameMode && GameSessionOffset >= 0 ? GameMode->Get<UObject*>(GameSessionOffset) : nullptr;
			if (GameSession)
			{
				static auto ReservationHostOffset = GameSession->GetOffset("ReservationBeaconHost");
				static auto LobbyHostOffset = GameSession->GetOffset("LobbyBeaconHost");
				if (ReservationHostOffset >= 0) GameSession->Get<UObject*>(ReservationHostOffset) = gPartyHost;
				if (LobbyHostOffset >= 0 && gLobbyHost) GameSession->Get<UObject*>(LobbyHostOffset) = gLobbyHost;
				LOG_INFO(LogDev, "Zone matchmaking: game session {} given the party host (at {:x}) and the lobby host (at {:x})", GameSession->GetName(), ReservationHostOffset, LobbyHostOffset);
			}
			else
			{
				LOG_WARN(LogDev, "Zone matchmaking: no game session on the game mode, lobby logins will fail");
			}
		}

		static void (*PauseBeaconRequests)(UObject*, bool) = decltype(PauseBeaconRequests)(Addresses::PauseBeaconRequests);
		PauseBeaconRequests(gBeaconHost, false);

		LOG_INFO(LogDev, "Zone matchmaking: reservation beacon open on port {} (game port {}, party host init {}, lobby host {}), registering with {}", gBeaconPort.load(), gGamePort.load(), bInit,
			gLobbyHost ? (bLobby ? "init" : "init FAILED") : "missing", Cfg.Backend);

		SendHeartbeat("open");
		gNextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	}

	void MatchmakingTick()
	{
		if (!gStarted || !gPartyHost) return;

		const auto Now = std::chrono::steady_clock::now();

		// The world manager builds a generated zone (Storm Shield Defense: the outpost tiles, its player spawn) and the client's loading screen
		// waits for it ("Waiting for building foundations to be replicated -1 => 0" = FinalNumChosenBuildingFoundations against
		// ChosenBuildingFoundations.Num()). Its state machine and the fields that drive it, logged when they change, show where it stops.
		{
			static UObject* WorldManager = nullptr;
			static bool bLooked = false;
			if (!bLooked)
			{
				bLooked = true;
				static auto WorldManagerClass = FindObject<UClass>(L"/Script/FortniteGame.FortWorldManager");
				auto Found = WorldManagerClass ? UGameplayStatics::GetAllActorsOfClass(GetWorld(), WorldManagerClass) : TArray<AActor*>();
				WorldManager = Found.Num() > 0 ? Found.at(0) : nullptr;
				Found.Free();
				LOG_INFO(LogDev, "Zone: world manager {}", WorldManager ? WorldManager->GetName() : "NONE");
			}
			if (WorldManager)
			{
				static auto StateOffset = WorldManager->GetOffset("WorldManagerState");
				static auto FinalNumOffset = WorldManager->GetOffset("FinalNumChosenBuildingFoundations");
				static auto ChosenOffset = WorldManager->GetOffset("ChosenBuildingFoundations");
				static auto ThemeOffset = WorldManager->GetOffset("Theme");
				static auto WorldRecordOffset = WorldManager->GetOffset("CurrentWorldRecord");
				static auto ZoneRecordOffset = WorldManager->GetOffset("CurrentZoneRecord");
				static auto StreamedOffset = WorldManager->GetOffset("StreamedLevels");
				static auto ConstructOffset = WorldManager->GetOffset("bConstructTileWorld");
				static auto ConstructMask = GetFieldMask(WorldManager->GetProperty("bConstructTileWorld"));
				static auto CreatedMissionsOffset = WorldManager->GetOffset("bHasCreatedMissions");
				static auto CreatedMissionsMask = GetFieldMask(WorldManager->GetProperty("bHasCreatedMissions"));

				const char* States[] = { "Created", "QueryingWorld", "WorldQueryComplete", "CreatingNewWorld", "LoadingExistingWorld", "Running", "Failed" };
				const int State = WorldManager->Get<uint8>(StateOffset);
				UObject* Theme = WorldManager->Get<UObject*>(ThemeOffset);
				UObject* WorldRecord = WorldManager->Get<UObject*>(WorldRecordOffset);
				UObject* ZoneRecord = WorldManager->Get<UObject*>(ZoneRecordOffset);
				const std::string Summary = std::string("state ") + (State >= 0 && State < 7 ? States[State] : "?") + ", final foundations " + std::to_string(WorldManager->Get<int>(FinalNumOffset))
					+ ", chosen " + std::to_string(WorldManager->Get<TArray<UObject*>>(ChosenOffset).Num()) + ", streamed levels " + std::to_string(WorldManager->Get<TArray<UObject*>>(StreamedOffset).Num())
					+ ", theme " + (Theme ? Theme->GetName() : "NONE") + ", world record " + (WorldRecord ? WorldRecord->GetName() : "NONE") + ", zone record " + (ZoneRecord ? ZoneRecord->GetName() : "NONE")
					+ ", construct tile world " + std::to_string(WorldManager->ReadBitfieldValue(ConstructOffset, ConstructMask)) + ", missions created " + std::to_string(WorldManager->ReadBitfieldValue(CreatedMissionsOffset, CreatedMissionsMask));
				static std::string LastSummary;
				if (Summary != LastSummary)
				{
					LOG_INFO(LogDev, "Zone: world manager {}", Summary);
					LastSummary = Summary;
				}
			}
		}

		// Storm Shield Defense picks its leg from the outpost owner's quests (Mission_Outpost: FortOutpostFunctionLibrary.GetOutpostQuestMissionConfigData
		// for Quest.Metadata.Mission.Primary.Outpost; nothing found = "no active quest", the storm shield already up and the tutorial leg skipped,
		// 2026-11-07). What those two functions answer while a player is in, every 5 s, says which of them comes up empty.
		{
			static auto LibraryDefault = FindObject<UObject>(L"/Script/FortniteGame.Default__FortOutpostFunctionLibrary");
			static auto GetOwnerFn = FindObject<UFunction>(L"/Script/FortniteGame.FortOutpostFunctionLibrary.GetOutpostOwner");
			static auto GetConfigFn = FindObject<UFunction>(L"/Script/FortniteGame.FortOutpostFunctionLibrary.GetOutpostQuestMissionConfigData");
			static auto NextProbe = Now;
			static bool bLoggedAddresses = false;
			if (!bLoggedAddresses && GetOwnerFn && GetConfigFn)
			{
				bLoggedAddresses = true;
				const uint64 ExeBase = (uint64)GetModuleHandleW(0);
				LOG_INFO(LogDev, "Zone: outpost library natives: GetOutpostOwner exe+0x{:x}, GetOutpostQuestMissionConfigData exe+0x{:x}", (uint64)GetOwnerFn->GetFunc() - ExeBase,
					(uint64)GetConfigFn->GetFunc() - ExeBase);
			}
			if (LibraryDefault && GetOwnerFn && GetConfigFn && CountPlayers() > 0 && Now >= NextProbe)
			{
				NextProbe = Now + std::chrono::seconds(5);

				struct { UObject* WorldContextObject; UObject* ReturnValue; } OwnerParams{ (UObject*)GetWorld(), nullptr };
				LibraryDefault->ProcessEvent(GetOwnerFn, &OwnerParams);

				struct { UObject* WorldContextObject; FName ConfigTag; UObject* ReturnValue; } ConfigParams{ (UObject*)GetWorld(), UKismetStringLibrary::Conv_StringToName(L"Quest.Metadata.Mission.Primary.Outpost"), nullptr };
				LibraryDefault->ProcessEvent(GetConfigFn, &ConfigParams);

				std::string Answer = std::string("Zone: outpost owner ") + (OwnerParams.ReturnValue ? OwnerParams.ReturnValue->GetName() : "NONE") + ", outpost quest config "
					+ (ConfigParams.ReturnValue ? ConfigParams.ReturnValue->GetName() : "NONE");

				// the storm shield bubble's size: BP_StormShield_Shield sets BubbleInitialScale = InitialScalesPVE01[Mission_Outpost.CurrentLegNumber]
				// (3.85 for the first legs, up to 14.5) and scales its mesh with it in OnRep_BubbleInitialScale; clients get it by replication
				static auto ShieldClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_StormShield_Shield.BP_StormShield_Shield_C");
				static auto MissionClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Mission_Outpost.Mission_Outpost_C");
				if (ShieldClass)
				{
					auto Shields = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ShieldClass);
					for (int i = 0; i < Shields.Num(); ++i)
					{
						auto Shield = Shields.at(i);

						// The server had BubbleInitialScale 3.85 while the player saw the dome at its default 1.0, small and beside the core
						// (2026-11-07): the shield is a dormant actor that wakes up with FlushNetDormancy to send its changes, which does not get
						// through here. Kept awake, its properties replicate like any other actor's.
						if (Shield->GetNetDormancy() != ENetDormancy::DORM_Awake && Shield->GetNetDormancy() != ENetDormancy::DORM_Never)
						{
							LOG_INFO(LogDev, "Zone: storm shield {} was dormant ({}), kept awake so its size reaches the clients", Shield->GetName(), (int)Shield->GetNetDormancy());
							Shield->SetNetDormancy(ENetDormancy::DORM_Awake);
							Shield->ForceNetUpdate();
						}

						static auto ScaleOffset = Shield->GetOffset("BubbleInitialScale", false);
						const FVector Scale = ScaleOffset >= 0 ? Shield->Get<FVector>(ScaleOffset) : FVector{};
						const FVector ShieldAt = Shield->GetActorLocation();
						Answer += ", shield " + Shield->GetName() + " BubbleInitialScale (" + std::to_string(Scale.X) + ", " + std::to_string(Scale.Y) + ", " + std::to_string(Scale.Z) + ") at ("
							+ std::to_string((int)ShieldAt.X) + ", " + std::to_string((int)ShieldAt.Y) + ", " + std::to_string((int)ShieldAt.Z) + ")";
					}
					Shields.Free();
				}
				if (MissionClass)
				{
					auto Missions = UGameplayStatics::GetAllActorsOfClass(GetWorld(), MissionClass);
					for (int i = 0; i < Missions.Num(); ++i)
					{
						static auto LegOffset = Missions.at(i)->GetOffset("CurrentLegNumber", false);
						if (LegOffset >= 0) Answer += ", mission leg " + std::to_string(Missions.at(i)->Get<int>(LegOffset));
						// where the mission spawns the shield (SpawnStormBubble: at EggLocation, set when it finds the core)
						static auto EggLocationOffset = Missions.at(i)->GetOffset("EggLocation", false);
						static auto EggOffset = Missions.at(i)->GetOffset("BP_StormShield_Egg", false);
						AActor* MissionEgg = EggOffset >= 0 ? Missions.at(i)->Get<AActor*>(EggOffset) : nullptr;
						if (EggLocationOffset >= 0)
						{
							const FVector EggAt = Missions.at(i)->Get<FTransform>(EggLocationOffset).Translation;
							Answer += ", mission EggLocation (" + std::to_string((int)EggAt.X) + ", " + std::to_string((int)EggAt.Y) + ", " + std::to_string((int)EggAt.Z) + "), mission core "
								+ (MissionEgg ? MissionEgg->GetName() : std::string("NONE"));
						}

						// SaveLocationofOutpostEgg takes EggLocation from the core's P_ArrowInteraction particle component, and on this server
						// it stayed (0, 0, 0) with the core in place before the mission loaded (2026-11-07): the dome was spawned at the map origin,
						// small around the player and with the core at its edge. With the core there and EggLocation still at the origin, the
						// location becomes the core's own and a shield standing at the origin is moved onto it.
						static auto EggClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_StormShield_Core_Egg.BP_StormShield_Core_Egg_C");
						if (EggLocationOffset >= 0 && EggClass)
						{
							FTransform& EggLocation = Missions.at(i)->Get<FTransform>(EggLocationOffset);
							AActor* Core = MissionEgg;
							if (!Core)
							{
								auto Cores = UGameplayStatics::GetAllActorsOfClass(GetWorld(), EggClass);
								Core = Cores.Num() > 0 ? Cores.at(0) : nullptr;
								Cores.Free();

								// The mission never took the core itself ("mission core NONE" with the core in the world before it loaded, 2026-11-07),
								// so it is handed over the way InitBuildingData does it: StoreStormShieldEggAndPingLocation keeps it as BP_StormShield_Egg
								// (what the mission enables, disables and pings) and sets MapPingLocation and EggLocation from the core's particle
								// components, which read as the origin here and are put right below.
								static auto StoreFn = FindObject<UFunction>(L"/Game/Missions/Primary/Outpost/Mission_Outpost.Mission_Outpost_C.StoreStormShieldEggAndPingLocation");
								if (Core && StoreFn)
								{
									std::vector<uint8_t> StoreParams(StoreFn->GetPropertiesSize() + 16, 0);
									*(AActor**)StoreParams.data() = Core;
									Missions.at(i)->ProcessEvent(StoreFn, StoreParams.data());
									LOG_INFO(LogDev, "Zone: gave the core {} to the mission (StoreStormShieldEggAndPingLocation), mission core now {}", Core->GetName(),
										EggOffset >= 0 && Missions.at(i)->Get<AActor*>(EggOffset) ? Missions.at(i)->Get<AActor*>(EggOffset)->GetName() : "STILL NONE");

									static auto PingOffset = Missions.at(i)->GetOffset("MapPingLocation", false);
									if (PingOffset >= 0)
									{
										FVector& Ping = Missions.at(i)->Get<FVector>(PingOffset);
										if (fabs(Ping.X) < 1 && fabs(Ping.Y) < 1) Ping = Core->GetActorLocation(); // P_Teleporter_Victory sits on the core's origin
									}
									EggLocation.Translation = FVector{}; // recomputed from the core below
								}
							}
							if (Core && fabs(EggLocation.Translation.X) < 1 && fabs(EggLocation.Translation.Y) < 1)
							{
								// The mission's own value is the world location of the core's P_ArrowInteraction (BP_StormShield_Core_Egg: (-162, 0, 480)
								// from the core's root). The core's origin alone put the dome 480 lower and 162 aside, and it cut into the vault door
								// (2026-11-07). That component's world position is what reads as the origin here, so it is made from the core's
								// transform and the component's relative location.
								FVector Relative{ -161.957f, 0.0f, 480.0f };
								static auto ArrowOffset = Core->GetOffset("P_ArrowInteraction", false);
								UObject* Arrow = ArrowOffset >= 0 ? Core->Get<UObject*>(ArrowOffset) : nullptr;
								static auto RelativeLocationOffset = Arrow ? Arrow->GetOffset("RelativeLocation", false) : -1;
								if (Arrow && RelativeLocationOffset >= 0) Relative = Arrow->Get<FVector>(RelativeLocationOffset);

								const FVector CoreAt = Core->GetActorLocation();
								const double Yaw = Core->GetActorRotation().Yaw * 3.14159265358979 / 180.0;
								EggLocation.Translation = FVector(CoreAt.X + (float)(Relative.X * cos(Yaw) - Relative.Y * sin(Yaw)), CoreAt.Y + (float)(Relative.X * sin(Yaw) + Relative.Y * cos(Yaw)),
									CoreAt.Z + Relative.Z);
								LOG_INFO(LogDev, "Zone: mission EggLocation was the origin, set to the core {} ({:.0f}, {:.0f}, {:.0f})", Core->GetName(), EggLocation.Translation.X,
									EggLocation.Translation.Y, EggLocation.Translation.Z);
							}
							if (Core && ShieldClass)
							{
								auto Shields = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ShieldClass);
								for (int s = 0; s < Shields.Num(); ++s)
								{
									auto Shield = Shields.at(s);
									const FVector ShieldAt = Shield->GetActorLocation();
									if (fabs(ShieldAt.X) < 1 && fabs(ShieldAt.Y) < 1 && fabs(ShieldAt.Z) < 1)
									{
										static auto SetLocationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_SetActorLocation");
										if (SetLocationFn)
										{
											std::vector<uint8_t> Params(SetLocationFn->GetPropertiesSize() + 64, 0);
											static int NewLocationOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "NewLocation");
											static int SweepOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "bSweep");
											static int TeleportOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "bTeleport");
											*(FVector*)(Params.data() + NewLocationOffset) = EggLocation.Translation;
											Params[SweepOffset] = 0;
											Params[TeleportOffset] = 1;
											Shield->ProcessEvent(SetLocationFn, Params.data());
										}
										Shield->ForceNetUpdate();
										const FVector NowAt = Shield->GetActorLocation();
										LOG_INFO(LogDev, "Zone: storm shield {} stood at the origin, moved onto the core: now at ({:.0f}, {:.0f}, {:.0f})", Shield->GetName(), NowAt.X, NowAt.Y, NowAt.Z);
									}
								}
								Shields.Free();
							}
						}
						// the shield finds its mission by this GUID (GetMissionFromGuid) before it picks the scale; a mission loaded without a world record may have none
						static auto GuidOffset = Missions.at(i)->GetOffset("MissionGuid", false);
						if (GuidOffset >= 0)
						{
							const uint32* Guid = (const uint32*)(__int64(Missions.at(i)) + GuidOffset);
							char Text[64];
							snprintf(Text, sizeof(Text), ", mission guid %08X-%08X-%08X-%08X", Guid[0], Guid[1], Guid[2], Guid[3]);
							Answer += Text;
						}
					}
					Missions.Free();
				}

				// The deployed storm shield: SpawnCoreAndConsoles spawns BP_StormShield_Core_New at the egg's TestSpawnLoc and its four consoles
				// (BP_Outpost_ControlPanel_New) at the core's ControlPanelSpawnLoc_1..4, all read through component world transforms, which have
				// read as the world origin on this server for the egg's particle components. Where they stand and whether the server collides with
				// them is logged; a console that landed far from its core (the origin) is put on its spawn point.
				{
					static auto CoreClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_StormShield_Core_New.BP_StormShield_Core_New_C");
					static auto PanelClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_Outpost_ControlPanel_New.BP_Outpost_ControlPanel_New_C");
					static auto GetCollisionFn = FindObject<UFunction>(L"/Script/Engine.PrimitiveComponent.GetCollisionEnabled");
					auto Describe = [](AActor* Actor) -> std::string
					{
						const FVector At = Actor->GetActorLocation();
						std::string Text = Actor->GetName() + " at (" + std::to_string((int)At.X) + ", " + std::to_string((int)At.Y) + ", " + std::to_string((int)At.Z) + ")";
						UObject* Root = Actor->Get<UObject*>("RootComponent");
						if (Root && GetCollisionFn)
						{
							uint8_t Params[16] = {};
							Root->ProcessEvent(GetCollisionFn, Params);
							Text += " collision " + std::to_string((int)Params[0]); // ECollisionEnabled: 0 none, 1 query, 2 physics, 3 both
						}
						return Text;
					};

					// the egg (the core before it is deployed): its root is the collision cylinder SM_GateCollisionCyl, yet the player walked through it (2026-11-08)
					static auto EggPropClass = FindObject<UClass>(L"/Game/Missions/Primary/Outpost/Props/BP_StormShield_Core_Egg.BP_StormShield_Core_Egg_C");
					if (EggPropClass)
					{
						auto Eggs = UGameplayStatics::GetAllActorsOfClass(GetWorld(), EggPropClass);
						for (int i = 0; i < Eggs.Num(); ++i)
						{
							Answer += ", egg " + Describe(Eggs.at(i));
							static int EnableCollisionOffset = Eggs.at(i)->GetOffset("bActorEnableCollision", false);
							if (EnableCollisionOffset >= 0)
								Answer += Eggs.at(i)->ReadBitfieldValue(EnableCollisionOffset, GetFieldMask(Eggs.at(i)->GetProperty("bActorEnableCollision"))) ? " actor collision on" : " actor collision OFF";
						}
						Eggs.Free();
					}

					AActor* Core = nullptr;
					if (CoreClass)
					{
						auto Cores = UGameplayStatics::GetAllActorsOfClass(GetWorld(), CoreClass);
						for (int i = 0; i < Cores.Num(); ++i)
						{
							if (!Core) Core = Cores.at(i);
							Answer += ", deployed core " + Describe(Cores.at(i));
						}
						Cores.Free();
					}
					if (PanelClass)
					{
						auto Panels = UGameplayStatics::GetAllActorsOfClass(GetWorld(), PanelClass);
						for (int i = 0; i < Panels.Num(); ++i)
						{
							AActor* Panel = Panels.at(i);
							Answer += ", console " + Describe(Panel);
							if (!Core || i >= 4) continue;

							const FVector PanelAt = Panel->GetActorLocation();
							const FVector CoreAt = Core->GetActorLocation();
							if (sqrtf((PanelAt.X - CoreAt.X) * (PanelAt.X - CoreAt.X) + (PanelAt.Y - CoreAt.Y) * (PanelAt.Y - CoreAt.Y)) < 3000.0f) continue;

							// spawned in the order 1..4 by SpawnCoreAndConsoles
							const std::string SpotName = "ControlPanelSpawnLoc_" + std::to_string(i + 1);
							const int SpotOffset = Core->GetOffset(SpotName, false);
							UObject* Spot = SpotOffset >= 0 ? Core->Get<UObject*>(SpotOffset) : nullptr;
							if (!Spot) continue;
							static int RelativeLocationOffset = Spot->GetOffset("RelativeLocation", false);
							static int RelativeRotationOffset = Spot->GetOffset("RelativeRotation", false);
							const FVector Relative = Spot->Get<FVector>(RelativeLocationOffset);
							const FRotator CoreRotation = Core->GetActorRotation();
							const double Yaw = CoreRotation.Yaw * 3.14159265358979 / 180.0;
							const FVector Target(CoreAt.X + (float)(Relative.X * cos(Yaw) - Relative.Y * sin(Yaw)), CoreAt.Y + (float)(Relative.X * sin(Yaw) + Relative.Y * cos(Yaw)), CoreAt.Z + Relative.Z);
							FRotator TargetRotation = CoreRotation;
							TargetRotation.Yaw += Spot->Get<FRotator>(RelativeRotationOffset).Yaw;

							static auto SetFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_SetActorLocationAndRotation");
							if (!SetFn) continue;
							static const char* SetPath = "/Script/Engine.Actor.K2_SetActorLocationAndRotation";
							static int NewLocationOffset = FindOffsetStruct(SetPath, "NewLocation");
							static int NewRotationOffset = FindOffsetStruct(SetPath, "NewRotation");
							static int SweepOffset = FindOffsetStruct(SetPath, "bSweep");
							static int TeleportOffset = FindOffsetStruct(SetPath, "bTeleport");
							std::vector<uint8_t> Params(SetFn->GetPropertiesSize() + 64, 0);
							*(FVector*)(Params.data() + NewLocationOffset) = Target;
							*(FRotator*)(Params.data() + NewRotationOffset) = TargetRotation;
							Params[SweepOffset] = 0;
							Params[TeleportOffset] = 1;
							Panel->ProcessEvent(SetFn, Params.data());
							Panel->ForceNetUpdate();
							const FVector NowAt = Panel->GetActorLocation();
							LOG_INFO(LogDev, "Zone: console {} stood at ({:.0f}, {:.0f}, {:.0f}), far from the core {}: moved to {} ({:.0f}, {:.0f}, {:.0f}), now at ({:.0f}, {:.0f}, {:.0f})", Panel->GetName(),
								PanelAt.X, PanelAt.Y, PanelAt.Z, Core->GetName(), SpotName, Target.X, Target.Y, Target.Z, NowAt.X, NowAt.Y, NowAt.Z);
						}
						Panels.Free();
					}
				}

				static std::string LastAnswer;
				if (Answer != LastAnswer) { LOG_INFO(LogDev, "{}", Answer); LastAnswer = Answer; }
			}
		}

		// when the lobby state goes away (or is replaced), say so: the login rebuilds it, this tells what took it
		if (gLobbyHost)
		{
			static UObject* LastLobbyState = nullptr;
			UObject* LobbyState = LobbyStateOf(gLobbyHost);
			if (LobbyState != LastLobbyState)
			{
				static auto MatchStateOffset = GetWorld() && GetWorld()->Get<UObject*>("AuthorityGameMode") ? GetWorld()->Get<UObject*>("AuthorityGameMode")->GetOffset("MatchState", false) : -1;
				UObject* GameMode = GetWorld() ? GetWorld()->Get<UObject*>("AuthorityGameMode") : nullptr;
				const std::string MatchState = GameMode && MatchStateOffset >= 0 ? GameMode->Get<FName>(MatchStateOffset).ToString() : "?";
				LOG_INFO(LogDev, "Zone matchmaking: the lobby host's lobby state is now {} (was {}), match state {}", LobbyState ? LobbyState->GetName() : "NONE",
					LastLobbyState ? "set" : "none", MatchState);
				LastLobbyState = LobbyState;
			}
		}

		if (gProceedPending && Now >= gProceedAt)
		{
			gProceedPending = false;
			static void (*AllowedToProceed)(UObject*) = decltype(AllowedToProceed)(ZoneFinder::AllowedToProceed());
			LOG_INFO(LogDev, "Zone matchmaking: telling the reserved party it may proceed to the Zone{}", AllowedToProceed ? "" : " (NOT: AllowedToProceed was not found)");
			if (AllowedToProceed) AllowedToProceed(gPartyHost);
		}

		if (gLobbyStartPending && Now >= gLobbyStartAt && gLobbyHost)
		{
			gLobbyStartPending = false;
			UObject* LobbyState = LobbyStateOf(gLobbyHost);
			const bool bStarted = IsLobbyStarted(LobbyState);
			static void (*StartLobby)(UObject*) = decltype(StartLobby)(ZoneFinder::StartLobby());
			if (LobbyState && !bStarted && StartLobby)
				StartLobby(LobbyState);
			LOG_INFO(LogDev, "Zone matchmaking: lobby {} (bLobbyStarted now {})", bStarted ? "had started by itself" : StartLobby ? "started" : "NOT started (StartLobby was not found)",
				IsLobbyStarted(LobbyState));

			gJoinGamePending = true;
			gJoinGameAt = Now + std::chrono::seconds(2);
		}

		if (!gLobbyPlayerStates.empty() && Now >= gNextLobbyReplication && gLobbyHost && gBeaconHost)
		{
			gNextLobbyReplication = Now + std::chrono::milliseconds(100);

			static auto NetDriverOffset = gBeaconHost->GetOffset("NetDriver");
			static auto LobbyClientClass = FindObject<UClass>(L"/Script/Lobby.LobbyBeaconClient");
			auto Driver = (UNetDriver*)gBeaconHost->Get<UObject*>(NetDriverOffset);
			AActor* LobbyState = (AActor*)LobbyStateOf(gLobbyHost);

			if (Driver && LobbyState && LobbyClientClass)
			{
				static int Logged = 0;
				auto& Connections = Driver->GetClientConnections();
				for (int i = 0; i < Connections.Num(); ++i)
				{
					UNetConnection* Connection = Connections.at(i);
					if (!Connection) continue;
					AActor* Owner = Connection->GetOwningActor();
					if (!Owner || !Owner->IsA(LobbyClientClass)) continue;

					// the client checks its own beacon client's LobbyState (replicated, set in ALobbyBeaconHost::HandlePlayerLogin) and that
					// state's bWorldRecordLoaded (AFortLobbyBeaconClient::IsWorldRecordLoaded)
					static auto ClientLobbyStateOffset = Owner->GetOffset("LobbyState", false);
					if (ClientLobbyStateOffset < 0) continue;
					UObject*& ClientLobbyState = Owner->Get<UObject*>(ClientLobbyStateOffset);
					const bool bHadState = ClientLobbyState == LobbyState;
					if (!ClientLobbyState) ClientLobbyState = LobbyState;
					static auto WorldRecordOffset = LobbyState->GetOffset("bWorldRecordLoaded");

					bool bOk = ZoneReplicateBeaconActor(LobbyState, Connection);
					for (UObject* PlayerState : gLobbyPlayerStates) bOk = ZoneReplicateBeaconActor((AActor*)PlayerState, Connection) && bOk;
					bOk = ZoneReplicateBeaconActor(Owner, Connection) && bOk;

					if (Logged < 3)
					{
						++Logged;
						LOG_INFO(LogDev, "Zone matchmaking: replicating the lobby to {} ({}; its LobbyState at {:x} {}, bWorldRecordLoaded at {:x} = {})", Owner->GetName(),
							bOk ? "ok" : "a channel could not be opened", ClientLobbyStateOffset, bHadState ? "was set" : "was NOT set, set now", WorldRecordOffset,
							WorldRecordOffset >= 0 ? (int)*(uint8*)(__int64(LobbyState) + WorldRecordOffset) : -1);
					}
				}
			}
		}

		if (gJoinGamePending && Now >= gJoinGameAt)
		{
			gJoinGamePending = false;
			static auto LobbyClientClass = FindObject<UClass>(L"/Script/Lobby.LobbyBeaconClient");
			static auto ClientJoinGameFn = FindObject<UFunction>(L"/Script/Lobby.LobbyBeaconClient.ClientJoinGame");
			if (LobbyClientClass && ClientJoinGameFn)
			{
				auto Clients = UGameplayStatics::GetAllActorsOfClass(GetWorld(), LobbyClientClass);
				for (int i = 0; i < Clients.Num(); ++i)
				{
					UObject* Client = (UObject*)Clients.at(i);
					if (!Client) continue;
					char Params[16] = {};
					Client->ProcessEvent(ClientJoinGameFn, Params);
					LOG_INFO(LogDev, "Zone matchmaking: told {} to join the game", Client->GetName());
				}
				if (Clients.Num() == 0) LOG_WARN(LogDev, "Zone matchmaking: no lobby clients to send to the game");
				Clients.Free();
			}
			else
			{
				LOG_WARN(LogDev, "Zone matchmaking: LobbyBeaconClient.ClientJoinGame not found");
			}
		}

		if (Now >= gNextHeartbeat)
		{
			gNextHeartbeat = Now + std::chrono::seconds(10);
			SendHeartbeat(gClosed ? "closed" : gReserved ? "reserved" : "open");
		}
	}

	bool IsBeaconNetDriver(UObject* NetDriver)
	{
		if (!gBeaconHost || !NetDriver) return false;
		static auto NetDriverOffset = gBeaconHost->GetOffset("NetDriver");
		return NetDriverOffset >= 0 && gBeaconHost->Get<UObject*>(NetDriverOffset) == NetDriver;
	}

	void SetGamePort(int Port)
	{
		gGamePort = Port;
	}

	void CloseMatchmaking()
	{
		if (!gStarted || gClosed) return;
		gClosed = true;
		gProceedPending = false;

		if (gBeaconHost)
		{
			static void (*PauseBeaconRequests)(UObject*, bool) = decltype(PauseBeaconRequests)(Addresses::PauseBeaconRequests);
			PauseBeaconRequests(gBeaconHost, true);
		}

		LOG_INFO(LogDev, "Zone matchmaking: the mission is over, no more reservations");
		SendHeartbeat("closed");
	}
}
