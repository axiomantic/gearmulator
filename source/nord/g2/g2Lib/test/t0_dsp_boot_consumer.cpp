// Tier T0: this test needs no firmware artifact of any kind.
//
// EVERY DRIVEN WORD IS NON-ZERO, in the count and address headers and in the
// body alike. Program memory is zero-filled, so a driven 0x000000 compares zero
// against a default-zero read and passes whether the word crossed or not.
//
// THE POST-COMPLETION CASE IS PINNED TO THE RECEIVE PATH AND NOT TO PROGRAM
// MEMORY. `dsp56k::DspBoot`'s Finished state swallows a word rather than
// storing it, so "it did not land at P:A+N" is true of a correct build and of a
// build that never swapped the host callback alike. Only the arrival on the
// DSP's own receive path tells the two apart.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build type is
// Release, which defines NDEBUG.

#include "dspSet.h"
#include "hdi08Adapter.h"
#include "hdi08Bridge.h"
#include "hdi08Decode.h"

#include "g2/timebase.h"

#include "mc68k/hdi08.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/types.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	using dsp56k::TWord;

	int g_cases = 0;
	int g_failures = 0;

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

	void checkEqualHex(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <0x" << std::hex << _expected
			<< ">, got <0x" << _actual << ">" << std::dec << std::endl;
		++g_failures;
	}

	void checkEqualCount(const size_t _actual, const size_t _expected, const std::string& _what)
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

	constexpr uint32_t g_secondBusFrameRateHz = G2_FRAME_RATE_HZ / G2_SECOND_BUS_FRAME_DIVIDER;

	struct Slot
	{
		dsp56k::Memory           memory;
		dsp56k::Peripherals56311 peripherals;
		dsp56k::DSP              dsp;

		Slot()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, peripherals(g_secondBusFrameRateHz)
			, dsp(memory, &peripherals, &peripherals.ySpace())
		{
		}

		dsp56k::HDI08& hdi08() { return peripherals.getHDI08(); }
	};

	constexpr int g_bridgedPort = 2;

	constexpr TWord g_bootAddress = 0x000240u;
	constexpr std::array<TWord, 5> g_program =
	{
		0x0a1101u, 0x0b2202u, 0x0c3303u, 0x0d4404u, 0x0e5505u
	};
	constexpr TWord g_wordAfterProgram = 0x0f6606u;

	TWord programWordCount()
	{
		return static_cast<TWord>(g_program.size());
	}

	void hostWriteWord(mc68k::Hdi08& _port, const uint32_t _word)
	{
		_port.write8(mc68k::PeriphAddress::HdiTXH, uint8_t(_word >> 16));
		_port.write8(mc68k::PeriphAddress::HdiTXM, uint8_t(_word >> 8));
		_port.write8(mc68k::PeriphAddress::HdiTXL, uint8_t(_word));
	}

	uint32_t programMemory(dsp56k::DSP& _dsp, const TWord _address)
	{
		return _dsp.memory().get(dsp56k::MemArea_P, _address);
	}

	// The header pair the mask ROM consumes before any body word.
	void driveBootHeaders(mc68k::Hdi08& _port, const TWord _count)
	{
		hostWriteWord(_port, _count);
		hostWriteWord(_port, g_bootAddress);
	}

	void driveProgramWords(mc68k::Hdi08& _port, const size_t _count)
	{
		for(size_t i = 0; i < _count; ++i)
			hostWriteWord(_port, g_program[i]);
	}

	std::string atIndex(const size_t _i)
	{
		return " at index " + std::to_string(_i);
	}

	/* ---------------- group 1: one push, one landed program */

	void aBootstrapPushLandsTheProgramAndPrimesTheCore()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());

		for(size_t i = 0; i < g_program.size(); ++i)
		{
			checkEqualHex(programMemory(slot.dsp, g_bootAddress + TWord(i)), 0u,
				"program memory holds none of the words about to be pushed" + atIndex(i));
		}
		checkEqualHex(programMemory(slot.dsp, g_bootAddress + programWordCount()), 0u,
			"the word past the program's end is zero before the push");
		check(slot.dsp.getPC().toWord() != g_bootAddress,
			"the core's program counter is not the boot address before the push");
		check(!*bridge.programLanded(),
			"the bridge reports no landed program before the push");

		driveBootHeaders(adapter.port(g_bridgedPort), programWordCount());
		driveProgramWords(adapter.port(g_bridgedPort), g_program.size());

		for(size_t i = 0; i < g_program.size(); ++i)
		{
			checkEqualHex(programMemory(slot.dsp, g_bootAddress + TWord(i)), g_program[i],
				"the pushed word landed in program memory" + atIndex(i));
		}
		checkEqualHex(slot.dsp.getPC().toWord(), g_bootAddress,
			"the core's program counter is the address the push named");
		check(*bridge.programLanded(),
			"the bridge reports the program landed once the last body word arrived");

		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"no bootstrap word reaches the DSP's own receive path");

		hostWriteWord(adapter.port(g_bridgedPort), g_wordAfterProgram);
		checkEqualHex(programMemory(slot.dsp, g_bootAddress + programWordCount()), 0u,
			"a word pushed after completion does not land past the program's end");
		checkEqualCount(slot.hdi08().rxData().size(), 1u,
			"a word pushed after completion reaches the DSP's own receive path");
		if(slot.hdi08().rxData().size() == 1u)
		{
			checkEqualHex(slot.hdi08().rxData()[0], g_wordAfterProgram,
				"the word on the receive path is the word pushed after completion");
		}
	}

	/* ---------------- group 2: a load one word short of its declared count */

	void anIncompleteLoadKeepsFeedingProgramMemory()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());

		driveBootHeaders(adapter.port(g_bridgedPort), programWordCount());
		driveProgramWords(adapter.port(g_bridgedPort), g_program.size() - 1);

		check(!*bridge.programLanded(),
			"a load one word short of its declared count reports no landed program");
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"a load one word short of its declared count moves no word to the receive path");
		checkEqualHex(programMemory(slot.dsp, g_bootAddress + programWordCount() - 1u), 0u,
			"the last word of the declared count is absent from program memory");

		hostWriteWord(adapter.port(g_bridgedPort), g_program[g_program.size() - 1]);

		checkEqualHex(programMemory(slot.dsp, g_bootAddress + programWordCount() - 1u),
			g_program[g_program.size() - 1],
			"the word completing the declared count lands in program memory");
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"the word completing the declared count does not reach the receive path");
		check(*bridge.programLanded(),
			"the word completing the declared count reports the program landed");
	}

	/* ---------------- group 3: the set owns the bridges and publishes the flag */

	constexpr unsigned g_landedSlot = 5;

	void theSetPublishesAPerSlotLandedFlag()
	{
		g2::DspSet set;

		check(set.programLanded(0) == nullptr,
			"a set with no bridges attached answers null for slot 0");
		check(set.programLanded(set.dspCount()) == nullptr,
			"a set answers null for a slot index past its last");

		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::attachHdi08Bridges(adapter, set);

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			const std::string slotName = " on slot " + std::to_string(i);
			const bool* const landed = set.programLanded(i);
			check(landed != nullptr, "an attached set answers a flag" + slotName);
			if(landed)
				check(!*landed, "an attached set reports no landed program" + slotName);
		}
		check(set.programLanded(set.dspCount()) == nullptr,
			"an attached set still answers null for a slot index past its last");

		const bool* const landedBefore = set.programLanded(g_landedSlot);

		driveBootHeaders(adapter.port(static_cast<int>(g_landedSlot)), programWordCount());
		driveProgramWords(adapter.port(static_cast<int>(g_landedSlot)), g_program.size());

		check(set.programLanded(g_landedSlot) == landedBefore,
			"the flag's address does not move when the program lands");
		if(landedBefore)
			check(*landedBefore, "the flag taken before the push reads set once the program lands");

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			if(i == g_landedSlot)
				continue;
			const bool* const landed = set.programLanded(i);
			if(landed)
			{
				check(!*landed, "a program landed on one slot leaves slot "
					+ std::to_string(i) + " not landed");
			}
		}

		for(size_t i = 0; i < g_program.size(); ++i)
		{
			checkEqualHex(programMemory(set.dsp(g_landedSlot), g_bootAddress + TWord(i)), g_program[i],
				"the set's own slot holds the pushed word" + atIndex(i));
		}
		checkEqualHex(set.dsp(g_landedSlot).getPC().toWord(), g_bootAddress,
			"the set's own slot has its program counter primed to the boot address");
		checkEqualHex(programMemory(set.dsp(0), g_bootAddress), 0u,
			"a program landed on one slot writes no program memory on another");
	}
}

int main()
{
	aBootstrapPushLandsTheProgramAndPrimesTheCore();
	anIncompleteLoadKeepsFeedingProgramMemory();
	theSetPublishesAPerSlotLandedFlag();

	if(g_failures != 0)
	{
		std::cout << "t0_dsp_boot_consumer: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_dsp_boot_consumer: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
