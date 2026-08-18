// Tier T0: this test needs no firmware artifact of any kind.
//
// THE HOST-SIDE RECEIVE ASSERTIONS READ `isr() & Rxdf` AND NEVER "the ISR is
// non-zero". `mc68k::Hdi08::isr()` ORs `Txde` into every read it answers and
// the adapter's init callback adds `Trdy` beside it, so a non-zero assertion
// is satisfied by a port with nothing behind it.
//
// EVERY DRIVEN WORD IS NON-ZERO, IN BOTH DIRECTIONS. A driven 0x000000
// compares zero against a default-zero read and passes whether the word
// crossed or not.
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

#include <cstdint>
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

	// The DSP is required and not decorative: `HDI08::writeRX` ends in
	// `IPeripherals::setDelayCycles`, which dereferences the attached DSP
	// unconditionally.
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

	constexpr int g_bridgedPort = 3;
	constexpr int g_unbridgedPort = 5;

	// HF2 and HF3 occupy bits 3 and 4 of the DSP's HCR and of the host ISR
	// alike, which is what makes the mirror a mask and not a translation.
	constexpr uint8_t g_hostFlagMask = mc68k::Hdi08::Hf2 | mc68k::Hdi08::Hf3;
	constexpr TWord g_dspHostFlags =
		TWord(1u << dsp56k::HDI08::HCR_HF2) | TWord(1u << dsp56k::HDI08::HCR_HF3);

	void hostWriteWord(mc68k::Hdi08& _port, const uint32_t _word)
	{
		_port.write8(mc68k::PeriphAddress::HdiTXH, uint8_t(_word >> 16));
		_port.write8(mc68k::PeriphAddress::HdiTXM, uint8_t(_word >> 8));
		_port.write8(mc68k::PeriphAddress::HdiTXL, uint8_t(_word));
	}

	uint32_t hostReadWord(mc68k::Hdi08& _port)
	{
		const uint32_t h = _port.read8(mc68k::PeriphAddress::HdiTXH);
		const uint32_t m = _port.read8(mc68k::PeriphAddress::HdiTXM);
		const uint32_t l = _port.read8(mc68k::PeriphAddress::HdiTXL);
		return (h << 16) | (m << 8) | l;
	}

	uint32_t ringWord(const dsp56k::HDI08& _dsp, const size_t _index)
	{
		return _dsp.rxData()[_index];
	}

	/* A BRIDGED PORT FEEDS ITS BOOT CONSUMER UNTIL A PROGRAM HAS LANDED, so the
	 * runtime wire is not reachable before that. The load driven here is the
	 * shortest one the protocol admits. */
	constexpr TWord g_bootAddress = 0x000300u;
	constexpr TWord g_bootBodyWord = 0x0b0071u;

	void driveBootstrap(mc68k::Hdi08& _port)
	{
		hostWriteWord(_port, 1u);
		hostWriteWord(_port, g_bootAddress);
		hostWriteWord(_port, g_bootBodyWord);
	}

	/* ---------------- group 1: one bridged pair, both directions */

	void aBridgedPairCarriesAWordEachWay()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		driveBootstrap(adapter.port(g_bridgedPort));

		// THE FAN-OUT CASE RUNS FIRST SO ITS EXPECTED RING SIZE IS ZERO RATHER
		// THAN A DELTA. A bridge that fans every host word to every DSP passes
		// every other case in this file.
		hostWriteWord(adapter.port(g_unbridgedPort), 0x0f00a5u);
		check(!slot.hdi08().hasRXData(),
			"a word driven at an unbridged port reports no receive data on the bridged DSP");
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"a word driven at an unbridged port leaves the bridged DSP's receive ring empty");

		hostWriteWord(adapter.port(g_bridgedPort), 0x0abcdeu);
		check(slot.hdi08().hasRXData(),
			"three byte writes to TXH, TXM and TXL report receive data on the DSP");
		checkEqualCount(slot.hdi08().rxData().size(), 1u,
			"three byte writes to TXH, TXM and TXL assemble exactly one word on the DSP");
		if(slot.hdi08().rxData().size() == 1u)
		{
			checkEqualHex(ringWord(slot.hdi08(), 0), 0x0abcdeu,
				"the word the DSP received is the word the host assembled");
		}

		slot.hdi08().writeTX(0x135791u);
		checkEqualHex(adapter.port(g_bridgedPort).isr() & mc68k::Hdi08::Rxdf, mc68k::Hdi08::Rxdf,
			"the host port's ISR reports Rxdf after the DSP writes its host-transmit path");
		checkEqualHex(hostReadWord(adapter.port(g_bridgedPort)), 0x135791u,
			"the host read returns the DSP's word unchanged");

		// A SECOND DSP WORD ARRIVES WHILE THE HOST RECEIVE REGISTER IS STILL
		// FULL, so it can only reach the host through the receive-empty hook.
		slot.hdi08().writeTX(0x2468acu);
		slot.hdi08().writeTX(0x0c0ffeu);
		checkEqualHex(hostReadWord(adapter.port(g_bridgedPort)), 0x2468acu,
			"the first of two queued DSP words reaches the host first");
		checkEqualHex(hostReadWord(adapter.port(g_bridgedPort)), 0x0c0ffeu,
			"the second of two queued DSP words reaches the host next");

		// Nothing host-side sets ISR Hf2 or Hf3, so the two bits stay zero
		// until a bridge mirrors the DSP's HCR into them.
		slot.hdi08().writeControlRegister(g_dspHostFlags);
		checkEqualHex(adapter.port(g_bridgedPort).isr() & g_hostFlagMask, g_hostFlagMask,
			"the DSP's HF2 and HF3 both reach the host ISR");

		slot.hdi08().writeControlRegister(0);
		checkEqualHex(adapter.port(g_bridgedPort).isr() & g_hostFlagMask, 0u,
			"the host ISR host flags follow the DSP's HCR back to clear");

		// `setInitHdi08Callback` holds one std::function and REPLACES it, so a
		// bridge that installed there would silently remove the adapter's own
		// ICR clear. This assertion is on a BRIDGED port for that reason.
		adapter.port(g_bridgedPort).write8(mc68k::PeriphAddress::HdiICR, mc68k::Hdi08::Init);
		checkEqualHex(adapter.port(g_bridgedPort).icr() & mc68k::Hdi08::Init, 0u,
			"a bridged port still clears the ICR INIT bit");
		checkEqualHex(adapter.port(g_bridgedPort).isr() & (mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy),
			mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy,
			"a bridged port still raises Txde and Trdy on the init request");
	}

	/* ---------------- group 2: the same drive with no bridge at all */

	void anUnbridgedPairCarriesNothingEitherWay()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};

		hostWriteWord(adapter.port(g_bridgedPort), 0x0dead1u);
		check(!slot.hdi08().hasRXData(),
			"an unbridged DSP reports no receive data after the host assembles a word");
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"an unbridged DSP's receive ring stays empty after the host assembles a word");

		slot.hdi08().writeTX(0x0beef2u);
		checkEqualHex(adapter.port(g_bridgedPort).isr() & mc68k::Hdi08::Rxdf, 0u,
			"an unbridged host port reports no Rxdf after the DSP writes its host-transmit path");
		checkEqualHex(hostReadWord(adapter.port(g_bridgedPort)), 0u,
			"an unbridged host read does not return the DSP's word");

		slot.hdi08().writeControlRegister(g_dspHostFlags);
		checkEqualHex(adapter.port(g_bridgedPort).isr() & g_hostFlagMask, 0u,
			"an unbridged host ISR reads the DSP's HF2 and HF3 clear");
	}

	/* ---------------- group 3: the per-quantum bound */

	void aFullReceiveRingDefersTheHostWordInsteadOfBlocking()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		driveBootstrap(adapter.port(g_bridgedPort));

		const size_t capacity = slot.hdi08().rxData().capacity();

		std::vector<TWord> filler(capacity);
		for(size_t i = 0; i < capacity; ++i)
			filler[i] = TWord(0x00c00000u | uint32_t(i));

		slot.hdi08().writeRX(filler.data(), capacity);
		checkEqualCount(slot.hdi08().rxData().size(), capacity,
			"the receive ring starts this group full");

		hostWriteWord(adapter.port(g_bridgedPort), 0x0a1b2cu);
		checkEqualCount(slot.hdi08().rxData().size(), capacity,
			"a host word offered to a full ring moves no word");

		slot.hdi08().clearRX();
		checkEqualCount(slot.hdi08().rxData().size(), 0u,
			"the receive ring is empty again after the DSP clears it");

		hostWriteWord(adapter.port(g_bridgedPort), 0x0d4e5fu);
		checkEqualCount(slot.hdi08().rxData().size(), 2u,
			"the deferred word and the next word both arrive once the ring has room");
		if(slot.hdi08().rxData().size() == 2u)
		{
			checkEqualHex(ringWord(slot.hdi08(), 0), 0x0a1b2cu,
				"the word deferred by the full ring arrives first");
			checkEqualHex(ringWord(slot.hdi08(), 1), 0x0d4e5fu,
				"the word driven after the ring drained arrives second");
		}
	}

	/* ---------------- group 4: the installation against the DSP set */

	uint32_t slotWord(const unsigned _slot)
	{
		return 0x0a0000u | ((_slot + 1u) * 0x0101u);
	}

	void theSetIsBridgedPortForPort()
	{
		g2::DspSet set;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};

		g2::attachHdi08Bridges(adapter, set);

		/* THE COUNT IS READ BACK THROUGH THE SET, WHICH OWNS THE BRIDGES. The
		 * per-slot flag is the only handle the set publishes on one, so a slot
		 * that answers it is a slot that got a bridge. */
		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			check(set.programLanded(i) != nullptr,
				"the installation creates a bridge on slot " + std::to_string(i));
		}
		check(set.programLanded(set.dspCount()) == nullptr,
			"the installation creates no bridge past the last slot");

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			driveBootstrap(adapter.port(static_cast<int>(i)));
			hostWriteWord(adapter.port(static_cast<int>(i)), slotWord(i));
		}

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			const dsp56k::HDI08& hdi08 = set.peripherals(i).getHDI08();
			const std::string slotName = " on slot " + std::to_string(i);

			checkEqualCount(hdi08.rxData().size(), 1u,
				"exactly one word arrives" + slotName);

			if(hdi08.rxData().size() != 1u)
				continue;

			checkEqualHex(ringWord(hdi08, 0), slotWord(i),
				"the arriving word is the one driven at that slot's own port" + slotName);
		}
	}
}

int main()
{
	aBridgedPairCarriesAWordEachWay();
	anUnbridgedPairCarriesNothingEitherWay();
	aFullReceiveRingDefersTheHostWordInsteadOfBlocking();
	theSetIsBridgedPortForPort();

	if(g_failures != 0)
	{
		std::cout << "t0_hdi08_dsp_bridge: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_hdi08_dsp_bridge: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
