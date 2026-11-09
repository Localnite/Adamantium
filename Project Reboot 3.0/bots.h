#pragma once
#include <map>
#include <unordered_set>

#include "FortGameModeAthena.h"
#include "OnlineReplStructs.h"
#include "FortAthenaAIBotController.h"
#include "BuildingContainer.h"
#include "botnames.h"
#include "BotRegistry.h"
#include "BotBrain.h"
#include "prodopts.h"
#include "RebootConfig.h"

// ---- crash diagnostics: a vectored handler that writes where we died to reboot.log (the minidump from Wine has no usable callstack) ----
inline const char* gBotBreadcrumb = "none";
inline int gBotBreadcrumbId = -1;
inline int gNextBotId = 0;

#define BOT_CRUMB(Text, Id) do { gBotBreadcrumb = Text; gBotBreadcrumbId = Id; } while (0)

inline std::string BotModuleOffset(uint64_t Address)
{
	HMODULE Module = nullptr;

	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)Address, &Module) || !Module)
		return "?";

	wchar_t Path[MAX_PATH]{};
	GetModuleFileNameW(Module, Path, MAX_PATH);
	std::wstring Name(Path);
	const auto Slash = Name.find_last_of(L"\\/");

	if (Slash != std::wstring::npos)
		Name = Name.substr(Slash + 1);

	return std::format("{}+0x{:x}", std::string(Name.begin(), Name.end()), Address - (uint64_t)Module);
}

// Plain function (no C++ objects) so __try is allowed. Collects values on the stack that point into a loaded module.
inline int BotScanStack(uint64_t Rsp, uint64_t* Out, int Max)
{
	int Count = 0;

	__try
	{
		for (int i = 0; i < 1024 && Count < Max; ++i)
		{
			const uint64_t Value = ((uint64_t*)Rsp)[i];
			HMODULE Module = nullptr;

			if (Value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)Value, &Module) && Module)
				Out[Count++] = Value;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	return Count;
}

// Plain function (no C++ objects) so __try is allowed. True when Value looks like a live UObject: readable, and its class (+0x10) is readable
// with a class of its own; the class goes to Class.
inline bool BotProbeObject(uint64_t Value, uint64_t* Class)
{
	if (Value < 0x10000 || (Value & 7)) return false;

	__try
	{
		const uint64_t VTable = *(uint64_t*)Value;
		const uint64_t ObjectClass = *(uint64_t*)(Value + 0x10);
		if (!VTable || !ObjectClass || (ObjectClass & 7)) return false;
		const uint64_t ClassClass = *(uint64_t*)(ObjectClass + 0x10);
		if (!ClassClass) return false;
		HMODULE Module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)VTable, &Module)) return false;
		*Class = ObjectClass;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// the registers at the crash, and for the ones that point at a UObject its name and class: which object a crash in game code was working on
inline void BotLogRegisters(PCONTEXT Context)
{
	const std::pair<const char*, uint64_t> Registers[] = { {"rax", Context->Rax}, {"rbx", Context->Rbx}, {"rcx", Context->Rcx}, {"rdx", Context->Rdx}, {"rsi", Context->Rsi},
		{"rdi", Context->Rdi}, {"r8", Context->R8}, {"r9", Context->R9}, {"r12", Context->R12}, {"r13", Context->R13}, {"r14", Context->R14}, {"r15", Context->R15} };

	for (const auto& [Name, Value] : Registers)
	{
		uint64_t Class = 0;
		if (BotProbeObject(Value, &Class))
		{
			LOG_ERROR(LogBots, "CRASHLOG   {} = 0x{:x} {} (class {})", Name, Value, ((UObject*)Value)->GetName(), ((UObject*)Class)->GetName());
		}
		else
		{
			LOG_ERROR(LogBots, "CRASHLOG   {} = 0x{:x}", Name, Value);
		}
	}
}

inline std::atomic<DWORD> gIgnoreFaultsThread = 0; // the hang watchdog while it copies another thread's stack (it reads past the end on purpose)

inline LONG CALLBACK BotVectoredExceptionHandler(PEXCEPTION_POINTERS Info)
{
	const DWORD Code = Info->ExceptionRecord->ExceptionCode;

	if (gIgnoreFaultsThread.load() == GetCurrentThreadId())
		return EXCEPTION_CONTINUE_SEARCH;

	// access violation, illegal instruction, int divide by zero, stack overflow
	if (Code != 0xC0000005 && Code != 0xC000001D && Code != 0xC0000094 && Code != 0xC00000FD)
		return EXCEPTION_CONTINUE_SEARCH;

	static int Reports = 0;

	if (Reports++ >= 40)
		return EXCEPTION_CONTINUE_SEARCH;

	const uint64_t Rip = Info->ContextRecord->Rip;
	const uint64_t Rsp = Info->ContextRecord->Rsp;

	LOG_ERROR(LogBots, "CRASHLOG exception 0x{:x} rip=0x{:x} ({}) info0={} info1=0x{:x} breadcrumb='{}' bot={}", Code, Rip, BotModuleOffset(Rip),
		(uint64_t)Info->ExceptionRecord->ExceptionInformation[0], (uint64_t)Info->ExceptionRecord->ExceptionInformation[1], gBotBreadcrumb, gBotBreadcrumbId);

	// only for crashes in the game's own code (a probe of ours that faults on purpose would flood the log otherwise)
	if (BotModuleOffset(Rip).rfind("FortniteClient", 0) == 0) BotLogRegisters(Info->ContextRecord);

	uint64_t Frames[48];
	const int NumFrames = BotScanStack(Rsp, Frames, 48);

	for (int i = 0; i < NumFrames; ++i)
		LOG_ERROR(LogBots, "CRASHLOG   stack[{}] {}", i, BotModuleOffset(Frames[i]));

	spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });

	return EXCEPTION_CONTINUE_SEARCH;
}

inline void InstallBotCrashLogger()
{
	static bool bInstalled = false;

	if (bInstalled)
		return;

	bInstalled = true;
	AddVectoredExceptionHandler(1, BotVectoredExceptionHandler);
	LOG_INFO(LogBots, "Bot crash logger installed");
}

// The executable on disk is encrypted; in memory it is plain. Writes the loaded image to exe_image.bin (next to reboot.log) so it can be read
// in a disassembler (raw file offset == RVA).
inline void DumpExeImage()
{
	const auto Base = (uintptr_t)GetModuleHandleW(nullptr);
	const auto Dos = (IMAGE_DOS_HEADER*)Base;
	const auto Nt = (IMAGE_NT_HEADERS64*)(Base + Dos->e_lfanew);
	const size_t ImageSize = Nt->OptionalHeader.SizeOfImage;
	FILE* Out = nullptr;
	fopen_s(&Out, "exe_image.bin", "wb");
	size_t Written = 0, Skipped = 0;

	if (Out)
	{
		std::vector<uint8_t> Zero(0x10000, 0);

		for (size_t At = 0; At < ImageSize;)
		{
			MEMORY_BASIC_INFORMATION Info{};
			size_t Chunk = 0x10000;

			if (VirtualQuery((void*)(Base + At), &Info, sizeof(Info)) && Info.State == MEM_COMMIT && !(Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			{
				Chunk = (std::min)((size_t)(Info.BaseAddress) + Info.RegionSize - (Base + At), ImageSize - At);
				fwrite((void*)(Base + At), 1, Chunk, Out);
				Written += Chunk;
			}
			else
			{
				const size_t Gap = (std::min)(Chunk, ImageSize - At);
				fwrite(Zero.data(), 1, Gap, Out);
				Chunk = Gap;
				Skipped += Gap;
			}

			At += Chunk;
		}

		fclose(Out);
	}

	LOG_INFO(LogBots, "NavDump: wrote the loaded image (base {:x}, size {:x}): {} bytes copied, {} bytes of unreadable gaps", Base, ImageSize, Written, Skipped);
}

// ---- hang diagnostics: a watchdog that writes where the game thread is stuck when it stops ticking ----
// A hang leaves no dump and no CRASHLOG (the backend just kills the server for missing heartbeats), so when TickFlush has not run for
// HangSeconds this thread suspends the game thread a few times and logs its rip and the module addresses on its stack.
inline std::atomic<DWORD> gGameThreadId = 0;
inline std::atomic<uint64_t> gLastTickMs = 0;

// Plain function (no C++ objects) so __try is allowed. Copies the top of a (suspended) thread's stack.
inline int HangCopyStack(uint64_t Rsp, uint64_t* Out, int Max)
{
	int Count = 0;

	__try
	{
		for (; Count < Max; ++Count)
			Out[Count] = ((uint64_t*)Rsp)[Count];
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	return Count;
}

inline DWORD WINAPI HangWatchdogThread(LPVOID)
{
	constexpr uint64_t HangSeconds = 20; // a 98-bot fill blocks the game thread ~11 s on 1.10; the backend gives up after 45 s
	constexpr int Samples = 4;
	static uint64_t Stack[Samples][4096];
	uint64_t ReportedFor = 0;

	while (true)
	{
		Sleep(1000);

		const uint64_t Last = gLastTickMs.load();
		const DWORD ThreadId = gGameThreadId.load();

		if (!Last || !ThreadId || GetTickCount64() - Last < HangSeconds * 1000 || ReportedFor == Last)
			continue;

		ReportedFor = Last; // once per stall

		HANDLE Thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ThreadId);

		if (!Thread)
		{
			LOG_ERROR(LogBots, "HANGLOG game thread has not ticked for {} s, but OpenThread failed ({})", (GetTickCount64() - Last) / 1000, GetLastError());
			continue;
		}

		CONTEXT Contexts[Samples]{};
		int StackCounts[Samples]{};

		// everything that allocates (the log line formatting) happens after ResumeThread: the game thread may be suspended holding the heap lock
		for (int s = 0; s < Samples; ++s)
		{
			if (s) Sleep(300);

			Contexts[s].ContextFlags = CONTEXT_FULL;

			if (SuspendThread(Thread) == (DWORD)-1)
				break;

			if (GetThreadContext(Thread, &Contexts[s]))
			{
				gIgnoreFaultsThread = GetCurrentThreadId();
				StackCounts[s] = HangCopyStack(Contexts[s].Rsp, Stack[s], 4096);
				gIgnoreFaultsThread = 0;
			}

			ResumeThread(Thread);
		}

		CloseHandle(Thread);

		LOG_ERROR(LogBots, "HANGLOG game thread (tid {}) has not ticked for {} s, breadcrumb='{}' bot={}", ThreadId, (GetTickCount64() - Last) / 1000, gBotBreadcrumb, gBotBreadcrumbId);

		for (int s = 0; s < Samples; ++s)
		{
			if (!Contexts[s].Rip)
				continue;

			LOG_ERROR(LogBots, "HANGLOG sample {} rip=0x{:x} ({})", s, Contexts[s].Rip, BotModuleOffset(Contexts[s].Rip));
		}

		// the full stack only for the first sample; the others just show how far the rip moves (a tight loop or a wider one)
		if (Contexts[0].Rip)
		{
			// raw values only: BotLogRegisters names the objects through the engine (GetName), and calling the engine from this thread
			// crashed the server (1.10, 2026-11-08 23:19, while a long bot fill set the watchdog off)
			const CONTEXT& C = Contexts[0];
			LOG_ERROR(LogBots, "HANGLOG   rax={:x} rbx={:x} rcx={:x} rdx={:x} rsi={:x} rdi={:x} r8={:x} r9={:x} r12={:x} r13={:x} r14={:x} r15={:x}",
				C.Rax, C.Rbx, C.Rcx, C.Rdx, C.Rsi, C.Rdi, C.R8, C.R9, C.R12, C.R13, C.R14, C.R15);

			int Logged = 0;

			for (int i = 0; i < StackCounts[0] && Logged < 160; ++i)
			{
				const uint64_t Value = Stack[0][i];
				HMODULE Module = nullptr;

				if (Value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)Value, &Module) && Module)
					LOG_ERROR(LogBots, "HANGLOG   stack[{}] {}", Logged++, BotModuleOffset(Value));
			}
		}

		spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });

		// the exe on disk is encrypted, so the addresses above can only be read against the loaded image
		static bool bDumped = false;

		if (!bDumped)
		{
			bDumped = true;
			DumpExeImage();
		}

		// a second look 15 s later: if the breadcrumb moved, it is slow work rather than a stuck loop
		Sleep(15000);

		if (gLastTickMs.load() == Last)
			LOG_ERROR(LogBots, "HANGLOG still not ticking after {} s, breadcrumb='{}' bot={}", (GetTickCount64() - Last) / 1000, gBotBreadcrumb, gBotBreadcrumbId);

		spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });
	}

	return 0;
}

// called from TickFlush on the game thread
inline void HangWatchdogTick()
{
	if (!gGameThreadId.load())
	{
		gGameThreadId = GetCurrentThreadId();
		gLastTickMs = GetTickCount64();
		CreateThread(nullptr, 0, HangWatchdogThread, nullptr, 0, nullptr);
		InstallBotCrashLogger();
		LOG_INFO(LogBots, "Hang watchdog started (game thread {})", GetCurrentThreadId());
	}

	gLastTickMs = GetTickCount64();
}

class BotPOI
{
	FVector CenterLocation;
	FVector Range; // this just has to be FVector2D
};

class BotPOIEncounter
{
public:
	int NumChestsSearched;
	int NumAmmoBoxesSearched;
	int NumPlayersEncountered;
};

static inline void EnableBotServerSimulation(AController* Controller, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState);

// Team numbers. The first player team is DefaultFirstTeam of the playlist (2 or 3 depending on the build; Reboot always assumed 3). The game turned down
// team 102 (a real player became team 0), and 101 is the highest that has been seen to work, so that is the top of the range whatever the playlist says.
inline constexpr int HighestAcceptedTeam = 101;

inline int PlaylistFirstTeam(UFortPlaylist* Playlist)
{
	static int Result = -1;

	if (Result != -1)
		return Result;

	int Value = 3;
	int Offset = Playlist ? Playlist->GetOffset("DefaultFirstTeam", false) : -1;

	// The game keeps one array slot per team number, 0 up to the last; with 100 teams that ends at the highest valid number. This build has 102 slots
	// (team 102 was turned down), so the 100 teams are 2-101 and the first real player is team 2, not the 3 Reboot assumes for later seasons.
	int FromArray = -1;

	if (auto GameState = Cast<AFortGameStateAthena>(GetWorld() ? GetWorld()->GetGameState() : nullptr))
	{
		if (auto Container = GameState->GetTeamsArrayContainer())
		{
			int TeamsWanted = 100;

			if (Playlist)
			{
				static auto MaxTeamCountOffset = Playlist->GetOffset("MaxTeamCount", false);

				if (MaxTeamCountOffset != -1 && Playlist->Get<int>(MaxTeamCountOffset) > 0 && Playlist->Get<int>(MaxTeamCountOffset) <= 100)
					TeamsWanted = Playlist->Get<int>(MaxTeamCountOffset);
			}

			const int Slots = Container->TeamsArray.Num();

			if (Slots - TeamsWanted >= 2 && Slots - TeamsWanted <= 3)
				FromArray = Slots - TeamsWanted;

			static int LoggedSlots = 0;

			if (++LoggedSlots <= 3)
				LOG_INFO(LogTeams, "Teams: the game keeps {} team slots for {} teams, so the first team is {}", Slots, TeamsWanted, FromArray);
		}
	}

	if (Offset != -1)
	{
		const int FromPlaylist = Playlist->Get<int>(Offset);

		if (FromPlaylist >= 2 && FromPlaylist <= 3)
			Value = FromPlaylist;
		else if (FromArray != -1)
			Value = FromArray;

		LOG_INFO(LogTeams, "Teams: the playlist says the first team is {} (using {}); highest team number used is {}", FromPlaylist, Value, HighestAcceptedTeam);
	}
	else if (Playlist)
	{
		if (FromArray != -1)
			Value = FromArray;

		LOG_INFO(LogTeams, "Teams: the playlist has no DefaultFirstTeam, using {}; highest team number used is {}", Value, HighestAcceptedTeam);
	}

	if (Playlist && FromArray != -1)
		Result = Value; // only remember it once the game could be asked

	return Value;
}

class PlayerBot
{
public:
	static inline UClass* PawnClass = nullptr;
	static inline UClass* ControllerClass = nullptr;

	AController* Controller = nullptr; // This can be 1. AFortAthenaAIBotController OR AFortPlayerControllerAthena
	bool bIsAthenaController = false;
	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerStateAthena* PlayerState = nullptr;
	BotPOIEncounter currentBotEncounter;
	int TotalPlayersEncountered;
	std::vector<BotPOI> POIsTraveled;
	float NextJumpTime = 1.0f;
	float NextLogTime = 0.0f;
	int LogCounter = 0;
	int Id = -1;
	bool bDead = false;
	BotBrainState Brain;
	float JumpAtTime = -1.0f; // when (in game seconds) to leave the bus; -1 = not scheduled yet
	bool bJumpedFromBus = false;
	FVector LastLoggedLocation;
	float DiagLastHealth = -1.0f; // 1.x death-freeze diagnostics (Bots::DeathDiag*)

	void OnPlayerEncountered()
	{
		currentBotEncounter.NumPlayersEncountered++;
		TotalPlayersEncountered++;
	}

	void MoveToNewPOI()
	{

	}

	static bool ShouldUseAIBotController()
	{
		return false;
		return Fortnite_Version >= 11 && Engine_Version < 500;
	}

	static void InitializeBotClasses()
	{
		InstallBotCrashLogger();

		static auto BlueprintGeneratedClassClass = FindObject<UClass>(L"/Script/Engine.BlueprintGeneratedClass");

		if (!ShouldUseAIBotController())
		{
			PawnClass = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

			// 1.10 runs this before the pawn blueprint is in memory, so the find gave null and no bot could ever spawn; load it then
			if (!PawnClass)
				PawnClass = LoadObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C", BlueprintGeneratedClassClass);

			ControllerClass = AFortPlayerControllerAthena::StaticClass();
		}
		else
		{
			PawnClass = LoadObject<UClass>(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C", BlueprintGeneratedClassClass);
			// ControllerClass = PawnClass->CreateDefaultObject()->GetAIControllerClass();
		}

		if (/* !ControllerClass
			|| */ !PawnClass
			)
		{
			LOG_ERROR(LogBots, "Failed to find a class for the bots!");
			return;
		}
	}

	static bool IsReadyToSpawnBot()
	{
		if (!PawnClass)
			InitializeBotClasses(); // the classes may not have been loaded when this ran at startup

		return PawnClass;
	}

	void SetupInventory()
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!ShouldUseAIBotController()) // TODO REWRITE
		{
			AFortInventory** Inventory = nullptr;

			if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
			{
				Inventory = &FortPlayerController->GetWorldInventory();
			}
			else
			{
				if (auto FortAthenaAIBotController = Cast<AFortAthenaAIBotController>(Controller))
				{
					static auto InventoryOffset = Controller->GetOffset("Inventory");
					Inventory = Controller->GetPtr<AFortInventory*>(InventoryOffset);
				}
			}

			if (!Inventory)
			{
				LOG_ERROR(LogBots, "No inventory pointer!");

				Pawn->K2_DestroyActor();
				Controller->K2_DestroyActor();
				return;
			}

			static auto FortInventoryClass = FindObject<UClass>(L"/Script/FortniteGame.FortInventory"); // AFortInventory::StaticClass()
			*Inventory = GetWorld()->SpawnActor<AFortInventory>(FortInventoryClass, FTransform{}, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AlwaysSpawn, false, Controller));

