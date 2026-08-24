// Task BRD-33. One MCF5307 general-purpose timer.
//
// Plan section 13.1, BRD-33. Design sections 9.4, 13.1.
//
// ---------------------------------------------------------------------------
// PROVENANCE. Plan section 13.1 requires a record of what this task took and
// where each fact was read.
//
// NO FILE OF MegabytePhreak/qemu-mcf5307 WAS OPENED FOR THIS TASK. The timer
// module is written from the manual alone.
//
// THE MANUAL. MCF5307 ColdFire Integrated Microprocessor User's Manual,
// Motorola, 1998 -- the same copy the provenance record at the head of
// sim.cpp documents. Section 9.4 "Timer Module" and Appendix B Table B-1.
//
//   * The register block and its offsets ($00 TMR, $04 TRR, $08 TCR, $0C TCN,
//     $11 TER) and the two module bases MBAR+$140 and MBAR+$180: section
//     9.4.1 Table 9-5 and Table B-1.
//   * TMR's fields -- PS at bits 15:8, CE at 7:6, OM at 5, ORI at 4, FRR at 3,
//     CLK at 2:1, RST at 0: section 9.4.1.1.
//   * THE PRESCALER DIVIDES THE INPUT CLOCK BY PS + 1: section 9.4.1.1. The
//     firmware's PS of 0x7F is therefore one counter tick per 128 input
//     clocks.
//   * FRR: SET restarts the counter from zero on a reference match, CLEAR runs
//     it on: section 9.4.1.1.
//   * ORI enables the interrupt on a reference match: section 9.4.1.1.
//   * RST: writing 0 disables AND resets the timer; the register file other
//     than TMR itself is not cleared by it: section 9.4.1.1.
//   * TER's REF (bit 1) and CAP (bit 0), and that BOTH ARE CLEARED BY WRITING
//     A ONE: section 9.4.1.5.
//   * THE TIMER PERIOD IS (PS + 1) * (TRR + 1) INPUT CLOCKS: section 9.4.2.
//     That period is what fixes WHERE the comparison sits relative to the
//     increment. A model that matched on the tick that carries the counter TO
//     TRR would have a period of (PS + 1) * TRR, which the manual's formula
//     contradicts. The comparison therefore reads the counter BEFORE the tick
//     increments it: a counter standing at TRR matches on its next tick, so
//     TRR = 4 matches on the fifth tick and TRR = 0 matches on every tick.
//
// THE INTERRUPT SOURCE INDEX IS THE ICR INDEX. MCF5307 UM Table 8-2 puts
// timer 1 on ICR1 and timer 2 on ICR2, and InterruptController's source index
// is that same index -- so timer 2's ICR is MBAR+$04C + 2 = MBAR+$04E, which
// is exactly the ICR the firmware programs with $84.
//
// THE INTERRUPT IS A LEVEL SOURCE AND NOT A PULSE, which is why it is
// recomputed from state rather than pulsed on the match. TER[REF] stays set
// until the handler writes a one to it, and the source drops when it does.
// That is Uart0::recomputeInterrupt's shape and it is deliberate: this task
// adds no second interrupt mechanism.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#include "timer.h"

namespace g2
{
	Timer::Timer(const int _interruptIndex, InterruptController* _interrupts)
		: m_interruptIndex(_interruptIndex)
		, m_interrupts(_interrupts)
	{
	}

	void Timer::setInterruptController(InterruptController* _interrupts)
	{
		m_interrupts = _interrupts;
	}

	void Timer::writeTmr(const uint16_t _value)
	{
		m_tmr = _value;

		// UM section 9.4.1.1: RST = 0 disables the timer and RESETS it. The
		// prescaler goes with the counter, or a timer re-enabled after a pause
		// would carry a fraction of a tick across the disable.
		if((m_tmr & gTmrRst) == 0)
		{
			m_tcn = 0;
			m_prescaler = 0;
		}

		// ORI lives in this register, so the interrupt condition can change
		// without the counter moving at all.
		recomputeInterrupt();
	}

	void Timer::writeTrr(const uint16_t _value)
	{
		m_trr = _value;
	}

	void Timer::writeTcn(const uint16_t _value)
	{
		m_tcn = _value;
	}

	void Timer::writeTer(const uint8_t _value)
	{
		// WRITE-ONE-TO-CLEAR, UM section 9.4.1.5. A one clears the bit it
		// names and a zero leaves it alone, so the firmware handler's write of
		// 2 to MBAR+$191 clears REF and a write of 0 clears nothing.
		m_ter = uint8_t(m_ter & ~uint8_t(_value & (gTerRef | gTerCap)));
		recomputeInterrupt();
	}

