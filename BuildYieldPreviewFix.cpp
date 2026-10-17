// SPDX-License-Identifier: AGPL-3.0-only
#include "GameCorePlugin.h"
#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// A GameCore Mod Loader plugin (see the GCML repository's README): the loader calls GcmlAttach every time
// GameCore is loaded while the mod is enabled, and the patch below goes into that copy of GameCore.
// It goes away with GameCore's memory, so GcmlDetach has nothing to undo and is not exported.
//

// The problem: Unit::Operations::XP2::BuildImprovement::CanStart (GameCore+0x747f90) writes the
// tooltip lines of a builder's improvement button. For every yield type it calls
// Map::Improvement::Yields::GetYieldFrom (GameCore+0x2605b0) with a contribution list, and prints
// "+N <yield>" when N > 0. With the list, GetYieldFrom returns
//     the improvement's own yield (base + adjacency + tech and civic bonuses)
//   + every plot-yield modifier the tile would get with the improvement on it.
// The tile's real yield, Rules::Yields::Instance::GetYield (GameCore+0x37ec60), adds the tile's
// current modifier total (the tile's PlotYieldData, kept by the effect system) and the current
// improvement's own yield. After the build, the new improvement's own yield replaces the old one,
// and the modifier total becomes what the tooltip computed. So the tooltip:
//   - counts the modifiers the tile already gets again (in a late game: Lady of the Reeds, Earth
//     Goddess, the Watermill's wheat and rice, Lighthouse coast food, Petra, Russia's tundra...);
//   - does not take off the improvement it replaces, nor the modifiers tied to that one;
//   - never shows a yield that goes down: the line is skipped when N <= 0, and the text
//     (LOC_OPERATION_BUILD_IMPROVEMENT_YIELDS, "+{1_Amount} ...") has a fixed plus sign.
//
// The fix: the tooltip's GetYieldFrom call goes to TooltipYieldChange, which subtracts the tile's
// current modifier total and the current improvement's own yield, the same parts GetYield adds.
// The line is skipped only when the change is 0, and it uses the game's LOC_TYPE_TRAIT_YIELD
// ("{1_Amount: number +#,###;-#,###} {2_Icon} {3_Name}"), which takes the same three values and
// prints the sign.
//
// Not covered: modifiers that the new improvement brings with it (ImprovementModifiers, such as
// the Lumber Mill's river bonus) start only once it is built, so neither the game's preview nor
// this one has them; yield changes on the neighbouring tiles (appeal, adjacency) are not part of
// this tooltip at all.
//
// What the game's code does (Steam build 15038592, read 2026-10-17):
//   CanStart's yield loop:
//     74822e  mov [rsp+0x20], rax                  the contribution list
//     748233  call GetYieldFrom(yields, improvement, yieldType, plot, list)
//     748238  mov [rsp+0x30], eax
//     74823c  test eax, eax
//     74823e  jle skip                             (7E 62)
//     748240  lea rdx, [LOC_OPERATION_BUILD_IMPROVEMENT_YIELDS]
//     After the call only eax and callee-saved registers are read.
//   GetYield's improvement part (skipped when plot+0x44 is -1 or plot+0x4d bit 0, pillaged):
//     if the improvement's yield list (definition +0x218..+0x220, entries -> +0x38 yield
//     definition -> +0x14 yield index) has the yield: GetYieldFrom(yields, imp, yield, plot, null);
//     if NameToYieldType(definition +0x28) is the yield: appeal * (definition +0x60) / 100.
//   GetYield's modifier part: Map::CityPlots::GetPlotYieldData(Map::GetCityPlots(map), plot index)
//     -> GameEffects::PlotYieldData::GetPlotYield(yield), 0 when the tile has no data.

namespace {
	// ===== The game's code =====

	constexpr uintptr_t kTooltipSiteRva = 0x74822e;
	constexpr uintptr_t kYieldCallOffset = 0x748233 - kTooltipSiteRva;
	constexpr uintptr_t kSkipBranchOffset = 0x74823e - kTooltipSiteRva;
	constexpr uintptr_t kTagDisplacementOffset = 0x748243 - kTooltipSiteRva;
	const uint8_t kTooltipSite[] = {
		0x48, 0x89, 0x44, 0x24, 0x20,         // mov [rsp+0x20], rax
		0xE8, 0x78, 0x83, 0xB1, 0xFF,         // call GetYieldFrom
		0x89, 0x44, 0x24, 0x30,               // mov [rsp+0x30], eax
		0x85, 0xC0,                           // test eax, eax
		0x7E, 0x62,                           // jle skip
		0x48, 0x8D, 0x15, 0xC1, 0x05, 0x2E, 0x00,   // lea rdx, [the tag]
	};
	constexpr uint8_t kJleShort = 0x7E;
	constexpr uint8_t kJeShort = 0x74;
	constexpr uintptr_t kGameTagRva = 0xa28808;
	const char kGameTag[] = "LOC_OPERATION_BUILD_IMPROVEMENT_YIELDS";
	const char kSignedTag[] = "LOC_TYPE_TRAIT_YIELD";

