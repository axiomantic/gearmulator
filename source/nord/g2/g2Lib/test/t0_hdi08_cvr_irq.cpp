// Tier T0: this test needs no firmware artifact of any kind.
//
// A PARTITION AT `dsp56k::Vba_IRQA` IS TAKEN INSTEAD OF AN EXACT VECTOR
// READBACK. The core's pending-vector queue is private, the only public
// predicates over it return bool, and `injectInterrupt` returns a constant, so
// no surface reports the value. What the partition rests on: out of reset both
// interrupt-mask bits are set, `isInterruptMasked` masks every vector at or
// above that base, and `execInterrupts` returns on a masked vector WITHOUT
// popping it -- so a vector below the base drains under a bounded pump and one
// at or above it never does. THE EXACT-VALUE ROUTE IS REJECTED HERE AND NOT
// DENIED: running the core and observing that it executed from the vector
// address would discriminate the number, and it needs distinguishable code
// planted at every candidate vector address, which this file does not build.
// The partition therefore separates the correct vector from every re-derivation
// of it and does not separate it from an arbitrary other value below the base.
//
// THE LANDED PROGRAM CLOSES A LOOP RATHER THAN RUNNING STRAIGHT. The pump
// EXECUTES from the boot address, and a straight-line program walks off the end
// of program memory into an out-of-range jit entry.
//
// assert() IS NOT USED FOR ANY ASSERTION HERE: NDEBUG compiles it out.

#include "hdi08Adapter.h"
#include "hdi08Bridge.h"
#include "hdi08Decode.h"

#include "g2/timebase.h"

