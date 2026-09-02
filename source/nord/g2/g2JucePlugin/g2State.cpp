/* The plugin state format's body: the single definition site of the item
 * layout, the insert-never-assign rule, and the version-mismatch policy
 * wired here from g2Lib/firmwareVersion.h.
 *
 * The format is what this file declares, and the round trip through it is
 * exact.
 */

#include "g2State.h"

#include "firmwareVersion.h"

#include <utility>

namespace g2
{
	namespace
	{
		/* Little-endian appenders. The format is a byte layout, not a struct
		 * dump, so padding and endianness cannot drift between hosts. */
		void appendU16(std::vector<uint8_t>& _dst, const uint16_t _v)
		{
			_dst.push_back(static_cast<uint8_t>(_v & 0xFFu));
			_dst.push_back(static_cast<uint8_t>(_v >> 8));
		}

		void appendU32(std::vector<uint8_t>& _dst, const uint32_t _v)
		{
			_dst.push_back(static_cast<uint8_t>(_v & 0xFFu));
			_dst.push_back(static_cast<uint8_t>((_v >> 8) & 0xFFu));
			_dst.push_back(static_cast<uint8_t>((_v >> 16) & 0xFFu));
			_dst.push_back(static_cast<uint8_t>((_v >> 24) & 0xFFu));
		}

		void appendBytes(std::vector<uint8_t>& _dst, const std::vector<uint8_t>& _src)
		{
			_dst.insert(_dst.end(), _src.begin(), _src.end());
		}

		/* A bounds-checked cursor over the image. Every read checks the
		 * remaining length first, so a truncated image is refused whole --
		 * BEFORE the first output value is written -- and a refused load
		 * changes nothing. */
		struct Reader
		{
			const uint8_t* data = nullptr;
			size_t size = 0;
			size_t pos = 0;

			bool canRead(const size_t _n) const { return pos + _n <= size; }

			bool readU16(uint16_t& _v)
			{
				if(!canRead(2))
					return false;
				_v = static_cast<uint16_t>(data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8));
				pos += 2;
				return true;
			}

			bool readU32(uint32_t& _v)
			{
				if(!canRead(4))
					return false;
				_v = static_cast<uint32_t>(data[pos])
					| (static_cast<uint32_t>(data[pos + 1]) << 8)
					| (static_cast<uint32_t>(data[pos + 2]) << 16)
					| (static_cast<uint32_t>(data[pos + 3]) << 24);
				pos += 4;
				return true;
			}

			bool readBytes(std::vector<uint8_t>& _dst, const uint32_t _count)
			{
				if(!canRead(_count))
					return false;
				_dst.insert(_dst.end(), data + pos, data + pos + _count);
				pos += _count;
				return true;
			}
		};

