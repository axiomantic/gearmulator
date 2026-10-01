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
#include "model.h"

namespace rg2
{
	// The panel identifier sits in bits 5:4 of the first latch. model.h carries
	// the map from a Model to those two bits, and what the OS does with each.
	constexpr uint32_t g_panelIdentifierOffset = 0u;
	constexpr int g_panelIdentifierShift = 4;
	constexpr uint8_t g_panelIdentifierMask = uint8_t(0x3u << g_panelIdentifierShift);

	constexpr uint8_t panelIdentifierByte(const Model _model)
	{
		return uint8_t(panelStrapCode(_model) << g_panelIdentifierShift);
	}

	class Latches final : public BusTarget
	{
	public:
		explicit Latches(uint32_t _windowSize, Model _model = Model::G2X);

		uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

	private:
		// One byte for every latch in the window. The first byte holds the
		// panel identifier and a write cannot change it, because on the panel
		// board it is two 0-ohm resistors and not a register. Every other byte
		// is an output latch that keeps what was written. No authority records
		// what any of them drives, so this model carries no meaning for them.
		std::vector<uint8_t> m_latch;
	};
}