	bool Timer::coversByte(const uint32_t _blockOffset)
	{
		// The bytes the five registers occupy, and no others. A byte inside
		// the block that no register claims is reserved and stays with the
		// SIM's own storage, so this task changes nothing about it.
		return _blockOffset == gTmrOffset || _blockOffset == gTmrOffset + 1
			|| _blockOffset == gTrrOffset || _blockOffset == gTrrOffset + 1
			|| _blockOffset == gTcrOffset || _blockOffset == gTcrOffset + 1
			|| _blockOffset == gTcnOffset || _blockOffset == gTcnOffset + 1
			|| _blockOffset == gTerOffset;
	}

	uint8_t Timer::readByte(const uint32_t _blockOffset) const
	{
		// The part is big-endian, so byte 0 of a 16-bit register holds its
		// most significant eight bits.
		switch(_blockOffset)
		{
			case gTmrOffset:     return uint8_t(m_tmr >> 8);
			case gTmrOffset + 1: return uint8_t(m_tmr & 0xffu);
			case gTrrOffset:     return uint8_t(m_trr >> 8);
			case gTrrOffset + 1: return uint8_t(m_trr & 0xffu);
			case gTcrOffset:     return uint8_t(m_tcr >> 8);
			case gTcrOffset + 1: return uint8_t(m_tcr & 0xffu);
			case gTcnOffset:     return uint8_t(m_tcn >> 8);
			case gTcnOffset + 1: return uint8_t(m_tcn & 0xffu);
			case gTerOffset:     return m_ter;
			default:             return 0u;
		}
	}

	void Timer::writeByte(const uint32_t _blockOffset, const uint8_t _value)
	{
		switch(_blockOffset)
		{
			case gTmrOffset:     writeTmr(uint16_t((m_tmr & 0x00ffu) | uint16_t(_value << 8))); break;
			case gTmrOffset + 1: writeTmr(uint16_t((m_tmr & 0xff00u) | _value)); break;
			case gTrrOffset:     writeTrr(uint16_t((m_trr & 0x00ffu) | uint16_t(_value << 8))); break;
			case gTrrOffset + 1: writeTrr(uint16_t((m_trr & 0xff00u) | _value)); break;
			case gTcnOffset:     writeTcn(uint16_t((m_tcn & 0x00ffu) | uint16_t(_value << 8))); break;
			case gTcnOffset + 1: writeTcn(uint16_t((m_tcn & 0xff00u) | _value)); break;
			case gTerOffset:     writeTer(_value); break;

			// TCR is READ ONLY, UM Table B-1. A write reaches no bit of it.
			default: break;
		}
	}

	void Timer::advance(const uint32_t _inputClocks)
	{
		if((m_tmr & gTmrRst) == 0)
			return;

		// THE PRESCALER DIVIDES BY PS + 1, UM section 9.4.1.1. The remainder
		// is carried in m_prescaler, so the count is a function of the total
		// input clocks and NOT of how they were split across calls -- which is
		// what keeps a tick deterministic under the scheduler's quantum.
		const uint32_t divisor = uint32_t((m_tmr >> gTmrPrescalerShift) & 0xffu) + 1u;

		m_prescaler += _inputClocks;

		while(m_prescaler >= divisor)
		{
			m_prescaler -= divisor;
			tick();
		}
	}

	void Timer::tick()
	{
		// The comparison reads the counter BEFORE the increment, which is what
		// makes the period (TRR + 1) ticks. See the provenance record above.
		if(m_tcn == m_trr)
		{
			m_ter = uint8_t(m_ter | gTerRef);

			// FRR SET restarts from zero; FRR CLEAR runs on past TRR.
			m_tcn = (m_tmr & gTmrFrr) ? uint16_t(0) : uint16_t(m_tcn + 1);

			recomputeInterrupt();
			return;
		}

		m_tcn = uint16_t(m_tcn + 1);
	}

	void Timer::recomputeInterrupt()
	{
		// ORI IS TESTED HERE AND NOWHERE ELSE. The other timer is programmed
		// TMR = 0x2B, whose ORI is clear, and it must stay silent.
		setPending((m_ter & gTerRef) != 0 && (m_tmr & gTmrOri) != 0);
	}

	void Timer::setPending(const bool _asserted)
	{
		if(_asserted == m_interruptAsserted)
			return;

		m_interruptAsserted = _asserted;

		if(m_interrupts)
			m_interrupts->setInternalPending(m_interruptIndex, _asserted);
	}
}
