// Task BRD-17. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.3, BRD-17. Design section 10.7.
//
// WHAT THIS TEST IS FOR. `dsp56k::HDI08` -- the DSP side, in `dsp56300`, NOT
// the `mc68k::Hdi08` host side BRD-16 adapts -- moves host words into a ring
// buffer whose push blocks when the ring is full. Under a single-threaded
// scheduler a blocking push is a deadlock. BRD-17 puts a bound on the words one
// quantum may move and EXPOSES the count it moved, and this test drives that
// bound.
//
// THE BOUND IS ASSERTED FROM TWO SIDES ON PURPOSE. A bound that is never hit
// and a bound that does not exist produce the same green, so this file drives a
// request ABOVE the bound and asserts the count is clamped, and a request BELOW
// the bound and asserts the count is NOT clamped. Either case alone cannot tell
// a working bound from a stuck constant.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). BRD-17's own block requires
// the bound to be checked in a release build as well as a debug build, and a
// release build removes an assertion. The production code keeps a debug
// assertion as well; it is not this check's predicate.
//
// THE TWO DECLARATIONS BELOW ARE WRITTEN OUT HERE RATHER THAN INCLUDED.
// BRD-17's `Files:` line and the G-M3 file union (plan section 7.2.2) both name
// `g2Lib/hdi08Adapter.cpp` and neither names `g2Lib/hdi08Adapter.h`, so this
// task adds no declaration to that header. A mismatch between these
// declarations and the definitions in hdi08Adapter.cpp is a LINK error, not a
// silent pass.

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/types.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace g2
{
	uint32_t hdi08QuantumWordBudget(const dsp56k::HDI08& _dsp);
	uint32_t hdi08MoveWordsForQuantum(dsp56k::HDI08& _dsp, const dsp56k::TWord* _words, uint32_t _count);
}

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

	void checkEqual(const uint64_t _actual, const uint64_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	// A REAL DSP IS REQUIRED AND THE REASON IS NOT COSMETIC. `HDI08::writeRX`
	// ends in `IPeripherals::setDelayCycles`, which dereferences `m_dsp`
	// unconditionally (peripherals.cpp:118). A PeripheralsNop with no DSP
	// attached segfaults there, so the environment below builds the DSP that
	// the DSP constructor attaches to both peripheral sets.
	struct Env
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop periphX;
		dsp56k::PeripheralsNop periphY;
		dsp56k::DSP            dsp;

		Env()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
		{
		}
	};

	// One HDI08 carries two 8192-entry rings BY VALUE, so it is heap-allocated,
	// and it is rebuilt per case group. A case that inherited the previous
	// case's ring contents could not say which clamp produced its count.
	struct Fixture
	{
		std::unique_ptr<dsp56k::HDI08> hdi08;

		explicit Fixture(Env& _env) : hdi08(new dsp56k::HDI08(_env.periphX)) {}

		size_t capacity() const	{ return hdi08->rxData().capacity(); }
		size_t size() const		{ return hdi08->rxData().size(); }
	};

	// `HDI08::writeRX` masks every word to 24 bits, so the pattern stays inside
	// 24 bits and every index up to the ring capacity gets a distinct value. A
	// word that landed at the wrong offset is then visible in the value.
	std::vector<dsp56k::TWord> words(const size_t _count)
	{
		std::vector<dsp56k::TWord> result(_count);
		for(size_t i = 0; i < _count; ++i)
			result[i] = dsp56k::TWord(0x00a00000u | uint32_t(i));
		return result;
	}

	// Compares the whole ring against the words that should be in it, and
	// reports the first offset that differs. A count check alone would pass over
	// a transfer that moved the right NUMBER of wrong words.
	void checkRingHolds(const dsp56k::HDI08& _dsp, const std::vector<dsp56k::TWord>& _expected,
		const size_t _expectedCount, const std::string& _what)
	{
		const auto& ring = _dsp.rxData();

		checkEqual(ring.size(), _expectedCount, _what + ": the ring holds the moved words");

		if(ring.size() != _expectedCount)
			return;

		++g_cases;
		for(size_t i = 0; i < _expectedCount; ++i)
		{
			if(ring[i] == _expected[i])
				continue;

			std::cout << "FAIL " << _what << ": ring[" << i << "] expected <"
				<< _expected[i] << ">, got <" << ring[i] << ">" << std::endl;
			++g_failures;
			return;
		}
		std::cout << "ok   " << _what << ": every moved word is the word the caller passed" << std::endl;
	}
}

