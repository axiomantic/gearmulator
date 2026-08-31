// One MCF5307 general-purpose timer.
//
// The timer period is (PS + 1) * (TRR + 1) input clocks, and that period is
// what fixes where the comparison sits relative to the increment. A model that
// matched on the tick that carries the counter to TRR would have a period of
// (PS + 1) * TRR. The comparison therefore reads the counter before the tick
// increments it: a counter standing at TRR matches on its next tick, so
// TRR = 4 matches on the fifth tick and TRR = 0 matches on every tick.
//
// The interrupt is a level source and not a pulse, which is why it is
// recomputed from state rather than pulsed on the match. TER[REF] stays set
// until the handler writes a one to it, and the source drops when it does.

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

		// RST = 0 disables the timer and resets it. The prescaler goes with the
		// counter, or a timer re-enabled after a pause would carry a fraction of
		// a tick across the disable.
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
		// Write-one-to-clear: a one clears the bit it names and a zero leaves it
		// alone, so the firmware handler's write of 2 to MBAR+$191 clears REF
		// and a write of 0 clears nothing.
		m_ter = uint8_t(m_ter & ~uint8_t(_value & (gTerRef | gTerCap)));
		recomputeInterrupt();
	}

	bool Timer::coversByte(const uint32_t _blockOffset)
	{
		// A byte inside the block that no register claims is reserved and stays
		// with the SIM's own storage.
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

			// TCR is read only. A write reaches no bit of it.
			default: break;
		}
	}

	void Timer::advance(const uint32_t _inputClocks)
	{
		if((m_tmr & gTmrRst) == 0)
			return;

		// The prescaler divides by PS + 1. The remainder is carried in
		// m_prescaler, so the count is a function of the total input clocks and
		// not of how they were split across calls, which is what keeps a tick
		// deterministic under the scheduler's quantum.
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
		// The comparison reads the counter before the increment, which is what
		// makes the period (TRR + 1) ticks.
		if(m_tcn == m_trr)
		{
			m_ter = uint8_t(m_ter | gTerRef);

			// FRR set restarts from zero; FRR clear runs on past TRR.
			m_tcn = (m_tmr & gTmrFrr) ? uint16_t(0) : uint16_t(m_tcn + 1);

			recomputeInterrupt();
			return;
		}

		m_tcn = uint16_t(m_tcn + 1);
	}

	void Timer::recomputeInterrupt()
	{
		// ORI is tested here and nowhere else. The other timer is programmed
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
