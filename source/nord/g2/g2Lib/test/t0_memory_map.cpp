#include <cassert>
#include <iostream>
#include "memoryMap.h"

int main()
{
	using namespace g2;

	// Test RAM boundary ($00000000 - $000FFFFF)
	assert(MemoryMap::isRam(0x00000000u));
	assert(MemoryMap::isRam(0x00080000u));
	assert(MemoryMap::isRam(0x000FFFFFu));
	assert(!MemoryMap::isRam(0x00100000u));
	assert(MemoryMap::decodeRegion(0x00050000u) == MemoryRegion::Ram);

	// Test MBAR Peripherals ($10000000)
	assert(MemoryMap::isMbar(0x10000000u));
	assert(MemoryMap::isMbar(0x10000100u));
	assert(!MemoryMap::isMbar(0x10001000u));
	assert(MemoryMap::decodeRegion(0x10000040u) == MemoryRegion::MbarPeripherals);

	// Test CS1 Flash ($20000000)
	assert(MemoryMap::isCs1(0x20000000u));
	assert(MemoryMap::isCs1(0x20100000u));
	assert(!MemoryMap::isCs1(0x30000000u));
	assert(MemoryMap::decodeRegion(0x20000000u) == MemoryRegion::Cs1Flash);

	// Test CS2 Boot/OS Flash ($30000000)
	assert(MemoryMap::isCs2(0x30000000u));
	assert(MemoryMap::isCs2(0x30050000u));
	assert(!MemoryMap::isCs2(0x40000000u));
	assert(MemoryMap::decodeRegion(0x30000000u) == MemoryRegion::Cs2BootFlash);

	// Test CS3 USB ($40000000)
	assert(MemoryMap::isCs3(0x40000000u));
	assert(MemoryMap::isCs3(0x40000010u));
	assert(!MemoryMap::isCs3(0x50000000u));
	assert(MemoryMap::decodeRegion(0x40000000u) == MemoryRegion::Cs3Usb);

	// Test Unmapped space
	assert(MemoryMap::decodeRegion(0x00100000u) == MemoryRegion::Unmapped);
	assert(MemoryMap::decodeRegion(0x50000000u) == MemoryRegion::Unmapped);

	std::cout << "t0_memory_map passed successfully." << std::endl;
	return 0;
}
