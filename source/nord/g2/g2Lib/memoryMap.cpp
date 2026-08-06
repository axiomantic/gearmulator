#include "memoryMap.h"

namespace g2
{
	MemoryRegion MemoryMap::decodeRegion(uint32_t _addr) noexcept
	{
		if (_addr <= kRamEnd)
		{
			return MemoryRegion::Ram;
		}
		if ((_addr & 0xFFFFF000u) == kMbarBase)
		{
			return MemoryRegion::MbarPeripherals;
		}
		if ((_addr & 0xF0000000u) == kCs1Base)
		{
			return MemoryRegion::Cs1Flash;
		}
		if ((_addr & 0xF0000000u) == kCs2Base)
		{
			return MemoryRegion::Cs2BootFlash;
		}
		if ((_addr & 0xF0000000u) == kCs3Base)
		{
			return MemoryRegion::Cs3Usb;
		}
		return MemoryRegion::Unmapped;
	}

	bool MemoryMap::isRam(uint32_t _addr) noexcept
	{
		return _addr <= kRamEnd;
	}

	bool MemoryMap::isMbar(uint32_t _addr) noexcept
	{
		return (_addr & 0xFFFFF000u) == kMbarBase;
	}

	bool MemoryMap::isCs1(uint32_t _addr) noexcept
	{
		return (_addr & 0xF0000000u) == kCs1Base;
	}

	bool MemoryMap::isCs2(uint32_t _addr) noexcept
	{
		return (_addr & 0xF0000000u) == kCs2Base;
	}

	bool MemoryMap::isCs3(uint32_t _addr) noexcept
	{
		return (_addr & 0xF0000000u) == kCs3Base;
	}
}
