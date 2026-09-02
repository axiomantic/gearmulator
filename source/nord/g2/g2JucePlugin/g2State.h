/* g2State.h -- the plugin state format.
 *
 * The serialization surface the plugin track owns. The Device's
 * getState/setState overrides (g2Device.cpp) delegate here; the hand-off pair
 * -- m_ready false, wait for m_inCallback clear -- runs in the Device before
 * either delegate, so this file does not restate it.
 *
 * The state items, exhaustive, defined here and nowhere else. The resampler's
 * block accumulator is deliberately absent: it lives in the framework's
 * ResamplerInOut, above the Device, and is re-created rather than saved.
 *
 *   1. The performance: all four slots, plus the global settings.
 *   2. Each slot's patch, as the protocol's own byte blob.
 *   3. A patch identifier for each slot (a hash of the patch's module and
 *      cable lists -- not the patch name).
 *   4. The parameter-slot bindings.
 *   5. The firmware version word, raw, 16-bit.
 *   6. A state format version.
 *   7. The parameter overflow count.
 *
 * The override contract the whole file exists to keep: getState appends to
 * the vector it is given, through insert() and push_back(), and never
 * assigns it. The synthLib::Plugin layer prepends its own header (plugin.cpp
 * pushes g_stateVersion and the StateType) before the Device is called; an
 * assign() overwrites that header in silence and the corrupted state then
 * loads as a different format version or fails, with no error at the point
 * of the defect.
 *
 * The wire format. One little-endian byte layout, not a struct dump:
 *
 *   u8x4 magic 'G','2','S','T'
 *   u16  stateFormatVersion            -- item 6, g_stateFormatVersion below
 *   u16  firmwareVersionWord           -- item 5, the word the writer ran
 *   u32  parameterOverflowCount        -- item 7
 *   per slot, g_stateSlotCount times:  -- items 2 and 3
 *     u32 patchByteCount; u8 patch[patchByteCount]; u16 patchId
 *   u32 bindingsByteCount; u8 bindings[bindingsByteCount]   -- item 4
 *   u32 performanceByteCount; u8 performance[...]           -- item 1
 *
 * A reader refuses an image whose headers do not check out -- wrong magic,
 * a format version it does not write, a geometry that overruns or leaves
 * bytes over -- whole, before any output value is written, so a refused load
 * changes nothing.
 *
 * The version-mismatch policy, decided through decideFirmwareVersion():
 * matching versions load normally; differing versions load the machine,
 * withhold the patch data, and return a message naming both versions with an
 * offer to load anyway. Never reinterpreted silently.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace g2
{
	/* Item 6. The version of the byte layout above, independent of the
	 * firmware version word (item 5): the format version tracks this file's
	 * layout, the firmware word tracks the machine's OS. Bumping one is not
	 * bumping the other. */
	constexpr uint16_t g_stateFormatVersion = 1;

	/* The magic the image opens with, checked before any length arithmetic. */
	constexpr uint8_t g_stateMagic[4] = {'G', '2', 'S', 'T'};

	/* The performance's slots, item 1. */
	constexpr size_t g_stateSlotCount = 4;

	/* What one restoration parsed and decided.
	 *
	 * The payload members hold what the image contained, whatever the
	 * version decision was; `patchDataWithheld` says whether the caller may
	 * apply the patch-shaped ones. On a firmware-version mismatch the
	 * machine is loaded and the patch data is not -- the caller applies the
	 * payload only after the user takes the offer, and the withheld flag is
	 * what stops a silent application. */
	struct StateLoadResult
	{
		bool machineLoaded = false;
		bool patchLoaded = false;
		bool offerToLoadAnyway = false;
		bool patchDataValid = false;      // the image parsed whole
		bool patchDataWithheld = false;   // parsed but not applied -- the mismatch row

		std::vector<uint8_t> performance;               // item 1
		std::vector<std::vector<uint8_t>> slotPatches;  // item 2
		std::vector<uint16_t> slotPatchIds;             // item 3
		std::vector<uint8_t> parameterBindings;         // item 4
		uint32_t parameterOverflowCount = 0;            // item 7

		/* Empty except on a mismatch, where it names both versions. The
		 * no-firmware row carries no message here: that text is owned
		 * elsewhere, and two texts for one state are two texts that
		 * drift. */
		std::string message;
	};

	/* The data the Device holds behind the state items. */
	struct StateData
	{
		std::vector<uint8_t> performance;
		std::vector<std::vector<uint8_t>> slotPatches;
		std::vector<uint16_t> slotPatchIds;
		std::vector<uint8_t> parameterBindings;
		uint32_t parameterOverflowCount = 0;
	};

	/* Serializes the state items and appends them to _state.
	 *
	 * The contract: this function never assigns _state. The Plugin layer has
	 * already pushed its header into it; the first write here is push_back
	 * and every later one is an append.
	 *
	 * Returns false when the slot vectors do not hold exactly
	 * g_stateSlotCount entries -- a caller asking to save a shape the format
	 * does not have is a caller bug, not a truncated image. */
	bool serializeState(std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _performance,
		const std::vector<std::vector<uint8_t>>& _slotPatches,
		const std::vector<uint16_t>& _slotPatchIds,
		const std::vector<uint8_t>& _parameterBindings,
		uint16_t _firmwareVersionWord,
		uint32_t _parameterOverflowCount);

	/* Parses an image the serializer wrote and decides the load.
	 *
	 * The out-parameters are written only when every header checked out and
	 * the versions matched: a refusal or a mismatch writes none of them, so
	 * a refused load changes nothing. On a mismatch the parsed payload
	 * travels in the returned result instead, marked withheld.
	 *
	 * _firmwarePresent and _machineFirmwareVersionWord come from the
	 * FirmwareStatus the Device resolved once at construction;
	 * decideFirmwareVersion owns the version decision and the mismatch
	 * message. */
	StateLoadResult deserializeState(const std::vector<uint8_t>& _state,
		std::vector<uint8_t>& _performance,
		std::vector<std::vector<uint8_t>>& _slotPatches,
		std::vector<uint16_t>& _slotPatchIds,
		std::vector<uint8_t>& _parameterBindings,
		uint32_t& _parameterOverflowCount,
		bool _firmwarePresent,
		uint16_t _machineFirmwareVersionWord);
}
