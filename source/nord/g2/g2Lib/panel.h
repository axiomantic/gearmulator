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

		// The panel is a scheduled body, not a context: it consumes no
		// emulated cycles, so it has no context index. `cycleDebt`,
		// `longDispatchQuanta`, `contextFaulted` and `contextFault` accept
		// `0 .. dspCount` and nothing else, and no member here indexes them.
		//
		// Each body is `noexcept` so that the scheduler's run phase can
		// advance the panel without an error channel across the boundary.
		void tick(uint64_t frameIndex) noexcept
		{
			(void)frameIndex;
		}

		size_t stateSize() const noexcept
		{
			return 0;
		}

		void stateSave(void* dst) const noexcept
		{
			(void)dst;
		}

		void stateLoad(const void* src) noexcept
		{
			(void)src;
		}

	private:
		// The display buffer. It starts at zero, which is the quiescent
		// report, and it keeps whatever is written into it.
		std::vector<uint8_t> m_display;
	};
}
