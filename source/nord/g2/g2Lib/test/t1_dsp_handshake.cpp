// Tier T1: the block declares T1, so this file resolves through
// ArtifactResolver and skips with a reason when NMG2_ARTIFACTS does not
// resolve. The gate is the tier and not a dependence on a file -- this check
// reads no firmware artifact; it composes a `g2::Board`, drives host flags and
// reads host registers, and every input it has is compiled in.
//
// The drive and the census run over all eight host ports: for every `i` in
// `[0, board.dspSet().dspCount())` it sets HF0 in host port `i`'s ICR, polls
// that port's ISR for HF2 under the firmware's own `0xFDE8` iteration bound,
// and after the drive returns it asserts that the count of ports whose ICR
// carries HF0 and whose ISR carries HF2 equals `board.dspSet().dspCount()`,
// which it separately asserts is eight.
//
// What it does not establish: that a DSP running the firmware's own kernel
// answers HF0 with HF2, nor that the firmware's `0x30039398` is ever reached.
// This is evidence that the drive and the census run over eight ports and that
// every slot's flags reach the host. It is not evidence that the handshake
// completes for the right reason, so a later pass that finds the
// firmware-driven handshake still failing must suspect the reply predicate
// first.
//
// HF0 is driven through the port's ICR and never through TXH, TXM and TXL.
// `dsp56k::DspBoot` sits unconditionally in front of every bridged port and
// absorbs the first transmit words as a count header and an address, so a
// driver that reached for the transmit registers would be driving a bootstrap.
// A control-register write is not a transmit word and reaches that consumer not
// at all, so this check drives no bootstrap and asserts nothing about program
// memory.
//
// The DSP-side flag is driven and not executed: no core executes here and no
// `Scheduler` is needed. The reply is given through
// `dsp56k::HDI08::writeControlRegister` with the HCR_HF2 bit, at the slot the
// port is bridged to, and it is given only when that slot's own HSR already
// carries HF0 -- so the poll loop measures the whole chain (host ICR -> bridge
// -> DSP HSR -> DSP HCR -> bridge -> host ISR) and a break anywhere along it
// exhausts the iteration bound instead of converging.
//
// The predicate is and-ed for a measured reason and not for symmetry:
// `mc68k::Hdi08::isr()` ORs `Txde` into every read it answers, so a census that
// tested a non-zero ISR would count eight ports with nothing behind any of them.

#include "board.h"
#include "dspSet.h"
#include "gatedFixture.h"
#include "hdi08Adapter.h"
#include "hdi08Decode.h"

#include "../artifactResolver.h"

#include "mc68k/hdi08.h"

