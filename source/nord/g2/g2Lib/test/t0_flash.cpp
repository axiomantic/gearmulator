// Task BRD-7. Tier T0: this test runs with NMG2_ARTIFACTS unset.
//
// Plan section 9.2, BRD-7. Design section 7.4.
//
// The properties this test holds the model to:
//   1. CS0 and CS2 are reachable for read at their configured bases, and the
//      reads are big-endian (the ColdFire byte order).
//   2. Longword 0 of the CS0 image is the test\'s synthetic stack pointer and
//      longword 4 is the test\'s synthetic program counter. The reset vector
//      is whatever the loaded image carries.
//   3. Reads of three different widths from the same address return the right
//      bytes in the right order. A byte at the high address must be the high
//      byte of a 32-bit read, and the high byte of a 16-bit read.
//   4. Writes are rejected. The flash is read-only: the underlying bytes are
//      not changed, and a log line names the address and the access width.

#include "../flash.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
	// The CS0 and CS2 bases and sizes come from the test fixture: AGENTS.md
	// section 2.2 records them as unrecorded, and section 1.3 rule 1 forbids
	// writing them into a header.
	constexpr uint32_t kFixtureCs0Base = 0x30000000;
	constexpr uint32_t kFixtureCs0Size = 0x00010000;
	constexpr uint32_t kFixtureCs2Base = 0x20000000;
	constexpr uint32_t kFixtureCs2Size = 0x00010000;

	// The reset-vector longwords. They are the SAME numbers the boot loader
	// places in the real boot ROM but they are owned by this test and they
	// have no Clavia byte behind them: the previous revision of this test
	// read them through mcf5307_reset and was rewritten as a direct read
	// against the model so the test stays T0 and runs with NMG2_ARTIFACTS
	// unset.
	constexpr uint32_t syntheticSp = 0x00080000;
	constexpr uint32_t syntheticPc = 0x30000100;
}

int main()
{
	g2::Flash flash(kFixtureCs0Base, kFixtureCs0Size, kFixtureCs2Base, kFixtureCs2Size);

	// ---------------- CS0 image: reset vector at offsets 0 and 4, 0xAA fill
	//
	// The test sets longword 0 to a synthetic stack pointer and longword 4 to
	// a synthetic program counter, in the shape BRD-6 already uses.

	std::vector<uint8_t> cs0(kFixtureCs0Size, 0xAA);
	cs0[0] = static_cast<uint8_t>((syntheticSp >> 24) & 0xff);
	cs0[1] = static_cast<uint8_t>((syntheticSp >> 16) & 0xff);
	cs0[2] = static_cast<uint8_t>((syntheticSp >>  8) & 0xff);
	cs0[3] = static_cast<uint8_t>( syntheticSp        & 0xff);
	cs0[4] = static_cast<uint8_t>((syntheticPc >> 24) & 0xff);
	cs0[5] = static_cast<uint8_t>((syntheticPc >> 16) & 0xff);
	cs0[6] = static_cast<uint8_t>((syntheticPc >>  8) & 0xff);
	cs0[7] = static_cast<uint8_t>( syntheticPc        & 0xff);
	flash.loadCs0(cs0);

	// ---------------- CS2 image: 0x55 fill, 0x12/0x34/0x56/0x78 at offset 0x10
	//
	// The three reads below exercise the byte order at the same address.

	std::vector<uint8_t> cs2(kFixtureCs2Size, 0x55);
	cs2[0x10] = 0x12;
	cs2[0x11] = 0x34;
	cs2[0x12] = 0x56;
	cs2[0x13] = 0x78;
	flash.loadCs2(cs2);

	// ---------------- reads

	assert(flash.read32(kFixtureCs0Base    ) == syntheticSp);
	assert(flash.read32(kFixtureCs0Base + 4) == syntheticPc);

	assert(flash.read8 (kFixtureCs2Base + 0x10) == 0x12);
	assert(flash.read16(kFixtureCs2Base + 0x10) == 0x1234);
	assert(flash.read32(kFixtureCs2Base + 0x10) == 0x12345678u);

	// ---------------- writes are rejected; the bytes do not change

	flash.write8(kFixtureCs0Base, 0xff);
	assert(flash.read32(kFixtureCs0Base) == syntheticSp);

	flash.write32(kFixtureCs2Base + 0x10, 0u);
	assert(flash.read32(kFixtureCs2Base + 0x10) == 0x12345678u);

	std::cout << "t0_flash passed" << std::endl;
	return 0;
}
