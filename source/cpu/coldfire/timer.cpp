// One MCF5407 / MCF5307 general-purpose timer.

#include "timer.h"

namespace coldfire
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

		if((m_tmr & gTmrRst) == 0)
		{
			m_tcn = 0;
			m_prescaler = 0;
		}

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
		m_ter = uint8_t(m_ter & ~uint8_t(_value & (gTerRef | gTerCap)));
		recomputeInterrupt();
	}

	bool Timer::coversByte(const uint32_t _blockOffset)
	{
		return _blockOffset == gTmrOffset || _blockOffset == gTmrOffset + 1
			|| _blockOffset == gTrrOffset || _blockOffset == gTrrOffset + 1
			|| _blockOffset == gTcrOffset || _blockOffset == gTcrOffset + 1
			|| _blockOffset == gTcnOffset || _blockOffset == gTcnOffset + 1
			|| _blockOffset == gTerOffset;
	}

	uint8_t Timer::readByte(const uint32_t _blockOffset) const
	{
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
			default: break;
		}
	}

	void Timer::advance(const uint32_t _inputClocks)
	{
		if((m_tmr & gTmrRst) == 0)
			return;

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
		if(m_tcn == m_trr)
		{
			m_ter = uint8_t(m_ter | gTerRef);
			m_tcn = (m_tmr & gTmrFrr) ? uint16_t(0) : uint16_t(m_tcn + 1);
			recomputeInterrupt();
			return;
		}

		m_tcn = uint16_t(m_tcn + 1);
	}

	void Timer::recomputeInterrupt()
	{
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