		/* The refusal value: nothing loaded, nothing offered, no message. A
		 * header that does not check out reaches this and changes nothing. */
		StateLoadResult refused()
		{
			return StateLoadResult{};
		}
	}

	bool serializeState(std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _performance,
		const std::vector<std::vector<uint8_t>>& _slotPatches,
		const std::vector<uint16_t>& _slotPatchIds,
		const std::vector<uint8_t>& _parameterBindings,
		const uint16_t _firmwareVersionWord,
		const uint32_t _parameterOverflowCount)
	{
		// Append, never assign: the Plugin layer has already pushed its own
		// version header into _state (plugin.cpp pushes g_stateVersion and
		// the StateType before the Device is called), and an assignment
		// would overwrite it in silence.
		if(_slotPatches.size() != g_stateSlotCount || _slotPatchIds.size() != g_stateSlotCount)
			return false;

		for(const uint8_t m : g_stateMagic)
			_state.push_back(m);

		appendU16(_state, g_stateFormatVersion);   // item 6
		appendU16(_state, _firmwareVersionWord);   // item 5, the raw word
		appendU32(_state, _parameterOverflowCount);// item 7

		for(size_t i = 0; i < g_stateSlotCount; ++i)   // items 2 and 3, per slot
		{
			appendU32(_state, static_cast<uint32_t>(_slotPatches[i].size()));
			appendBytes(_state, _slotPatches[i]);
			appendU16(_state, _slotPatchIds[i]);
		}

		appendU32(_state, static_cast<uint32_t>(_parameterBindings.size()));
		appendBytes(_state, _parameterBindings);   // item 4

		appendU32(_state, static_cast<uint32_t>(_performance.size()));
		appendBytes(_state, _performance);         // item 1

		return true;
	}

	StateLoadResult deserializeState(const std::vector<uint8_t>& _state,
		std::vector<uint8_t>& _performance,
		std::vector<std::vector<uint8_t>>& _slotPatches,
		std::vector<uint16_t>& _slotPatchIds,
		std::vector<uint8_t>& _parameterBindings,
		uint32_t& _parameterOverflowCount,
		const bool _firmwarePresent,
		const uint16_t _machineFirmwareVersionWord)
	{
		Reader r;
		r.data = _state.data();
		r.size = _state.size();

		uint8_t magic[4] = {};
		if(!r.canRead(std::size(g_stateMagic)))
			return refused();
		for(size_t i = 0; i < std::size(g_stateMagic); ++i)
			magic[i] = r.data[r.pos++];

		for(size_t i = 0; i < std::size(g_stateMagic); ++i)
		{
			if(magic[i] != g_stateMagic[i])
				return refused();
		}

		uint16_t formatVersion = 0;
		uint16_t firmwareVersionWord = 0;
		uint32_t overflowCount = 0;

		if(!r.readU16(formatVersion) || !r.readU16(firmwareVersionWord) || !r.readU32(overflowCount))
			return refused();

		// Item 6, the state format version. A differing version is an image
		// this build will not take back: refused whole, before any output is
		// written, never reinterpreted silently.
		if(formatVersion != g_stateFormatVersion)
			return refused();

		std::vector<std::vector<uint8_t>> slotPatches;
		std::vector<uint16_t> slotPatchIds;

		slotPatches.reserve(g_stateSlotCount);
		slotPatchIds.reserve(g_stateSlotCount);

		for(size_t i = 0; i < g_stateSlotCount; ++i)
		{
			uint32_t patchSize = 0;
			if(!r.readU32(patchSize))
				return refused();
			std::vector<uint8_t> patch;
			if(!r.readBytes(patch, patchSize))
				return refused();
			uint16_t patchId = 0;
			if(!r.readU16(patchId))
				return refused();
			slotPatches.push_back(std::move(patch));
			slotPatchIds.push_back(patchId);
		}

		uint32_t bindingSize = 0;
		if(!r.readU32(bindingSize))
			return refused();
		std::vector<uint8_t> bindings;
		if(!r.readBytes(bindings, bindingSize))
			return refused();

		uint32_t perfSize = 0;
		if(!r.readU32(perfSize))
			return refused();
		std::vector<uint8_t> performance;
		if(!r.readBytes(performance, perfSize))
			return refused();

		if(r.pos != r.size)
			return refused();

		// Every header checked out and the whole image is consumed. Only now
		// does the load begin, and decideFirmwareVersion decides what it
		// loads.
		const FirmwareVersionDecision decision =
			decideFirmwareVersion(_firmwarePresent, _machineFirmwareVersionWord, firmwareVersionWord);

		StateLoadResult result;
		result.message = decision.message;
		result.machineLoaded = decision.loadMachine;
		result.patchLoaded = decision.loadPatchData;
		result.offerToLoadAnyway = decision.offerToLoadAnyway;

		if(!result.machineLoaded)
		{
			// No firmware present: the firmware-state surface carries the
			// message. Nothing loads here.
			return result;
		}

		if(!result.patchLoaded)
		{
			// Versions differ: load the machine, do NOT load the patch data,
			// offer to load anyway. The firmware word and the format version
			// still stand; the patch-shaped items are withheld, so a caller
			// that ignores the offer cannot have applied them in silence.
			result.slotPatches = std::move(slotPatches);
			result.slotPatchIds = std::move(slotPatchIds);
			result.parameterBindings = std::move(bindings);
			result.performance = std::move(performance);
			result.parameterOverflowCount = overflowCount;
			result.patchDataValid = true;
			result.patchDataWithheld = true;
			return result;
		}

		// Matching versions: the whole image applies.
		_performance = std::move(performance);
		_slotPatches = std::move(slotPatches);
		_slotPatchIds = std::move(slotPatchIds);
		_parameterBindings = std::move(bindings);
		_parameterOverflowCount = overflowCount;

		result.patchDataValid = true;

		return result;
	}
}