	constexpr uintptr_t kGetYieldFromRva = 0x2605b0;
	constexpr uintptr_t kEditMapRva = 0x44d40;
	constexpr uintptr_t kGetCityPlotsRva = 0x52ab0;
	constexpr uintptr_t kPlotIndexRva = 0x5f2d0;
	constexpr uintptr_t kGetPlotYieldDataRva = 0x4c240;
	constexpr uintptr_t kGetPlotYieldRva = 0x954be0;
	constexpr uintptr_t kImprovementDefinitionsRva = 0x7d0ee0;
	constexpr uintptr_t kNameToYieldTypeRva = 0x7d8d70;

	typedef int32_t (*GetYieldFromFn)(void* yields, int32_t improvement, int32_t yieldType,
		void* plot, void* contributions);
	typedef void* (*EditMapFn)();
	typedef void* (*GetCityPlotsFn)(void* map);
	typedef int32_t (*PlotIndexFn)(void* plot);
	typedef const void* (*GetPlotYieldDataFn)(void* cityPlots, int32_t plotIndex);
	typedef int32_t (*GetPlotYieldFn)(const void* plotYieldData, int32_t yieldType);
	struct DefinitionList {
		uint8_t** begin;
		uint8_t** end;
	};
	typedef const DefinitionList* (*ImprovementDefinitionsFn)();
	typedef int32_t (*NameToYieldTypeFn)(uint32_t nameHash);
	typedef int32_t (*PlotAppealFn)(void* plot);

	GetYieldFromFn gGetYieldFrom = nullptr;
	EditMapFn gEditMap = nullptr;
	GetCityPlotsFn gGetCityPlots = nullptr;
	PlotIndexFn gPlotIndex = nullptr;
	GetPlotYieldDataFn gGetPlotYieldData = nullptr;
	GetPlotYieldFn gGetPlotYield = nullptr;
	ImprovementDefinitionsFn gImprovementDefinitions = nullptr;
	NameToYieldTypeFn gNameToYieldType = nullptr;

	// Map::Plot::Instance
	constexpr size_t kPlotImprovementOffset = 0x44;   // int16, -1 = none
	constexpr size_t kPlotFlagsOffset = 0x4d;
	constexpr uint8_t kPlotPillagedBit = 0x01;
	constexpr size_t kPlotAppealSlot = 0x28 / sizeof(void*);   // virtual: int Appeal()
	// An improvement definition
	constexpr size_t kImprovementYieldsBeginOffset = 0x218;
	constexpr size_t kImprovementYieldsEndOffset = 0x220;
	constexpr size_t kYieldEntryDefinitionOffset = 0x38;
	constexpr size_t kYieldDefinitionIndexOffset = 0x14;
	constexpr size_t kImprovementAppealYieldOffset = 0x28;
	constexpr size_t kImprovementAppealPercentOffset = 0x60;
	constexpr int32_t kNoImprovement = -1;

	// ===== What the tile has now =====

	// The plot-yield modifiers the tile gets now: the part GetYield adds from the effect system.
	int32_t ModifierYieldNow(void* plot, int32_t yieldType) {
		void* map = gEditMap();
		if (!map) {
			return 0;
		}
		const void* data = gGetPlotYieldData(gGetCityPlots(map), gPlotIndex(plot));
		return data ? gGetPlotYield(data, yieldType) : 0;
	}

	// True when the improvement definition lists this yield, the test GetYield makes before it
	// counts an improvement's own yield.
	bool ListsYield(const uint8_t* definition, int32_t yieldType) {
		uint8_t* const* entry = *(uint8_t* const* const*)(definition
			+ kImprovementYieldsBeginOffset);
		uint8_t* const* end = *(uint8_t* const* const*)(definition + kImprovementYieldsEndOffset);
		for (; entry != end; entry++) {
			const uint8_t* yieldDefinition = *(const uint8_t* const*)(*entry
				+ kYieldEntryDefinitionOffset);
			if (yieldDefinition
				&& *(const int32_t*)(yieldDefinition + kYieldDefinitionIndexOffset) == yieldType) {
				return true;
			}
		}
		return false;
	}