			if (!*Inventory)
			{
				LOG_ERROR(LogBots, "Failed to spawn Inventory!");

				Pawn->K2_DestroyActor();
				Controller->K2_DestroyActor();
				return;
			}

			(*Inventory)->GetInventoryType() = EFortInventoryType::World;

			if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
			{
				static auto bHasInitializedWorldInventoryOffset = FortPlayerController->GetOffset("bHasInitializedWorldInventory");
				FortPlayerController->Get<bool>(bHasInitializedWorldInventoryOffset) = true;
			}

			// if (false)
			{
				if (Inventory)
				{
					auto& StartingItems = GameMode->GetStartingItems();

					for (int i = 0; i < StartingItems.Num(); ++i)
					{
						auto& StartingItem = StartingItems.at(i, FItemAndCount::GetStructSize());

						// TODO: Check if it is FortSmartBuildingItemDefinition

						(*Inventory)->AddItem(StartingItem.GetItem(), nullptr, StartingItem.GetCount());
					}

					if (auto FortPlayerController = Cast<AFortPlayerController>(Controller))
					{
						UFortItem* PickaxeInstance = FortPlayerController->AddPickaxeToInventory();

						if (PickaxeInstance)
						{
							Brain.PickaxeDef = PickaxeInstance->GetItemEntry()->GetItemDefinition();
							Brain.PickaxeGuid = PickaxeInstance->GetItemEntry()->GetItemGuid();
							FortPlayerController->ServerExecuteInventoryItemHook(FortPlayerController, PickaxeInstance->GetItemEntry()->GetItemGuid());
						}
					}

					(*Inventory)->Update();
				}
			}
		}
	}

	void PickRandomLoadout()
	{
		auto AllHeroTypes = GetAllObjectsOfClass(FindObject<UClass>(L"/Script/FortniteGame.FortHeroType"));
		std::vector<UFortItemDefinition*> AthenaHeroTypes;

		UFortItemDefinition* HeroType = FindObject<UFortItemDefinition>(L"/Game/Athena/Heroes/HID_030_Athena_Commando_M_Halloween.HID_030_Athena_Commando_M_Halloween");

		for (int i = 0; i < AllHeroTypes.size(); ++i)
		{
			auto CurrentHeroType = (UFortItemDefinition*)AllHeroTypes.at(i);

			if (CurrentHeroType->GetPathName().starts_with("/Game/Athena/Heroes/"))
				AthenaHeroTypes.push_back(CurrentHeroType);
		}

		if (AthenaHeroTypes.size())
		{
			HeroType = AthenaHeroTypes.at(std::rand() % AthenaHeroTypes.size());
		}

		static auto HeroTypeOffset = PlayerState->GetOffset("HeroType");
		PlayerState->Get(HeroTypeOffset) = HeroType;
	}

	void ApplyCosmeticLoadout()
	{
		static auto HeroTypeOffset = PlayerState->GetOffset("HeroType");
		const auto CurrentHeroType = PlayerState->Get(HeroTypeOffset);

		if (!CurrentHeroType)
		{
			LOG_WARN(LogBots, "CurrentHeroType called with an invalid HeroType!");
			return;
		}

		ApplyHID(Pawn, CurrentHeroType, true);
		GiveGlider();
	}

	// A real player's pawn gets CustomizationLoadout.Glider from the locker; a bot's stayed null. 1.10's parachute code plays the glider's
	// OpenSound (Pawn->CustomizationLoadout.Glider + 0x330) when it opens, and crashed the server at the first bot parachute.
	// Each bot gets a glider of its own (any loaded one, so they do not all look the same), DefaultGlider if none is loaded.
	void GiveGlider()
	{
		static int LoadoutOffset = Pawn->GetOffset("CustomizationLoadout", false);
		static int GliderOffset = FindOffsetStruct("/Script/FortniteGame.FortAthenaLoadout", "Glider", false);

		if (LoadoutOffset == -1 || GliderOffset == -1)
			return;

		auto& Glider = *(UObject**)(__int64(Pawn) + LoadoutOffset + GliderOffset);

		if (Glider)
			return;

		static std::vector<UObject*> Gliders;
		static bool bCollected = false;

		if (!bCollected)
		{
			bCollected = true;

			if (auto GliderClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaGliderItemDefinition"))
			{
				auto All = GetAllObjectsOfClass(GliderClass);

				for (int i = 0; i < All.size(); ++i)
				{
					if (All.at(i) && !All.at(i)->GetName().starts_with("Default__"))
						Gliders.push_back(All.at(i));
				}
			}

			if (Gliders.empty())
			{
				if (auto Default = LoadObject<UObject>(L"/Game/Athena/Items/Cosmetics/Gliders/DefaultGlider.DefaultGlider", nullptr))
					Gliders.push_back(Default);
			}

			LOG_INFO(LogBots, "Bot gliders: {} to pick from", Gliders.size());
		}

		if (!Gliders.empty())
			Glider = Gliders.at(std::rand() % Gliders.size());
	}

	void SetName(const FString& NewName)
	{
		if (// true ||
			Fortnite_Version < 9
			)
		{
			if (auto PlayerController = Cast<APlayerController>(Controller))
			{
				PlayerController->ServerChangeName(NewName);
			}
		}
		else
		{
			auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
			GameMode->ChangeName(Controller, NewName, true);
		}

		PlayerState->OnRep_PlayerName(); // ?
	}

	// FString::Set keeps the pointer it is given, so the name's characters must outlive the call (BotBrain::StableWide).
	FString GetRandomName()
	{
		FString NewName;
		NewName.Set(BotBrain::StableWide(BotBrain::MakeBotName()));
		return NewName;
	}

	// Solo: bots take team numbers from the top of the range downwards, and the real players get the lowest ones from Reboot's own PickTeam as they join.
	// The game turned down the 100th team number (index 102) and left that player on team 0, outside the alive list, so a real player must never be the
	// one who gets the highest number. Other playlists keep the normal grouping.
	static int PickBotTeam(AFortGameModeAthena* GameMode, AController* Controller)
	{
		auto GameState = Cast<AFortGameStateAthena>(GameMode->GetGameState());
		UFortPlaylist* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
		int MaxSquadSize = 1, TeamsNum = 100;

		if (Playlist)
		{
			static auto MaxSquadSizeOffset = Playlist->GetOffset("MaxSquadSize", false);
			static auto MaxTeamCountOffset = Playlist->GetOffset("MaxTeamCount", false);

			if (MaxSquadSizeOffset != -1)
				MaxSquadSize = Playlist->Get<int>(MaxSquadSizeOffset);

			if (MaxTeamCountOffset != -1)
				TeamsNum = Playlist->Get<int>(MaxTeamCountOffset);
		}

		const int FirstTeam = PlaylistFirstTeam(Playlist);
		static int Next = -1;
		static int LastListen = -1;

		if (Next < 0 || LastListen != Globals::AmountOfListens)
		{
			LastListen = Globals::AmountOfListens;
			Next = (std::min)(FirstTeam + TeamsNum - 1, HighestAcceptedTeam);
		}

		if (MaxSquadSize != 1 || Next < FirstTeam)
			return GameMode->Athena_PickTeamHook(GameMode, 0, Controller);

		return Next--;
	}

	void Initialize(const FTransform& SpawnTransform, AActor* InSpawnLocator)
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		if (!IsReadyToSpawnBot())
		{
			LOG_ERROR(LogBots, "We are not prepared to spawn a bot!");
			return;
		}

		if (!ShouldUseAIBotController())
		{
			Controller = GetWorld()->SpawnActor<AController>(ControllerClass);
			Pawn = GetWorld()->SpawnActor<AFortPlayerPawnAthena>(PawnClass, SpawnTransform, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn));
			PlayerState = Cast<AFortPlayerStateAthena>(Controller->GetPlayerState());
		}
		else
		{
			Pawn = GameMode->GetServerBotManager()->GetCachedBotMutator()->SpawnBot(PawnClass, InSpawnLocator, SpawnTransform.Translation, SpawnTransform.Rotation.Rotator(), false);

			if (Fortnite_Version < 17)
				Controller = Cast<AFortAthenaAIBotController>(Pawn->GetController());
			else
				Controller = GetWorld()->SpawnActor<AFortAthenaAIBotController>(Pawn->GetAIControllerClass());

			PlayerState = Cast<AFortPlayerStateAthena>(Controller->GetPlayerState());
		}

		if (!Controller || !Pawn || !PlayerState)
		{
			LOG_ERROR(LogBots, "Failed to spawn controller, pawn or playerstate ({} {})!", bool(__int64(Controller)), bool(__int64(Pawn)), bool(__int64(Controller->GetPlayerState())));
			return;
		}

		PlayerState->SetIsBot(true);

		if (Controller->GetPawn() != Pawn)
		{
			Controller->Possess(Pawn);
		}

		FString BotNewName = GetRandomName();
		
		LOG_INFO(LogBots, "BotNewName: {}", BotNewName.ToString());
		SetName(BotNewName);

		PlayerState->GetTeamIndex() = PickBotTeam(GameMode, Controller);

		static auto SquadIdOffset = PlayerState->GetOffset("SquadId", false);

		if (SquadIdOffset != -1)
			PlayerState->GetSquadId() = PlayerState->GetTeamIndex() - NumToSubtractFromSquadId;

		GameState->AddPlayerStateToGameMemberInfo(PlayerState);

		Pawn->SetHealth(100);
		Pawn->SetMaxHealth(100);

		auto PlayerAbilitySet = GetPlayerAbilitySet();
		auto AbilitySystemComponent = PlayerState->GetAbilitySystemComponent();

		if (PlayerAbilitySet && AbilitySystemComponent)
		{
			PlayerAbilitySet->GiveToAbilitySystem(AbilitySystemComponent);
		}

		SetupInventory();
		PickRandomLoadout();
		ApplyCosmeticLoadout();

		if (!ShouldUseAIBotController())
		{
			++GameState->GetPlayersLeft();
			GameState->OnRep_PlayersLeft();
		}

		if (auto FortPlayerControllerAthena = Cast<AFortPlayerControllerAthena>(Controller))
		{
			GameMode->GetAlivePlayers().Add(FortPlayerControllerAthena);
		}

		BotBrain::RegisterBotPlayerState(PlayerState);
		BotRegistry::BotLabelByPlayerState[PlayerState] = std::format("#{} {}", Id, BotNewName.ToString());

		// Every bot must be told apart from the others: Reboot gives a real player a PlayerID and copies it into WorldPlayerId (which the
		// client uses to look players up, e.g. for the elimination screen), but a bot never goes through that, so all bots had the same ids.
		{
			const int OldPlayerId = PlayerState->GetPlayerID();
			const int OldWorldPlayerId = PlayerState->GetWorldPlayerId();
			PlayerState->GetPlayerID() = 5000 + Id;
			PlayerState->GetWorldPlayerId() = 5000 + Id;
			LOG_INFO(LogBots, "Bot #{} ids: PlayerID {} -> {}, WorldPlayerId {} -> {}", Id, OldPlayerId, 5000 + Id, OldWorldPlayerId, 5000 + Id);
		}

		BotRegistry::ControllerByPawn[Pawn] = Controller; // so Reboot's pickup hook can find this pawn's inventory
		BotRegistry::BotControllers.insert(Controller);
		BOT_CRUMB("Init:EnableBotServerSimulation", Id);
		EnableBotServerSimulation(Controller, Pawn, PlayerState);

		LOG_INFO(LogDev, "Finished spawning bot!")
	}
};

inline std::vector<PlayerBot> AllPlayerBotsToTick; // not static: Tick() (NetDriver.cpp) and SpawnBot() (FortGameModeAthena.cpp) must share one list

// ENetRole: 0 None, 1 SimulatedProxy, 2 AutonomousProxy, 3 Authority
static inline bool IsLocalControllerCall(AController* Controller)
{
	static auto IsLocalControllerFn = FindObject<UFunction>(L"/Script/Engine.Controller.IsLocalController");

	if (!IsLocalControllerFn)
		return false;

	struct { bool ReturnValue; } Params{};
	Controller->ProcessEvent(IsLocalControllerFn, &Params);
	return Params.ReturnValue;
}

// Recipe from Bertogim/Project-Reboot-3.0-bots (CustomBotMovement.h EnableServerSimulation), tested there on v3.5:
// a pawn with a player controller waits for client movement packets, so the server never simulates it. Without any controller
// (and bRunPhysicsWithNoController) the server simulates it itself.
static inline void EnableBotServerSimulation(AController* Controller, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState)
{
	PlayerState->SetIsBot(false);

	if (Controller->GetPawn() == Pawn)
		Controller->UnPossess();

	// UnPossess clears Pawn->PlayerState; put it back so cosmetics and name keep working.
	static auto PawnPlayerStateOffset = Pawn->GetOffset("PlayerState", false);

	if (PawnPlayerStateOffset != -1)
		Pawn->Get<UObject*>(PawnPlayerStateOffset) = (UObject*)PlayerState;

	static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement");
	auto CharacterMovement = Pawn->Get<UObject*>(CharacterMovementOffset);

	bool bRunPhysicsSet = false;

	if (CharacterMovement)
	{
		static auto RunPhysicsOffset = CharacterMovement->GetOffset("bRunPhysicsWithNoController", false);
		static auto RunPhysicsProp = CharacterMovement->GetProperty("bRunPhysicsWithNoController", false);

		if (RunPhysicsOffset != -1 && RunPhysicsProp)
		{
			CharacterMovement->SetBitfieldValue(RunPhysicsOffset, GetFieldMask(RunPhysicsProp), true);
			bRunPhysicsSet = true;
		}

		static auto SetTickFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.SetComponentTickEnabled");
		static auto ActivateFn = FindObject<UFunction>(L"/Script/Engine.ActorComponent.Activate");

		// The brain sets the bot's facing itself; the movement component must not also turn it towards its walking direction (it strafes while aiming).
		for (auto FlagName : { "bOrientRotationToMovement", "bUseControllerDesiredRotation" })
		{
			const int FlagOffset = CharacterMovement->GetOffset(FlagName, false);
			auto FlagProp = CharacterMovement->GetProperty(FlagName, false);

			if (FlagOffset != -1 && FlagProp)
				CharacterMovement->SetBitfieldValue(FlagOffset, GetFieldMask(FlagProp), false);
		}

		if (SetTickFn) { struct { bool bEnabled; } Params{ true }; CharacterMovement->ProcessEvent(SetTickFn, &Params); }
		if (ActivateFn) { struct { bool bReset; } Params{ false }; CharacterMovement->ProcessEvent(ActivateFn, &Params); }
	}

	// Fortnite gates that can stop the pawn from moving at all.
	static auto SimGravityOffset = Pawn->GetOffset("bSimGravityDisabled", false);
	static auto SimGravityProp = Pawn->GetProperty("bSimGravityDisabled", false);
	static auto DisableMovementOffset = Pawn->GetOffset("bDisableMovementAndTurnInPlace", false);
	static auto DisableMovementProp = Pawn->GetProperty("bDisableMovementAndTurnInPlace", false);
	static auto AllowMovementOffset = Pawn->GetOffset("bAllowMovement", false);
	static auto AllowMovementProp = Pawn->GetProperty("bAllowMovement", false);

	if (SimGravityProp && SimGravityOffset != -1) Pawn->SetBitfieldValue(SimGravityOffset, GetFieldMask(SimGravityProp), false);
	if (DisableMovementProp && DisableMovementOffset != -1) Pawn->SetBitfieldValue(DisableMovementOffset, GetFieldMask(DisableMovementProp), false);
	if (AllowMovementProp && AllowMovementOffset != -1) Pawn->SetBitfieldValue(AllowMovementOffset, GetFieldMask(AllowMovementProp), true);

	if (Pawn->GetNetUpdateFrequency() < 50.0f) Pawn->GetNetUpdateFrequency() = 50.0f;
	if (Pawn->GetMinNetUpdateFrequency() < 50.0f) Pawn->GetMinNetUpdateFrequency() = 50.0f;

	LOG_INFO(LogBots, "BotSim enabled: bRunPhysicsWithNoController={} gates(sim={} disable={} allow={}) controllerHasPawn={}", bRunPhysicsSet,
		SimGravityProp != nullptr, DisableMovementProp != nullptr, AllowMovementProp != nullptr, Controller->GetPawn() != nullptr);
}

// EMovementMode: 0 None, 1 Walking, 2 NavWalking, 3 Falling, 4 Swimming, 5 Flying, 6 Custom
static inline uint8_t* GetMovementModePtr(AActor* Pawn)
{
	static auto CharacterMovementOffset = Pawn->GetOffset("CharacterMovement");
	auto CharacterMovement = Pawn->Get<UObject*>(CharacterMovementOffset);

	if (!CharacterMovement)
		return nullptr;

	static auto MovementModeOffset = CharacterMovement->GetOffset("MovementMode");
	return CharacterMovement->GetPtr<uint8_t>(MovementModeOffset);
}

static inline AActor* GetCurrentAircraft(AFortGameStateAthena* GameState)
{
	static auto AircraftsOffset = GameState->GetOffset("Aircrafts", false);

	if (AircraftsOffset == -1)
	{
		static auto AircraftOffset = GameState->GetOffset("Aircraft");
		return GameState->Get<AActor*>(AircraftOffset);
	}

	auto Aircrafts = GameState->GetPtr<TArray<AActor*>>(AircraftsOffset);
	return Aircrafts->Num() > 0 ? Aircrafts->at(0) : nullptr;
}

// STEP-1 EXPERIMENT: a bare pawn driven by an AI controller (no inventory, name, team...), only to see whether the server simulates its movement.
inline std::vector<AFortPlayerPawnAthena*> AITestPawns;
inline bool bSpawnAITestPawn = false; // proved its point (AI-controlled pawns are simulated); off so it can't be a crash variable

