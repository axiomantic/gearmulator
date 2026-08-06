#pragma once

#include <cstdint>

namespace g2
{
	/* Memory map definitions for MCF5307 on Nord Lead / G2 board.
	 *
	 * Address map ranges per BRD-1:
	 * - RAM:               $00000000 - $000FFFFF (1MB)
	 * - MBAR Peripherals:  $10000000
	 * - CS1 Flash:         $20000000
	 * - CS2 Boot/OS Flash: $30000000
	 * - CS3 USB:           $40000000
	 */
	constexpr uint32_t kRamBase        = 0x00000000u;
	constexpr uint32_t kRamEnd         = 0x000FFFFFu;
	constexpr uint32_t kRamSize        = 0x00100000u; // 1MB

	constexpr uint32_t kMbarBase       = 0x10000000u; // SIM / Peripherals MBAR
	constexpr uint32_t kMbarSize       = 0x00001000u; // 4KB

	constexpr uint32_t kCs1Base        = 0x20000000u; // Flash (CS1)
	constexpr uint32_t kCs2Base        = 0x30000000u; // Boot/OS Flash (CS2)
	constexpr uint32_t kCs3Base        = 0x40000000u; // USB (CS3)

	enum class MemoryRegion
	{
		Ram,
		MbarPeripherals,
		Cs1Flash,
		Cs2BootFlash,
		Cs3Usb,
		Unmapped
	};

	class MemoryMap
	{
	public:
		static constexpr uint32_t RamBase      = kRamBase;
		static constexpr uint32_t RamEnd       = kRamEnd;
		static constexpr uint32_t RamSize      = kRamSize;
		static constexpr uint32_t MbarBase     = kMbarBase;
		static constexpr uint32_t Cs1FlashBase = kCs1Base;
		static constexpr uint32_t Cs2BootBase  = kCs2Base;
		static constexpr uint32_t Cs3UsbBase   = kCs3Base;

		static MemoryRegion decodeRegion(uint32_t _addr) noexcept;
		static bool isRam(uint32_t _addr) noexcept;
		static bool isMbar(uint32_t _addr) noexcept;
		static bool isCs1(uint32_t _addr) noexcept;
		static bool isCs2(uint32_t _addr) noexcept;
		static bool isCs3(uint32_t _addr) noexcept;
	};
}
