// Tier T0: this test needs no firmware artifact of any kind.
//
// The OS store-helper executes `movea.l #$13000010,a0` then `move.b #$f6,(a0)`
// during USB init. $13000010 is CS3's command port, and with nothing answering
// Region::Cs3 the write returns MCF5307_BUS_UNMAPPED and the core collapses
// through the absolute vector at $8 into erased flash and a double fault.
//
// Board::onRead and Board::onWrite are the exact function pointers handed to
// mcf5307_create, so driving them drives the path the core drives. Driving
// busRead instead stays green against a broken forwarding body.
//
// The size unit of those two callbacks is a count of bytes -- 1, 2 or 4 -- and
// not the MemoryMap's width in bits. The OS's store is `move.b`, whose width is
// 8 bits, which is the byte count 1 on the callback side.
//
// The assertions read the status and never the return value. The unmapped read
// path zeroes its return exactly as a benign stub answer might, so an assertion
// on the value passes without any wiring. The status is the fact under test:
// MCF5307_BUS_UNMAPPED before the wiring, MCF5307_BUS_OK after it.
//
// What this test cannot see: the device answers identically at every offset in
// the window, so a forwarding error that swaps the A0/A4 command and data
// offsets still answers BUS_OK here. And reverting main.cpp's config line
// leaves this test green, because the fixture below builds its own config.

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

	// The CS3 window. The base is the one memoryMap.h publishes; the size is
	// 64 KiB, derived from CSMR3 at 0x100000A8. It is stated here independently
	// because this fixture builds its own BoardConfig.
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

	// The OS store-helper's byte write of 0xf6 to the ISP1181 command port at
	// 0x13000010. Before the wiring this
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
