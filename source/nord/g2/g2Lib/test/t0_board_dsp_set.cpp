// Task BRD-26. Tier T0: this test needs no firmware artifact of any kind.
//
// EVERY DRIVEN WORD IS NON-ZERO, the count and address headers as well as the
// body. Program memory is zero-filled and a receive path reads zero for a word
// that never arrived, so 0x000000 compares equal against both and would pass
// whether it crossed or not.
//
// THE HANDSHAKE IS DRIVEN BEFORE EVERY RUNTIME WORD, AND AT THE PORT THE
// RUNTIME WORD IS DRIVEN AT. `dsp56k::DspBoot` sits in front of every bridged
// port, so a bare word is absorbed as the bootstrap count header and reaches no
// receive path -- under the correct composition as much as under a broken one.
// Driving the headers at the port under test is also what makes a wiring that
// sends port `i` to another slot land its whole program there and read
// differently from the correct one.
//
// EVERY SLOT'S RECEIVE PATH IS READ AT EVERY STEP. Reading only the slot being
// driven was rejected: a wiring that sends every port to one slot answers the
// same way at the first port driven, so the slots nobody drove are what
// separates it from the correct one.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build type is
// Release, which defines NDEBUG.

#include "board.h"
#include "dspSet.h"

#include "mc68k/hdi08.h"

#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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

	constexpr unsigned g_dspCount = 8;

	constexpr TWord g_bootAddress = 0x000240u;
	constexpr size_t g_programWordCount = 5;

	// Each slot takes its own program and its own runtime word: one value
	// shared across the slots would compare equal at the wrong slot.
	TWord programWord(const unsigned _slot, const size_t _index)
	{
		return TWord(0x0a1101u + (uint32_t(_slot) << 8) + uint32_t(_index) + 1u);
	}

	TWord runtimeWord(const unsigned _slot)
	{
		return TWord(0x0f6606u + (uint32_t(_slot) << 8) + 1u);
	}

	void hostWriteWord(mc68k::Hdi08& _port, const uint32_t _word)
	{
		_port.write8(mc68k::PeriphAddress::HdiTXH, uint8_t(_word >> 16));
		_port.write8(mc68k::PeriphAddress::HdiTXM, uint8_t(_word >> 8));
		_port.write8(mc68k::PeriphAddress::HdiTXL, uint8_t(_word));
	}

	void driveBootstrap(mc68k::Hdi08& _port, const unsigned _slot)
	{
		hostWriteWord(_port, TWord(g_programWordCount));
		hostWriteWord(_port, g_bootAddress);

		for(size_t i = 0; i < g_programWordCount; ++i)
			hostWriteWord(_port, programWord(_slot, i));
	}

	const auto& receivePath(g2::Board& _board, const unsigned _slot)
	{
		return _board.dspSet().peripherals(_slot).getHDI08().rxData();
	}

	std::string onSlot(const unsigned _slot, const std::string& _when)
	{
		return " on slot " + std::to_string(_slot) + " " + _when;
	}

	void checkEveryReceivePath(g2::Board& _board, const unsigned _slotsDriven, const std::string& _when)
	{
		for(unsigned i = 0; i < g_dspCount; ++i)
		{
			const auto& rx = receivePath(_board, i);
			const bool expectWord = i < _slotsDriven;

			checkEqualCount(rx.size(), expectWord ? 1u : 0u,
				"the receive path holds only what its own port took" + onSlot(i, _when));

			if(expectWord && rx.size() == 1u)
			{
				checkEqualHex(rx[0], runtimeWord(i),
					"the word on the receive path is the one driven at that slot's own port"
						+ onSlot(i, _when));
			}
		}
	}

	void theBoardDeliversEveryPortsRuntimeWordToItsOwnDsp()
	{
		g2::Board board;

		checkEqualCount(board.dspSet().dspCount(), g_dspCount,
			"the board's DSP set holds one slot per host port");

		checkEveryReceivePath(board, 0, "before any port is driven");

		for(unsigned i = 0; i < g_dspCount; ++i)
		{
			driveBootstrap(board.hdi08().port(static_cast<int>(i)), i);

			checkEveryReceivePath(board, i, "once the bootstrap handshake completed on port "
				+ std::to_string(i));

			hostWriteWord(board.hdi08().port(static_cast<int>(i)), runtimeWord(i));

			checkEveryReceivePath(board, i + 1, "once the runtime word was driven at port "
				+ std::to_string(i));
		}
	}

	/* THE TEARDOWN ORDER, BOUND ON A LIVING BOARD BECAUSE A DESTROYED ONE
	 * CANNOT BE READ -- the host port the assertion would have to examine is
	 * the one the destroyed Board owned. Members sharing an access control are
	 * allocated so that later ones have higher addresses, and they are
	 * destroyed in reverse declaration order, so the set outranking the adapter
	 * in address IS the set dying while the ports it uninstalls from are still
	 * alive. Reading the addresses through the two production accessors keeps
	 * the assertion off any member this class does not already publish.
	 *
	 * IT IS AN ADDRESS AND NOT THE TEARDOWN ITSELF, AND NO ASSERTION HERE CAN BE.
	 * The property wanted is that no callback survives on a port; the Board owns
	 * the ports, so they are gone with it and there is nothing left to read. The
	 * reading is sound only while the two members share one access specifier --
	 * an access specifier between them leaves their relative addresses
	 * unspecified while destruction order still follows declaration order.
	 * t0_dsp_boot_consumer takes the observation itself, against an adapter that
	 * the test owns and that outlives the set. */
	void theDspSetIsDestroyedBeforeTheAdapterItUninstallsFrom()
	{
		g2::Board board;

		const void* const adapter = &board.hdi08();
		const void* const set     = &board.dspSet();

		check(std::greater<const void*>{}(set, adapter),
			"the DSP set is declared after the HDI08 adapter, so it is destroyed before it");
	}
}

int main()
{
	theBoardDeliversEveryPortsRuntimeWordToItsOwnDsp();
	theDspSetIsDestroyedBeforeTheAdapterItUninstallsFrom();

	if(g_failures != 0)
	{
		std::cout << "t0_board_dsp_set: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_board_dsp_set: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
