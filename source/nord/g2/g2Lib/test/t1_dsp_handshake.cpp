// Task BRD-18, with BRD-27 absorbed into it as STEP 2 on 2026-08-24 (plan
// section 24.6 row W3-390). Tier T1: the block declares T1, so this file
// resolves through ArtifactResolver and SKIPS with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// THE GATE IS THE TIER AND NOT A DEPENDENCE ON A FILE, AND THAT IS SAID HERE
// RATHER THAN LEFT TO BE DISCOVERED. This check reads no firmware artifact: it
// composes a `g2::Board`, drives host flags and reads host registers, and every
// input it has is compiled in. The gate is written because the block's heading
// declares the tier, in the shape t1_sprintf_isolated already uses, so a
// machine without artifacts reports the section 18.5 skip line rather than a
// silent pass.
//
// WHAT THIS CHECK ESTABLISHES. The drive AND the census run over ALL EIGHT
// host ports: for every `i` in `[0, board.dspSet().dspCount())` it sets HF0 in
// host port `i`'s ICR, polls that port's ISR for HF2 under the firmware's own
// `0xFDE8` iteration bound, and after the drive returns it asserts that the
// count of ports whose ICR carries HF0 AND whose ISR carries HF2 EQUALS
// `board.dspSet().dspCount()`, which it separately asserts is eight.
//
// WHAT IT DOES NOT ESTABLISH, AND THE LIST IS THE POINT RATHER THAN A HEDGE.
// It does NOT establish that a DSP running the firmware's own kernel answers
// HF0 with HF2 -- plan section 6.6.11's closing paragraph records that predicate
// as UNMEASURED -- nor that the firmware's `0x30039398` is ever reached, which
// FINDING 2 puts behind case 6 of the message-dispatch switch at `0x30012050`.
// This is evidence that the drive and the census run over eight ports and that
// every slot's flags reach the host. IT IS NOT EVIDENCE THAT THE HANDSHAKE
// COMPLETES FOR THE RIGHT REASON, and a later pass that finds the
// firmware-driven handshake still failing must suspect the reply predicate
// first. No reply behaviour is invented here and none may be inferred from the
// census.
//
// HF0 IS DRIVEN THROUGH THE PORT'S ICR AND NEVER THROUGH TXH, TXM AND TXL, and
// that sentence is load-bearing rather than a detail. `dsp56k::DspBoot` sits
// unconditionally in front of every bridged port and absorbs the first transmit
// words as a count header and an address, so a driver that reached for the
// transmit registers would be driving a bootstrap. A CONTROL-REGISTER WRITE IS
// NOT A TRANSMIT WORD AND REACHES THAT CONSUMER NOT AT ALL, so this check
// drives no bootstrap and asserts nothing about program memory.
//
// THE DSP-SIDE FLAG IS DRIVEN AND NOT EXECUTED, AND THAT IS A DECISION RATHER
// THAN A SHORTCUT. No core executes here and no `Scheduler` is needed. The
// reply is given through `dsp56k::HDI08::writeControlRegister` with the HCR_HF2
// bit, at the slot the port is bridged to, and it is given ONLY WHEN THAT
// SLOT'S OWN HSR ALREADY CARRIES HF0 -- so the poll loop measures the whole
// chain (host ICR -> bridge -> DSP HSR -> DSP HCR -> bridge -> host ISR) and a
// break anywhere along it exhausts the iteration bound instead of converging.
//
// THE PREDICATE IS AND-ED FOR A MEASURED REASON AND NOT FOR SYMMETRY:
// `mc68k::Hdi08::isr()` ORs `Txde` into every read it answers, so a census that
// tested a non-zero ISR would count eight ports with nothing behind any of them.
//
// EVERY ASSERTION IN THIS FILE IS A RUNTIME check() AND NEVER A LANGUAGE
// assert(). The default build type is Release, which defines NDEBUG.

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

	// The eight host ports the machine presents. `g_hdi08PortCount` is BRD-15's
	// own constant at hdi08Decode.h line 35 and is not restated as a literal.
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

	// THIS FILE'S OWN CENSUS AND NOT ANOTHER FILE'S. t1_boot.cpp's helper is
	// INT-1's and is outside this task's Files: line; this one is written over
	// the same two register reads, `icr() & Hf0` and `isr() & Hf2`, whose
	// enumerators BRD-16's own evidence table names.
	//
	// The `Hdi08Adapter` the `Board` already owns through `board.hdi08()` is the
	// way in to port `i`. No per-port accessor spelling is invented here,
	// because this plan declares none.
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

	// The drive for ONE port. HF0 goes in through the ICR and through nothing
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

		/* THE NEGATIVE CONTROL, AND IT IS NOT DECORATION. A census that answered
		 * eight whatever the machine did would satisfy every line below it; this
		 * one line is what a always-eight census cannot satisfy. */
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

			/* A WIRING THAT SENDS EVERY PORT TO ONE SLOT IS WHAT THIS SEPARATES
			 * FROM THE CORRECT ONE. Under such a wiring the slot just replied to
			 * mirrors its HF2 out to every port at once, so a port nobody has
			 * driven reads HF2 set. The census above cannot see it, because an
			 * undriven port has no HF0 in its ICR and the predicate is AND-ed. */
			for(unsigned j = i + 1; j < dspCount; ++j)
			{
				const uint8_t isr = board.hdi08().port(static_cast<int>(j)).isr();

				check((isr & mc68k::Hdi08::Hf2) == 0,
					"a port that has not been driven reports HF2 clear" + onPort(j)
						+ ", after driving" + onPort(i));
			}
		}

		// THE CHECK: LINE'S CENSUS, ASSERTED AFTER THE DRIVE LOOP RETURNS.
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