namespace Bots
{
	static void SpawnAITestPawn(const FTransform& BaseTransform)
	{
		static auto AIControllerClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaAIController");

		if (!AIControllerClass || !PlayerBot::PawnClass)
		{
			LOG_ERROR(LogBots, "BotAITest: missing AthenaAIController ({}) or pawn class ({})", AIControllerClass != nullptr, PlayerBot::PawnClass != nullptr);
			return;
		}

		FTransform HighTransform = BaseTransform;
		HighTransform.Translation.Z += 3000;

		LOG_INFO(LogBots, "BotAITest: spawning AI controller");
		auto AIController = GetWorld()->SpawnActor<AController>(AIControllerClass);

		LOG_INFO(LogBots, "BotAITest: spawning pawn at z={:.0f}", HighTransform.Translation.Z);
		auto TestPawn = GetWorld()->SpawnActor<AFortPlayerPawnAthena>(PlayerBot::PawnClass, HighTransform, CreateSpawnParameters(ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn));

		if (!AIController || !TestPawn)
		{
			LOG_ERROR(LogBots, "BotAITest: spawn failed (controller={} pawn={})", AIController != nullptr, TestPawn != nullptr);
			return;
		}

		LOG_INFO(LogBots, "BotAITest: possessing");
		AIController->Possess(TestPawn);

		LOG_INFO(LogBots, "BotAITest: possessed, controller={} playerState={}", TestPawn->GetController() != nullptr, AIController->GetPlayerState() != nullptr);
		AITestPawns.push_back(TestPawn);
	}

	static void TickAITestPawns()
	{
		static float NextLogTime = 0.0f;
		const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

		if (NextLogTime > Now)
			return;

		NextLogTime = Now + 1.0f;

		for (auto TestPawn : AITestPawns)
		{
			if (!TestPawn || TestPawn->IsActorBeingDestroyed())
				continue;

			const FVector Loc = TestPawn->GetActorLocation();
			auto ModePtr = GetMovementModePtr(TestPawn);
			LOG_INFO(LogBots, "BotAITest pos=({:.0f}, {:.0f}, {:.0f}) mode={} hasController={}", Loc.X, Loc.Y, Loc.Z, ModePtr ? (int)*ModePtr : -1, TestPawn->GetController() != nullptr);
		}
	}

	static AController* SpawnBot(FTransform SpawnTransform, AActor* InSpawnLocator)
	{
		auto playerBot = PlayerBot();
		playerBot.Id = gNextBotId++;
		BOT_CRUMB("SpawnBot", playerBot.Id);
		playerBot.Initialize(SpawnTransform, InSpawnLocator);

		if (!playerBot.Controller || !playerBot.Pawn || !playerBot.PlayerState)
			return nullptr; // Initialize logged why; a half-made bot must not reach the tick

		AllPlayerBotsToTick.push_back(playerBot);
		return playerBot.Controller;
	}

	// ---- test mode: a match with bots only, driven from the command line (no player needed) ----
	//   -BOT_TEST            start the bus on its own a while after the bots exist
	//   -BOT_COUNT=N         how many bots (overrides the number in the window)
	//   -BOT_BUS_DELAY=S     seconds between the bots appearing and the bus leaving (default 20)
	//   -BOT_MAX_SECONDS=S   the test ends after this long in the air/on the map (default 900)
	//   -BOT_EXIT            close the server five seconds after the test ends, so a script can start the next one
	//   -BOT_SKILL=N         give every bot this aim skill
	struct BotTestOptions
	{
		bool bParsed = false;
		bool bEnabled = false;
		bool bExit = false;
		std::string Playlist;     // -BOT_PLAYLIST=<path>: play this playlist (e.g. the 50v50 one)
		int FillTotal = -1;       // -BOT_FILL=N: add the bots just before the bus, up to N players in all, like a real match
		bool bNavBuild = false;   // -BOT_NAVBUILD: explore the whole navigation map as fast as possible, save it and close the server
		int BotCount = -1;
		float BusDelay = 20.0f;
		float MaxSeconds = 900.0f;
	};

	static inline BotTestOptions gTest;

	static const BotTestOptions& TestOptions()
	{
		if (gTest.bParsed)
			return gTest;

		gTest.bParsed = true;

		// reboot.ini first, the command line below overrides it
		{
			const auto& Cfg = RebootConfig::Get().Bots;
			gTest.bEnabled = Cfg.bTest;
			gTest.bExit = Cfg.bExit;
			gTest.bNavBuild = Cfg.bNavBuild;
			gTest.FillTotal = Cfg.Fill > 0 ? Cfg.Fill : -1;
			gTest.BusDelay = Cfg.BusDelay;
			gTest.MaxSeconds = Cfg.MaxSeconds;
			if (Cfg.Skill >= 0.0f) gSkillFixed = Cfg.Skill;

			if (!Cfg.Playlist.empty())
			{
				gTest.Playlist = Cfg.Playlist;
				PlaylistName = gTest.Playlist;
				LOG_INFO(LogBots, "BotTest: playlist {} (reboot.ini)", PlaylistName);
			}
		}

		std::wstring Command = GetCommandLineW();

		for (auto& Letter : Command)
			Letter = (wchar_t)towlower(Letter);

		auto Number = [&](const wchar_t* Name, float& Out) -> bool
		{
			const size_t At = Command.find(Name);

			if (At == std::wstring::npos)
				return false;

			Out = (float)_wtof(Command.c_str() + At + wcslen(Name));
			return true;
		};

		float Value = 0.0f;
		gTest.bEnabled = gTest.bEnabled || Command.find(L"-bot_test") != std::wstring::npos;
		gTest.bExit = gTest.bExit || Command.find(L"-bot_exit") != std::wstring::npos;
		gTest.bNavBuild = gTest.bNavBuild || Command.find(L"-bot_navbuild") != std::wstring::npos;

		if (Number(L"-bot_count=", Value)) gTest.BotCount = (int)Value;
		if (Number(L"-bot_fill=", Value)) gTest.FillTotal = (int)Value;

		{
			// the playlist keeps its capital letters, so it is read from the original command line
			std::wstring Original = GetCommandLineW();
			const size_t At = Command.find(L"-bot_playlist=");

			if (At != std::wstring::npos)
			{
				size_t Start = At + wcslen(L"-bot_playlist="), End = Start;

				while (End < Original.size() && Original[End] != L' ' && Original[End] != L'"')
					++End;

				gTest.Playlist.clear();

				for (size_t i = Start; i < End; ++i)
					gTest.Playlist += (char)Original[i];

				if (!gTest.Playlist.empty())
				{
					PlaylistName = gTest.Playlist;
					LOG_INFO(LogBots, "BotTest: playlist {}", PlaylistName);
				}
			}
		}
		if (Number(L"-bot_bus_delay=", Value)) gTest.BusDelay = Value;
		if (Number(L"-bot_max_seconds=", Value)) gTest.MaxSeconds = Value;
		if (Number(L"-bot_skill=", Value)) gSkillFixed = Value;

		if (gTest.bEnabled || gTest.BotCount > 0)
			LOG_INFO(LogBots, "BotTest: test={} bots={} bus delay={:.0f}s max={:.0f}s exit={} skill={:.0f}", gTest.bEnabled, gTest.BotCount, gTest.BusDelay, gTest.MaxSeconds, gTest.bExit, gSkillFixed);

		return gTest;
	}

	static bool gBotsFilled = false;
	static void FillBots(const char* Why);
	static void MaybeFillBots(AFortGameStateAthena* GameState, float Now);

	// Starts the bus by itself, watches the match and ends the test (see the options above).
	// What the game itself uses to navigate its Ice Storm Fiends (the Deimos playlists): the Deimos AthenaAISettings asset and every navigation / AI actor that is
	// alive in the world. Read-only; everything goes to reboot.log as "AiProbe". Runs twice (about 40 s and 160 s after the first bot exists) so a navmesh that a
	// playlist creates late shows up in the second pass.
	static void AiProbe(float Now, const std::vector<AFortPlayerPawnAthena*>& Pawns)
	{
		static float StartedAt = 0.0f;
		static int Pass = 0;

		if (Pawns.empty() || Pass >= 2)
			return;

		if (StartedAt <= 0.0f)
			StartedAt = Now;

		if (Now - StartedAt < (Pass == 0 ? 40.0f : 160.0f))
			return;

		++Pass;

		auto World = GetWorld();

		auto Settings = FindObject<UObject>(L"/Game/Athena/Playlists/Deimos/Athena_AI_Settings.Athena_AI_Settings");
		auto Default = FindObject<UObject>(L"/Script/FortniteGame.Default__AthenaAISettings");

		for (auto Entry : { std::make_pair("Deimos", Settings), std::make_pair("Default", Default) })
		{
			auto Object = Entry.second;

			if (!Object || !Object->IsValidLowLevel())
			{
				LOG_INFO(LogBots, "AiProbe #{}: AthenaAISettings {} is not loaded", Pass, Entry.first);
				continue;
			}

			const int ConfigOffset = Object->GetOffset("NavigationSystemConfig", false);
			UObject* Config = ConfigOffset != -1 ? *(UObject**)(__int64(Object) + ConfigOffset) : nullptr;

			auto Flag = [&](const char* Name) -> int
			{
				const int Offset = Object->GetOffset(Name, false);
				return Offset == -1 ? -1 : (Object->ReadBitfieldValue(Offset, GetFieldMask(Object->GetProperty(Name))) ? 1 : 0);
			};

			auto Number = [&](const char* Name) -> float
			{
				const int Offset = Object->GetOffset(Name, false);
				return Offset == -1 ? -1.0f : *(float*)(__int64(Object) + Offset);
			};

			LOG_INFO(LogBots, "AiProbe #{}: AthenaAISettings {} navConfig={} goalManager={} director={} forceRVO={} hearing max={:.0f} footstep {:.0f}..{:.0f} damaged={:.0f} deaggro={:.0f}",
				Pass, Entry.first, Config && Config->IsValidLowLevel() ? Config->GetFullName() : std::string(ConfigOffset == -1 ? "no such field" : "null"),
				Flag("bAllowAIGoalManager"), Flag("bAllowAIDirector"), Flag("bForceRVOUse"), Number("MaxNPCHearingRange"), Number("MinFootstepHearingRange"),
				Number("MaxFootstepHearingRange"), Number("DamagedHearingRange"), Number("DeAggroRange"));

			if (Config && Config->IsValidLowLevel())
				BotBrain::ListFieldsMatching("deimosNavConfig", Config, { "" });
		}

		auto Count = [&](const wchar_t* Path) -> int
		{
			auto Class = FindObject<UClass>(Path);

			if (!Class)
				return -1;

			auto Found = UGameplayStatics::GetAllActorsOfClass(World, Class);
			const int Num = Found.Num();
			Found.Free();
			return Num;
		};

		UObject* NavSystem = World ? World->Get<UObject*>("NavigationSystem") : nullptr;

		LOG_INFO(LogBots, "AiProbe #{}: world navigationSystem={} recastNavMesh={} athenaNavMesh={} boundsVolume={} edgeNavLinks={} navLinkProxy={} buildingAutoNav={} navModifierVolume={}",
			Pass, NavSystem && NavSystem->IsValidLowLevel() ? NavSystem->GetFullName() : std::string("null"),
			Count(L"/Script/NavigationSystem.RecastNavMesh"), Count(L"/Script/FortniteGame.AthenaNavMesh"), Count(L"/Script/NavigationSystem.NavMeshBoundsVolume"),
			Count(L"/Script/FortniteGame.EdgeNavLinks"), Count(L"/Script/AIModule.NavLinkProxy"), Count(L"/Script/FortniteGame.BuildingAutoNav"),
			Count(L"/Script/NavigationSystem.NavModifierVolume"));

		LOG_INFO(LogBots, "AiProbe #{}: aiControllers={} fortAiPawns={} deimosFiends={} rifts={} aiDirectors={}",
			Pass, Count(L"/Script/FortniteGame.AthenaAIController"), Count(L"/Script/FortniteGame.FortAIPawn"), Count(L"/Game/Athena/Deimos/Pawns/Deimos_Fiend.Deimos_Fiend_C"),
			Count(L"/Game/Athena/Deimos/Spawners/RiftSpawners/BP_DeimosRift.BP_DeimosRift_C"), Count(L"/Script/FortniteGame.AthenaAIDirector"));
	}

