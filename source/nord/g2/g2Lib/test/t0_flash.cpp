#include "flash.h"
#include <cassert>
#include <iostream>
#include <vector>

int main()
{
	constexpr uint32_t kFixtureCs0Base = 0x30000000u;
	constexpr uint32_t kFixtureCs0Size = 0x00010000u; // 64KB
	constexpr uint32_t kFixtureCs2Base = 0x20000000u;
	constexpr uint32_t kFixtureCs2Size = 0x00010000u; // 64KB

	g2::Flash flash(kFixtureCs0Base, kFixtureCs0Size, kFixtureCs2Base, kFixtureCs2Size);

	// Create synthetic CS0 image with synthetic SP and PC at reset vector
	std::vector<uint8_t> cs0Image(kFixtureCs0Size, 0xAAu);
	const uint32_t syntheticSp = 0x00080000u;
	const uint32_t syntheticPc = 0x30000100u;

	// SP at byte 0..3 (big endian)
	cs0Image[0] = static_cast<uint8_t>((syntheticSp >> 24) & 0xFFu);
	cs0Image[1] = static_cast<uint8_t>((syntheticSp >> 16) & 0xFFu);
	cs0Image[2] = static_cast<uint8_t>((syntheticSp >> 8) & 0xFFu);
	cs0Image[3] = static_cast<uint8_t>(syntheticSp & 0xFFu);

	// PC at byte 4..7 (big endian)
	cs0Image[4] = static_cast<uint8_t>((syntheticPc >> 24) & 0xFFu);
	cs0Image[5] = static_cast<uint8_t>((syntheticPc >> 16) & 0xFFu);
	cs0Image[6] = static_cast<uint8_t>((syntheticPc >> 8) & 0xFFu);
	cs0Image[7] = static_cast<uint8_t>(syntheticPc & 0xFFu);

	flash.loadCs0(cs0Image);

	// Create synthetic CS2 image
	std::vector<uint8_t> cs2Image(kFixtureCs2Size, 0x55u);
	cs2Image[0x10] = 0x12;
	cs2Image[0x11] = 0x34;
	cs2Image[0x12] = 0x56;
	cs2Image[0x13] = 0x78;

	flash.loadCs2(cs2Image);

	// Assert SP and PC reads from CS0
	assert(flash.read32(kFixtureCs0Base) == syntheticSp);
	assert(flash.read32(kFixtureCs0Base + 4) == syntheticPc);

	// Assert CS2 reads
	assert(flash.read8(kFixtureCs2Base + 0x10) == 0x12);
	assert(flash.read16(kFixtureCs2Base + 0x10) == 0x1234);
	assert(flash.read32(kFixtureCs2Base + 0x10) == 0x12345678u);

	// Assert writes are rejected (read value does not change)
	flash.write8(kFixtureCs0Base, 0xFFu);
	assert(flash.read32(kFixtureCs0Base) == syntheticSp);

	flash.write32(kFixtureCs2Base + 0x10, 0x00000000u);
	assert(flash.read32(kFixtureCs2Base + 0x10) == 0x12345678u);

	std::cout << "t0_flash passed" << std::endl;
	return 0;
}
