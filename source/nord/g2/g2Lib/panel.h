// The panel.
//
// The panel display buffer sits on CS4 and no authority records CS4's base.
// The base and the size of the display window are therefore configuration, and
// this file carries no number for either.
//
// The model reports a quiescent panel: no key down, no encoder moving, no
// button pressed. It answers every poll at every legal width, so no boot loop
// spins for ever.

#pragma once

#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	class Panel final : public BusTarget
	{
	public:
		explicit Panel(uint32_t _displaySize);

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

	private:
		// The display buffer. It starts at zero, which is the quiescent
		// report, and it keeps whatever is written into it.
		std::vector<uint8_t> m_display;
	};
}
