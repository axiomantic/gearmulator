// Task BRD-31. Tier T0: this test runs with NMG2_ARTIFACTS unset.
//
// Plan section 13.4, BRD-31. Design section 7.4.
//
// RED: read8 at the CS2 status address returns the raw image byte (0x05).
// GREEN: the intercept in Flash::read8 returns the AMD status value 3.
// The neighbour read proves the intercept is narrow -- returning 3 for
// EVERY CS2 read would pass the status check but fail the neighbour.
//
// UNGATED AND T0 ON PURPOSE. The flash-type gate at 0x3001BB4C is an M3
// blocker, and a gated test cannot report on a blocker that gates the
// gate. NMG2_ARTIFACTS is unset: every byte the test loads is synthetic
// and no Clavia byte is read.

#include "../flash.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
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

	// The CS2 base and size come from the test fixture. The CS0 values are
	// present because the Flash constructor requires them; the CS0 window
	// is not exercised.
	//
	// 0x30110000..0x3011FFFF covers 0x30119848 at offset 0x9848.
	constexpr uint32_t kFixtureCs0Base = 0x00000000u;
	constexpr uint32_t kFixtureCs0Size = 0x00010000u;
	constexpr uint32_t kFixtureCs2Base = 0x30110000u;
	constexpr uint32_t kFixtureCs2Size = 0x00010000u;

	// The status address the firmware reads: 0x30119848, the absolute
	// address the MOVE.B at 0x3001BAF2 targets. Plan section 24.6 row
	// W3-352 gives the instruction and the diagnosis.
	constexpr uint32_t kStatusAddr    = 0x30119848u;
	constexpr uint32_t kStatusOffset  = kStatusAddr - kFixtureCs2Base;

	// The neighbour address, one byte past the status address. Its raw
	// image byte must survive the intercept, because a model that
	// hardcodes 3 for EVERY CS2 read passes the status check and hides
	// the absence of narrowness.
	constexpr uint32_t kNeighbourAddr   = kStatusAddr + 1u;
	constexpr uint32_t kNeighbourOffset = kStatusOffset + 1u;

	// The image bytes the flash gate compares against. 0x05 is the byte
	// measured in CODE_30000400.bin at offset 0x119848: the value the raw
	// CS2 read returns without the intercept, and the value that causes
	// the CMP.L #3 to fail. 0xAA is the neighbour byte, chosen to differ
	// from both 0x05 and 3 so no accidental equality can mask a failure.
	constexpr uint8_t kImageByte       = 0x05u;
	constexpr uint8_t kNeighbourByte   = 0xAAu;
	constexpr uint8_t kInterceptValue  = 3u;
}

int main()
{
	g2::Flash flash(kFixtureCs0Base, kFixtureCs0Size, kFixtureCs2Base, kFixtureCs2Size);

	// Build a CS2 image large enough to cover the status and neighbour
	// offsets. The bytes between 0 and the status offset are filled with
	// 0x55, a value that differs from 3, 0x05 and 0xAA so that an offset
	// mistake cannot hit any of them by accident.
	std::vector<uint8_t> cs2(kNeighbourOffset + 1u, 0x55u);
	cs2[kStatusOffset]    = kImageByte;
	cs2[kNeighbourOffset] = kNeighbourByte;
	flash.loadCs2(cs2);

	// ------ the status read: MUST return 3 after the intercept lands
	//
	// RED without the intercept: returns 0x05 (the raw image byte).
	// GREEN with the intercept:  returns 3 (AMD "ready/completed").

	checkEqual(uint32_t(flash.read8(kStatusAddr)), uint32_t(kInterceptValue),
		"read8 at the CS2 status address returns the AMD status value 3");

	// ------ the neighbour read: MUST return the raw image byte unchanged
	//
	// A model that hardcodes 3 for EVERY CS2 read passes the status check
	// and this assertion catches it. The raw byte is 0xAA, not 0x05, so a
	// stray equality with the status-byte value is also caught.

	checkEqual(uint32_t(flash.read8(kNeighbourAddr)), uint32_t(kNeighbourByte),
		"read8 at the neighbour address returns the raw image byte unchanged");

	if(g_failures)
	{
		std::cout << "t0_cs2_status: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_cs2_status: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}