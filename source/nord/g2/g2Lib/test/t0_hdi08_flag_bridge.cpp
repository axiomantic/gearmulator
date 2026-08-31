// Tier T0: this test needs no firmware artifact of any kind.
//
// The DSP is required and not decorative. `HDI08::readStatusRegister` applies
// pending host flags from the atomic latch that `setPendingHostFlags01` writes;
// no DSP means no latch and no status read.

#include "dspSet.h"
#include "hdi08Adapter.h"
#include "hdi08Bridge.h"
#include "hdi08Decode.h"

#include "mc68k/hdi08.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
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

	struct Slot
	{
		dsp56k::Memory           memory;
		dsp56k::Peripherals56311 peripherals;
		dsp56k::DSP              dsp;

		Slot()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, peripherals(48000u / 16u) // second-bus frame rate
			, dsp(memory, &peripherals, &peripherals.ySpace())
		{
		}

		dsp56k::HDI08& hdi08() { return peripherals.getHDI08(); }
	};

	constexpr int g_bridgedPort   = 3;
	constexpr int g_unbridgedPort = 5;

	// HF0 sits at bit 3 of the host ICR and at bit 3 (HSR_HF0) of the DSP HSR.
	constexpr TWord g_dspHf0 = TWord(1u << dsp56k::HDI08::HSR_HF0);

	void hostWriteWord(mc68k::Hdi08& _port, const uint32_t _word)
	{
		_port.write8(mc68k::PeriphAddress::HdiTXH, uint8_t(_word >> 16));
		_port.write8(mc68k::PeriphAddress::HdiTXM, uint8_t(_word >> 8));
		_port.write8(mc68k::PeriphAddress::HdiTXL, uint8_t(_word));
	}

	// The shortest program the protocol admits, so the runtime TX path is
	// reachable. The ICR callback fires independently of the TX path, but the
	// bridged port's TX callback is replaced on program-land, so driving the
	// bootstrap first exercises the real arrangement.
	constexpr TWord g_bootAddress = 0x000300u;
	constexpr TWord g_bootBodyWord = 0x0b0071u;

	void driveBootstrap(mc68k::Hdi08& _port)
	{
		hostWriteWord(_port, 1u);
		hostWriteWord(_port, g_bootAddress);
		hostWriteWord(_port, g_bootBodyWord);
	}

	/* ---------------- an ICR write of HF0 reaches the DSP HSR */

	void anIcrWriteOfHf0ReachesTheDsp()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		driveBootstrap(adapter.port(g_bridgedPort));

		// Write 0x08 (HF0 alone) to the host ICR.
		adapter.port(g_bridgedPort).write8(mc68k::PeriphAddress::HdiICR, mc68k::Hdi08::Hf0);

		// The DSP's status register should reflect HF0. readStatusRegister
		// applies any pending flags written by setPendingHostFlags01.
		const TWord hsr = slot.hdi08().readStatusRegister();

		checkEqualHex(hsr & g_dspHf0, g_dspHf0,
			"writing 0x08 to a bridged host ICR sets HF0 in the DSP HSR");
	}

	/* ---------------- an ICR write without HF0 does not reach the DSP */

	void anIcrWriteOfZeroLeavesTheDspHf0Clear()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::Hdi08Bridge bridge(adapter.port(g_bridgedPort), slot.dsp, slot.hdi08());
		driveBootstrap(adapter.port(g_bridgedPort));

		// First set HF0 and drain it so the latch is clear.
		adapter.port(g_bridgedPort).write8(mc68k::PeriphAddress::HdiICR, mc68k::Hdi08::Hf0);
		slot.hdi08().readStatusRegister();

		// Now write 0x00 -- no flag bits set.
		adapter.port(g_bridgedPort).write8(mc68k::PeriphAddress::HdiICR, 0);

		const TWord hsr = slot.hdi08().readStatusRegister();

		checkEqualHex(hsr & g_dspHf0, 0u,
			"writing 0x00 to a bridged host ICR after a drain leaves the DSP HSR HF0 clear");
	}

	/* ---------------- an unbridged port does not forward */

	void anIcrWriteOnAnUnbridgedPortDoesNotReachTheDsp()
	{
		Slot slot;
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		// No bridge -- the port's ICR-write callback stays the default no-op.

		adapter.port(g_unbridgedPort).write8(mc68k::PeriphAddress::HdiICR, mc68k::Hdi08::Hf0);

		const TWord hsr = slot.hdi08().readStatusRegister();

		checkEqualHex(hsr & g_dspHf0, 0u,
			"writing 0x08 to an unbridged host ICR leaves the attached DSP HSR HF0 clear");

		// The write IS stored in the port's own ICR register -- the callback
		// default is no-op, not a write-through blocker.
		checkEqualHex(adapter.port(g_unbridgedPort).icr() & mc68k::Hdi08::Hf0, mc68k::Hdi08::Hf0,
			"an unbridged port still stores the ICR byte; the callback alone is absent");
	}

	/* ---------------- the callback is installed on every bridged port */

	void theCallbackIsInstalledOnEveryBridgedPort()
	{
		// The eight-slot set attachment from t0_hdi08_dsp_bridge, adapted for
		// the flag path. A bridge whose ICR-write callback is the default no-op
		// stores the ICR byte on the host side and forwards nothing.
		Slot slots[8];
		g2::Hdi08Adapter adapter{g2::Hdi08Decode(g2::g_hdi08ExpandedPorts)};
		g2::DspSet set;

		g2::attachHdi08Bridges(adapter, set);

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			driveBootstrap(adapter.port(static_cast<int>(i)));
		}

		// Write HF0 to every port's ICR simultaneously through the adapter.
		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			adapter.port(static_cast<int>(i)).write8(
				mc68k::PeriphAddress::HdiICR, mc68k::Hdi08::Hf0);
		}

		for(unsigned i = 0; i < set.dspCount(); ++i)
		{
			const TWord hsr = set.peripherals(i).getHDI08().readStatusRegister();
			const std::string slotName = " on slot " + std::to_string(i);

			checkEqualHex(hsr & g_dspHf0, g_dspHf0,
				"the bridged DSP's HF0 flag is set" + slotName);
		}
	}
}

int main()
{
	anIcrWriteOfHf0ReachesTheDsp();
	anIcrWriteOfZeroLeavesTheDspHf0Clear();
	anIcrWriteOnAnUnbridgedPortDoesNotReachTheDsp();
	theCallbackIsInstalledOnEveryBridgedPort();

	if(g_failures != 0)
	{
		std::cout << "t0_hdi08_flag_bridge: " << g_failures << " failure(s) in "
		          << g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_hdi08_flag_bridge: all " << g_cases << " cases passed" << std::endl;
	return 0;
}