	// The yield the tile's current improvement gives it now, as GetYield counts it: its own
	// yield (base, adjacency, tech and civic bonuses) and its yield from appeal. 0 without an
	// improvement or when it is pillaged.
	int32_t ImprovementYieldNow(void* yields, void* plot, int32_t yieldType) {
		const uint8_t* plotBytes = (const uint8_t*)plot;
		const int32_t improvement = *(const int16_t*)(plotBytes + kPlotImprovementOffset);
		if (improvement == kNoImprovement
			|| (plotBytes[kPlotFlagsOffset] & kPlotPillagedBit)) {
			return 0;
		}
		const DefinitionList* definitions = gImprovementDefinitions();
		if (improvement < 0 || improvement >= definitions->end - definitions->begin) {
			return 0;
		}
		const uint8_t* definition = definitions->begin[improvement];
		if (!definition) {
			return 0;
		}
		int32_t yield = 0;
		if (ListsYield(definition, yieldType)) {
			yield += gGetYieldFrom(yields, improvement, yieldType, plot, nullptr);
		}
		const uint32_t appealYield = *(const uint32_t*)(definition
			+ kImprovementAppealYieldOffset);
		if (gNameToYieldType(appealYield) == yieldType) {
			const PlotAppealFn appeal = (*(PlotAppealFn* const*)plot)[kPlotAppealSlot];
			const int32_t percent = *(const int32_t*)(definition
				+ kImprovementAppealPercentOffset);
			yield += appeal(plot) * percent / 100;
		}
		return yield;
	}

	// ===== Patching =====

	// Reserves one executable page as close to `target` as possible, so that a 32-bit call or
	// rip-relative address at `target` reaches it. nullptr when no page is free within reach.
	void* AllocateNear(uintptr_t target) {
		// How far from the target the page may be. A displacement is 32 bits.
		constexpr uintptr_t kMaxDistance = 0x70000000;
		SYSTEM_INFO systemInfo;
		GetSystemInfo(&systemInfo);
		const uintptr_t granularity = systemInfo.dwAllocationGranularity;
		for (uintptr_t offset = granularity; offset < kMaxDistance; offset += granularity) {
			for (int direction = 0; direction < 2; direction++) {
				const uintptr_t raw = direction ? target + offset : target - offset;
				const uintptr_t address = raw & ~(granularity - 1);
				void* page = VirtualAlloc((void*)address, 0x1000, MEM_RESERVE | MEM_COMMIT,
					PAGE_EXECUTE_READWRITE);
				if (page) {
					return page;
				}
			}
		}
		return nullptr;
	}

	// ===== The patched call =====

	// Called by the tooltip in place of GetYieldFrom: the change the build makes to the tile's
	// yield.
	int32_t TooltipYieldChange(void* yields, int32_t improvement, int32_t yieldType, void* plot,
		void* contributions) {
		const int32_t preview = gGetYieldFrom(yields, improvement, yieldType, plot,
			contributions);
		if (!plot) {
			return preview;
		}
		return preview - ModifierYieldNow(plot, yieldType)
			- ImprovementYieldNow(yields, plot, yieldType);
	}

	// The page next to GameCore that the patched call and the new tag go through: a 32-bit
	// call or rip-relative address in GameCore cannot reach this DLL. One page per GameCore
	// address; a reload at the same address reuses it.
	constexpr size_t kStubOffset = 0;
	constexpr size_t kTagOffset = 0x40;
	uint8_t* gNearPage = nullptr;
	uintptr_t gNearPageFor = 0;

	uint8_t* PrepareNearPage(uintptr_t gameCoreBase) {
		if (gNearPage && gNearPageFor == gameCoreBase) {
			return gNearPage;
		}
		uint8_t* page = (uint8_t*)AllocateNear(gameCoreBase + kTooltipSiteRva);
		if (!page) {
			return nullptr;
		}
		// movabs rax, TooltipYieldChange / jmp rax: the return address stays the tooltip's.
		const uint8_t movRax[] = { 0x48, 0xB8 };
		const uint8_t jmpRax[] = { 0xFF, 0xE0 };
		const uint64_t target = (uint64_t)(uintptr_t)&TooltipYieldChange;
		uint8_t* cursor = page + kStubOffset;
		memcpy(cursor, movRax, sizeof(movRax));
		cursor += sizeof(movRax);
		memcpy(cursor, &target, sizeof(target));
		cursor += sizeof(target);
		memcpy(cursor, jmpRax, sizeof(jmpRax));
		memcpy(page + kTagOffset, kSignedTag, sizeof(kSignedTag));
		FlushInstructionCache(GetCurrentProcess(), page, kTagOffset);
		gNearPage = page;
		gNearPageFor = gameCoreBase;
		return page;
	}