int main()
{
	Env env;

	// -----------------------------------------------------------------------
	// Case group 0. THE BOUND ITSELF IS BELOW THE RING CAPACITY.
	//
	// BRD-17 requires the per-quantum bound to sit BELOW the buffer capacity,
	// not at it. A bound equal to the capacity would leave the DSP no headroom
	// and is the mutation this group exists to catch.
	{
		Fixture f(env);
		const uint32_t budget = g2::hdi08QuantumWordBudget(*f.hdi08);

		checkEqual(f.capacity(), 8192u, "the dsp56k receive ring capacity is 8192 words");
		check(budget < f.capacity(), "the per-quantum word budget is strictly below the ring capacity");
		check(budget > 0, "the per-quantum word budget is not zero");
		checkEqual(budget, f.capacity() / 2u, "the per-quantum word budget is half the ring capacity");
	}

	// -----------------------------------------------------------------------
	// Case group 1. A REQUEST BELOW THE BOUND IS NOT CLAMPED.
	//
	// This is the half that separates a working bound from a stuck constant. If
	// the transfer returned the budget no matter what it was asked for, this
	// group fails and case group 2 still passes.
	{
		Fixture f(env);
		const uint32_t budget = g2::hdi08QuantumWordBudget(*f.hdi08);
		const uint32_t requested = budget - 1;

		const std::vector<dsp56k::TWord> data = words(requested);
		const uint32_t moved = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), requested);

		checkEqual(moved, requested, "a request one word below the budget moves every word asked for");
		check(moved < budget, "a request below the budget is not raised to the budget");
		checkRingHolds(*f.hdi08, data, requested, "below the budget");
	}

	// -----------------------------------------------------------------------
	// Case group 2. A REQUEST OF EXACTLY THE RING CAPACITY IS CLAMPED.
	//
	// Exactly the capacity is the largest request an UNBOUNDED transfer can
	// satisfy without blocking, so this group observes a removed clamp as a
	// wrong COUNT rather than as a hang. Case group 3 drives the plan's literal
	// over-capacity case, where a removed clamp deadlocks instead.
	{
		Fixture f(env);
		const uint32_t budget = g2::hdi08QuantumWordBudget(*f.hdi08);
		const uint32_t requested = uint32_t(f.capacity());

		const std::vector<dsp56k::TWord> data = words(requested);
		const uint32_t moved = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), requested);

		checkEqual(moved, budget, "a request of the whole ring capacity moves exactly the budget");
		check(moved < f.capacity(), "the moved count stays below the ring capacity");
		checkRingHolds(*f.hdi08, data, budget, "at the ring capacity");
	}

	// -----------------------------------------------------------------------
	// Case group 3. MORE WORDS THAN THE CAPACITY IN ONE QUANTUM.
	//
	// BRD-17's Check: drive more words than the queue capacity within one
	// quantum and assert the exposed moved-word count against the capacity.
	{
		Fixture f(env);
		const uint32_t budget = g2::hdi08QuantumWordBudget(*f.hdi08);
		const uint32_t requested = uint32_t(f.capacity()) + 1u;

		const std::vector<dsp56k::TWord> data = words(requested);
		const uint32_t moved = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), requested);

		checkEqual(moved, budget, "a request above the ring capacity moves exactly the budget");
		check(moved < f.capacity(), "the moved count stays below the ring capacity");
		checkRingHolds(*f.hdi08, data, budget, "above the ring capacity");

		// The words the bound refused are still the caller's to re-offer next
		// quantum: the transfer consumed a prefix and reported how long it was.
		const uint32_t remainingToOffer = requested - moved;
		checkEqual(remainingToOffer, requested - budget,
			"the words the bound refused are the caller's remainder");
	}

	// -----------------------------------------------------------------------
	// Case group 4. A NEARLY FULL RING MOVES ONLY WHAT FITS.
	//
	// The budget alone does not make the transfer non-blocking: a ring with less
	// free space than the budget would still block. This group leaves seven free
	// slots and asks for the whole budget.
	{
		Fixture f(env);
		const uint32_t budget = g2::hdi08QuantumWordBudget(*f.hdi08);
		const uint32_t freeSlots = 7u;
		const uint32_t prefill = uint32_t(f.capacity()) - freeSlots;

		// Filled through the library's own path, not through the code under
		// test, so the pre-state owes the transfer nothing.
		const std::vector<dsp56k::TWord> filler = words(prefill);
		f.hdi08->writeRX(filler.data(), prefill);
		checkEqual(f.size(), prefill, "the ring is pre-filled to seven slots short of full");

		const std::vector<dsp56k::TWord> data = words(budget);
		const uint32_t moved = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), budget);

		checkEqual(moved, freeSlots, "a nearly full ring moves only the words that fit");
		check(moved < budget, "the free-space clamp binds tighter than the budget here");
		checkEqual(f.size(), f.capacity(), "the ring is full after the bounded transfer");

		// And the next quantum on a full ring moves nothing and still returns.
		const uint32_t movedAgain = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), budget);
		checkEqual(movedAgain, 0u, "a full ring moves no words");
		checkEqual(f.size(), f.capacity(), "a refused transfer leaves the ring unchanged");
	}

	// -----------------------------------------------------------------------
	// Case group 5. A ZERO-WORD QUANTUM.
	{
		Fixture f(env);
		const std::vector<dsp56k::TWord> data = words(1);
		const uint32_t moved = g2::hdi08MoveWordsForQuantum(*f.hdi08, data.data(), 0u);

		checkEqual(moved, 0u, "a zero-word request moves nothing");
		checkEqual(f.size(), 0u, "a zero-word request leaves the ring empty");
	}

	if(g_failures)
	{
		std::cout << "t0_hdi08_nonblocking: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_hdi08_nonblocking: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
