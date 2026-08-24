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
// The manual is on disk and every citation is verified against it. See the
// provenance record at the head of sim.cpp for what was taken, which source it
// came from, the ONE CONFLICT between the two, and the list of modelled values
// the 1998 manual contradicts. That conflict is a silicon mask revision, not a
// bad citation. No file of MegabytePhreak/qemu-mcf5307 was opened for this
// task.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "memoryMap.h"
#include "timer.h"

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

		// Task BRD-33. The two general-purpose timer modules. The SIM owns
		// them because the ten addresses they answer are SIM addresses; the
		// Board advances them from the cycles it ran.
		Timer& timer1() { return m_timer1; }
		Timer& timer2() { return m_timer2; }

		// Advance both modules by the same input clocks.
		void advanceTimers(uint32_t _inputClocks);

		// Point both modules at the interrupt controller they assert on.
		void setInterruptController(InterruptController* _interrupts);

	private:
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		// Task BRD-33. The timer module ONE BYTE of the MBAR window belongs
		// to, or NULLPTR when the byte is not one of the ten register bytes
		// the two modules answer. _blockOffset receives the byte's offset
		// within that module's block.
		//
		// THE ROUTE IS PER BYTE AND NOT PER REGISTER, because the SIM's read
		// and write already walk the access one byte at a time and a second
		// walk here could disagree with that one.
		Timer* timerForByte(uint32_t _index, uint32_t& _blockOffset);

		uint8_t m_space[g_simSpaceSize] = {};

		// The bits a write cannot change. Every byte starts fully protected,
		// because UM Table 9-5 footnote 1 says a write to a reserved address
		// has no effect, and each register clears the bits it lets a write
		// reach.
		uint8_t m_writeProtect[g_simSpaceSize] = {};

		Timer m_timer1{Timer::gTimer1InterruptIndex};
		Timer m_timer2{Timer::gTimer2InterruptIndex};

		std::vector<std::string> m_log;
	};
}
