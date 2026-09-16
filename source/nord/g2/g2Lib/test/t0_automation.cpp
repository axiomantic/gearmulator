/* The host automation surface.
 *
 * The named morph group amounts sit outside the slot pool, so the reported
 * parameter count is the sum of the two and does not move when a patch
 * loads.
 *
 * Slot allocation is path-independent: a fresh surface loading X and a
 * surface that loaded Y and then X hold identical maps. Pinning the
 * survivors of Y would keep them at Y's slot numbers, and the two maps
 * would differ.
 *
 * No assertion in this file is a language assert() and nothing depends on
 * NDEBUG.
 */

#include "g2JucePlugin/g2Parameters.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	void checkEqual(const size_t _actual, const size_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << " (" << _actual << ")" << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": got " << _actual << ", expected " << _expected << std::endl;
		++g_failures;
	}

	std::vector<g2::PatchParameterId> makePatch(const uint32_t _instances, const uint32_t _parametersPerInstance)
	{
		std::vector<g2::PatchParameterId> patch;
		for(uint32_t m=0; m<_instances; ++m)
		{
			for(uint32_t p=0; p<_parametersPerInstance; ++p)
				patch.push_back(g2::PatchParameterId{m, p});
		}
		return patch;
	}

	/* Two maps are identical when the same slot indices hold the same
	 * identities, and the same slot indices hold nothing. */
	bool sameMap(const g2::SlotAllocation& _a, const g2::SlotAllocation& _b)
	{
		if(_a.slots.size() != _b.slots.size())
			return false;
		for(size_t i=0; i<_a.slots.size(); ++i)
		{
			if(_a.slots[i].bound != _b.slots[i].bound)
				return false;
			if(_a.slots[i].bound && _a.slots[i].parameter != _b.slots[i].parameter)
				return false;
		}
		return _a.boundCount == _b.boundCount && _a.unboundCount == _b.unboundCount;
	}

	size_t firstDifferingSlot(const g2::SlotAllocation& _a, const g2::SlotAllocation& _b)
	{
		const size_t n = _a.slots.size() < _b.slots.size() ? _a.slots.size() : _b.slots.size();
		for(size_t i=0; i<n; ++i)
		{
			if(_a.slots[i].bound != _b.slots[i].bound)
				return i;
			if(_a.slots[i].bound && _a.slots[i].parameter != _b.slots[i].parameter)
				return i;
		}
		return g2::g_noSlot;
	}

	void testPoolSizeAndNames()
	{
		checkEqual(g2::g_automationSlotCount, 512, "the pool holds 512 slots");
		checkEqual(g2::g_morphGroupCount, 8, "eight morph group amounts");
		checkEqual(g2::reportedParameterCount(), 520, "the reported parameter count is the pool plus the morph amounts");

		const auto& slotNames = g2::automationSlotNames();
		const auto& morphNames = g2::morphGroupNames();

		checkEqual(slotNames.size(), g2::g_automationSlotCount, "one name per pool slot");
		checkEqual(morphNames.size(), g2::g_morphGroupCount, "one name per morph amount");

		check(slotNames.front() == "Param 0", "the first pool slot is named Param 0");
		check(slotNames.back() == "Param 511", "the last pool slot is named Param 511");
		check(morphNames.front() == "Morph 1", "the first morph amount is named Morph 1");
		check(morphNames.back() == "Morph 8", "the last morph amount is named Morph 8");

		/* The morph lanes are outside the pool: no pool name is a morph
		 * name, so a morph sweep can never land on a pool slot's lane. */
		bool collision = false;
		for(const auto& morph : morphNames)
		{
			for(const auto& slot : slotNames)
			{
				if(morph == slot)
					collision = true;
			}
		}
		check(!collision, "no morph amount shares a name with a pool slot");

		bool duplicate = false;
		for(size_t i=0; i<slotNames.size(); ++i)
		{
			if(slotNames[i] != ("Param " + std::to_string(i)))
				duplicate = true;
		}
		check(!duplicate, "every pool slot name is its own index");
	}

	void testRegisteredOnce()
	{
		const auto& first = g2::automationSlotNames();
		const uint32_t buildsAfterFirst = g2::parameterListBuildCount();
		checkEqual(buildsAfterFirst, 1, "the const list is built one time");

		for(int i=0; i<8; ++i)
		{
			const auto& again = g2::automationSlotNames();
			const auto& morphAgain = g2::morphGroupNames();
			check(&again == &first, "repeated registration returns the same list object");
			checkEqual(morphAgain.size(), g2::g_morphGroupCount, "the morph list does not grow on a repeated call");
		}

		checkEqual(g2::parameterListBuildCount(), 1, "repeated calls do not rebuild the list");
		checkEqual(g2::automationSlotNames().size(), g2::g_automationSlotCount, "the pool does not grow");
	}

	void testAllocationOrder()
	{
		/* Presented deliberately out of order: descending instance,
		 * descending parameter, with one identity listed twice. */
		std::vector<g2::PatchParameterId> scrambled;
		scrambled.push_back(g2::PatchParameterId{7, 3});
		scrambled.push_back(g2::PatchParameterId{2, 9});
		scrambled.push_back(g2::PatchParameterId{7, 1});
		scrambled.push_back(g2::PatchParameterId{2, 0});
		scrambled.push_back(g2::PatchParameterId{2, 9});
		scrambled.push_back(g2::PatchParameterId{1, 4});

		const auto allocation = g2::allocateSlots(scrambled);

		checkEqual(allocation.slots.size(), g2::g_automationSlotCount, "an allocation returns the whole pool");
		checkEqual(allocation.boundCount, 5, "the duplicate identity binds one slot between the two listings");
		checkEqual(allocation.unboundCount, 0, "nothing is unbound when the patch fits");

		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{1, 4}), 0, "the lowest instance takes slot 0");
		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{2, 0}), 1, "ascending parameter index within an instance");
		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{2, 9}), 2, "the second parameter of that instance follows");
		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{7, 1}), 3, "the next instance follows the previous one whole");
		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{7, 3}), 4, "and its parameters ascend too");

		check(!allocation.slots[5].bound, "the slot after the last binding is free");
		checkEqual(g2::findSlot(allocation, g2::PatchParameterId{99, 99}), g2::g_noSlot, "an identity the patch does not hold is in no slot");
	}

	void testPathIndependence()
	{
		const auto patchX = makePatch(6, 7);   // 42 identities
		const auto patchY = makePatch(4, 20);  // 80 identities, overlapping X

		g2::AutomationSurface fresh;
		const g2::SlotAllocation mapFresh = fresh.loadPatch(patchX);

		g2::AutomationSurface afterY;
		afterY.loadPatch(patchY);
		const g2::SlotAllocation mapAfterY = afterY.loadPatch(patchX);

		const bool identical = sameMap(mapFresh, mapAfterY);
		check(identical, "the map of X is the same whether or not Y loaded first");
		if(!identical)
		{
			const size_t slot = firstDifferingSlot(mapFresh, mapAfterY);
			std::cout << "     first differing slot " << slot << std::endl;
		}

		/* A third order, to say the property is about the patch and not
		 * about one particular previous load. */
		g2::AutomationSurface afterYAndX;
		afterYAndX.loadPatch(patchX);
		afterYAndX.loadPatch(patchY);
		const g2::SlotAllocation mapAfterTwo = afterYAndX.loadPatch(patchX);
		check(sameMap(mapFresh, mapAfterTwo), "the map of X survives two unrelated loads before it");
	}

	void testExhaustion()
	{
		const uint32_t perInstance = 25;
		const uint32_t instances = 25;  // 625 distinct identities against 512 slots
		const auto overFull = makePatch(instances, perInstance);
		checkEqual(overFull.size(), 625, "the over-full patch binds more parameters than the pool holds");

		const auto allocation = g2::allocateSlots(overFull);

		checkEqual(allocation.slots.size(), g2::g_automationSlotCount, "the pool does not grow for an over-full patch");
		checkEqual(allocation.boundCount, g2::g_automationSlotCount, "every slot is bound");
		checkEqual(allocation.unboundCount, 625 - g2::g_automationSlotCount, "the excess is counted, not hidden");
		checkEqual(g2::reportedParameterCount(), 520, "the reported count is unchanged by exhaustion");

		size_t boundSlots = 0;
		for(const auto& slot : allocation.slots)
		{
			if(slot.bound)
				++boundSlots;
		}
		checkEqual(boundSlots, g2::g_automationSlotCount, "the counted bindings agree with the pool");

		/* The fixed order puts the highest instance last, so the last
		 * identity of the last instance is the one left out. */
		const g2::PatchParameterId last{instances - 1, perInstance - 1};
		checkEqual(g2::findSlot(allocation, last), g2::g_noSlot, "the identity the fixed order puts last is unbound");

		const g2::PatchParameterId first{0, 0};
		checkEqual(g2::findSlot(allocation, first), 0, "the identity the fixed order puts first holds slot 0");

		/* The cut is a function of the patch, not of the load history. */
		g2::AutomationSurface surface;
		surface.loadPatch(makePatch(3, 3));
		const g2::SlotAllocation afterOther = surface.loadPatch(overFull);
		checkEqual(g2::findSlot(afterOther, last), g2::g_noSlot, "the same identity is unbound after a different patch");
		check(sameMap(allocation, afterOther), "an over-full patch produces one map whatever preceded it");
	}

	void testSlotReuse()
	{
		g2::AutomationSurface surface;
		surface.loadPatch(makePatch(10, 10));

		const auto small = makePatch(1, 3);
		const g2::SlotAllocation after = surface.loadPatch(small);

		checkEqual(after.boundCount, 3, "a smaller patch binds only its own parameters");
		checkEqual(g2::findSlot(after, g2::PatchParameterId{0, 0}), 0, "a freed slot is reused from the bottom");

		size_t bound = 0;
		for(const auto& slot : after.slots)
		{
			if(slot.bound)
				++bound;
		}
		checkEqual(bound, 3, "the slots the previous patch held are free again");
	}
}

int main()
{
	testPoolSizeAndNames();
	testRegisteredOnce();
	testAllocationOrder();
	testPathIndependence();
	testExhaustion();
	testSlotReuse();

	std::cout << (g_cases - g_failures) << "/" << g_cases << " cases passed" << std::endl;

	if(g_failures)
	{
		std::cout << g_failures << " FAILURES" << std::endl;
		return 1;
	}
	return 0;
}
