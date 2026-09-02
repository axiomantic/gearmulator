#include "g2Parameters.h"

#include <algorithm>

namespace g2
{
	namespace
	{
		uint32_t g_listBuildCount = 0;

		struct NameLists
		{
			std::vector<std::string> slots;
			std::vector<std::string> morphs;

			NameLists()
			{
				slots.reserve(g_automationSlotCount);
				for(size_t i=0; i<g_automationSlotCount; ++i)
					slots.push_back("Param " + std::to_string(i));

				morphs.reserve(g_morphGroupCount);
				for(size_t i=0; i<g_morphGroupCount; ++i)
					morphs.push_back("Morph " + std::to_string(i + 1));

				++g_listBuildCount;
			}
		};

		/* The one build. A function-local static is constructed on first use
		 * and never again, so the pool cannot grow and a second registration
		 * cannot disagree with the first. */
		const NameLists& nameLists()
		{
			static const NameLists lists;
			return lists;
		}
	}

	bool operator==(const PatchParameterId& _a, const PatchParameterId& _b)
	{
		return _a.moduleInstance == _b.moduleInstance && _a.parameterIndex == _b.parameterIndex;
	}

	bool operator!=(const PatchParameterId& _a, const PatchParameterId& _b)
	{
		return !(_a == _b);
	}

	bool operator<(const PatchParameterId& _a, const PatchParameterId& _b)
	{
		if(_a.moduleInstance != _b.moduleInstance)
			return _a.moduleInstance < _b.moduleInstance;
		return _a.parameterIndex < _b.parameterIndex;
	}

	const std::vector<std::string>& automationSlotNames()
	{
		return nameLists().slots;
	}

	const std::vector<std::string>& morphGroupNames()
	{
		return nameLists().morphs;
	}

	uint32_t parameterListBuildCount()
	{
		return g_listBuildCount;
	}

	SlotAllocation allocateSlots(const std::vector<PatchParameterId>& _patchParameters)
	{
		SlotAllocation result;

		/* Every slot free, before one binding is read. This is where the
		 * absence of survivor pinning lives: there is no previous
		 * allocation in scope to carry a slot number over from. */
		result.slots.assign(g_automationSlotCount, SlotBinding{});

		std::vector<PatchParameterId> ordered(_patchParameters);
		std::sort(ordered.begin(), ordered.end());
		ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

		/* The lowest free slot is tracked rather than assumed to be the
		 * loop counter: the rule is "lowest-numbered free slot", and a
		 * counter would agree with it only while the pool starts empty. */
		size_t lowestFree = 0;

		for(const auto& parameter : ordered)
		{
			while(lowestFree < result.slots.size() && result.slots[lowestFree].bound)
				++lowestFree;

			if(lowestFree >= result.slots.size())
			{
				++result.unboundCount;
				continue;
			}

			result.slots[lowestFree].bound = true;
			result.slots[lowestFree].parameter = parameter;
			++result.boundCount;
		}

		return result;
	}

	AutomationSurface::AutomationSurface()
	{
		/* The pool exists from construction, empty. Registration reports
		 * this many slots whether a patch has loaded or not. */
		m_allocation.slots.assign(g_automationSlotCount, SlotBinding{});
	}

	const SlotAllocation& AutomationSurface::loadPatch(const std::vector<PatchParameterId>& _patchParameters)
	{
		m_allocation = allocateSlots(_patchParameters);
		return m_allocation;
	}

	size_t findSlot(const SlotAllocation& _allocation, const PatchParameterId& _parameter)
	{
		for(size_t i=0; i<_allocation.slots.size(); ++i)
		{
			if(_allocation.slots[i].bound && _allocation.slots[i].parameter == _parameter)
				return i;
		}
		return g_noSlot;
	}
}