#include "mc68k/hdi08.h"
#include "mc68k/peripheralTypes.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/types.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

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
			/* ONE exec() IS ONE BLOCK HERE. With block linking on, a dispatch
			 * unit runs a chain, and the chain this fixture's self-jump program
			 * forms has no end. */
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
		}

		dsp56k::HDI08& hdi08() { return peripherals.getHDI08(); }
	};

	constexpr int g_bridgedPort = 2;
	constexpr int g_unbridgedPort = 5;

	/* `V` IS CHOSEN RATHER THAN ARBITRARY, AND THE static_asserts BELOW ARE
	 * WHAT FIX IT -- TO A RANGE, NOT TO A VALUE. Each one carries a single
	 * clause, so a failure names the premise that broke, and each one can be
	 * falsified on its own by some byte.
	 *
	 * WHAT THEY DO NOT CONSTRAIN, SAID HERE SO NO READER TAKES MORE FROM THEM.
	 * Of the re-derivations this check catches, only the second shift is pinned
	 * by the choice of `V`. The rest are at or above `Vba_IRQA` for EVERY byte
	 * rather than for this one: the raw CVR value always carries `Hc` and so
	 * can never fall below the base, and any vector the first assert admits,
	 * plus `Vba_Host_Command`, is already above it. Asserting them would add a
	 * clause that no choice of `V` could falsify, which reads as a constraint
	 * and is not one. They are caught by the check at run time all the same;
	 * they are simply not what this byte is chosen for. More than one byte
	 * satisfies the set below, and the partition does not separate the ones it
	 * admits -- the same residue the file header prices. */
	constexpr uint8_t g_hostVector = 0x07;
	constexpr uint8_t g_expectedVector = uint8_t((g_hostVector & mc68k::Hdi08::Hv) << 1);

	static_assert(g_expectedVector < dsp56k::Vba_IRQA,
		"the host vector under test must fall below Vba_IRQA or the pump can never drain");
	static_assert((g_hostVector & mc68k::Hdi08::Hc) == 0,
		"the constant is the vector field alone -- writeHostCommand supplies the Hc bit");
	static_assert(uint8_t(g_expectedVector << 1) >= dsp56k::Vba_IRQA,
		"a second shift of the computed vector must land at or above Vba_IRQA or that "
		"re-derivation would drain and pass");

	constexpr TWord g_bootAddress = 0x000240u;

	/* THE PUMP IS BOUNDED SO THAT A STUCK VECTOR IS A FAILING ASSERTION AND NOT
	 * A HANGING TEST. A masked vector is never popped, so an unbounded loop on
	 * `hasPendingInterrupts()` would spin until ctest killed it and report a
	 * timeout instead of a value. */
	constexpr int g_pumpLimit = 64;

	void hostWriteWord(mc68k::Hdi08& _port, const uint32_t _word)
	{
		_port.write8(mc68k::PeriphAddress::HdiTXH, uint8_t(_word >> 16));
		_port.write8(mc68k::PeriphAddress::HdiTXM, uint8_t(_word >> 8));
		_port.write8(mc68k::PeriphAddress::HdiTXL, uint8_t(_word));
	}

	// Assembled rather than spelled as a literal opcode, so that the loop is the
	// assembler's word and not a hand-copied constant.
	const std::vector<TWord>& selfJumpProgram()
	{
		static const std::vector<TWord> program = []
		{
			const dsp56k::Assembler assembler;
			char text[32];
			snprintf(text, sizeof(text), "jmp $%x", static_cast<unsigned>(g_bootAddress));

			const dsp56k::AssembleResult result = assembler.assemble(text);

			std::vector<TWord> words;
			if(!result.success())
				return words;

			for(uint32_t i = 0; i < result.wordCount; ++i)
				words.push_back(result.word[i]);
			return words;
		}();

		return program;
	}

	void landProgram(mc68k::Hdi08& _port)
	{
		const std::vector<TWord>& program = selfJumpProgram();

		check(!program.empty(), "the fixture assembled the self-jump the pump runs");

		hostWriteWord(_port, static_cast<TWord>(program.size()));
		hostWriteWord(_port, g_bootAddress);
		for(const auto word : program)
			hostWriteWord(_port, word);
	}

	void writeHostCommand(mc68k::Hdi08& _port, const uint8_t _vector)
	{
		_port.write8(mc68k::PeriphAddress::HdiCVR, uint8_t(_vector | mc68k::Hdi08::Hc));
	}

	// Returns the number of exec() steps the core needed to come back to rest,
	// or g_pumpLimit if it never did.
	int pumpUntilDrained(dsp56k::DSP& _dsp)
	{
		for(int i = 0; i < g_pumpLimit; ++i)
		{
			if(!_dsp.hasPendingInterrupts())
				return i;
			_dsp.exec();
		}
		return g_pumpLimit;
	}

	/* ---------------- group 1: the vector leaves the host port
	 *
	 * THE UNBRIDGED HALF IS THE CONTROL AND IT EXERCISES `mc68k` RATHER THAN
	 * THIS TASK. It earns its place by fixing the expected byte by measurement
	 * instead of by hand, and by naming where the vector goes when no bridge
	 * takes it rather than only where it does not go. */
	void anUnbridgedPortKeepsTheVectorOnItsOwnQueue()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		mc68k::Hdi08& port = adapter.port(g_unbridgedPort);

		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core of an unbridged pair is in default processing before the host command");
		check(!slot.dsp.hasPendingInterrupts(),
			"the core of an unbridged pair holds no pending interrupt before the host command");

		uint8_t queued = 0;
		check(!port.pollInterruptRequest(queued),
			"an unbridged port holds no interrupt request before the host command");

		writeHostCommand(port, g_hostVector);

		check(port.pollInterruptRequest(queued),
			"an unbridged port holds an interrupt request after the host command");
		checkEqualHex(queued, g_expectedVector,
			"the byte an unbridged port queued is the vector the port computed");

		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core of an unbridged pair is still in default processing after the host command");
		check(!slot.dsp.hasPendingInterrupts(),
			"a host command on an unbridged port reaches no core");
	}

	void aBridgedPortKeepsNothingOnItsOwnQueue()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		mc68k::Hdi08& port = adapter.port(g_bridgedPort);

		landProgram(port);
		check(*bridge.programLanded(),
			"the program landed before the host command is written");

		writeHostCommand(port, g_hostVector);

		uint8_t queued = 0;
		check(!port.pollInterruptRequest(queued),
			"a bridged port keeps no interrupt request of its own after the host command");
	}

	/* ---------------- group 2: the core acquires a pending interrupt
	 *
	 * `getProcessingMode()` IS READ BESIDE EVERY `hasPendingInterrupts()`
	 * BECAUSE THE PREDICATE OPENS WITH `if(m_processingMode != Default) return
	 * true;`. Without that reading, a true could come from a core that is mid
	 * interrupt for some other reason and say nothing about this wire. */
	void aHostCommandGivesTheCoreAPendingInterrupt()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		mc68k::Hdi08& port = adapter.port(g_bridgedPort);

		landProgram(port);

		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core is in default processing before the host command");
		check(!slot.dsp.hasPendingInterrupts(),
			"the core holds no pending interrupt before the host command");

		writeHostCommand(port, g_hostVector);

		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core is still in default processing after the host command");
		check(slot.dsp.hasPendingInterrupts(),
			"the core holds a pending interrupt after the host command");
	}

	/* ---------------- group 3: the pending vector is one the core can service */
	void thePendingVectorDrainsUnderABoundedPump()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		mc68k::Hdi08& port = adapter.port(g_bridgedPort);

		landProgram(port);

		checkEqualHex(static_cast<uint32_t>(pumpUntilDrained(slot.dsp)), 0u,
			"a core with no host command behind it needs no pump to be at rest");

		writeHostCommand(port, g_hostVector);

		check(slot.dsp.hasPendingInterrupts(),
			"the core holds the pending interrupt the pump is about to drain");

		const int steps = pumpUntilDrained(slot.dsp);

		check(steps < g_pumpLimit,
			"the pending vector drains within the bounded pump");
		check(!slot.dsp.hasPendingInterrupts(),
			"the core holds no pending interrupt once the pump has run");
		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core is back in default processing once the pump has run");
	}

	/* ---------------- group 4: a destroyed bridge leaves no callback behind
	 *
	 * THE PORT'S OWN STATE IS THE SENTINEL AND NOT THE DEAD BRIDGE. Reading a
	 * destroyed object to see whether it was called is the fault under test, so
	 * the only readings taken after the destruction are the port's own queue and
	 * the core the bridge used to reach.
	 *
	 * THE ADAPTER AND THE SLOT BOTH OUTLIVE THE BRIDGE, which is the direction
	 * hdi08Bridge.h states: the destructor uninstalls through the port it was
	 * handed, so a port that died first would be dereferenced dead. */
	void aDestroyedBridgeReturnsTheHostCommandToThePort()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		mc68k::Hdi08& port = adapter.port(g_bridgedPort);

		{
			g2::Hdi08Bridge bridge(port, slot.dsp, slot.hdi08());

			landProgram(port);
			writeHostCommand(port, g_hostVector);

			uint8_t queuedWhileAlive = 0;
			check(!port.pollInterruptRequest(queuedWhileAlive),
				"a bridge that is alive keeps the host command off the port's own queue");
			check(slot.dsp.hasPendingInterrupts(),
				"a bridge that is alive puts the host command on its core");

			check(pumpUntilDrained(slot.dsp) < g_pumpLimit,
				"the core is brought back to rest before the bridge is destroyed");
		}

		writeHostCommand(port, g_hostVector);

		uint8_t queued = 0;
		check(port.pollInterruptRequest(queued),
			"a host command written after the bridge is destroyed stays on the port's own queue");
		checkEqualHex(queued, g_expectedVector,
			"the byte the port kept is the vector it computed for the later host command");
		check(slot.dsp.getProcessingMode() == dsp56k::DSP::Default,
			"the core is in default processing after the bridge is destroyed");
		check(!slot.dsp.hasPendingInterrupts(),
			"a host command written after the bridge is destroyed reaches no core");
	}
}

int main()
{
	anUnbridgedPortKeepsTheVectorOnItsOwnQueue();
	aBridgedPortKeepsNothingOnItsOwnQueue();
	aHostCommandGivesTheCoreAPendingInterrupt();
	thePendingVectorDrainsUnderABoundedPump();
	aDestroyedBridgeReturnsTheHostCommandToThePort();

	if(g_failures != 0)
	{
		std::cout << "t0_hdi08_cvr_irq: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_hdi08_cvr_irq: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
