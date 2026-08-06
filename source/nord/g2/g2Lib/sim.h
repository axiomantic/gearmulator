// Task BRD-2. The SIM registers.
//
// Plan section 13.1, BRD-2. Design sections 6.4, 8.2.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1, 4.2.
//
// WHAT THIS FILE IS. The MCF5307 carries its peripherals in one window that
// MBAR points at. This model answers that window: the chip-select registers,
// the DRAM controller registers, the two timers, the parallel port and the one
// UART offset the firmware reads as a model strap.
//
// IT IS A BusTarget, so it takes the offset the BRD-1 decode produced and it
// carries no knowledge of where the boot loader put MBAR.
//
// CLEAN-ROOM. The chip-select register family is read from AGENTS.md section
// 3.8, which MEASURES the boot loader programming CS3. Every other offset,
// width, reset value and access rule is read from the MCF5307 User's Manual.
// See the provenance record at the head of sim.cpp for what was taken, which
// source it came from, the ONE CONFLICT between the two and how it resolved,
// and the one evidence gap that stays open. No file of
// MegabytePhreak/qemu-mcf5307 was opened for this task.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	// The MBAR window this model answers. MCF5307 UM Appendix B Table B-1 runs
	// to MBAR+$3D4, so one kilobyte covers the whole programming model.
	constexpr uint32_t g_simSpaceSize = 0x400u;

	class Sim final : public BusTarget
	{
	public:
		Sim();

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// One line for every access the model rejected, and one for every
		// access to an offset the manual assigns to no register this model
		// carries. BRD-5 owns the full anomaly log; this is the trace the SIM
		// itself writes.
		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		uint8_t m_space[g_simSpaceSize] = {};

		// The bits a write cannot change. Every byte starts fully protected,
		// because UM Table 9-5 footnote 1 says a write to a reserved address
		// has no effect, and each register clears the bits it lets a write
		// reach.
		uint8_t m_writeProtect[g_simSpaceSize] = {};

		std::vector<std::string> m_log;
	};
}