	// The 32-bit displacement from the end of a 4-byte field at `field` to `target`.
	int32_t DisplacementTo(const uint8_t* field, const void* target) {
		return (int32_t)((int64_t)(uintptr_t)target - (int64_t)((uintptr_t)field + 4));
	}

	bool CodeIsKnownBuild(uintptr_t gameCoreBase) {
		const uint8_t* site = (const uint8_t*)(gameCoreBase + kTooltipSiteRva);
		if (memcmp(site, kTooltipSite, sizeof(kTooltipSite)) != 0) {
			return false;
		}
		const char* tag = (const char*)(gameCoreBase + kGameTagRva);
		return memcmp(tag, kGameTag, sizeof(kGameTag)) == 0;
	}

	void PatchTooltip(uintptr_t gameCoreBase, uint8_t* nearPage) {
		uint8_t* site = (uint8_t*)(gameCoreBase + kTooltipSiteRva);
		DWORD oldProtect = 0;
		VirtualProtect(site, sizeof(kTooltipSite), PAGE_EXECUTE_READWRITE, &oldProtect);
		uint8_t* callDisplacement = site + kYieldCallOffset + 1;
		*(int32_t*)callDisplacement = DisplacementTo(callDisplacement, nearPage + kStubOffset);
		site[kSkipBranchOffset] = kJeShort;
		uint8_t* tagDisplacement = site + kTagDisplacementOffset;
		*(int32_t*)tagDisplacement = DisplacementTo(tagDisplacement, nearPage + kTagOffset);
		VirtualProtect(site, sizeof(kTooltipSite), oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), site, sizeof(kTooltipSite));
	}

	void Log(const GcmlHost* host, const char* fmt, ...) {
		char line[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf(line, sizeof(line), fmt, args);
		va_end(args);
		host->Log(line);
	}

	// Installs the patch into this copy of GameCore. Returns false when it is not the build the
	// patch was written for, or no memory near it could be had; the game is then left as it is.
	bool Install(const GcmlHost* host, uintptr_t gameCoreBase) {
		if (!CodeIsKnownBuild(gameCoreBase)) {
			Log(host, "BuildYieldPreviewFix: GameCore+0x%llx does not look like the builder's "
				"improvement tooltip (other game build?): left alone",
				(unsigned long long)kTooltipSiteRva);
			return false;
		}
		uint8_t* nearPage = PrepareNearPage(gameCoreBase);
		if (!nearPage) {
			Log(host, "BuildYieldPreviewFix: no memory near GameCore for the patch: left alone");
			return false;
		}
		gGetYieldFrom = (GetYieldFromFn)(gameCoreBase + kGetYieldFromRva);
		gEditMap = (EditMapFn)(gameCoreBase + kEditMapRva);
		gGetCityPlots = (GetCityPlotsFn)(gameCoreBase + kGetCityPlotsRva);
		gPlotIndex = (PlotIndexFn)(gameCoreBase + kPlotIndexRva);
		gGetPlotYieldData = (GetPlotYieldDataFn)(gameCoreBase + kGetPlotYieldDataRva);
		gGetPlotYield = (GetPlotYieldFn)(gameCoreBase + kGetPlotYieldRva);
		gImprovementDefinitions = (ImprovementDefinitionsFn)(gameCoreBase
			+ kImprovementDefinitionsRva);
		gNameToYieldType = (NameToYieldTypeFn)(gameCoreBase + kNameToYieldTypeRva);
		PatchTooltip(gameCoreBase, nearPage);
		return true;
	}
}

extern "C" __declspec(dllexport) int __cdecl GcmlAttach(const GcmlHost* host) {
	if (!Install(host, (uintptr_t)host->gameCore)) {
		return 0;
	}
	Log(host, "BuildYieldPreviewFix: the builder's improvement tooltip shows the change to the "
		"tile's yields, losses included (GameCore load %u)", host->loadCount);
	return 1;
}
