// Task BRD-29. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.4, BRD-29. Plan section 24.6 row W3-343 is the diagnosis
// this task executes: the OS store-helper executes `movea.l #$13000010,a0`
// then `move.b #$f6,(a0)` during USB init, $13000010 is CS3's command port,
// and until this task lands the write returns MCF5307_BUS_UNMAPPED because
// nothing answers Region::Cs3. The fault chain from there -- absolute vector
// $8, erased flash, double-fault collapse -- is row W3-343's record.
//
// WHAT THIS TEST DRIVES, AND THROUGH WHICH ENTRY POINT. Board::onRead and
// Board::onWrite are the exact function pointers handed to mcf5307_create,
// so driving them drives the path the core drives; the reason is recorded in
// t0_board_routing, whose earlier revision drove busRead directly and stayed
// green against a broken forwarding body.
//
// THE SIZE UNIT OF THOSE TWO CALLBACKS IS A COUNT OF BYTES -- 1, 2 or 4 --
// and NOT the MemoryMap's width in bits. board.h states the unit twice and
// t0_board_routing carries the measured defect that made the distinction
// load-bearing. The OS's store is `move.b`, whose width is 8 BITS, which is
// the byte count 1 on the callback side; the constants below are named for
// the same reason t0_board_routing names its three.
//
// STATUS ONLY, AND NEVER THE RETURN VALUE. The unmapped READ path zeroes its
// return exactly as a benign stub answer might, so an assertion on the value
// passes WITHOUT any wiring -- the mirage the plan block names. The status is
// the fact under test: MCF5307_BUS_UNMAPPED before the wiring, MCF5307_BUS_OK
// after it.
//
// WHAT THIS TEST CANNOT SEE, STATED RATHER THAN HIDDEN. The stub answers
// identically at every offset in the window, so a forwarding error that swaps
// the A0/A4 command and data offsets still answers BUS_OK here; that decode
// is CPU-22's business. And reverting main.cpp's config line leaves THIS test
// green, because the fixture below builds its own config -- the config half's
// observable lives at the boot tier.

#include "board.h"
#include "memoryMap.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected << ">, got <"
		          << _actual << ">" << std::endl;
		++g_failures;
	}

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	// The CS3 window. The base is the one AGENTS.md section 2.2 records and
	// memoryMap.h publishes. The size is the workspace logbook's figure --
	// section 3.8 derives 64 KiB from CSMR3 at 0x100000A8 -- and it is the
	// same value main.cpp configures, stated here independently because this
	// fixture builds its own BoardConfig.
	constexpr uint32_t g_cs3Size = 0x00010000u;

	constexpr int g_byte = 1;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;

		config.memory.cs3 = {g2::g_cs3Base, g_cs3Size};

		return config;
	}
}

int main()
{
	g2::Board board(makeConfig());

	mcf5307_bus_status status = MCF5307_BUS_OK;

	// THE STORE THAT STARTED THIS TASK. The OS store-helper's byte write of
	// 0xf6 to the ISP1181 command port at 0x13000010. Before the wiring this
	// answers MCF5307_BUS_UNMAPPED and the core faults on it; after the
	// wiring the adapter accepts it and reports MCF5307_BUS_OK.
	status = MCF5307_BUS_OK;
	(void)g2::Board::onWrite(&board, 0x13000010u, g_byte, 0xf6u, &status);
	checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
	           "the OS's command-port byte write at " + hex32(0x13000010u)
		           + " completes with BUS_OK");

	// A read inside the same window. The driver reads the device word-sized
	// in the register-file idiom, and the adapter widens rather than refuses,
	// but the case pinned here is the smallest one: the byte read at the
	// window base completes.
	status = MCF5307_BUS_OK;
	(void)g2::Board::onRead(&board, 0x13000000u, g_byte, &status);
	checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
	           "a byte read at the CS3 window base " + hex32(0x13000000u)
		           + " completes with BUS_OK");

	if(g_failures)
	{
		std::cout << "t0_cs3_wire: " << g_failures << " of " << g_cases
		          << " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_cs3_wire: " << g_cases << " of " << g_cases
	          << " cases passed" << std::endl;
	return 0;
}