#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/types.h"

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

	void checkEqualCount(const unsigned _actual, const unsigned _expected, const std::string& _what)
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

	// The eight host ports the machine presents, taken from hdi08Decode.h's own
	// constant rather than restated as a literal.
	constexpr unsigned g_expectedPortCount = unsigned(g2::g_hdi08PortCount);

	// The firmware's own retry count for the handshake at `0x30039398`. It
	// bounds this harness's polling too, so a machine that never converges stops
	// rather than hanging the suite.
	constexpr uint32_t g_handshakeIterations = 0xFDE8u;

	// HF0 sits at bit 3 of the host ICR and at HSR_HF0 of the DSP status
	// register; HF2 sits at bit 3 of the host ISR and at HCR_HF2 of the DSP
	// control register. Both are masked copies rather than translations, which
	// is what hdi08Bridge.cpp's two comments already record.
	constexpr TWord g_dspHf0 = TWord(1u) << dsp56k::HDI08::HSR_HF0;
	constexpr TWord g_dspHf2 = TWord(1u) << dsp56k::HDI08::HCR_HF2;

	// The census is written over two register reads, `icr() & Hf0` and
	// `isr() & Hf2`. The `Hdi08Adapter` the `Board` already owns through
	// `board.hdi08()` is the way in to port `i`.
	unsigned handshakePortCount(g2::Board& _board)
	{
		unsigned completed = 0;

		for(int port = 0; port < g2::g_hdi08PortCount; ++port)
		{
			auto& hdi08 = _board.hdi08().port(port);

			// icr() and isr() are the model's own register readers. The
			// BusTarget-facing read8 override is deliberately not used: this is
			// an inspection and it must not look like a bus cycle.
			const uint8_t icr = hdi08.icr();
			const uint8_t isr = hdi08.isr();

			if((icr & mc68k::Hdi08::Hf0) && (isr & mc68k::Hdi08::Hf2))
				++completed;
		}

		return completed;
	}

	struct DriveResult
	{
		bool     completed  = false;
		uint32_t iterations = 0;
	};

	// The drive for one port. HF0 goes in through the ICR and through nothing
	// else; the existing ICR byte is preserved rather than overwritten, so this
	// sets a flag and does not reset the port.
	DriveResult driveOnePort(g2::Board& _board, const unsigned _slot)
	{
		auto& port = _board.hdi08().port(static_cast<int>(_slot));
		auto& dsp  = _board.dspSet().peripherals(_slot).getHDI08();

		port.write8(mc68k::PeriphAddress::HdiICR,
			uint8_t(port.icr() | mc68k::Hdi08::Hf0));

		DriveResult result;

		for(uint32_t i = 0; i < g_handshakeIterations; ++i)
		{
			result.iterations = i + 1;

			// The reply, given only once this slot's own HSR carries HF0. A
			// bridge that never forwarded the ICR write leaves HF0 clear here
			// and no reply is ever given, which is what makes the bound expire
			// rather than the census pass for the wrong reason.
			if(dsp.readStatusRegister() & g_dspHf0)
				dsp.writeControlRegister(dsp.readControlRegister() | g_dspHf2);

			if(port.isr() & mc68k::Hdi08::Hf2)
			{
				result.completed = true;
				return result;
			}
		}

		return result;
	}

	std::string onPort(const unsigned _slot)
	{
		return " on port " + std::to_string(_slot);
	}

	/* ---------------- the drive and the census over all eight host ports */

	void theDriveAndTheCensusRunOverAllEightPorts()
	{
		g2::Board board;

		const unsigned dspCount = board.dspSet().dspCount();

		checkEqualCount(dspCount, g_expectedPortCount,
			"the board's DSP set holds one slot per host port");

		/* The negative control: a census that answered eight whatever the machine
		 * did would satisfy every line below it, and this one line is what such a
		 * census cannot satisfy. */
		checkEqualCount(handshakePortCount(board), 0u,
			"no port completes the handshake before anything is driven");

		for(unsigned i = 0; i < dspCount; ++i)
		{
			const DriveResult r = driveOnePort(board, i);

			check(r.completed,
				"the port answers HF0 with HF2 inside the firmware's own 0xFDE8 iterations"
					+ onPort(i) + " (iterations used " + std::to_string(r.iterations) + ")");

			checkEqualCount(handshakePortCount(board), i + 1u,
				"exactly the ports driven so far have completed the handshake, after driving"
					+ onPort(i));

			/* This separates a wiring that sends every port to one slot from the
			 * correct one. Under such a wiring the slot just replied to mirrors
			 * its HF2 out to every port at once, so a port nobody has driven
			 * reads HF2 set. The census above cannot see it, because an undriven
			 * port has no HF0 in its ICR and the predicate is and-ed. */
			for(unsigned j = i + 1; j < dspCount; ++j)
			{
				const uint8_t isr = board.hdi08().port(static_cast<int>(j)).isr();

				check((isr & mc68k::Hdi08::Hf2) == 0,
					"a port that has not been driven reports HF2 clear" + onPort(j)
						+ ", after driving" + onPort(i));
			}
		}

		// The census, asserted after the drive loop returns.
		checkEqualCount(handshakePortCount(board), dspCount,
			"every host port of the assembled machine completed the HF0/HF2 handshake");
	}
}

int main()
{
	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		theDriveAndTheCensusRunOverAllEightPorts();

		if(g_failures != 0)
		{
			std::cout << "t1_dsp_handshake: " << g_failures << " failure(s) in "
				<< g_cases << " case(s)" << std::endl;
			return false;
		}

		std::cout << "t1_dsp_handshake: all " << g_cases << " cases passed" << std::endl;
		return true;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
