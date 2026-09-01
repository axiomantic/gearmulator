/* g2Parameters.h -- the host automation surface: a fixed pool of parameter
 * slots plus the named morph group amounts.
 *
 * The pool never grows. Allocation does not create a slot; it binds one. The
 * host's reported parameter count is fixed at registration and never varies
 * with the patch.
 *
 * The allocation order is a contract: ascending module instance, then
 * ascending parameter index within each instance, each bound to the
 * lowest-numbered free slot. That order is what makes one patch produce one
 * slot map on every load, on every machine and in every host; an order taken
 * from hash iteration or from parse order would not.
 *
 * There is no survivor pinning. Every allocation starts from an empty pool, so
 * the map is a function of the patch and of nothing else. Pinning would make
 * it a function of the load history instead, and would make the set of
 * parameters that fall past the last slot depend on which slots the survivors
 * held.
 *
 * Exhaustion is not a refusal. A patch that binds more parameters than the
 * pool holds is a valid patch: allocation stops after the last slot and every
 * parameter after that is unbound. An unbound parameter still plays and still
 * edits, and the only thing it lacks is a host automation lane.
 *
 * The morph group amounts are outside the pool. They are patch-independent and
 * mean the same thing in every patch, so they are registered under their own
 * names and never take part in allocation. Their lanes never retarget.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace g2
{
	/* Changing the pool size is this line plus a state format version bump. */
	constexpr size_t g_automationSlotCount = 512;

	/* The morph group amounts, registered outside the pool. */
	constexpr size_t g_morphGroupCount = 8;

	/* The value a slot index carries when no slot holds a binding. It is
	 * outside the pool's index range on purpose: a caller that forgets to
	 * test it indexes out of range loudly instead of reading slot 0. */
	constexpr size_t g_noSlot = static_cast<size_t>(-1);

	/* A parameter's stable identity: which module instance owns it, and
	 * which of that module's parameters it is. Not a knob index -- a knob
	 * index names a position on the hardware's assignment surface, which a
	 * different patch fills differently. */
	struct PatchParameterId
	{
		uint32_t moduleInstance = 0;
		uint32_t parameterIndex = 0;
	};

	bool operator==(const PatchParameterId& _a, const PatchParameterId& _b);
	bool operator!=(const PatchParameterId& _a, const PatchParameterId& _b);

	/* The allocation order, as a comparison: ascending module instance, then
	 * ascending parameter index within one instance. */
	bool operator<(const PatchParameterId& _a, const PatchParameterId& _b);

	struct SlotBinding
	{
		bool bound = false;
		PatchParameterId parameter;
	};

	/* One allocation's result. `slots` always holds exactly
	 * g_automationSlotCount entries, whatever the patch bound: the vector is
	 * the pool, and a pool that changed size with the patch would be the
	 * varying parameter count the host must never see.
	 *
	 * `boundCount` and `unboundCount` sum to the number of distinct
	 * parameter identities the patch presented. */
	struct SlotAllocation
	{
		std::vector<SlotBinding> slots;
		uint32_t boundCount = 0;
		uint32_t unboundCount = 0;
	};

	/* The pool's slot names, built one time from a const list and returned
	 * by reference. Every call returns the same object: the framework's rule
	 * is that registration runs one time, and a function that rebuilt the
	 * list per call would let a second registration disagree with the first.
	 *
	 * Size is g_automationSlotCount, and entry i is "Param i". */
	const std::vector<std::string>& automationSlotNames();

	/* The morph group amount names, "Morph 1" upward, built the same way and
	 * disjoint from the pool's names. A user who automates a morph sweep gets
	 * a lane called "Morph 1", not a pool slot number. */
	const std::vector<std::string>& morphGroupNames();

	/* How many times the two lists above have actually been built in this
	 * process. It is 0 before the first call and 1 for every call after,
	 * which is the observable form of "registered one time". */
	uint32_t parameterListBuildCount();

	/* The count the host is told at registration. */
	constexpr size_t reportedParameterCount()
	{
		return g_automationSlotCount + g_morphGroupCount;
	}

	/* Binds the patch's parameters to slots.
	 *
	 * Every slot starts free -- this function does not read a previous
	 * allocation and cannot pin a survivor. Duplicate identities in
	 * _patchParameters bind one slot between them, so the result is a
	 * function of the set of identities and not of how many times the
	 * caller listed one.
	 *
	 * Allocation stops when the pool is full; the remaining identities, the
	 * ones the fixed order puts last, are counted in unboundCount. */
	SlotAllocation allocateSlots(const std::vector<PatchParameterId>& _patchParameters);

	/* The slot holding _parameter, or g_noSlot when no slot does. */
	size_t findSlot(const SlotAllocation& _allocation, const PatchParameterId& _parameter);

	/* One plugin instance's live slot map, and the place a patch load
	 * reaches it.
	 *
	 * It exists so that loading is a real event with a before and an after:
	 * the map a plugin holds after loading Y and then X must equal the map a
	 * fresh plugin holds after loading X alone. */
	class AutomationSurface
	{
	public:
		AutomationSurface();

		/* Frees every slot and allocates the whole set in the fixed order.
		 * The previous map is not consulted. */
		const SlotAllocation& loadPatch(const std::vector<PatchParameterId>& _patchParameters);

		const SlotAllocation& allocation() const { return m_allocation; }

	private:
		SlotAllocation m_allocation;
	};
}
