// The SIM registers.
//
// The MCF5307 carries its peripherals in one window that MBAR points at. This
// model answers that window: the chip-select registers, the DRAM controller
// registers, the two timers, the parallel port and the one UART offset the
// firmware reads as a model strap.
//
// It is a BusTarget, so it takes the offset the memory decode produced and it
// carries no knowledge of where the boot loader put MBAR.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	// The MBAR window this model answers. The programming model runs to
	// MBAR+$3D4, so one kilobyte covers all of it.
	constexpr uint32_t g_simSpaceSize = 0x400u;

	class Sim final : public BusTarget
	{
	public:
		Sim();

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// One line for every access the model rejected, and one for every
		// access to an offset the manual assigns to no register this model
		// carries.
		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		uint8_t m_space[g_simSpaceSize] = {};

		// The bits a write cannot change. Every byte starts fully protected,
		// because a write to a reserved address has no effect, and each
		// register clears the bits it lets a write reach.
		uint8_t m_writeProtect[g_simSpaceSize] = {};

		std::vector<std::string> m_log;
	};
}
