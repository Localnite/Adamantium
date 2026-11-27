#pragma once

#include <Windows.h>
#include <iostream>
#include <format>
#include <string>

typedef unsigned short uint16;
typedef unsigned char uint8;
typedef char int8;
typedef short int16;
typedef int int32;
typedef __int64 int64;
typedef unsigned int uint32;
typedef char ANSICHAR;
typedef uint32_t CodeSkipSizeType;
typedef unsigned __int64 uint64;

// IsBadReadPtr is not a safe probe under Wine: for an unmapped address it raises an access violation
// (kernel32+0x20eef) instead of simply returning TRUE. That fault escaped the __try in BotBrain::IsLiveBuilding
// and produced 40 CRASHLOG lines per stale cached actor. VirtualQuery never faults, so use it instead.
inline bool RebootIsReadable(const void* Ptr, size_t Size)
{
	if (!Ptr || !Size) return false;

	const uintptr_t First = (uintptr_t)Ptr;
	const uintptr_t Last = First + Size - 1;
	const uintptr_t PageMask = ~(uintptr_t)0xfff;

	for (uintptr_t Page = First & PageMask; Page <= (Last & PageMask); Page += 0x1000)
	{
		MEMORY_BASIC_INFORMATION Info{};
		if (!VirtualQuery((LPCVOID)Page, &Info, sizeof(Info))) return false;
		if (Info.State != MEM_COMMIT || (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
	}

	return true;
}

extern inline int Engine_Version = 0; // For example, 420, 421, etc. // Prevent using this when possible.
extern inline double Fortnite_Version = 0; // For example, 4.1, 6.21, etc. // Prevent using this when possible.
extern inline int Fortnite_CL = 0;

// #define PROD // this doesnt do anything besides remove processeventhook and some assert stuff
// DEPRECATED ^^^ (see Globals::bDeveloperMode)

// #define ABOVE_S20

struct PlaceholderBitfield
{
	uint8_t First : 1;
	uint8_t Second : 1;
	uint8_t Third : 1;
	uint8_t Fourth : 1;
	uint8_t Fifth : 1;
	uint8_t Sixth : 1;
	uint8_t Seventh : 1;
	uint8_t Eighth : 1;
};

#define MS_ALIGN(n) __declspec(align(n))
#define FORCENOINLINE __declspec(noinline)	

#define ENUM_CLASS_FLAGS(Enum) \
	inline           Enum& operator|=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	inline           Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	inline           Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator| (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator& (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator^ (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	inline constexpr bool  operator! (Enum  E)             { return !(__underlying_type(Enum))E; } \
	inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~(__underlying_type(Enum))E; }

#define UNLIKELY(x)			(x)

inline bool AreVehicleWeaponsEnabled()
{
	return Fortnite_Version > 6;
}

inline bool IsRestartingSupported()
{
	return Engine_Version >= 419 && Engine_Version < 424;
}

/*

enum class AllocatorType
{
	VIRTUALALLOC,
	FMEMORY
};

class InstancedAllocator
{
public:
	AllocatorType allocatorType;

	static void* Allocate(AllocatorType type, size_t Size);

	void* Allocate(size_t Size)
	{
		return Allocate(allocatorType, Size);
	}
};

*/

struct PadHex10 { char Pad[0x10]; };
struct PadHex18 { char Pad[0x18]; };
struct PadHexA8 { char Pad[0xA8]; };
struct PadHexB0 { char Pad[0xB0]; };