	// Does this map have (or can we give it) a navmesh? Read-only unless gNavExperiment is set. Everything it finds goes to reboot.log as "NavProbe".
	static void NavProbe(float Now, const std::vector<AFortPlayerPawnAthena*>& Pawns)
	{
		static float StartedAt = 0.0f;
		static float NextRun = 0.0f;
		static int Runs = 0;
		static bool bBuiltSomething = false;

		if (Pawns.empty())
			return;

		if (StartedAt <= 0.0f)
			StartedAt = Now;

		if (Now < StartedAt + 20.0f || Now < NextRun || Runs >= 12)
			return;

		NextRun = Now + (Runs < 2 ? 15.0f : 40.0f);
		++Runs;

		auto World = GetWorld();
		static int NavSysOffset = World->GetOffset("NavigationSystem", false);
		UObject* NavSys = NavSysOffset != -1 ? World->Get<UObject*>(NavSysOffset) : nullptr;

		static auto GetNavFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.GetNavigationSystem");
		static auto NavDefault = FindObject(L"/Script/NavigationSystem.Default__NavigationSystemV1");
		UObject* Current = nullptr;

		if (GetNavFn && NavDefault)
		{
			static int ContextOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.GetNavigationSystem", "WorldContextObject", false);
			static int ReturnOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.GetNavigationSystem", "ReturnValue", false);
			std::vector<uint8_t> Params(GetNavFn->GetPropertiesSize() + 32, 0);

			if (ContextOffset != -1 && ReturnOffset != -1)
			{
				*(UObject**)(Params.data() + ContextOffset) = World;
				NavDefault->ProcessEvent(GetNavFn, Params.data());
				Current = *(UObject**)(Params.data() + ReturnOffset);
			}
		}

		auto CountOf = [&](const wchar_t* Path) -> int
		{
			auto Class = FindObject<UClass>(Path);

			if (!Class)
				return -1;

			return UGameplayStatics::GetAllActorsOfClass(World, Class).Num();
		};

		LOG_INFO(LogBots, "NavProbe #{}: World.NavigationSystem={} GetNavigationSystem={} class={} recastNavMesh={} navMeshBoundsVolume={} abstractNavData={}",
			Runs, (void*)NavSys, (void*)Current, Current ? Current->GetName() : std::string("-"),
			CountOf(L"/Script/NavigationSystem.RecastNavMesh"), CountOf(L"/Script/NavigationSystem.NavMeshBoundsVolume"), CountOf(L"/Script/NavigationSystem.AbstractNavData"));

		if (Runs == 1 && (Current || NavSys))
			BotBrain::ListFieldsMatching("NavSys", Current ? Current : NavSys, { "invoker", "generat", "build", "lock", "navdata", "agent", "world", "dirty", "runtime" });

		// can the route finder answer for a short hop from the first bot?
		{
			auto Pawn = Pawns.front();
			const FVector From = Pawn->GetActorLocation();
			std::vector<FVector> Points;
			const bool bAnswer = BotBrain::QueryNavPath(From, FVector{ From.X + 2500.0f, From.Y, From.Z }, Points);
			LOG_INFO(LogBots, "NavProbe #{}: path from ({:.0f}, {:.0f}, {:.0f}) 2500 units east: {} ({} points)", Runs, From.X, From.Y, From.Z, bAnswer ? "ANSWERED" : "no answer", (int)Points.size());

			if (bAnswer && (Current || NavSys) && BotBrain::gNavState != 1 && !gNavExperiment && !gNavMesh)
			{
				BotBrain::gNavState = 1;
				LOG_INFO(LogBots, "NavProbe: the navigation system answers, bots will route with it");
			}
		}

		if (!Current && !NavSys && BotBrain::gNavState == 0 && !gNavExperiment)
		{
			BotBrain::gNavState = -1;
			LOG_INFO(LogBots, "NavProbe: no navigation system in this world, the bots use their own grid");
		}

		if (gNavExperiment && (Current || NavSys))
		{
			// does the navigation mesh exist under the bots? (the system's own path finder answers even without one)
			static auto ProjectFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
			static auto NavDefault2 = FindObject(L"/Script/NavigationSystem.Default__NavigationSystemV1");
			static int PWorld = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "WorldContextObject", false);
			static int PPoint = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "Point", false);
			static int POut = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "ProjectedLocation", false);
			static int PExtent = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "QueryExtent", false);
			static int PReturn = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "ReturnValue", false);
			int Hit = 0, Tried = 0;

			if (ProjectFn && NavDefault2 && PWorld != -1 && PPoint != -1 && PReturn != -1)
			{
				for (auto Pawn : Pawns)
				{
					if (!Pawn || Tried >= 6)
						break;

					const FVector At = Pawn->GetActorLocation();
					std::vector<uint8_t> Params(ProjectFn->GetPropertiesSize() + 32, 0);
					*(UObject**)(Params.data() + PWorld) = World;
					*(FVector*)(Params.data() + PPoint) = FVector{ At.X, At.Y, At.Z - 90.0f };

					if (PExtent != -1)
						*(FVector*)(Params.data() + PExtent) = FVector{ 600.0f, 600.0f, 600.0f };

					NavDefault2->ProcessEvent(ProjectFn, Params.data());
					++Tried;

					if (*(bool*)(Params.data() + PReturn))
						++Hit;
				}
			}

			LOG_INFO(LogBots, "NavProbe #{}: the navigation mesh exists under {} of {} bots", Runs, Hit, Tried);

			// compare the mesh's paths with the bots' own grid on the same trips
			if (Hit > 0)
			{
				int Cases = 0;

				for (auto Pawn : Pawns)
				{
					if (!Pawn || Cases >= 4)
						break;

					const FVector From = Pawn->GetActorLocation();
					const float Angle = BotBrain::RandRange(0.0f, 6.2831853f), Distance = BotBrain::RandRange(1500.0f, 4000.0f);
					const FVector To{ From.X + std::cos(Angle) * Distance, From.Y + std::sin(Angle) * Distance, From.Z };
					std::vector<FVector> MeshPath, GridPoints;

					const double T0 = BotBrain::PerfNow();
					const bool bMesh = BotBrain::QueryNavPath(From, To, MeshPath, true);
					const double T1 = BotBrain::PerfNow();
					const int GridResult = BotBrain::GridPath(From, To, GridPoints);
					const double T2 = BotBrain::PerfNow();

					auto Length = [](const std::vector<FVector>& P) { float L = 0.0f; for (size_t i = 1; i < P.size(); ++i) L += std::sqrt((P[i].X - P[i - 1].X) * (P[i].X - P[i - 1].X) + (P[i].Y - P[i - 1].Y) * (P[i].Y - P[i - 1].Y)); return L; };
					LOG_INFO(LogBots, "NavCompare: {:.0f} units away: mesh {} points {} length {:.0f} ({:.2f} ms) | grid {} points {} length {:.0f} ({:.2f} ms)", Distance,
						bMesh ? "ok" : "no", (int)MeshPath.size(), Length(MeshPath), (T1 - T0), GridResult == 1 ? "ok" : "no", (int)GridPoints.size(), Length(GridPoints), (T2 - T1));
					++Cases;
				}
			}
		}

		if (!gNavExperiment)
			return;

		// Experiment: no navmesh data. Let the system generate tiles around the bots (invokers), which needs no bounds volume.
		static auto RegisterFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker");
		auto System = Current ? Current : NavSys;

		static int CreateTries = 0;

		if (!System && CreateTries < 3)
		{
			++CreateTries;

			// The map's world settings do not create a navigation system, but the game has a way to: a NavSystemConfigOverride actor that holds a
			// NavigationSystemConfig makes the world build its navigation system from it when the actor's components register (the Deimos mode relies on this).
			// So spawn one, deferred, put the config in, and let it finish spawning. The config is the one Athena's AI settings name, else the Athena default.
			auto OverrideClass = FindObject<UClass>(L"/Script/FortniteGame.AthenaNavSystemConfigOverride");
			auto Settings = FindObject(L"/Script/FortniteGame.Default__AthenaAISettings");
			UObject* Config = nullptr;

			if (Settings)
			{
				static int ConfigOffset = Settings->GetOffset("NavigationSystemConfig", false);

				if (ConfigOffset != -1)
					Config = Settings->Get<UObject*>(ConfigOffset);
			}

			if (!Config)
				Config = FindObject(L"/Script/FortniteGame.Default__AthenaNavSystemConfig");

			LOG_INFO(LogBots, "NavCreate #{}: override class {} settings {} config {} ({})", CreateTries, OverrideClass != nullptr, Settings != nullptr, (void*)Config, Config ? Config->GetPathName() : std::string("-"));

			// The executable on disk is encrypted; in memory it is plain. Write the loaded image out once so it can be read in a disassembler
			// (loaded at the same base as in this process, so the pointers in it still match).
			if (CreateTries == 1)
				DumpExeImage();

			// the virtual table of the config object, as offsets from the executable's base, so the functions can be found in a disassembly
			if (CreateTries == 1)
			{
				auto Dump = [&](const char* Label, UObject* Object)
				{
					if (!Object)
						return;

					const auto Base = (uintptr_t)GetModuleHandleW(nullptr);
					auto Table = *(uintptr_t**)Object;
					std::string Line;

					for (int i = 0; i < 90; ++i)
						Line += std::format(" {:x}", Table[i] - Base);

					LOG_INFO(LogBots, "NavVtable {}: object {} class {} base {:x} table@{:x}:{}", Label, (void*)Object, Object->GetName(), Base, (uintptr_t)Table - Base, Line);
				};

				Dump("AthenaNavSystemConfig", Config);
				Dump("NavigationSystemModuleConfig", FindObject(L"/Script/NavigationSystem.Default__NavigationSystemModuleConfig"));
				Dump("NavigationSystemConfig", FindObject(L"/Script/Engine.Default__NavigationSystemConfig"));
				Dump("AthenaNavSystem", FindObject(L"/Script/FortniteGame.Default__AthenaNavSystem"));
				Dump("NavigationSystemV1", FindObject(L"/Script/NavigationSystem.Default__NavigationSystemV1"));
				Dump("Object", FindObject(L"/Script/CoreUObject.Default__Object"));
			}

			// The engine's own AddNavigationSystemToWorld(World, RunMode, Config, bInitializeForWorld) at 0x24ae500 in this build (found by the signature Reboot
			// uses for 4.21): creates the system from the config, sets it on the world and runs InitializeForWorld.
			if (Config && CreateTries == 1)
			{
				const auto Base = (uintptr_t)GetModuleHandleW(nullptr);
				const uint8_t* Code = (const uint8_t*)(Base + 0x24ae500);
				static const uint8_t Expected[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x38, 0x01 };
				const bool bRight = memcmp(Code, Expected, sizeof(Expected)) == 0;
				LOG_INFO(LogBots, "NavCreate: AddNavigationSystemToWorld at {:x} looks right: {}", 0x24ae500, bRight);

				if (bRight)
				{
					using AddFn = void (*)(UObject*, uint8_t, UObject*, uint8_t);
					((AddFn)Code)(World, 1, Config, 1);
					UObject* NavSystem = NavSysOffset != -1 ? World->Get<UObject*>(NavSysOffset) : nullptr;
					LOG_INFO(LogBots, "NavCreate: World.NavigationSystem is now {} ({})", (void*)NavSystem, NavSystem ? NavSystem->GetName() : std::string("-"));

					if (NavSystem)
					{
						// build tiles only around the bots
						BotBrain::SetPawnBitOn(NavSystem, "bGenerateNavigationOnlyAroundNavigationInvokers", true);
						BotBrain::SetPawnBitOn(NavSystem, "bInitialBuildingLocked", false);

						// the data actors for the supported agents (virtual slot 98, "SpawnMissingNavigationData")
						using SpawnFn = void (*)(UObject*);
						(*(SpawnFn**)NavSystem)[98](NavSystem);
						LOG_INFO(LogBots, "NavCreate: SpawnMissingNavigationData done, recast actors now {}", UGameplayStatics::GetAllActorsOfClass(World, FindObject<UClass>(L"/Script/NavigationSystem.RecastNavMesh")).Num());

						// one huge navigation bounds volume: its brush is empty, so write its component bounds directly, then tell the system
						if (auto VolumeClass = FindObject<UClass>(L"/Script/NavigationSystem.NavMeshBoundsVolume"))
						{
							auto Volume = World->SpawnActor<AActor>(VolumeClass, FTransform{});
							LOG_INFO(LogBots, "NavCreate: bounds volume {}", (void*)Volume);

							if (Volume)
							{
								// the actor's bounds as the engine sees them (the brush may be empty)
								auto CallVec = [&](const char* Fn, const char* Param, FVector& Out0, FVector& Out1)
								{
									std::string Path = std::string("/Script/Engine.Actor.") + Fn;
									auto Function = FindObject<UFunction>(std::wstring(Path.begin(), Path.end()).c_str());
									if (!Function) return false;
									std::vector<uint8_t> Params(Function->GetPropertiesSize() + 32, 0);
									const int Origin = FindOffsetStruct(Path.c_str(), "Origin", false), Extent = FindOffsetStruct(Path.c_str(), "BoxExtent", false);
									Volume->ProcessEvent(Function, Params.data());
									if (Origin != -1) Out0 = *(FVector*)(Params.data() + Origin);
									if (Extent != -1) Out1 = *(FVector*)(Params.data() + Extent);
									return true;
								};

								FVector Origin{}, Extent{};
								CallVec("GetActorBounds", "", Origin, Extent);
								LOG_INFO(LogBots, "NavCreate: volume bounds origin ({:.0f}, {:.0f}, {:.0f}) extent ({:.0f}, {:.0f}, {:.0f})", Origin.X, Origin.Y, Origin.Z, Extent.X, Extent.Y, Extent.Z);

								static int BrushOffset = Volume->GetOffset("Brush", false);
								LOG_INFO(LogBots, "NavCreate: volume Brush model {}", BrushOffset != -1 ? (void*)Volume->Get<UObject*>(BrushOffset) : nullptr);

								// The bounds member of a scene component is not a reflected property, so find where it is: on a bot's capsule it holds the bot's position
								// followed by the capsule's half sizes. Write a huge box at that place in the volume's component, then let the system re-read it.
								{
									static int PawnRoot = Pawns.front()->GetOffset("RootComponent", false);
									UObject* Capsule = PawnRoot != -1 ? Pawns.front()->Get<UObject*>(PawnRoot) : nullptr;
									const FVector Here = Pawns.front()->GetActorLocation();
									int Found = -1;

									for (int At = 0x40; Capsule && At < 0x400; At += 4)
									{
										const float* F = (const float*)((uint8_t*)Capsule + At);

										if (std::fabs(F[0] - Here.X) < 2.0f && std::fabs(F[1] - Here.Y) < 2.0f && std::fabs(F[2] - Here.Z) < 2.0f && F[3] > 10.0f && F[3] < 100.0f && F[5] > 10.0f && F[5] < 200.0f && F[6] > 20.0f && F[6] < 300.0f)
										{
											Found = At;
											break;
										}
									}

									static int RootOff2 = Volume->GetOffset("RootComponent", false);
									UObject* VolumeRoot = RootOff2 != -1 ? Volume->Get<UObject*>(RootOff2) : nullptr;
									LOG_INFO(LogBots, "NavCreate: scene component bounds found at offset {} (capsule {}, volume root {})", Found, (void*)Capsule, (void*)VolumeRoot);

									if (Found != -1 && VolumeRoot)
									{
										float* Box = (float*)((uint8_t*)VolumeRoot + Found);
										Box[0] = 0.0f; Box[1] = 0.0f; Box[2] = 0.0f;
										Box[3] = 250000.0f; Box[4] = 250000.0f; Box[5] = 60000.0f;
										Box[6] = 400000.0f;
										Extent = FVector{ 250000.0f, 250000.0f, 60000.0f };

										if (auto BoundsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated"))
										{
											static int VolumeParam = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated", "NavVolume", false);
											std::vector<uint8_t> Params(BoundsFn->GetPropertiesSize() + 16, 0);

											if (VolumeParam != -1)
												*(UObject**)(Params.data() + VolumeParam) = Volume;

											NavSystem->ProcessEvent(BoundsFn, Params.data());
											LOG_INFO(LogBots, "NavCreate: wrote the bounds and called OnNavigationBoundsUpdated");
										}

										CallVec("GetActorBounds", "", Origin, Extent);
										LOG_INFO(LogBots, "NavCreate: the engine now reads the volume's extent as ({:.0f}, {:.0f}, {:.0f})", Extent.X, Extent.Y, Extent.Z);
									}
								}

								if (false)
								{
									static auto ScaleFn = FindObject<UFunction>(L"/Script/Engine.Actor.SetActorScale3D");
									static int NewScale = FindOffsetStruct("/Script/Engine.Actor.SetActorScale3D", "NewScale3D", false);

									if (ScaleFn && NewScale != -1)
									{
										std::vector<uint8_t> Params(ScaleFn->GetPropertiesSize() + 16, 0);
										*(FVector*)(Params.data() + NewScale) = FVector{ 250000.0f / Extent.X, 250000.0f / Extent.Y, 60000.0f / Extent.Z };
										Volume->ProcessEvent(ScaleFn, Params.data());
									}

									CallVec("GetActorBounds", "", Origin, Extent);
									LOG_INFO(LogBots, "NavCreate: after scaling, extent ({:.0f}, {:.0f}, {:.0f})", Extent.X, Extent.Y, Extent.Z);

									if (auto BoundsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated"))
									{
										static int VolumeParam = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated", "NavVolume", false);
										std::vector<uint8_t> Params(BoundsFn->GetPropertiesSize() + 16, 0);

										if (VolumeParam != -1)
											*(UObject**)(Params.data() + VolumeParam) = Volume;

										NavSystem->ProcessEvent(BoundsFn, Params.data());
										LOG_INFO(LogBots, "NavCreate: OnNavigationBoundsUpdated called");
									}
								}
							}
						}
					}
				}
			}

			if (OverrideClass && Config && false)
			{
				BotBrain::ListFieldsMatching("NavConfig", Config, { "class", "agent", "enable", "overrid" });

				// what the config's NavigationSystemClass holds (read only)
				{
					static int ClassOffset = Config->GetOffset("NavigationSystemClass", false);
					static int OverridenOffset = Config->GetOffset("bIsOverriden", false);

					if (ClassOffset != -1)
					{
						std::string Hex;

						for (int i = 0; i < 40; ++i)
							Hex += std::format("{:02x}", *((uint8_t*)Config + ClassOffset + i));

						LOG_INFO(LogBots, "NavCreate #{}: NavigationSystemClass@{} bytes {} ; bIsOverriden@{}", CreateTries, ClassOffset, Hex, OverridenOffset);

					}
				}

				auto Spawned = UGameplayStatics::BeginDeferredActorSpawnFromClass(World, OverrideClass, FTransform{});
				LOG_INFO(LogBots, "NavCreate #{}: deferred override actor {}", CreateTries, (void*)Spawned);

				if (Spawned)
				{
					static int OverrideConfigOffset = Spawned->GetOffset("NavigationSystemConfig", false);
					LOG_INFO(LogBots, "NavCreate #{}: NavigationSystemConfig property offset {}", CreateTries, OverrideConfigOffset);

					if (OverrideConfigOffset != -1)
						Spawned->Get<UObject*>(OverrideConfigOffset) = Config;

					UGameplayStatics::FinishSpawningActor(Spawned, FTransform{});

					UObject* After = NavSysOffset != -1 ? World->Get<UObject*>(NavSysOffset) : nullptr;
					LOG_INFO(LogBots, "NavCreate #{}: after finishing, World.NavigationSystem = {} ({})", CreateTries, (void*)After, After ? After->GetName() : std::string("-"));
				}
			}

			return;
		}

		if (!System)
		{
			LOG_INFO(LogBots, "NavProbe: experiment wanted, but the world has no navigation system object to work with");
			return;
		}

		if (!bBuiltSomething)
		{
			bBuiltSomething = true;

			if (auto RecastClass = FindObject<UClass>(L"/Script/NavigationSystem.RecastNavMesh"))
			{
				if (UGameplayStatics::GetAllActorsOfClass(World, RecastClass).Num() == 0)
				{
					auto Nav = World->SpawnActor<AActor>(RecastClass, FTransform{});
					LOG_INFO(LogBots, "NavProbe: spawned a RecastNavMesh actor: {}", (void*)Nav);

					if (Nav)
						BotBrain::ListFieldsMatching("RecastNavMesh", Nav, { "runtime", "generation", "tile", "agent", "cell", "dynamic" });
				}
			}

			BotBrain::SetPawnBitOn(System, "bGenerateNavigationOnlyAroundNavigationInvokers", true);
			BotBrain::SetPawnBitOn(System, "bInitialBuildingLocked", false);
		}

		if (RegisterFn)
		{
			static int InvokerOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "Invoker", false);
			static int GenerationOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "TileGenerationRadius", false);
			static int RemovalOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "TileRemovalRadius", false);
			int Registered = 0;

			for (auto Pawn : Pawns)
			{
				if (!Pawn || Registered >= 8)
					break;

				std::vector<uint8_t> Params(RegisterFn->GetPropertiesSize() + 32, 0);

				if (InvokerOffset != -1) *(UObject**)(Params.data() + InvokerOffset) = Pawn;
				if (GenerationOffset != -1) *(float*)(Params.data() + GenerationOffset) = 4000.0f;
				if (RemovalOffset != -1) *(float*)(Params.data() + RemovalOffset) = 7000.0f;

				System->ProcessEvent(RegisterFn, Params.data());
				++Registered;
			}

			LOG_INFO(LogBots, "NavProbe: registered {} bots as navigation invokers", Registered);
		}
	}


	// The navigation mesh for real (gNavMesh): the game's own Athena navigation system is created in the world, given one huge bounds volume, and
	// every bot is registered as an "invoker" so tiles of mesh get built around it. Found by taking the engine apart (see the notes in the NavProbe experiment above).
	static void NavMeshSetup(float Now, const std::vector<AFortPlayerPawnAthena*>& Pawns)
	{
		static bool bCreated = false, bFailed = false;
		static float StartedAt = 0.0f, NextReadyCheck = 0.0f, NextLog = 0.0f;
		static std::unordered_set<uint64_t> Registered;
		static AActor* BoundsVolume = nullptr;
		static float NextNudge = 0.0f;
		static int Nudges = 0;

		if (!gNavMesh || bFailed || Pawns.empty())
			return;

		BotBrain::gNavState = -1; // the grid stays the base route mode: the mesh is tried first, and the grid answers when the mesh cannot

		if (StartedAt <= 0.0f)
			StartedAt = Now;

		if (Now < StartedAt + 10.0f)
			return;

		auto World = GetWorld();
		static int NavSysOffset = World->GetOffset("NavigationSystem", false);

		if (!bCreated)
		{
			bCreated = true;

			UObject* Config = nullptr;

			if (auto Settings = FindObject(L"/Script/FortniteGame.Default__AthenaAISettings"))
			{
				static int ConfigOffset = Settings->GetOffset("NavigationSystemConfig", false);

				if (ConfigOffset != -1)
					Config = Settings->Get<UObject*>(ConfigOffset);
			}

			if (!Config)
				Config = FindObject(L"/Script/FortniteGame.Default__AthenaNavSystemConfig");

			// the engine function, by its place in this exact build and the first bytes it must start with
			const auto Base = (uintptr_t)GetModuleHandleW(nullptr);
			const uint8_t* Code = (const uint8_t*)(Base + 0x24ae500);
			static const uint8_t Expected[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x38, 0x01 };

			if (!Config || NavSysOffset == -1 || memcmp(Code, Expected, sizeof(Expected)) != 0)
			{
				bFailed = true;
				LOG_WARN(LogBots, "NavMesh: cannot set up (config {}, navigation system offset {}, function bytes match {}): the bots keep their own grid", (void*)Config, NavSysOffset, memcmp(Code, Expected, sizeof(Expected)) == 0);
				return;
			}

			using AddFn = void (*)(UObject*, uint8_t, UObject*, uint8_t);
			((AddFn)Code)(World, 1, Config, 1);
			UObject* NavSystem = World->Get<UObject*>(NavSysOffset);

			if (!NavSystem)
			{
				bFailed = true;
				LOG_WARN(LogBots, "NavMesh: the game made no navigation system, the bots keep their own grid");
				return;
			}

			// tiles only around the bots, and the links the game itself uses for climbing and dropping
			BotBrain::SetPawnBitOn(NavSystem, "bGenerateNavigationOnlyAroundNavigationInvokers", true);
			BotBrain::SetPawnBitOn(NavSystem, "bInitialBuildingLocked", false);
			BotBrain::SetPawnBitOn(NavSystem, "bUseStaticWorldLinksUp", true);
			BotBrain::SetPawnBitOn(NavSystem, "bUseStaticWorldLinksDown", true);
			BotBrain::SetPawnBitOn(NavSystem, "bUseStaticMeshLinks", true);
			BotBrain::SetPawnBitOn(NavSystem, "bUseJumpLinkActors", true);

			// a few tile jobs at a time so the server's frame time is not eaten
			if (auto JobsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.SetMaxSimultaneousTileGenerationJobsCount"))
			{
				static int JobsParam = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.SetMaxSimultaneousTileGenerationJobsCount", "InMaxNumberOfJobs", false);
				std::vector<uint8_t> Params(JobsFn->GetPropertiesSize() + 16, 0);

				if (JobsParam != -1)
					*(int*)(Params.data() + JobsParam) = 4;

				NavSystem->ProcessEvent(JobsFn, Params.data());
			}

			using SpawnFn = void (*)(UObject*);
			(*(SpawnFn**)NavSystem)[98](NavSystem); // SpawnMissingNavigationData

			// the bounds volume: no shape on the server, so write its component's bounds (not a reflected property; found by looking for a pawn's position in its capsule)
			if (auto VolumeClass = FindObject<UClass>(L"/Script/NavigationSystem.NavMeshBoundsVolume"))
			{
				auto Volume = World->SpawnActor<AActor>(VolumeClass, FTransform{});
				BoundsVolume = Volume;
				static int PawnRoot = Pawns.front()->GetOffset("RootComponent", false);
				UObject* Capsule = PawnRoot != -1 ? Pawns.front()->Get<UObject*>(PawnRoot) : nullptr;
				const FVector Here = Pawns.front()->GetActorLocation();
				int Found = -1;

				for (int At = 0x40; Capsule && At < 0x400; At += 4)
				{
					const float* F = (const float*)((uint8_t*)Capsule + At);

					if (std::fabs(F[0] - Here.X) < 2.0f && std::fabs(F[1] - Here.Y) < 2.0f && std::fabs(F[2] - Here.Z) < 2.0f && F[3] > 10.0f && F[3] < 100.0f && F[5] > 10.0f && F[5] < 200.0f && F[6] > 20.0f && F[6] < 300.0f)
					{
						Found = At;
						break;
					}
				}

				static int VolumeRootOffset = Volume ? Volume->GetOffset("RootComponent", false) : -1;
				UObject* VolumeRoot = (Volume && VolumeRootOffset != -1) ? Volume->Get<UObject*>(VolumeRootOffset) : nullptr;

				if (Found != -1 && VolumeRoot)
				{
					float* Box = (float*)((uint8_t*)VolumeRoot + Found);
					Box[0] = 0.0f; Box[1] = 0.0f; Box[2] = 0.0f;
					Box[3] = 250000.0f; Box[4] = 250000.0f; Box[5] = 60000.0f;
					Box[6] = 400000.0f;

					if (auto BoundsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated"))
					{
						static int VolumeParam = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated", "NavVolume", false);
						std::vector<uint8_t> Params(BoundsFn->GetPropertiesSize() + 16, 0);

						if (VolumeParam != -1)
							*(UObject**)(Params.data() + VolumeParam) = Volume;

						NavSystem->ProcessEvent(BoundsFn, Params.data());
					}
				}

				LOG_INFO(LogBots, "NavMesh: navigation system {} created, bounds volume {} (component bounds at offset {})", (void*)NavSystem, (void*)Volume, Found);
			}
		}

		auto NavSystem = NavSysOffset != -1 ? World->Get<UObject*>(NavSysOffset) : nullptr;

		if (!NavSystem)
			return;

		// every bot builds mesh around itself
		static auto RegisterFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker");
		static int InvokerOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "Invoker", false);
		static int GenerationOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "TileGenerationRadius", false);
		static int RemovalOffset = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.RegisterNavigationInvoker", "TileRemovalRadius", false);

		if (RegisterFn && InvokerOffset != -1)
		{
			for (auto Pawn : Pawns)
			{
				if (!Pawn)
					continue;

				auto Item = GetItemByIndex(Pawn->InternalIndex);
				const uint64_t Key = ((uint64_t)Pawn->InternalIndex << 32) | (uint32_t)(Item ? Item->SerialNumber : 0);

				if (!Registered.insert(Key).second)
					continue;

				std::vector<uint8_t> Params(RegisterFn->GetPropertiesSize() + 32, 0);
				*(UObject**)(Params.data() + InvokerOffset) = Pawn;
				if (GenerationOffset != -1) *(float*)(Params.data() + GenerationOffset) = 3000.0f;
				if (RemovalOffset != -1) *(float*)(Params.data() + RemovalOffset) = 5000.0f;
				NavSystem->ProcessEvent(RegisterFn, Params.data());
			}
		}

		// is there mesh under the bots yet?
		if (Now >= NextReadyCheck)
		{
			NextReadyCheck = Now + 5.0f;

			static auto ProjectFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation");
			static auto NavDefault = FindObject(L"/Script/NavigationSystem.Default__NavigationSystemV1");
			static int PWorld = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "WorldContextObject", false);
			static int PPoint = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "Point", false);
			static int PExtent = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "QueryExtent", false);
			static int PReturn = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.K2_ProjectPointToNavigation", "ReturnValue", false);
			int Hit = 0, Tried = 0;

			if (ProjectFn && NavDefault && PWorld != -1 && PPoint != -1 && PReturn != -1)
			{
				for (auto Pawn : Pawns)
				{
					if (!Pawn || Tried >= 12)
						break;

					const FVector At = Pawn->GetActorLocation();

					if (At.Z > 20000.0f)
						continue; // still in the bus or gliding

					std::vector<uint8_t> Params(ProjectFn->GetPropertiesSize() + 32, 0);
					*(UObject**)(Params.data() + PWorld) = World;
					*(FVector*)(Params.data() + PPoint) = FVector{ At.X, At.Y, At.Z - 90.0f };

					if (PExtent != -1)
						*(FVector*)(Params.data() + PExtent) = FVector{ 600.0f, 600.0f, 600.0f };

					NavDefault->ProcessEvent(ProjectFn, Params.data());
					++Tried;

					if (*(bool*)(Params.data() + PReturn))
						++Hit;
				}
			}

			if (Hit > 0 && !gMeshReady)
				LOG_INFO(LogBots, "NavMesh: the mesh is up under {} of {} bots, routes now use it", Hit, Tried);

			static float NextTried = 0.0f;

			if (!gMeshReady && Now >= NextTried)
			{
				NextTried = Now + 30.0f;
				LOG_INFO(LogBots, "NavMesh: not up yet ({} of {} grounded bots have mesh under them; {} recast data actors, {} bounds volumes)", Hit, Tried,
					UGameplayStatics::GetAllActorsOfClass(World, FindObject<UClass>(L"/Script/NavigationSystem.RecastNavMesh")).Num(),
					UGameplayStatics::GetAllActorsOfClass(World, FindObject<UClass>(L"/Script/NavigationSystem.NavMeshBoundsVolume")).Num());
			}

			// still nothing a minute or two in: tell the system about the bounds again and register everybody again (a few times at most)
			if (!gMeshReady && Nudges < 4 && Now >= NextNudge && Now > StartedAt + 70.0f)
			{
				NextNudge = Now + 60.0f;
				++Nudges;

				if (BoundsVolume)
				{
					if (auto BoundsFn = FindObject<UFunction>(L"/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated"))
					{
						static int VolumeParam = FindOffsetStruct("/Script/NavigationSystem.NavigationSystemV1.OnNavigationBoundsUpdated", "NavVolume", false);
						std::vector<uint8_t> Params(BoundsFn->GetPropertiesSize() + 16, 0);

						if (VolumeParam != -1)
							*(UObject**)(Params.data() + VolumeParam) = BoundsVolume;

						NavSystem->ProcessEvent(BoundsFn, Params.data());
					}
				}

				Registered.clear();
				LOG_INFO(LogBots, "NavMesh: nudge {} - bounds re-announced, bots will register again", Nudges);
			}

			gMeshReady = Hit > 0;
		}

		// Does the mesh have links? From each bot, look for ledges (a drop of 2 to 14 metres a few steps away, nothing in the way) and for railings or walls between two
		// spots on the same level, and ask the mesh for a path across: down the ledge, or through the obstacle.
		static float NextLinkTest = 0.0f;
		static int Drops = 0, DropOk = 0, DropSteps = 0, Rails = 0, RailOk = 0, RailThrough = 0, Cycles = 0;

		if (gNavMeshTest && gMeshReady && Now >= NextLinkTest)
		{
			NextLinkTest = Now + 10.0f;
			++Cycles;
			int Used = 0;

			for (auto Pawn : Pawns)
			{
				if (!Pawn || Used >= 40)
					break;

				const FVector At = Pawn->GetActorLocation();

				if (At.Z > 20000.0f)
					continue;

				++Used;
				const float FeetZ = At.Z - 88.0f;
				const float Spin = BotBrain::RandRange(0.0f, 45.0f);

				for (int k = 0; k < 8; ++k)
				{
					const float Angle = (Spin + k * 45.0f) * BotBrain::DegToRad;
					const float Dx = std::cos(Angle), Dy = std::sin(Angle);

					for (float Reach : { 450.0f, 750.0f })
					{
						const FVector P{ At.X + Dx * Reach, At.Y + Dy * Reach, At.Z };
						FVector Ground{};

						if (!BotBrain::TraceBlocked(Pawn, nullptr, FVector{ P.X, P.Y, At.Z + 50.0f }, FVector{ P.X, P.Y, FeetZ - 1600.0f }, nullptr, nullptr, nullptr, &Ground))
							continue;

						const float Drop = FeetZ - Ground.Z;
						const bool bLow = BotBrain::TraceBlocked(Pawn, nullptr, FVector{ At.X, At.Y, At.Z - 40.0f }, FVector{ P.X, P.Y, At.Z - 40.0f });
						const bool bHigh = BotBrain::TraceBlocked(Pawn, nullptr, FVector{ At.X, At.Y, At.Z + 40.0f }, FVector{ P.X, P.Y, At.Z + 40.0f });
						std::vector<FVector> Path;

						auto Length = [](const std::vector<FVector>& Q) { float L = 0.0f; for (size_t i = 1; i < Q.size(); ++i) L += BotBrain::Dist2D(Q[i - 1], Q[i]); return L; };

						if (Drop > 200.0f && Drop < 1400.0f && !bLow && !bHigh)
						{
							++Drops;
							const bool bOk = BotBrain::QueryNavPath(FVector{ At.X, At.Y, FeetZ }, FVector{ P.X, P.Y, Ground.Z }, Path, true);
							bool bStep = false;

							for (size_t i = 1; i < Path.size(); ++i)
								bStep |= std::fabs(Path[i].Z - Path[i - 1].Z) > 100.0f;

							if (bOk) ++DropOk;
							if (bOk && bStep) ++DropSteps;

							static int Shown = 0;

							if (++Shown <= 12)
								LOG_INFO(LogBots, "NavLinkTest: ledge {:.0f} down, {:.0f} away: mesh path {} ({} points, length {:.0f}, has a vertical step: {})", Drop, Reach, bOk ? "found" : "none", (int)Path.size(), Length(Path), bStep);
						}
						else if (std::fabs(Drop) < 60.0f && bLow && bHigh)
						{
							++Rails;
							const bool bOk = BotBrain::QueryNavPath(FVector{ At.X, At.Y, FeetZ }, FVector{ P.X, P.Y, Ground.Z }, Path, true);

							if (bOk) ++RailOk;
							if (bOk && Length(Path) < Reach * 1.25f) ++RailThrough;

							static int ShownRail = 0;

							if (++ShownRail <= 12)
								LOG_INFO(LogBots, "NavLinkTest: obstacle between two spots {:.0f} apart: mesh path {} ({} points, length {:.0f})", Reach, bOk ? "found" : "none", (int)Path.size(), Length(Path));
						}
					}
				}
			}

			if (Cycles % 3 == 0)
				LOG_INFO(LogBots, "NavLinkTest totals: ledges {} (mesh path found {}, with a vertical step {}), obstacles {} (path found {}, nearly straight through {})", Drops, DropOk, DropSteps, Rails, RailOk, RailThrough);
		}

		if (Now >= NextLog)
		{
			NextLog = Now + 60.0f;
			LOG_INFO(LogBots, "NavMesh: {} bots registered, {} routes from the mesh, {} fell back to the grid", (int)Registered.size(), gMeshRoutes, gMeshFallbacks);
		}
	}

	static void RunBotTest(AFortGameStateAthena* GameState, AFortGameModeAthena* GameMode, float Now)
	{
		const auto& Test = TestOptions();

		if (!Test.bEnabled || !GameState || !GameMode)
			return;

		static float ReadySince = -1.0f, BusStartedAt = -1.0f, NextStatus = 0.0f, FinishedAt = -1.0f;
		const auto Phase = GameState->GetGamePhase();

		if (ReadySince < 0.0f)
			ReadySince = Now;

		if (Test.FillTotal > 0 && !gBotsFilled && Phase == EAthenaGamePhase::Warmup && Now - ReadySince >= (std::max)(Test.BusDelay - gBotFillLead, 1.0f))
			FillBots("test, a little before the bus");

		if (BusStartedAt < 0.0f)
		{
			if (Phase == EAthenaGamePhase::Aircraft || Phase == EAthenaGamePhase::SafeZones)
			{
				BusStartedAt = Now; // somebody else started it
			}
			else if (Phase == EAthenaGamePhase::Warmup && Now - ReadySince >= Test.BusDelay)
			{
				BusStartedAt = Now;
				GameMode->StartAircraftPhase();
				LOG_INFO(LogBots, "BotTest: bus started with {} players left", GameState->GetPlayersLeft());
			}
			else if (Now >= NextStatus)
			{
				NextStatus = Now + 10.0f;
				LOG_INFO(LogBots, "BotTest: waiting to start the bus (phase {}, {:.0f}s since the bots appeared, players {})", (int)Phase, Now - ReadySince, GameState->GetPlayersLeft());
			}

			return;
		}

		if (Now >= NextStatus)
		{
			NextStatus = Now + 30.0f;
			LOG_INFO(LogBots, "BotTest: {:.0f}s into the match, phase {}, {} players left", Now - BusStartedAt, (int)Phase, GameState->GetPlayersLeft());
		}

		if (FinishedAt < 0.0f && (GameState->GetPlayersLeft() <= 1 || Now - BusStartedAt > Test.MaxSeconds))
		{
			FinishedAt = Now;
			LOG_INFO(LogBots, "BotTest: finished after {:.0f}s with {} players left", Now - BusStartedAt, GameState->GetPlayersLeft());
			// per-bot figures for the report
			int TotalChests = 0, TotalKills = 0, TotalShots = 0, TotalHits = 0, TotalPicked = 0, TotalBuilds = 0, TotalHarvests = 0, TotalHeals = 0, Alive = 0;

			for (auto& Bot : AllPlayerBotsToTick)
			{
				const auto& Brain = Bot.Brain;
				TotalChests += Brain.ChestsOpened; TotalKills += Brain.Kills; TotalShots += Brain.Shots; TotalHits += Brain.ShotsHit; TotalPicked += Brain.ItemsPicked;
				TotalBuilds += Brain.Builds; TotalHarvests += Brain.HarvestHits; TotalHeals += Brain.Heals; Alive += !Bot.bDead;
				LOG_INFO(LogBots, "BotStats #{} skill={:.0f} alive={} kills={} chests={} picked={} shots={} hits={} heals={} builds={} harvests={} paces={} watchdogs={} attacks={}",
					Bot.Id, Brain.Skill, !Bot.bDead, Brain.Kills, Brain.ChestsOpened, Brain.ItemsPicked, Brain.Shots, Brain.ShotsHit, Brain.Heals, Brain.Builds, Brain.HarvestHits, Brain.Paces, Brain.WatchdogResets, Brain.Attacks);
			}

			const int N = (std::max)((int)AllPlayerBotsToTick.size(), 1);
			LOG_INFO(LogBots, "BotStatsTotal: {} bots, {} alive; per bot: chests {:.1f}, picked up {:.1f}, kills {:.2f}, shots {:.1f} (hit rate {:.2f}), heals {:.1f}, builds {:.1f}, harvest swings {:.1f}",
				N, Alive, (float)TotalChests / N, (float)TotalPicked / N, (float)TotalKills / N, (float)TotalShots / N, TotalShots ? (float)TotalHits / TotalShots : 0.0f, (float)TotalHeals / N, (float)TotalBuilds / N, (float)TotalHarvests / N);

		}

		if (FinishedAt > 0.0f && Test.bExit && Now - FinishedAt > 5.0f)
		{
			LOG_INFO(LogBots, "BotTest: closing the server");
			if (BotBrain::gNavDirty) BotBrain::SaveNavCache();
			spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });
			ExitProcess(0);
		}
	}

	// Bots hold the high team numbers and real players the low ones; the two only meet when there are more than 100 players. When a real
	// player lands on a bot's team the bot would be his teammate, so the bot is removed to make room.
	// players per team in the current playlist (1 in solo, 2 duos, 4 squads, 50 in 50v50)
	static int PlaylistSquadSize()
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		UFortPlaylist* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;

		if (!Playlist)
			return 1;

		static auto MaxSquadSizeOffset = Playlist->GetOffset("MaxSquadSize", false);
		static auto MaxTeamSizeOffset = Playlist->GetOffset("MaxTeamSize", false);
		int Size = MaxSquadSizeOffset != -1 ? Playlist->Get<int>(MaxSquadSizeOffset) : 1;

		if (MaxTeamSizeOffset != -1)
			Size = (std::max)(Size, Playlist->Get<int>(MaxTeamSizeOffset)); // 50v50: squads of 4 inside teams of 50

		return (std::max)(1, Size);
	}

	// players in all the playlist holds (teams x players per team)
	static int PlaylistCapacity()
	{
		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		UFortPlaylist* Playlist = GameState ? GameState->GetCurrentPlaylist() : nullptr;
		int Teams = 100;

		if (Playlist)
		{
			static auto MaxPlayersOffset = Playlist->GetOffset("MaxPlayers", false);

			if (MaxPlayersOffset != -1 && Playlist->Get<int>(MaxPlayersOffset) > 1 && Playlist->Get<int>(MaxPlayersOffset) <= 100)
				return Playlist->Get<int>(MaxPlayersOffset);

			static auto MaxTeamCountOffset = Playlist->GetOffset("MaxTeamCount", false);

			if (MaxTeamCountOffset != -1 && Playlist->Get<int>(MaxTeamCountOffset) > 0 && Playlist->Get<int>(MaxTeamCountOffset) <= 100)
				Teams = Playlist->Get<int>(MaxTeamCountOffset);
		}

		return Teams * PlaylistSquadSize();
	}

	static int CountHumans()
	{
		int Humans = 0;
		auto Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AFortPlayerPawnAthena::StaticClass());

		for (int i = 0; i < Actors.Num(); ++i)
		{
			auto Pawn = (AFortPlayerPawnAthena*)Actors.at(i);

			if (!Pawn || Pawn->IsActorBeingDestroyed())
				continue;

			auto State = Cast<AFortPlayerStateAthena>((UObject*)Pawn->GetPlayerState());

			if (State && !BotBrain::IsBotPlayerState(State))
				++Humans;
		}

		Actors.Free();
		return Humans;
	}

	static void EvictBotOnTeam(int TeamIndex, AFortPlayerStateAthena* Human)
	{
		if (PlaylistSquadSize() > 1)
			return; // in a team game real players are meant to share a team with bots

		for (auto& Bot : AllPlayerBotsToTick)
		{
			if (Bot.bDead || !Bot.PlayerState || Bot.PlayerState == Human || (int)Bot.PlayerState->GetTeamIndex() != TeamIndex)
				continue;

			LOG_WARN(LogBots, "Bot #{} shares team {} with a real player who just joined (the match is full): removing the bot", Bot.Id, TeamIndex);

			if (Bot.Pawn && Bot.Pawn->IsValidLowLevel())
				Bot.Pawn->SetHealth(0.0f);
		}
	}

	static void SpawnBotsAtPlayerStarts(int AmountOfBots, bool bFill = false)
	{
		if (TestOptions().BotCount > 0 && !bFill)
			AmountOfBots = TestOptions().BotCount;

		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		static auto FortPlayerStartCreativeClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartCreative");
		static auto FortPlayerStartWarmupClass = FindObject<UClass>(L"/Script/FortniteGame.FortPlayerStartWarmup");
		TArray<AActor*> PlayerStarts = UGameplayStatics::GetAllActorsOfClass(GetWorld(), Globals::bCreative ? FortPlayerStartCreativeClass : FortPlayerStartWarmupClass);

		int ActorsNum = PlayerStarts.Num();

		// Actors.Free();

		if (ActorsNum == 0)
		{
			// LOG_INFO(LogDev, "No Actors!");
			return;
		}

		// A match holds 100 teams (numbers 3-102). Bots take them from the top, real players from the bottom (see PickBotTeam).
		int MaxBotsWithOnePlayer = HighestAcceptedTeam - PlaylistFirstTeam(GameState ? GameState->GetCurrentPlaylist() : nullptr); // one team stays free for the first real player

		if (PlaylistSquadSize() > 1)
			MaxBotsWithOnePlayer = PlaylistCapacity() - (bFill ? CountHumans() : 1); // teams of many: the room is the player limit, not the team numbers
		else if (bFill)
			MaxBotsWithOnePlayer = (HighestAcceptedTeam - PlaylistFirstTeam(GameState ? GameState->GetCurrentPlaylist() : nullptr) + 1) - CountHumans(); // the humans already hold their own teams

		if (AmountOfBots > MaxBotsWithOnePlayer)
		{
			LOG_WARN(LogBots, "Asked for {} bots; limiting to {} (teams {}-{} hold {} players, one is left for you)", AmountOfBots, MaxBotsWithOnePlayer, PlaylistFirstTeam(GameState ? GameState->GetCurrentPlaylist() : nullptr), HighestAcceptedTeam, MaxBotsWithOnePlayer + 1);
			AmountOfBots = MaxBotsWithOnePlayer;
		}

		// Find playerstart (scuffed)

		for (int i = 0; i < AmountOfBots; ++i)
		{
			AActor* PlayerStart = PlayerStarts.at(std::rand() % (PlayerStarts.size() - 1));

			if (!PlayerStart)
			{
				return;
			}

			auto NewBot = SpawnBot(PlayerStart->GetTransform(), PlayerStart);

			if (!NewBot)
			{
				// Initialize logged why; trying the other bots would fail the same way (this used to crash on the null controller)
				LOG_ERROR(LogBots, "Bot spawn failed; stopping after {} of {} bots", i, AmountOfBots);
				return;
			}

			NewBot->SetCanBeDamaged(Fortnite_Version < 7); // idk lol for spawn island

			static bool bSpawnedAITestPawn = false;

			if (bSpawnAITestPawn && !bSpawnedAITestPawn)
			{
				bSpawnedAITestPawn = true;
				SpawnAITestPawn(PlayerStart->GetTransform());
			}
		}

		return;
	}

	// ---- filling the match with bots just before the bus ----
	// Instead of guessing at the start how many players will come, the bots are added when the bus countdown is near, up to the player count asked for
	// (-BOT_FILL=N): the server then knows exactly who joined. In a team game each team ends up with its share, and nobody has to be removed.
	static bool FillMode()
	{
		return gBotFillTotal > 0 || TestOptions().FillTotal > 0 || ProdMatch::Get().bEnabled; // a backend-run server fills the match itself (prodmatch.h)
	}

	static int FillTarget()
	{
		int Asked = TestOptions().FillTotal > 0 ? TestOptions().FillTotal : gBotFillTotal;

		if (ProdMatch::Get().bEnabled)
			Asked = ProdMatch::Get().FillTotal > 0 ? ProdMatch::Get().FillTotal : PlaylistCapacity(); // everything the playlist holds
		return (std::min)(Asked, PlaylistCapacity());
	}

	static void FillBots(const char* Why)
	{
		if (gBotsFilled || !RebootConfig::Get().Bots.bEnabled)
			return;

		gBotsFilled = true;
		const int Humans = CountHumans();
		const int Existing = (int)AllPlayerBotsToTick.size();
		const int Need = FillTarget() - Humans - Existing;
		LOG_INFO(LogBots, "BotFill ({}): {} real players here, {} bots already, target {} players -> adding {} bots (playlist {}, {} per team)", Why, Humans, Existing, FillTarget(), Need > 0 ? Need : 0, PlaylistName, PlaylistSquadSize());

		if (Need > 0)
			SpawnBotsAtPlayerStarts(Need, true);

		std::map<int, int> PerTeam;

		for (auto& Bot : AllPlayerBotsToTick)
		{
			if (Bot.PlayerState)
				++PerTeam[(int)Bot.PlayerState->GetTeamIndex()];
		}

		std::string Summary;

		for (auto& Pair : PerTeam)
		{
			if (Summary.size() < 400)
				Summary += std::format("team {}: {} bots; ", Pair.first, Pair.second);
		}

		LOG_INFO(LogBots, "BotFill: {} bots now on {} teams: {}", (int)AllPlayerBotsToTick.size(), (int)PerTeam.size(), Summary);
	}

	static void MaybeFillBots(AFortGameStateAthena* GameState, float Now)
	{
		if (!GameState || gBotsFilled || !RebootConfig::Get().Bots.bEnabled || TestOptions().bEnabled || ProdMatch::Get().bEnabled || !FillMode())
			return; // (a backend-run server decides for itself when to add the bots)

		static int EndOffset = GameState->GetOffset("WarmupCountdownEndTime", false);
		const auto Phase = GameState->GetGamePhase();

		if (EndOffset != -1 && Phase == EAthenaGamePhase::Warmup)
		{
			const float End = GameState->Get<float>(EndOffset);

			if (End > Now && End - Now <= gBotFillLead)
				FillBots("bus countdown");
		}
		else if (Phase == EAthenaGamePhase::Aircraft || Phase == EAthenaGamePhase::SafeZones)
		{
			FillBots("the bus has already left");
		}
	}

	// Diagnostics for the kill feed: what the client could use to tell players apart (unique id bytes, both name fields).
	static void LogPlayerIdentity(const char* Label, AFortPlayerStateAthena* State)
	{
		if (!State)
			return;

		static int UniqueIdOffset = State->GetOffset("UniqueId", false);
		static int NamePrivateOffset = State->GetOffset("PlayerNamePrivate", false);
		static int NameOffset = State->GetOffset("PlayerName", false);

		std::string Bytes;

		if (UniqueIdOffset != -1 && FUniqueNetIdRepl::HasReplicationBytes())
		{
			auto Id = State->GetPtr<FUniqueNetIdRepl>(UniqueIdOffset);
			auto& Raw = Id->GetReplicationBytes();

			for (int i = 0; i < Raw.Num() && i < 40; ++i)
				Bytes += std::format("{:02x}", Raw.at(i));

			Bytes = std::format("{} bytes: {}", Raw.Num(), Bytes);
		}

		auto ReadString = [](FString& Text) -> std::string
		{
			const wchar_t* Chars = Text.Data.Data;
			const int Count = Text.Data.Num();

			if (!Chars || Count <= 1 || Count > 200 || IsBadReadPtr(Chars, Count * sizeof(wchar_t)))
				return std::string("(empty)");

			std::string Narrow;

			for (int i = 0; i < Count - 1; ++i)
				Narrow += Chars[i] < 0x80 ? (char)Chars[i] : '?';

			return Narrow;
		};

		std::string Private = NamePrivateOffset != -1 ? ReadString(State->Get<FString>(NamePrivateOffset)) : std::string("(no PlayerNamePrivate)");
		std::string Public = NameOffset != -1 ? ReadString(State->Get<FString>(NameOffset)) : std::string("(no PlayerName)");

		LOG_INFO(LogBots, "Identity {} state={} uniqueId[{}] PlayerNamePrivate='{}' PlayerName='{}' (offsets uid={} private={} public={})", Label, (void*)State, Bytes, Private, Public, UniqueIdOffset, NamePrivateOffset, NameOffset);
	}

	// One-off listing of every field / function of an object's class chain whose name could matter to the kill feed.
	static void DumpRelatedFields(const char* Label, UObject* Object)
	{
		if (!Object)
			return;

		int Listed = 0;

		for (UStruct* Struct = (UStruct*)Object->ClassPrivate; Struct && Listed < 200; Struct = Struct->GetSuperStruct())
		{
			for (void* Child = *(void**)(__int64(Struct) + Offsets::Children); Child && Listed < 200; Child = GetNext(Child))
			{
				const std::string Name = GetFNameOfProp(Child)->ToString();
				const std::string Lower = BotBrain::Lower(Name);

				if (!BotBrain::Contains(Lower, "name") && !BotBrain::Contains(Lower, "id") && !BotBrain::Contains(Lower, "kill") && !BotBrain::Contains(Lower, "death")
					&& !BotBrain::Contains(Lower, "feed") && !BotBrain::Contains(Lower, "elim") && !BotBrain::Contains(Lower, "finish"))
					continue;

				++Listed;
				LOG_INFO(LogBots, "Fields {}: {}::{}", Label, ((UObject*)Struct)->GetName(), Name);
			}
		}
	}

	static int KillFeedCount(AFortGameStateAthena* GameState)
	{
		static int Offset = GameState ? GameState->GetOffset("KillFeedEntry", false) : -1;

		if (Offset == -1 || !GameState)
			return -1;

		return GameState->Get<TArray<char>>(Offset).Num();
	}

	// EDeathCause value by (part of) the enum member's name; the first candidate that exists wins.
	static int64 FindDeathCause(std::initializer_list<const char*> Candidates)
	{
		static auto Enum = FindObject<UEnum>(L"/Script/FortniteGame.EDeathCause");

		if (!Enum)
			return -1;

		for (auto Candidate : Candidates)
		{
			const int64 Value = Enum->GetValue(Candidate);

			if (Value != -1)
				return Value;
		}

		return -1;
	}

	static void LogDeathCauses()
	{
		static auto Enum = FindObject<UEnum>(L"/Script/FortniteGame.EDeathCause");

		if (!Enum)
		{
			LOG_INFO(LogBots, "EDeathCause: enum not found");
			return;
		}

		auto Names = (TArray<TPair<FName, int64>>*)(__int64(Enum) + sizeof(UField) + sizeof(FString));

		for (int i = 0; i < Names->Num(); ++i)
		{
			auto& Pair = Names->At(i);
			LOG_INFO(LogBots, "EDeathCause: {} = {}", Pair.Key().ToString(), (long long)Pair.Value());
		}
	}

	// the kill feed shows a different line for a gun, a pickaxe, or the storm
	static uint8 PickDeathCause(const std::string& WeaponPath, bool bStorm)
	{
		static bool bLogged = false;

		if (!bLogged)
		{
			bLogged = true;
			LogDeathCauses();
		}

		int64 Value = -1;

		if (bStorm)
			Value = FindDeathCause({ "OutsideSafeZone", "SafeZone", "Storm" });
		else if (WeaponPath == "\x01" "fall")
			Value = FindDeathCause({ "FallDamage", "Fall" });
		else if (BotBrain::Contains(WeaponPath, "shotgun"))
			Value = FindDeathCause({ "Shotgun" });
		else if (BotBrain::Contains(WeaponPath, "sniper"))
			Value = FindDeathCause({ "Sniper" });
		else if (BotBrain::Contains(WeaponPath, "smg") || BotBrain::Contains(WeaponPath, "pdw"))
			Value = FindDeathCause({ "SMG", "Smg", "SubMachine", "Pistol" });
		else if (BotBrain::Contains(WeaponPath, "pistol") || BotBrain::Contains(WeaponPath, "revolver"))
			Value = FindDeathCause({ "Pistol" });
		else if (BotBrain::Contains(WeaponPath, "harvest") || BotBrain::Contains(WeaponPath, "pickaxe") || WeaponPath.empty())
			Value = FindDeathCause({ "Melee", "Pickaxe" });
		else
			Value = FindDeathCause({ "Rifle", "Assault" });

		return Value >= 0 && Value < 255 ? (uint8)Value : 0;
	}

	// ---- 1.x death-freeze diagnostics ----
	// A bot killed by a real player froze for 1-2 s before its death animation and the kill count (1.10, 2026-11-09). Logs, with game time,
	// every health change of a bot and then what the pawn looks like for 2.5 s after our death handling, to see which comes first.
	// After the death only raw memory is read (cached offsets): the engine may be freeing the pawn, and engine calls on it crashed before.
	struct FDeathDiagOffsets
	{
		bool bReady = false;
		int Dying = -1, PlayedDying = -1, Hidden = -1, Root = -1;
		uint8 DyingMask = 0, PlayedDyingMask = 0, HiddenMask = 0;
	};

	static FDeathDiagOffsets& DeathDiagOffsets(AFortPlayerPawnAthena* LivePawn)
	{
		static FDeathDiagOffsets Offsets;

		if (!Offsets.bReady && LivePawn)
		{
			Offsets.bReady = true;
			auto Mask = [&](const char* Name, int& Offset, uint8& FieldMask)
			{
				Offset = LivePawn->GetOffset(Name, false);
				auto Prop = LivePawn->GetProperty(Name, false);
				FieldMask = Prop ? GetFieldMask(Prop) : 0;
			};
			Mask("bIsDying", Offsets.Dying, Offsets.DyingMask);
			Mask("bPlayedDying", Offsets.PlayedDying, Offsets.PlayedDyingMask);
			Mask("bIsHiddenForDeath", Offsets.Hidden, Offsets.HiddenMask);
			Offsets.Root = LivePawn->GetOffset("RootComponent", false);
		}

		return Offsets;
	}

	static std::string DeathDiagFlags(void* Pawn)
	{
		const auto& O = DeathDiagOffsets(nullptr);

		if (!O.bReady || !Pawn)
			return "?";

		auto Bit = [&](int Offset, uint8 FieldMask) { return Offset == -1 || !FieldMask ? -1 : ((*(uint8*)(__int64(Pawn) + Offset) & FieldMask) ? 1 : 0); };
		const uint64_t Root = O.Root == -1 ? 0 : *(uint64_t*)(__int64(Pawn) + O.Root);
		return std::format("dying={} playedDying={} hiddenForDeath={} root={:#x}", Bit(O.Dying, O.DyingMask), Bit(O.PlayedDying, O.PlayedDyingMask), Bit(O.Hidden, O.HiddenMask), Root);
	}

	struct FDeathWatch { void* Pawn; int Id; float Until; float NextLog; };
	inline static std::vector<FDeathWatch> gDeathWatch;

	static void DeathDiagHealth(PlayerBot& Bot, AFortPlayerPawnAthena* Pawn, float Health)
	{
		if (Fortnite_Version >= 2)
			return;

		DeathDiagOffsets(Pawn);

		if (Bot.DiagLastHealth >= 0.0f && Health != Bot.DiagLastHealth)
		{
			const FVector At = Pawn->GetActorLocation();
			LOG_INFO(LogBots, "DeathDiag #{} t={:.2f} health {:.0f} -> {:.0f} {} at ({:.0f}, {:.0f}, {:.0f})", Bot.Id, UGameplayStatics::GetTimeSeconds(GetWorld()),
				Bot.DiagLastHealth, Health, DeathDiagFlags(Pawn), At.X, At.Y, At.Z);
		}

		Bot.DiagLastHealth = Health;
	}

	static void DeathDiagOnDeath(PlayerBot& Bot, void* Pawn)
	{
		if (Fortnite_Version >= 2 || !Pawn)
			return;

		const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());
		LOG_INFO(LogBots, "DeathDiag #{} t={:.2f} death handled {}", Bot.Id, Now, DeathDiagFlags(Pawn));
		gDeathWatch.push_back({ Pawn, Bot.Id, Now + 2.5f, Now + 0.25f });
	}

	static void DeathDiagTick()
	{
		if (gDeathWatch.empty())
			return;

		const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

		for (auto& Watch : gDeathWatch)
		{
			if (Now < Watch.NextLog || Now > Watch.Until)
				continue;

			Watch.NextLog = Now + 0.25f;
			LOG_INFO(LogBots, "DeathDiag #{} t={:.2f} after death {}", Watch.Id, Now, DeathDiagFlags(Watch.Pawn));
		}

		std::erase_if(gDeathWatch, [Now](const FDeathWatch& Watch) { return Now > Watch.Until; });
	}

	// 1.x diagnostics for the skydive crash: the pawn's parachute code reads a native pointer at pawn+0x1698 that is null on an unpossessed bot.
	// Log, once, what a real player's pawn holds there (and around it) next to a bot's, so bots can be given the same thing.
	static void LogPawnParachuteFieldOnce()
	{
		static bool bDone = false;

		if (bDone || Fortnite_Version >= 2)
			return;

		APawn* HumanPawn = nullptr;
		APawn* BotPawn = nullptr;

		for (auto& Info : BotBrain::gPawns)
		{
			if (!Info.Pawn)
				continue;

			if (Info.bIsBot) { if (!BotPawn) BotPawn = (APawn*)Info.Pawn; }
			else if (!HumanPawn) HumanPawn = (APawn*)Info.Pawn;
		}

		if (!HumanPawn || !BotPawn)
			return;

		bDone = true;

		for (auto [Label, Pawn] : { std::pair<const char*, APawn*>{ "player", HumanPawn }, std::pair<const char*, APawn*>{ "bot", BotPawn } })
		{
			for (int Offset = 0x1688; Offset <= 0x16a8; Offset += 8)
			{
				const uint64_t Value = *(uint64_t*)(__int64(Pawn) + Offset);
				uint64_t Class = 0;

				if (BotProbeObject(Value, &Class))
				{
					LOG_INFO(LogBots, "PawnField {} {} +0x{:x} = 0x{:x} {} (class {})", Label, Pawn->GetName(), Offset, Value, ((UObject*)Value)->GetFullName(), ((UObject*)Class)->GetName());
				}
				else
				{
					LOG_INFO(LogBots, "PawnField {} {} +0x{:x} = 0x{:x}", Label, Pawn->GetName(), Offset, Value);
				}
			}
		}
	}

	// The client tells players apart by their unique id, and every bot has an empty one: the kill feed (and the elimination screen) then resolve to the
	// same bot every time. Give each bot its own, copied from a real player's (the id is a shared pointer to a string object plus a reference controller).
	static void AssignBotUniqueIds(AFortGameModeAthena* GameMode)
	{
		static bool bDone = false;

		if (bDone || !GameMode)
			return;

		static int UniqueIdOffset = FindOffsetStruct("/Script/Engine.PlayerState", "UniqueId");

		AFortPlayerStateAthena* Human = nullptr;
		auto& Alive = GameMode->GetAlivePlayers();

		auto HasId = [&](AFortPlayerStateAthena* State)
		{
			return State && !BotBrain::IsBotPlayerState(State) && *(void**)(__int64(State) + UniqueIdOffset) != nullptr;
		};

		for (int i = 0; i < Alive.Num() && !Human; ++i)
		{
			auto Other = Alive.at(i);
			auto OtherState = Other ? Cast<AFortPlayerStateAthena>(Other->GetPlayerState()) : nullptr;

			if (HasId(OtherState))
				Human = OtherState;
		}

		// players the brain has seen walking about count too (the alive list can lag behind a player who has only just joined)
		for (size_t i = 0; i < BotBrain::gPawns.size() && !Human; ++i)
		{
			auto Pawn = BotBrain::gPawns[i].Pawn;
			auto PawnState = (Pawn && !BotBrain::gPawns[i].bIsBot) ? Cast<AFortPlayerStateAthena>((UObject*)Pawn->GetPlayerState()) : nullptr;

			if (HasId(PawnState))
				Human = PawnState;
		}

		if (!Human)
		{
			static float NextWaitLog = 0.0f;
			const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

			if (Now >= NextWaitLog)
			{
				NextWaitLog = Now + 30.0f;
				LOG_INFO(LogBots, "UniqueId: waiting for a real player to copy an id from ({} alive, {} pawns seen)", Alive.Num(), (int)BotBrain::gPawns.size());
			}

			return; // nobody to copy from yet; try again next tick
		}

		bDone = true;

		uint8* Template = (uint8*)Human + UniqueIdOffset;
		uint8* TemplateObject = *(uint8**)(Template + 8);      // layout seen in the logs: [0] vtable, [8] object, [16] reference controller
		uint8* TemplateController = *(uint8**)(Template + 16);

		auto Readable = [](void* Pointer, size_t Size) { return Pointer && !IsBadReadPtr(Pointer, Size); };

		// the object: [0] vtable, [8] weak "this" (points at itself), [16] its controller, [24] the id as an FString
		wchar_t* Text = nullptr;
		int Length = 0;
		bool bSane = Readable(TemplateObject, 64) && Readable(TemplateController, 48) && *(uint8**)(TemplateObject + 8) == TemplateObject;

		if (bSane)
		{
			Text = *(wchar_t**)(TemplateObject + 24);
			Length = *(int*)(TemplateObject + 32);
			bSane = Length >= 8 && Length <= 64 && Readable(Text, Length * 2) && Text[Length - 1] == 0;

			for (int i = 0; bSane && i < Length - 1; ++i)
				bSane = Text[i] >= 0x20 && Text[i] < 0x7f;
		}

		int ObjectSlot = -1;

		for (int Slot = 16; bSane && Slot <= 40 && ObjectSlot == -1; Slot += 8)
		{
			if (*(uint8**)(TemplateController + Slot) == TemplateObject)
				ObjectSlot = Slot;
		}

		LOG_INFO(LogBots, "UniqueId template: object {} controller {} sane={} text length={} object slot in controller={}", (void*)TemplateObject, (void*)TemplateController, bSane, Length, ObjectSlot);

		if (!bSane || ObjectSlot == -1)
		{
			LOG_WARN(LogBots, "UniqueId template does not look like a string id; bots keep empty ids");
			return;
		}

		const int StructSize = FUniqueNetIdRepl::GetSizeOfStruct();
		int Assigned = 0;

		for (auto& Bot : AllPlayerBotsToTick)
		{
			if (!Bot.PlayerState)
				continue;

			uint8* Destination = (uint8*)Bot.PlayerState + UniqueIdOffset;

			if (*(void**)(Destination + 8) != nullptr)
				continue;

			uint8* Object = (uint8*)calloc(1, 96);
			memcpy(Object, TemplateObject, 64);

			wchar_t* BotText = (wchar_t*)calloc(Length + 1, sizeof(wchar_t));
			memcpy(BotText, Text, Length * sizeof(wchar_t));

			// same shape as a real account id, with the last 8 characters replaced by ones that belong to this bot only
			const std::wstring Suffix = std::format(L"{:08x}", 0xB07B0000u + (unsigned)Bot.Id);

			for (int i = 0; i < 8; ++i)
				BotText[Length - 9 + i] = Suffix[i];

			*(wchar_t**)(Object + 24) = BotText;
			*(int*)(Object + 32) = Length;
			*(int*)(Object + 36) = Length;

			uint8* Controller = (uint8*)calloc(1, 96);
			memcpy(Controller, TemplateController, 64);
			*(uint8**)(Controller + ObjectSlot) = Object;
			*(int*)(Controller + 8) = 0x10000000; // shared references: never released
			*(int*)(Controller + 12) = 0x10000000; // weak references: same

			*(uint8**)(Object + 8) = Object;       // the weak "this" of the copy points at the copy
			*(uint8**)(Object + 16) = Controller;

			memcpy(Destination, Template, StructSize);
			*(uint8**)(Destination + 8) = Object;
			*(uint8**)(Destination + 16) = Controller;

			// the byte array was copied by pointer; the copy must not share it (1.10 has no such array: writing it corrupted memory)
			if (FUniqueNetIdRepl::HasReplicationBytes())
			{
				auto& Raw = ((FUniqueNetIdRepl*)Destination)->GetReplicationBytes();
				Raw.Data = nullptr;
				Raw.ArrayNum = 0;
				Raw.ArrayMax = 0;
			}

			++Assigned;
		}

		LOG_INFO(LogBots, "UniqueId: gave {} bots their own id (template '{}')", Assigned, std::string(Text, Text + Length - 1));
	}

	// The kill feed ("A killed B") is driven by the dead player's replicated DeathInfo, which the engine only fills in its own death flow.
	static void FillBotDeathInfo(AFortPlayerStateAthena* DeadState, AFortPlayerStateAthena* KillerState, const FVector& DeathLocation, float Distance, uint8 DeathCause)
	{
		if (!DeadState)
			return;

		auto DeathInfo = DeadState->GetDeathInfo();

		if (!DeathInfo)
			return;

		static int FinisherOffset = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "FinisherOrDowner", false);
		static int DownerOffset = FindOffsetStruct("/Script/FortniteGame.DeathInfo", "Downer", false);
		static bool bLoggedOffsets = false;

		if (!bLoggedOffsets)
		{
			bLoggedOffsets = true;
			LOG_INFO(LogBots, "DeathInfo offsets: FinisherOrDowner={} Downer={} DeathCause={} bDBNO={} Distance={} DeathLocation={} bInitialized={} size={}",
				FinisherOffset, DownerOffset, MemberOffsets::DeathInfo::DeathCause, MemberOffsets::DeathInfo::bDBNO, MemberOffsets::DeathInfo::Distance,
				MemberOffsets::DeathInfo::DeathLocation, MemberOffsets::DeathInfo::bInitialized, FDeathInfo::GetStructSize());
		}

		DeadState->ClearDeathInfo();

		AActor* Finisher = KillerState ? (AActor*)KillerState : (AActor*)DeadState;

		*(bool*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::bDBNO) = false;
		*(uint8*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::DeathCause) = DeathCause;

		if (FinisherOffset != -1)
			*(AActor**)(__int64(DeathInfo) + FinisherOffset) = Finisher;

		if (DownerOffset != -1)
			*(AActor**)(__int64(DeathInfo) + DownerOffset) = Finisher;

		if (MemberOffsets::DeathInfo::DeathLocation != -1)
			*(FVector*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::DeathLocation) = DeathLocation;

		if (MemberOffsets::DeathInfo::Distance != -1)
			*(float*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::Distance) = Distance;

		if (MemberOffsets::DeathInfo::bInitialized != -1)
			*(bool*)(__int64(DeathInfo) + MemberOffsets::DeathInfo::bInitialized) = true;

		if (MemberOffsets::FortPlayerState::PawnDeathLocation != -1)
			DeadState->Get<FVector>(MemberOffsets::FortPlayerState::PawnDeathLocation) = DeathLocation;

		static int IdentityLogs = 0;
		auto FeedState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		const int FeedBefore = KillFeedCount(FeedState);

		DeadState->OnRep_DeathInfo();

		if (IdentityLogs < 6)
		{
			++IdentityLogs;
			if (IdentityLogs == 1)
			{
				DumpRelatedFields("PlayerState", DeadState);
				DumpRelatedFields("GameState", FeedState);
			}

			LogPlayerIdentity("victim", DeadState);
			LogPlayerIdentity("killer", KillerState);

			auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

			if (GameMode)
			{
				auto& Alive = GameMode->GetAlivePlayers();

				for (int i = 0; i < Alive.Num(); ++i)
				{
					auto Other = Alive.at(i);
					auto OtherState = Other ? Cast<AFortPlayerStateAthena>(Other->GetPlayerState()) : nullptr;

					if (OtherState && !BotBrain::IsBotPlayerState(OtherState))
					{
						LogPlayerIdentity("human", OtherState);
						break;
					}
				}
			}
		}

		LOG_INFO(LogBots, "KillFeed: entries {} -> {} after OnRep_DeathInfo", FeedBefore, KillFeedCount(FeedState));
	}

	// The engine's death flow never runs for these bots (no controller on the pawn), so do what ClientOnPawnDiedHook does for a player:
	// let the native RemoveFromAlivePlayers update the alive list, PlayersLeft, place and win check, and credit the killer.
	static void HandleBotDeath(PlayerBot& Bot, void* DeadPawnKey, AFortGameModeAthena* GameMode, AFortGameStateAthena* GameState, const FVector& DeathLocation)
	{
		Bot.bDead = true;

		auto Controller = Cast<AFortPlayerControllerAthena>(Bot.Controller);
		auto DeadState = Cast<AFortPlayerStateAthena>(Bot.PlayerState);

		LOG_INFO(LogBots, "Bot #{} died at ({:.0f}, {:.0f}, {:.0f}); PlayersLeft was {}", Bot.Id, DeathLocation.X, DeathLocation.Y, DeathLocation.Z, GameState->GetPlayersLeft());

		if (!Controller || !Addresses::RemoveFromAlivePlayers)
		{
			LOG_WARN(LogBots, "Bot #{} death: no controller or RemoveFromAlivePlayers address, PlayersLeft not updated", Bot.Id);
			return;
		}

		// Credit whoever hurt it last (a bot or a player, recorded when the shot landed); if nobody did, the nearest real player within reach.
		AFortPlayerStateAthena* KillerState = nullptr;
		AFortPlayerPawn* KillerPawn = nullptr;
		float BestDistance = 6000.0f;

		std::string WeaponPath;

		if (auto WeaponFound = BotBrain::gLastWeaponPath.find(DeadPawnKey); WeaponFound != BotBrain::gLastWeaponPath.end())
		{
			WeaponPath = WeaponFound->second;
			BotBrain::gLastWeaponPath.erase(WeaponFound);
		}

		// the 'fall' mark stays on a bot after it lands; a bot shot dead minutes later has not died of that fall
		if (WeaponPath == "\x01" "fall")
		{
			auto FellAt = BotBrain::gFallTime.find(DeadPawnKey);

			if (FellAt == BotBrain::gFallTime.end() || UGameplayStatics::GetTimeSeconds(GetWorld()) - FellAt->second > 4.0f)
				WeaponPath.clear();
		}

		BotBrain::gFallTime.erase(DeadPawnKey);

		auto Recorded = BotBrain::gLastDamager.find(DeadPawnKey);
		const bool bFell = WeaponPath == "\x01" "fall";

		if (Recorded != BotBrain::gLastDamager.end())
		{
			// somebody who hurt it long ago did not kill it (the storm or a fall did)
			auto When = BotBrain::gLastDamageTime.find(DeadPawnKey);
			const float Ago = When != BotBrain::gLastDamageTime.end() ? UGameplayStatics::GetTimeSeconds(GetWorld()) - When->second : 0.0f;

			if (!bFell && Ago < 12.0f)
			{
				KillerState = Recorded->second;
				BestDistance = -1.0f; // already decided
			}

			BotBrain::gLastDamager.erase(Recorded);
		}

		BotBrain::gLastDamageTime.erase(DeadPawnKey);

		if (bFell)
			BestDistance = -1.0f; // a fall has no killer

		// The engine tells us nothing about a bot a real player killed, so work it out: the player who fired a moment ago and is in range,
		// or failing that the closest one. Players are found from the brain's own pawn scan (the game's alive list can miss them).
		const float CreditNow = UGameplayStatics::GetTimeSeconds(GetWorld());
		float BestScore = 1e30f;

		for (size_t i = 0; BestDistance >= 0.0f && i < BotBrain::gPawns.size(); ++i)
		{
			auto& Other = BotBrain::gPawns[i];

			if (!Other.Pawn || Other.bIsBot)
				continue;

			auto OtherState = Cast<AFortPlayerStateAthena>((UObject*)Other.Pawn->GetPlayerState());

			if (!OtherState || OtherState == DeadState)
				continue;

			const float Distance = BotBrain::Dist2D(Other.Pos, DeathLocation);
			auto Shot = BotBrain::gHumanShotTime.find(Other.Pawn);
			const bool bShotJustNow = Shot != BotBrain::gHumanShotTime.end() && CreditNow - Shot->second < 4.0f;

			// a recent shooter within 30000 units, or anybody within 6000 (the 0.4s scan means a very fresh shot can still be missing)
			if (!(bShotJustNow && Distance < 30000.0f) && Distance >= 6000.0f)
				continue;

			const float Score = Distance * (bShotJustNow ? 0.3f : 1.0f);

			if (Score < BestScore)
			{
				BestScore = Score;
				KillerState = OtherState;
				KillerPawn = Other.Pawn;
			}
		}

		if (KillerState && KillerState != DeadState)
		{
			for (auto& Other : AllPlayerBotsToTick)
			{
				if (Other.PlayerState == KillerState)
					++Other.Brain.Kills;
			}

			if (MemberOffsets::FortPlayerStateAthena::KillScore != -1)
				KillerState->Get<int>(MemberOffsets::FortPlayerStateAthena::KillScore)++;

			if (MemberOffsets::FortPlayerStateAthena::TeamKillScore != -1)
				KillerState->Get<int>(MemberOffsets::FortPlayerStateAthena::TeamKillScore)++;

			if (DeadState)
				KillerState->ClientReportKill(DeadState);
		}

		{
			// distance for the feed entry: from the killer's pawn (a bot's or a player's) to where the victim fell
			float KillDistance = 0.0f;
			AActor* KillerActor = KillerPawn;

			if (!KillerActor && KillerState)
			{
				for (auto& Other : AllPlayerBotsToTick)
				{
					if (Other.PlayerState == KillerState && Other.Pawn && Other.Pawn->IsValidLowLevel())
					{
						KillerActor = Other.Pawn;
						break;
					}
				}
			}

			if (KillerActor)
				KillDistance = BotBrain::Dist2D(KillerActor->GetActorLocation(), DeathLocation);

			FillBotDeathInfo(DeadState, KillerState, DeathLocation, KillDistance, PickDeathCause(WeaponPath, KillerState == nullptr && WeaponPath != "\x01" "fall"));
		}

		// A player's items fall where they die (everything except the pickaxe); a bot's do too, so its weapons and heals can be picked up from the body.
		if (Controller)
		{
			if (auto WorldInventory = Controller->GetWorldInventory())
			{
				auto& ItemInstances = WorldInventory->GetItemList().GetItemInstances();
				std::vector<std::pair<FGuid, int>> ToRemove;
				int Dropped = 0;

				for (int i = 0; i < ItemInstances.Num(); ++i)
				{
					auto ItemInstance = ItemInstances.at(i);

					if (!ItemInstance)
						continue;

					auto ItemEntry = ItemInstance->GetItemEntry();
					auto WorldItemDefinition = Cast<UFortWorldItemDefinition>(ItemEntry->GetItemDefinition());

					if (!WorldItemDefinition || !WorldItemDefinition->CanBeDropped() || ItemEntry->GetCount() <= 0)
						continue;

					PickupCreateData CreateData;
					CreateData.bToss = true;
					CreateData.ItemEntry = ItemEntry;
					CreateData.SourceType = EFortPickupSourceTypeFlag::GetPlayerValue();
					CreateData.Source = EFortPickupSpawnSource::GetPlayerEliminationValue();
					CreateData.SpawnLocation = FVector{ DeathLocation.X, DeathLocation.Y, DeathLocation.Z + 40.0f };

					AFortPickup::SpawnPickup(CreateData);
					ToRemove.push_back({ ItemEntry->GetItemGuid(), ItemEntry->GetCount() });
					++Dropped;
				}

				for (auto& Pair : ToRemove)
					WorldInventory->RemoveItem(Pair.first, nullptr, Pair.second, true);

				WorldInventory->Update();

				static int Logged = 0;

				if (++Logged <= 12)
					LOG_INFO(LogBots, "Bot #{} dropped {} item stacks where it died", Bot.Id, Dropped);
			}
		}

		static void (*RemoveFromAlivePlayers)(AFortGameModeAthena * GameMode, AFortPlayerController * PlayerController, APlayerState * PlayerState, APawn * FinisherPawn,
			UFortWeaponItemDefinition * FinishingWeapon, uint8_t DeathCause, char a7) = decltype(RemoveFromAlivePlayers)(Addresses::RemoveFromAlivePlayers);

		RemoveFromAlivePlayers(GameMode, Controller, KillerState, KillerPawn, nullptr, 0, 0);

		LOG_INFO(LogBots, "KillFeed: entries now {} after RemoveFromAlivePlayers", KillFeedCount(GameState));

		LOG_INFO(LogBots, "Bot #{} removed from the alive list; PlayersLeft is now {}, TeamsLeft {}, killer={} ({}), weapon '{}', fell={}", Bot.Id, GameState->GetPlayersLeft(), GameState->GetTeamsLeft(), KillerState != nullptr, KillerState ? (BotBrain::IsBotPlayerState(KillerState) ? BotRegistry::LabelFor(KillerState) : std::string("a real player")) : std::string("nobody"), WeaponPath, bFell);
	}


	// How long the bots take: per second-ish report of the server's frame rate and what the bot code costs inside a frame.
	struct BotPerf
	{
		double LastStart = 0.0, NextLog = 0.0, IntervalMax = 0.0;
		double TickTotal = 0.0, TickMax = 0.0, BrainTotal = 0.0, BrainMax = 0.0, BrainFrame = 0.0;
		int Frames = 0;
	};

	static inline BotPerf gPerf;

	static double PerfNowMs()
	{
		static LARGE_INTEGER Frequency = [] { LARGE_INTEGER F; QueryPerformanceFrequency(&F); return F; }();
		LARGE_INTEGER Counter;
		QueryPerformanceCounter(&Counter);
		return (double)Counter.QuadPart * 1000.0 / (double)Frequency.QuadPart;
	}

	struct PerfScope
	{
		double Start;

		PerfScope()
		{
			Start = PerfNowMs();

			if (gPerf.LastStart > 0.0)
				gPerf.IntervalMax = (std::max)(gPerf.IntervalMax, Start - gPerf.LastStart);

			gPerf.LastStart = Start;
			gPerf.BrainFrame = 0.0;
		}

		~PerfScope()
		{
			const double Now = PerfNowMs();
			const double Duration = Now - Start;
			++gPerf.Frames;
			gPerf.TickTotal += Duration;
			gPerf.TickMax = (std::max)(gPerf.TickMax, Duration);
			gPerf.BrainTotal += gPerf.BrainFrame;
			gPerf.BrainMax = (std::max)(gPerf.BrainMax, gPerf.BrainFrame);

			if (gPerf.NextLog <= 0.0)
				gPerf.NextLog = Now + 10000.0;

			if (Now >= gPerf.NextLog)
			{
				const double Seconds = 10.0;
				const double F = (std::max)(gPerf.Frames, 1);
				LOG_INFO(LogBots, "BotPerf: {} bots, {:.1f} frames/s, longest gap between frames {:.0f} ms; bot code per frame avg {:.2f} ms max {:.1f} ms (brains avg {:.2f} ms max {:.1f} ms: observe {:.2f}, decide {:.2f}, apply {:.2f}); decisions {}/s ({} of them far from players), {} waited for budget; ray casts {}/s taking {:.2f} ms per frame in the engine, {:.2f} ms with the wrapper",
					(int)AllPlayerBotsToTick.size(), gPerf.Frames / Seconds, gPerf.IntervalMax, gPerf.TickTotal / F, gPerf.TickMax, gPerf.BrainTotal / F, gPerf.BrainMax,
					gTimeObserve / F, gTimeDecide / F, gTimeApply / F, gDecisions / 10, gDecisionsFar / 10, gDecisionsSkipped / 10, gTracesTotal / 10, gTraceTime / F, gTraceWhole / F);
				gTracesTotal = 0; gTraceTime = 0.0; gTraceWhole = 0.0;
				LOG_INFO(LogBots, "BotPerf observe parts (ms per frame): height {:.2f}, inventory {:.2f}, nav {:.2f}, probes {:.2f}, escape {:.2f}, enemy {:.2f}, sounds {:.2f}, pickups {:.2f}",
					gObsSection[0] / F, gObsSection[1] / F, gObsSection[2] / F, gObsSection[3] / F, gObsSection[4] / F, gObsSection[5] / F, gObsSection[6] / F, gObsSection[7] / F);
				LOG_INFO(LogBots, "BotPerf probe parts (ms per frame): heading+cells {:.2f}, wall probes {:.2f}, blocked scan or lookahead {:.2f}, drop check {:.2f}", gObsSection[8] / F, gObsSection[9] / F, gObsSection[10] / F, gObsSection[3] / F);

				for (auto& Part : gObsSection)
					Part = 0.0;

				LOG_INFO(LogBots, "BotPerf update parts (ms per frame): before observe {:.2f}, the observe call {:.2f}", gUpdParts[0] / F, gUpdParts[1] / F);
				gUpdParts[0] = gUpdParts[1] = 0.0;
				gTimeObserve = gTimeDecide = gTimeApply = 0.0;
				gDecisions = gDecisionsFar = gDecisionsSkipped = 0;

				gPerf.NextLog = Now + 10000.0;
				gPerf.Frames = 0;
				gPerf.IntervalMax = gPerf.TickTotal = gPerf.TickMax = gPerf.BrainTotal = gPerf.BrainMax = 0.0;
			}
		}
	};

	static void Tick()
	{
		if (AllPlayerBotsToTick.size() == 0)
		{
			// no bots yet: in fill mode they come just before the bus, so this is where that gets noticed
			if (FillMode())
			{
				auto EmptyGameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
				auto EmptyGameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());
				const float EmptyNow = UGameplayStatics::GetTimeSeconds(GetWorld());
				RunBotTest(EmptyGameState, EmptyGameMode, EmptyNow);
				MaybeFillBots(EmptyGameState, EmptyNow);
			}

			return;
		}

		PerfScope Perf;

		auto GameState = Cast<AFortGameStateAthena>(GetWorld()->GetGameState());
		auto GameMode = Cast<AFortGameModeAthena>(GetWorld()->GetGameMode());

		BOT_CRUMB("Tick:begin", -1);
		TickAITestPawns();

		const float TickNow = UGameplayStatics::GetTimeSeconds(GetWorld());
		BotBrain::RefreshShared(TickNow, GameMode);
		AssignBotUniqueIds(GameMode);
		LogPawnParachuteFieldOnce();
		DeathDiagTick();
		RunBotTest(GameState, GameMode, TickNow);
		MaybeFillBots(GameState, TickNow);

		{
			std::vector<AFortPlayerPawnAthena*> NavPawns;

			for (auto& PlayerBot : AllPlayerBotsToTick)
			{
				if (PlayerBot.Pawn && PlayerBot.Pawn->IsValidLowLevel() && !PlayerBot.Pawn->IsActorBeingDestroyed())
					NavPawns.push_back(PlayerBot.Pawn);
			}

			NavProbe(TickNow, NavPawns);
			AiProbe(TickNow, NavPawns);
			NavMeshSetup(TickNow, NavPawns);
			BotBrain::RecordSession(TickNow);

			if (!NavPawns.empty())
			{
				BotBrain::gNavTracePawn = NavPawns.front();
				BotBrain::SeedNavMap();

				if (TestOptions().bNavBuild)
				{
					BotBrain::gNavScanBudget = 4000;
					static float LastReport = 0.0f;

					if (TickNow - LastReport > 20.0f)
					{
						LastReport = TickNow;
						LOG_INFO(LogBots, "BotNav build: {} tiles, {} nodes, {} frontier, {} rays; coarse {} tiles, {} nodes, {} frontier, lattice {}", (int)BotBrain::gNavTiles.size(), BotBrain::gNavNodes, (int)BotBrain::gNavFrontier.size(), BotBrain::gNavRays, (int)BotBrain::gCoarseTiles.size(), BotBrain::gCoarseNodes, (int)BotBrain::gCoarseFrontier.size(), BotBrain::gCoarseLattice);
					}
				}

				BotBrain::StepNavMap();

				if (TestOptions().bNavBuild && BotBrain::gNavScanDone && BotBrain::gCoarseDone)
				{
					static float DoneAt = 0.0f;

					if (DoneAt <= 0.0f)
						DoneAt = TickNow;
					else if (TickNow - DoneAt > 3.0f)
					{
						LOG_INFO(LogBots, "BotNav build: finished, closing the server");
						spdlog::apply_all([](std::shared_ptr<spdlog::logger> Logger) { Logger->flush(); });
						ExitProcess(0);
					}
				}

				static float NextGridLog = 0.0f;

				if (TickNow >= NextGridLog)
				{
					NextGridLog = TickNow + 60.0f;
					LOG_INFO(LogBots, "BotNavGrid stats: {} tiles ({} nodes, {} frontier, {} rays), {} queries: {} routes, {} unreachable goals, {} no opinion, {:.1f} ms spent in A* (no opinion because: outside windows {}, no node under the bot {}, same node {}, goal window not done {}, no rim {})", (int)BotBrain::gNavTiles.size(), BotBrain::gNavNodes, (int)BotBrain::gNavFrontier.size(), BotBrain::gNavRays, BotBrain::gGridCalls, BotBrain::gGridRoutes, BotBrain::gGridUnreach, BotBrain::gGridNoOpinion, BotBrain::gGridTimeUs / 1000.0f, BotBrain::gGridNoWindow, BotBrain::gGridNoStart, BotBrain::gGridSame, BotBrain::gGridNotDone, BotBrain::gGridRimless);
				}
			}
		}

		{
			static float NextPhaseLogTime = 0.0f;
			const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

			if (NextPhaseLogTime <= Now)
			{
				NextPhaseLogTime = Now + 2.0f;
				AActor* Aircraft = GetCurrentAircraft(GameState);
				const FVector Bus = Aircraft ? Aircraft->GetActorLocation() : FVector();
				LOG_INFO(LogBots, "BotPhase phase={} step={} aircraft={} bus=({:.0f}, {:.0f}, {:.0f})", (int)GameState->GetGamePhase(), (int)GameState->GetGamePhaseStep(), Aircraft != nullptr, Bus.X, Bus.Y, Bus.Z);
			}
		}

		// auto AllBuildingContainers = UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingContainer::StaticClass());

		// for (int i = 0; i < GameMode->GetAlivePlayers().Num(); ++i)
		const double FrameStart = PerfNowMs();
		gTracesFrame = 0;
		static size_t RotateStart = 0;
		const size_t BotCount = AllPlayerBotsToTick.size();
		RotateStart = (RotateStart + 7) % BotCount; // a different bot goes first each frame, so the budget does not always run out on the same ones

		for (size_t BotIndex = 0; BotIndex < BotCount; ++BotIndex)
		{
			auto& PlayerBot = AllPlayerBotsToTick[(RotateStart + BotIndex) % BotCount];
			auto CurrentPlayer = PlayerBot.Controller;

			if (!CurrentPlayer || CurrentPlayer->IsActorBeingDestroyed())
				continue;

			if (PlayerBot.bDead)
				continue;

			BOT_CRUMB("Tick:bot", PlayerBot.Id);

			auto CurrentPawn = PlayerBot.Pawn; // controller has no pawn once server simulation is enabled

			// The engine can destroy a bot's pawn; never touch a stale pointer.
			if (CurrentPawn && !CurrentPawn->IsValidLowLevel())
			{
				LOG_WARN(LogBots, "Bot {} pawn is no longer valid, dropping it", PlayerBot.Id);
				void* DeadPawnKey = PlayerBot.Pawn;
				PlayerBot.Pawn = nullptr;
				CurrentPawn = nullptr;

				BOT_CRUMB("Tick:bot-death", PlayerBot.Id);
				HandleBotDeath(PlayerBot, DeadPawnKey, GameMode, GameState, PlayerBot.LastLoggedLocation);
				continue;
			}

			if (CurrentPawn)
				DeathDiagHealth(PlayerBot, CurrentPawn, CurrentPawn->GetHealth());

			if (CurrentPawn && CurrentPawn->GetHealth() <= 0.0f)
			{
				BOT_CRUMB("Tick:bot-death", PlayerBot.Id);
				DeathDiagOnDeath(PlayerBot, CurrentPawn);
				HandleBotDeath(PlayerBot, CurrentPawn, GameMode, GameState, CurrentPawn->GetActorLocation());
				continue;
			}

			// EXPERIMENT: leave the bus the way Reboot's late-game path does for real players (RestartPlayer + teleport to the aircraft)
			if (GameState->GetGamePhase() == EAthenaGamePhase::Aircraft && !PlayerBot.bJumpedFromBus)
			{
				const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());
				AActor* Aircraft = GetCurrentAircraft(GameState);

				if (Aircraft && BotBrain::ShouldJump(PlayerBot.Brain, Aircraft->GetActorLocation(), Now, PlayerBot.Id, BotBrain::GetActorYaw(Aircraft)))
				{
					BOT_CRUMB("Tick:bot-jump", PlayerBot.Id);
					PlayerBot.bJumpedFromBus = true;

					const FVector Before = CurrentPawn ? CurrentPawn->GetActorLocation() : FVector();
					const FVector BusLoc = Aircraft->GetActorLocation();

					if (CurrentPawn && !CurrentPawn->IsActorBeingDestroyed())
					{
						// TeleportTo refuses a spot another pawn already occupies, and every bot leaves from the same bus position: 26 of 94 bots
						// stayed on the spawn island (then "glided" down at once and fought each other with pickaxes on the floor of the bus phase).
						// Spread the exit spots and try again if one is taken; as a last resort move the pawn without any collision check.
						bool bMoved = false;

						for (int Try = 0; Try < 8 && !bMoved; ++Try)
						{
							const float Angle = BotBrain::RandRange(0.0f, 6.2831853f);
							const float Radius = BotBrain::RandRange(0.0f, 500.0f + 200.0f * Try);
							const FVector Spot{ BusLoc.X + std::cos(Angle) * Radius, BusLoc.Y + std::sin(Angle) * Radius, BusLoc.Z + BotBrain::RandRange(-250.0f, 250.0f) };
							bMoved = CurrentPawn->TeleportTo(Spot, FRotator());
						}

						if (!bMoved)
						{
							static auto SetLocationFn = FindObject<UFunction>(L"/Script/Engine.Actor.K2_SetActorLocation");

							if (SetLocationFn)
							{
								std::vector<uint8_t> Params(SetLocationFn->GetPropertiesSize() + 64, 0);
								static int NewLocationOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "NewLocation");
								static int SweepOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "bSweep");
								static int TeleportOffset = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "bTeleport");
								*(FVector*)(Params.data() + NewLocationOffset) = BusLoc;
								Params[SweepOffset] = 0;
								Params[TeleportOffset] = 1;
								CurrentPawn->ProcessEvent(SetLocationFn, Params.data());
							}

							LOG_WARN(LogBots, "BotJump #{}: every teleport to the bus was refused, moved it without a collision check", PlayerBot.Id);
						}

						static auto CharacterMovementOffset = CurrentPawn->GetOffset("CharacterMovement");

						if (auto CharacterMovement = CurrentPawn->Get<UObject*>(CharacterMovementOffset))
						{
							static auto VelocityOffset = CharacterMovement->GetOffset("Velocity");
							CharacterMovement->Get<FVector>(VelocityOffset) = FVector();
						}

						if (auto ModePtr = GetMovementModePtr(CurrentPawn))
							*ModePtr = 3; // MOVE_Falling

						BotBrain::OnJump(PlayerBot.Brain, CurrentPawn, BusLoc, Now, PlayerBot.Id);

						const FVector After = CurrentPawn->GetActorLocation();

						LOG_INFO(LogBots, "BotJump before=({:.0f}, {:.0f}, {:.0f}) bus=({:.0f}, {:.0f}, {:.0f}) afterTeleport=({:.0f}, {:.0f}, {:.0f})",
							Before.X, Before.Y, Before.Z, BusLoc.X, BusLoc.Y, BusLoc.Z, After.X, After.Y, After.Z);
					}
					else
					{
						LOG_WARN(LogBots, "BotJump: bot has no valid pawn!");
					}

					continue;
				}
			}

			// The engine marks every alive player as in the aircraft when the bus starts and, when it ends, spawns a fresh pawn for each
			// of them. Bots have their own pawn, so they must not count (the flood of spawns is what left real players pawnless).
			if (auto AircraftState = Cast<AFortPlayerStateAthena>(PlayerBot.Controller->GetPlayerState()))
			{
				if (AircraftState->IsInAircraft())
					BotBrain::SetPawnBitOn(AircraftState, "bInAircraft", false);
			}

			// Never left the bus (phase moved on): drop the bot near a chest so it still joins the match.
			if ((int)GameState->GetGamePhase() >= (int)EAthenaGamePhase::SafeZones && !PlayerBot.bJumpedFromBus && CurrentPawn && !CurrentPawn->IsActorBeingDestroyed())
			{
				BOT_CRUMB("Tick:bot-latedrop", PlayerBot.Id);
				PlayerBot.bJumpedFromBus = true;

				const FVector Drop = BotBrain::PickLateDropLocation(PlayerBot.Brain, FVector(), PlayerBot.Id);
				CurrentPawn->TeleportTo(Drop, FRotator());

				static auto LateMovementOffset = CurrentPawn->GetOffset("CharacterMovement");

				if (auto CharacterMovement = CurrentPawn->Get<UObject*>(LateMovementOffset))
				{
					static auto LateVelocityOffset = CharacterMovement->GetOffset("Velocity");
					CharacterMovement->Get<FVector>(LateVelocityOffset) = FVector();
				}

				if (auto ModePtr = GetMovementModePtr(CurrentPawn))
					*ModePtr = 3; // MOVE_Falling

				BotBrain::OnJump(PlayerBot.Brain, CurrentPawn, Drop, TickNow, PlayerBot.Id);
				LOG_INFO(LogBots, "Bot #{} was still on the bus, late drop at ({:.0f}, {:.0f}, {:.0f})", PlayerBot.Id, Drop.X, Drop.Y, Drop.Z);
				continue;
			}

			if (!CurrentPawn || CurrentPawn->IsActorBeingDestroyed())
				continue;

			auto CurrentPlayerState = Cast<AFortPlayerStateAthena>(CurrentPlayer->GetPlayerState());

			if (!CurrentPlayerState 
				// || !CurrentPlayerState->IsBot()
				)
				continue;

			{
				const float Now = UGameplayStatics::GetTimeSeconds(GetWorld());

				if (PlayerBot.NextLogTime <= Now)
				{
					BOT_CRUMB("Tick:bot-log", PlayerBot.Id);
					const FVector Loc = CurrentPawn->GetActorLocation();
					const float Dx = Loc.X - PlayerBot.LastLoggedLocation.X;
					const float Dy = Loc.Y - PlayerBot.LastLoggedLocation.Y;
					const float Dz = Loc.Z - PlayerBot.LastLoggedLocation.Z;

					if (PlayerBot.LogCounter++ % 15 == 0)
						LOG_INFO(LogBots, "Bot #{} pos=({:.0f}, {:.0f}, {:.0f}) moved/s=({:.0f}, {:.0f}, {:.0f}) inAircraft={} mode={} brain={}", PlayerBot.Id,
							Loc.X, Loc.Y, Loc.Z, Dx, Dy, Dz, CurrentPlayerState->IsInAircraft(), GetMovementModePtr(CurrentPawn) ? (int)*GetMovementModePtr(CurrentPawn) : -1, BotBrainStateName(PlayerBot.Brain.State));

					PlayerBot.LastLoggedLocation = Loc;
					PlayerBot.NextLogTime = Now + 1.0f;
				}
			}

			if (GameState->GetGamePhase() == EAthenaGamePhase::Warmup)
			{
				/* if (!CurrentPlayer->IsPlayingEmote())
				{
					static auto AthenaDanceItemDefinitionClass = FindObject<UClass>("/Script/FortniteGame.AthenaDanceItemDefinition");
					auto RandomDanceID = GetRandomObjectOfClass(AthenaDanceItemDefinitionClass);

					CurrentPlayer->ServerPlayEmoteItemHook(CurrentPlayer, RandomDanceID);
				} */
			}	

			if (PlayerBot.bIsAthenaController && CurrentPlayerState->IsInAircraft() && !CurrentPlayerState->HasThankedBusDriver())
			{
				static auto ServerThankBusDriverFn = FindObject<UFunction>(L"/Script/FortniteGame.FortPlayerControllerAthena.ServerThankBusDriver");
				CurrentPlayer->ProcessEvent(ServerThankBusDriverFn);
			}

			BOT_CRUMB("Tick:bot-brain", PlayerBot.Id);
			const double BrainStart = PerfNowMs();
			BotBrain::Update(PlayerBot.Brain, CurrentPawn, CurrentPlayerState, PlayerBot.Controller, Cast<AFortPlayerControllerAthena>(PlayerBot.Controller) ? Cast<AFortPlayerControllerAthena>(PlayerBot.Controller)->GetWorldInventory() : nullptr, PlayerBot.Id, GameState->GetGamePhase(), TickNow, PlayerBot.bJumpedFromBus, PerfNowMs() - FrameStart < gDecisionBudgetMs);
			gPerf.BrainFrame += PerfNowMs() - BrainStart;

			/* bool bShouldJumpFromBus = CurrentPlayerState->IsInAircraft(); // TODO (Milxnor) add a random percent thing

			if (bShouldJumpFromBus)
			{
				CurrentPlayer->ServerAttemptAircraftJumpHook(CurrentPlayer, FRotator());
			} */
		}

		// AllBuildingContainers.Free();
	}
}

namespace Bosses
{

}
