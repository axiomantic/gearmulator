// Tier T0: this test needs no firmware artifact of any kind.
//
// Every driven word is non-zero, in the count and address headers and in the
// body alike. Program memory is zero-filled, so 0x000000 is not a value that
// distinguishes anything there.
//
// The post-completion case is pinned to the receive path and not to program
// memory. `dsp56k::DspBoot`'s Finished state swallows a word rather than
// storing it, so no word reaches P:A+N for either side to read. The DSP's own
// receive path is where a post-completion word does arrive.
//
// No assertion in this file is a language assert(). The default build type is
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
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

/* THE BRIDGE's address is fixed by construction, and this is what pins it.
 * `programLanded` hands out a pointer into the bridge, so a bridge that could be
 * copied or moved would hand out an address a later relocation invalidates. The
 * runtime case below compares one address against another, which no
 * implementation that compiles can answer differently. */
static_assert(!std::is_copy_constructible_v<g2::Hdi08Bridge>
	&& !std::is_move_constructible_v<g2::Hdi08Bridge>
	&& !std::is_copy_assignable_v<g2::Hdi08Bridge>
	&& !std::is_move_assignable_v<g2::Hdi08Bridge>,
	"Hdi08Bridge must be neither copyable nor movable: programLanded borrows its address.");

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

	// HF2 and HF3 sit at bits 3 and 4 of the DSP's control register and of the
	// host ISR alike, which is what makes the bridge's mirror a masked copy.
	constexpr uint8_t g_hostFlagMask = mc68k::Hdi08::Hf2 | mc68k::Hdi08::Hf3;

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

	std::string onPort(const unsigned _port)
	{
		return " on port " + std::to_string(_port);
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

	/* The adapter is declared before the set, and that order is the one
	 * hdi08Bridge.h states and Board KEEPS. Locals are destroyed in reverse
	 * declaration order and `~Hdi08Bridge` uninstalls through the host port it
	 * was handed, so a set declared first would reach a destroyed port once per
	 * slot. */
	void theSetPublishesAPerSlotLandedFlag()
	{
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::DspSet set;

		check(set.programLanded(0) == nullptr,
			"a set with no bridges attached answers null for slot 0");
		check(set.programLanded(set.dspCount()) == nullptr,
			"a set answers null for a slot index past its last");

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

	/* ---------------- group 4: a destroyed bridge leaves the port as it found it */

	/* THE PORT's own state is the sentinel, and not the dead bridge. Reading a
	 * destroyed object to see whether it was called is the very fault under test,
	 * so the assertions read only what the port and the slot hold: an uninstalled
	 * write-transmit callback leaves the word in the port's own transmit queue,
	 * and an uninstalled receive drain leaves the host receive register empty. */
	void aDestroyedBridgeLeavesNoCallbackBehind()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		mc68k::Hdi08& port = adapter.port(g_bridgedPort);

		std::deque<uint32_t> transmitted;

		{
			g2::Hdi08Bridge bridge(port, slot.dsp, slot.hdi08());

			driveBootHeaders(port, programWordCount());
			driveProgramWords(port, g_program.size());

			check(*bridge.programLanded(),
				"the program landed on the bridge that is about to be destroyed");

			port.pollTx(transmitted);
			checkEqualCount(transmitted.size(), 0u,
				"a bridge that is alive leaves no word in the port's own transmit queue");

			/* The control for the flag mirror is taken while the bridge is alive,
			 * so the assertion after its destruction is a change and not a value
			 * that was never there. */
			slot.hdi08().writeControlRegister(g_hostFlagMask);
			check((port.isr() & g_hostFlagMask) == g_hostFlagMask,
				"a bridge that is alive mirrors the DSP's host flags into the port's status");
		}

		hostWriteWord(port, g_wordAfterProgram);

		transmitted.clear();
		port.pollTx(transmitted);
		checkEqualCount(transmitted.size(), 1u,
			"a word written after the bridge is destroyed stays in the port's own transmit queue");
		if(transmitted.size() == 1u)
		{
			checkEqualHex(transmitted[0], g_wordAfterProgram,
				"the word the port kept is the word written after the bridge was destroyed");
		}
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"a word written after the bridge is destroyed reaches no DSP receive path");

		check((port.isr() & g_hostFlagMask) == 0,
			"the DSP's host flags stop reaching the port's status once the bridge is destroyed");

		check((port.isr() & mc68k::Hdi08::Rxdf) == 0,
			"the host receive register is empty before the DSP transmits");

		slot.hdi08().writeTX(g_wordAfterProgram);

		check((port.isr() & mc68k::Hdi08::Rxdf) == 0,
			"a DSP transmit after the bridge is destroyed reaches no host receive register");
		check(slot.hdi08().hasTX(),
			"the word the DSP transmitted after the bridge is destroyed is still on the DSP side");

		/* The empty receive read is the second way back in. A port that finds its
		 * receive register empty asks for data, so the slot that answers that
		 * question needs its own case. `HdiTXH` is the read side of the shared
		 * byte register and is the first byte of a read sequence, which is the
		 * one byte of the three that asks. */
		checkEqualHex(port.read8(mc68k::PeriphAddress::HdiTXH), 0u,
			"an empty receive read after the bridge is destroyed answers zero");
		check(slot.hdi08().hasTX(),
			"an empty receive read after the bridge is destroyed pulls no word off the DSP");
	}

	/* ---------------- group 5: a second attach on one set is refused */

	/* The adapter is declared before the set for the reason stated above. */
	void aSecondAttachOnOneSetIsRefused()
	{
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::DspSet set;

		g2::attachHdi08Bridges(adapter, set);

		const bool* const landedBefore = set.programLanded(g_landedSlot);
		check(landedBefore != nullptr, "the first attach answers a flag on the slot under test");

		bool refused = false;
		try
		{
			g2::attachHdi08Bridges(adapter, set);
		}
		catch(const std::logic_error&)
		{
			refused = true;
		}

		check(refused, "a second attach on a set that already holds bridges is refused");
		check(set.programLanded(g_landedSlot) == landedBefore,
			"a refused second attach leaves the borrowed flag pointer where it was");

		driveBootHeaders(adapter.port(static_cast<int>(g_landedSlot)), programWordCount());
		driveProgramWords(adapter.port(static_cast<int>(g_landedSlot)), g_program.size());

		if(landedBefore)
		{
			check(*landedBefore,
				"the flag borrowed before the refused attach still answers for its own bridge");
		}
	}

	/* ---------------- group 6: a destroyed set leaves every port as it found it
	 *
	 * The adapter outlives the set, and that is what makes the ports readable
	 * afterwards. The same observation cannot be taken from a Board: a Board owns
	 * both, so its ports are gone by the time its set has finished dying, and the
	 * only reading left there is the declaration order t0_board_dsp_set asserts.
	 *
	 * The control is taken on the living set, so each assertion after the
	 * destruction is a change and not a value that was never there. */
	void aDestroyedSetLeavesNoCallbackBehindOnAnyPort()
	{
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};

		{
			g2::DspSet set;
			g2::attachHdi08Bridges(adapter, set);

			for(unsigned i = 0; i < set.dspCount(); ++i)
			{
				mc68k::Hdi08& port = adapter.port(static_cast<int>(i));

				driveBootHeaders(port, programWordCount());
				driveProgramWords(port, g_program.size());

				const bool* const landed = set.programLanded(i);
				check(landed != nullptr && *landed,
					"the program landed before the set is destroyed" + onPort(i));

				std::deque<uint32_t> alive;
				hostWriteWord(port, g_wordAfterProgram);
				port.pollTx(alive);
				checkEqualCount(alive.size(), 0u,
					"a set that is alive leaves no word in the port's own transmit queue"
						+ onPort(i));

				set.peripherals(i).getHDI08().writeControlRegister(g_hostFlagMask);
				check((port.isr() & g_hostFlagMask) == g_hostFlagMask,
					"a set that is alive mirrors the DSP's host flags into the port's status"
						+ onPort(i));
			}
		}

		for(int i = 0; i < g2::g_hdi08PortCount; ++i)
		{
			mc68k::Hdi08& port = adapter.port(i);
			const unsigned slot = static_cast<unsigned>(i);

			std::deque<uint32_t> transmitted;
			hostWriteWord(port, g_wordAfterProgram);
			port.pollTx(transmitted);

			checkEqualCount(transmitted.size(), 1u,
				"a word written after the set is destroyed stays in the port's own "
				"transmit queue" + onPort(slot));
			if(transmitted.size() == 1u)
			{
				checkEqualHex(transmitted[0], g_wordAfterProgram,
					"the word the port kept is the word written after the set was destroyed"
						+ onPort(slot));
			}

			check((port.isr() & g_hostFlagMask) == 0,
				"the DSP's host flags stop reaching the port's status once the set is "
				"destroyed" + onPort(slot));
		}
	}

	/* ---------------- group 7: a snapshot does not carry the bridges' state
	 *
	 * What the snapshot leaves out. dspSet.cpp walks the slots and nothing else,
	 * so `m_programLanded` and `dsp56k::DspBoot`'s download cursor are outside
	 * it. A post-boot snapshot restored into a set whose bridges are fresh would
	 * carry the right program memory behind a gate that never opens, and
	 * attachHdi08Bridges refuses the second attach that would replace the
	 * bridges, so nothing could reopen it. */
	void aStateLoadIntoASetHoldingBridgesIsRefused()
	{
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::DspSet set;

		std::vector<uint8_t> snapshot(set.stateSize());
		set.stateSave(snapshot.data());

		check(set.stateLoad(snapshot.data()) == g2::Status::Ok,
			"a set holding no bridges takes back the snapshot it wrote");

		g2::attachHdi08Bridges(adapter, set);

		check(set.stateLoad(snapshot.data()) == g2::Status::BridgesAttached,
			"a set holding bridges refuses a snapshot that carries none of their state");

		const bool* const landed = set.programLanded(g_landedSlot);
		check(landed != nullptr && !*landed,
			"the slot under test has not landed a program before the push");

		driveBootHeaders(adapter.port(static_cast<int>(g_landedSlot)), programWordCount());
		driveProgramWords(adapter.port(static_cast<int>(g_landedSlot)), g_program.size());

		check(landed != nullptr && *landed,
			"the slot under test has landed its program before the refused load");

		/* The snapshot predates the push, so a load that ran at all would zero the
		 * program memory the push filled. The reads below are what separates a
		 * refusal from a load that reported one and copied anyway. */
		check(set.stateLoad(snapshot.data()) == g2::Status::BridgesAttached,
			"a set holding bridges refuses the load once a program has landed too");

		check(landed != nullptr && *landed,
			"a refused load leaves a landed slot landed");

		for(size_t i = 0; i < g_program.size(); ++i)
		{
			checkEqualHex(programMemory(set.dsp(g_landedSlot), g_bootAddress + TWord(i)),
				g_program[i],
				"a refused load leaves the slot's program memory where it was" + atIndex(i));
		}

		checkEqualHex(set.dsp(g_landedSlot).getPC().toWord(), g_bootAddress,
			"a refused load leaves the slot's program counter where the push primed it");
	}
}

int main()
{
	aBootstrapPushLandsTheProgramAndPrimesTheCore();
	anIncompleteLoadKeepsFeedingProgramMemory();
	theSetPublishesAPerSlotLandedFlag();
	aDestroyedBridgeLeavesNoCallbackBehind();
	aSecondAttachOnOneSetIsRefused();
	aDestroyedSetLeavesNoCallbackBehindOnAnyPort();
	aStateLoadIntoASetHoldingBridgesIsRefused();

	if(g_failures != 0)
	{
		std::cout << "t0_dsp_boot_consumer: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_dsp_boot_consumer: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
