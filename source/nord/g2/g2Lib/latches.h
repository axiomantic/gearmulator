// The CS5 latches.
//
// The CS5 latch sits at 0x15000000, and panel_id() at 0x3005BFFE drives it and
// takes bits 5:4. The base itself lives in memoryMap.h as g_cs5Base, so this
// file carries no address.
//
// No authority records how wide the CS5 window is, so a caller supplies it.

#pragma once

#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	// The panel identifier sits in bits 5:4 of the first latch. 0b00 is model
	// code 0, a plain G2; 0b11 is model code 1, the G2X; 0b10 is model code 2,
	// the Rack that never shipped; and 0b01 makes the OS stop on OS-HARDWARE
	// ERR at 0x3001B86C. This machine is 0b11.
	constexpr uint32_t g_panelIdentifierOffset = 0u;
	constexpr int g_panelIdentifierShift = 4;
	constexpr uint8_t g_panelIdentifierMask = uint8_t(0x3u << g_panelIdentifierShift);
	constexpr uint8_t g_panelIdentifierG2X = uint8_t(0x3u << g_panelIdentifierShift);

	class Latches final : public BusTarget
	{
	public:
		explicit Latches(uint32_t _windowSize);

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

	private:
		// One byte for every latch in the window. The first byte holds the
		// panel identifier and a write cannot change it, because on the panel
		// board it is two 0-ohm resistors and not a register. Every other byte
		// is an output latch that keeps what was written. No authority records
		// what any of them drives, so this model carries no meaning for them.
		std::vector<uint8_t> m_latch;
	};
}
