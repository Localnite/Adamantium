#include "ZoneFinder.h"
#include "inc.h"
#include "log.h"
#include <memcury.h>

namespace ZoneFinder
{
	namespace
	{
		uint64_t Rva(uint64_t Address) { return Address ? Address - (uint64_t)GetModuleHandleW(0) : 0; }

		// a function or instruction found by its signature, plus an optional byte offset into the pattern
		uint64_t Find(const char* Name, const char* Signature, int Offset = 0)
		{
			if (!Is730())
			{
				LOG_WARN(LogDev, "ZoneFinder: {} skipped, its signature is for 7.30 (Engine_Version {})", Name, Engine_Version);
				return 0;
			}

			auto Address = Memcury::Scanner::FindPattern(Signature, false).Get();
			if (!Address)
			{
				LOG_WARN(LogDev, "ZoneFinder: {} not found", Name);
				return 0;
			}

			Address += Offset;
			LOG_INFO(LogDev, "ZoneFinder: {} at exe+0x{:x}", Name, Rva(Address));
			return Address;
		}

		// where the call (E8 rel32) at that instruction goes
		uint64_t CallTarget(const char* Name, uint64_t CallSite)
		{
			if (!CallSite) return 0;
			auto Target = Memcury::Scanner(CallSite).RelativeOffset(1).Get();
			LOG_INFO(LogDev, "ZoneFinder: {} at exe+0x{:x} (called from exe+0x{:x})", Name, Rva(Target), Rva(CallSite));
			return Target;
		}

		// a 32-bit displacement the game's own instruction uses
		int ReadDisplacement(const char* Name, uint64_t Instruction, int FieldOffset)
		{
			if (!Instruction) return -1;
			const int Value = *(int32_t*)(Instruction + FieldOffset);
			LOG_INFO(LogDev, "ZoneFinder: {} is 0x{:x} (read at exe+0x{:x})", Name, Value, Rva(Instruction));
			return Value;
		}
	}

	bool Is730() { return Engine_Version >= 422 && Engine_Version <= 424; }

	// ---- matchmaking / lobby beacons
	uint64_t DoesSessionMatch() { static auto A = Find("DoesSessionMatch", "48 89 5C 24 10 55 56 57 48 83 EC 40 33 ED 48 8B F2"); return A; }
	uint64_t AddEmptyServerReservation() { static auto A = Find("AddEmptyServerReservationRequest", "40 55 53 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 48 83 B9 D0 03 00 00 00"); return A; }
	uint64_t AllowedToProceed()
	{
		static auto A = Find("AllowedToProceed", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 80 3D ? ? ? ? 06 48 8B F9 72 2C 48 8B 05 ? ? ? ? 4C 8D 44 24 40 48 89 44 24 40 41 B9 06 00 00 00 48 8D 05 ? ? ? ? 33 D2 33 C9 48 89 44 24 20 E8 ? ? ? ? 48 8B 9F 48 03 00 00 48 63 87 50 03 00 00 48 8D 34 C3 48 3B DE 74 47");
		return A;
	}
	uint64_t ReceivedNetworkEncryptionToken() { static auto A = Find("ReceivedNetworkEncryptionToken", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 A0"); return A; }
	uint64_t LobbyDoesSessionMatch() { static auto A = Find("LobbyDoesSessionMatch", "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 89 6C 24 58"); return A; }
	uint64_t LobbyHandlePlayerLogin() { static auto A = Find("LobbyHandlePlayerLogin", "48 8B C4 55 41 55 41 56 41 57 48 8D 68 C8 48 81 EC 18 01 00 00"); return A; }
	uint64_t StartLobby() { static auto A = Find("StartLobby", "40 53 48 83 EC 20 80 B9 18 01 00 00 03 48 8B D9 75 64"); return A; }

	// ---- zone world, missions, player
	uint64_t InitMissionManagerRecord() { static auto A = Find("InitMissionManagerRecord", "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 85 D2 74 24"); return A; }
	uint64_t ActivateAIDirector() { static auto A = Find("ActivateAIDirector", "40 53 48 83 EC 20 8B 81 80 0C 00 00 48 8B D9 A8 01"); return A; }
	uint64_t NotifyWorldReady() { static auto A = Find("NotifyWorldReady", "48 8B C4 41 55 48 83 EC 40 83 B9 90 04 00 00 00"); return A; }
	uint64_t OnRetrieveZoneInformationComplete() { static auto A = Find("OnRetrieveZoneInformationComplete", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 83 B9 E0 07 00 00 00"); return A; }
	uint64_t OnRetrieveTheaterInformationComplete() { static auto A = Find("OnRetrieveTheaterInformationComplete", "40 53 48 83 EC 30 83 B9 E0 07 00 00 00 7E 66 48 8B 99 D8 07 00 00"); return A; }
	uint64_t CreateNewWorldRecord() { static auto A = Find("CreateNewWorldRecord", "48 89 5C 24 20 56 41 56 41 57 48 83 EC 50 4C 8B F2"); return A; }
	uint64_t OnInitWorldRecordComplete() { static auto A = Find("OnInitWorldRecordComplete", "48 89 5C 24 10 57 48 83 EC 30 48 8B D9 E8 ? ? ? ? 48 8B 03 48 8B CB FF 90 48 0C 00 00"); return A; }
	uint64_t GetPawnGoalSelectionCriteria() { static auto A = Find("GetPawnGoalSelectionCriteria", "48 89 5C 24 18 57 48 83 EC 30 65 48 8B 04 25 58 00 00 00"); return A; }
	uint64_t AddItemToQuickBars() { static auto A = Find("AddItemToQuickBars", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 83 B9 E8 13 00 00 00"); return A; }
	uint64_t SetHasFinishedLoading() { static auto A = Find("SetHasFinishedLoading", "48 8B C4 53 48 81 EC 80 00 00 00 80 3D ? ? ? ? 04"); return A; }
	uint64_t GetOrInitializeHero() { static auto A = Find("GetOrInitializeHero", "48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70"); return A; }

	// ---- AI crash patches
	uint64_t GoalCriteriaCompareSite() { static auto A = Find("GetPawnGoalSelectionCriteria compare", "48 3B DF 0F 84 ? ? ? ? 66 66 0F 1F 84 00 00 00 00 00 48 8B 43 60"); return A; }
	uint64_t AIPawnBeginPlayAssignSite() { static auto A = Find("AFortAIPawn::BeginPlay assignment branch", "74 0B 48 8B D7 48 8B C8 E8 ? ? ? ? 48 8B 8F 28 05 00 00"); return A; }
	uint64_t AssignmentRegistration() { static auto A = Find("AssignmentRegistration", "40 55 53 56 48 8D 6C 24 B9 48 81 EC D0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 37"); return A; }
	uint64_t MakeGoalSelectionQuery() { static auto A = Find("MakeGoalSelectionQuery", "48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8B EC 48 83 EC 60 4D 8B F0"); return A; }

	// ---- AI manager lookups
	namespace
	{
		// each pattern starts at the call instruction
		const char* GoalManagerSitePatterns[] = {
			"E8 ? ? ? ? 48 85 C0 74 0B 48 8B D3 48 8B C8 E8 ? ? ? ? 8B D7",
			"E8 ? ? ? ? 48 85 C0 74 0B 48 8B D3 48 8B C8 E8 ? ? ? ? 33 D2",
			"E8 ? ? ? ? 48 85 C0 74 0B 48 8B D7 48 8B C8 E8 ? ? ? ? 48 8B 8F 28 05 00 00",
			"E8 ? ? ? ? 48 85 C0 74 10 48 8B D3 48 8B C8 48 83 C4 20 5B E9 ? ? ? ? 48 83 C4 20 5B C3 8B 45 10",
			"E8 ? ? ? ? 48 85 C0 74 10 48 8B D3 48 8B C8 48 83 C4 20 5B E9 ? ? ? ? 48 83 C4 20 5B C3 8B 05 ? ? ? ?",
			"E8 ? ? ? ? 48 85 C0 74 10 48 8B C8 E8 ? ? ? ? 48 8B D8 48 85 C0 75 65",
			"E8 ? ? ? ? 48 85 C0 74 10 48 8B C8 E8 ? ? ? ? 48 8B D8 48 85 C0 75 1A",
		};
		// the next four are copies of one inlined function; only the bytes after their padding tell them apart
		const char* ContextGoalManagerSitePatterns[] = {
			"E8 ? ? ? ? 48 85 C0 74 5A 33 C9 C6 44 24 20 01",
			"E8 ? ? ? ? 48 85 C0 74 48 4C 8B C3 C6 45 DF 01",
			"E8 ? ? ? ? 48 8B F8 48 85 C0 74 20 48 8B 13 48 8B CB FF 52 10",
			"E8 ? ? ? ? 48 85 C0 74 1E 80 3B 01 74 03 C6 03 01 4D 8B 0E 4C 8B C3 48 8B D7 48 89 74 24 20 48 8B C8 E8 ? ? ? ? 48 8B 4B 18 48 85 C9 74 05 E8 ? ? ? ? 48 8B 4B 08 48 85 C9 74 05 E8 ? ? ? ? 48 8B 5C 24 40 48 8B 74 24 48 48 8B 7C 24 50 48 83 C4 30 41 5E C3 CC 8A E8",
			"E8 ? ? ? ? 48 85 C0 74 1E 80 3B 01 74 03 C6 03 01 4D 8B 0E 4C 8B C3 48 8B D7 48 89 74 24 20 48 8B C8 E8 ? ? ? ? 48 8B 4B 18 48 85 C9 74 05 E8 ? ? ? ? 48 8B 4B 08 48 85 C9 74 05 E8 ? ? ? ? 48 8B 5C 24 40 48 8B 74 24 48 48 8B 7C 24 50 48 83 C4 30 41 5E C3 90",
			"E8 ? ? ? ? 48 85 C0 74 1E 80 3B 01 74 03 C6 03 01 4D 8B 0E 4C 8B C3 48 8B D7 48 89 74 24 20 48 8B C8 E8 ? ? ? ? 48 8B 4B 18 48 85 C9 74 05 E8 ? ? ? ? 48 8B 4B 08 48 85 C9 74 05 E8 ? ? ? ? 48 8B 5C 24 40 48 8B 74 24 48 48 8B 7C 24 50 48 83 C4 30 41 5E C3 CC 48 8B 0D ? ? ? ?",
			"E8 ? ? ? ? 48 85 C0 74 1E 80 3B 01 74 03 C6 03 01 4D 8B 0E 4C 8B C3 48 8B D7 48 89 74 24 20 48 8B C8 E8 ? ? ? ? 48 8B 4B 18 48 85 C9 74 05 E8 ? ? ? ? 48 8B 4B 08 48 85 C9 74 05 E8 ? ? ? ? 48 8B 5C 24 40 48 8B 74 24 48 48 8B 7C 24 50 48 83 C4 30 41 5E C3 CC A8 44",
		};
		const char* ContextDirectorSitePatterns[] = {
			"E8 ? ? ? ? 48 85 C0 74 12 8B 88 E8 0D 00 00",
			"E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B D7 48 8B C8 E8 ? ? ? ? 84 C0",
			"E8 ? ? ? ? 48 8B B7 60 03 00 00 4C 8B F0 48 85 F6",
			"E8 ? ? ? ? 48 8B F8 48 85 C0 74 45 48 8B 53 08",
			"E8 ? ? ? ? 48 8B F0 48 85 C0 0F 84 ? ? ? ? 48 8B 53 08",
			"E8 ? ? ? ? 48 85 C0 74 11 F6 80 80 0C 00 00 02",
			"E8 ? ? ? ? 48 85 C0 74 08 48 8B C8 E8 ? ? ? ? C6 86 3C 08 00 00 00",
		};

		template <int N>
		int FindSites(const char* What, const char* (&Patterns)[N], uint64_t (&Out)[N])
		{
			int Found = 0;
			for (int i = 0; i < N; ++i)
			{
				Out[i] = Is730() ? Memcury::Scanner::FindPattern(Patterns[i], false).Get() : 0;
				Found += Out[i] ? 1 : 0;
			}
			LOG_INFO(LogDev, "ZoneFinder: {} {} of {} call sites found", What, Found, N);
			return N;
		}
	}

	int GoalManagerSites(const uint64_t*& Out)
	{
		static uint64_t Sites[7] = {};
		static int N = FindSites("goal manager", GoalManagerSitePatterns, Sites);
		Out = Sites;
		return N;
	}

	int ContextGoalManagerSites(const uint64_t*& Out)
	{
		static uint64_t Sites[7] = {};
		static int N = FindSites("context goal manager", ContextGoalManagerSitePatterns, Sites);
		Out = Sites;
		return N;
	}

	int ContextDirectorSites(const uint64_t*& Out)
	{
		static uint64_t Sites[7] = {};
		static int N = FindSites("context director", ContextDirectorSitePatterns, Sites);
		Out = Sites;
		return N;
	}

	uint64_t AIManagerLookupStub()
	{
		// the first goal manager site calls it
		static auto A = CallTarget("AIManagerLookupStub", Find("AIManagerLookupStub call", GoalManagerSitePatterns[0]));
		return A;
	}

	// ---- replication graph: the policy lookup's caller passes the graph's map with lea rcx, [rbx+map]
	namespace { uint64_t PolicyCaller() { static auto A = Find("ClassRepNodePolicies caller", "48 8D 8B ? ? ? ? 49 8B D2 E8 ? ? ? ?"); return A; } }
	uint64_t ClassRepNodePolicyLookup() { static auto A = CallTarget("ClassRepNodePolicyLookup", PolicyCaller() ? PolicyCaller() + 10 : 0); return A; }
	int ClassRepNodePoliciesOffset() { static int A = ReadDisplacement("ClassRepNodePolicies offset", PolicyCaller(), 3); return A; }

	// ---- curve warning verbosity: cmp byte ptr [rip+verbosity], 3 (ELogVerbosity::Warning) in FCurveTableRowHandle::Eval
	uint8_t* CurveTableWarningVerbosity()
	{
		static auto A = []() -> uint8_t*
		{
			auto Site = Find("curve table warning check", "80 3D ? ? ? ? 03 72 7C 48 8D 1D ? ? ? ?");
			if (!Site) return nullptr;
			auto Byte = (uint8_t*)Memcury::Scanner(Site).RelativeOffset(2, 1).Get();
			LOG_INFO(LogDev, "ZoneFinder: curve table warning verbosity byte at exe+0x{:x}", Rva((uint64_t)Byte));
			return Byte;
		}();
		return A;
	}

	// ---- navigation
	uint64_t PrepareBoundsAndUnlock() { static auto A = Find("PrepareBoundsAndUnlock", "4C 8B DC 49 89 5B 20 56 48 83 EC 60 8B 02 48 8B F2"); return A; }
	uint64_t AthenaPrepareBoundsAndUnlock() { static auto A = Find("AthenaPrepareBoundsAndUnlock", "48 89 5C 24 08 57 48 83 EC 20 80 B9 BC 05 00 00 00"); return A; }
	int NavSystemBoundsOffset()
	{
		// its first store: mov [rcx+bounds], eax (the bounds' unique id)
		static int A = ReadDisplacement("nav system bounds offset", Find("nav system bounds store", "89 81 ? ? ? ? 0F 10 42 04 49 89 7B 18"), 2);
		return A;
	}

	// ---- net driver fallbacks
	uint64_t CreateChannelByName() { static auto A = Find("CreateChannelByName", "40 53 55 56 57 41 54 41 57 48 83 EC 68 48 8B 01"); return A; }
	uint64_t SetChannelActor() { static auto A = Find("SetChannelActor", "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 45 33 E4"); return A; }
	uint64_t ReplicateActor() { static auto A = Find("ReplicateActor", "48 8B C4 48 89 48 08 55 53 48 8D A8 38 FE FF FF"); return A; }
}
