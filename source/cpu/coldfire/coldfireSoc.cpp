#include "coldfireSoc.h"

namespace coldfire
{
	ColdfireSoc::ColdfireSoc(void* _irqUser, InterruptPresentFn _irqPresent, bool _engineStrap)
		: m_ownedIntc(_irqUser, _irqPresent)
		, m_ownedSim(_engineStrap)
		, m_ownedUart0(&m_ownedIntc)
		, m_interrupts(&m_ownedIntc)
		, m_sim(&m_ownedSim)
		, m_uart0(&m_ownedUart0)
	{
		m_ownedSim.setInterruptController(&m_ownedIntc);
	}

	ColdfireSoc::ColdfireSoc(Sim& _sim, Uart0& _uart0, BusTarget& _mbus, InterruptController& _interrupts)
		: m_interrupts(&_interrupts)
		, m_sim(&_sim)
		, m_uart0(&_uart0)
		, m_mbus(&_mbus)
	{
	}

	ColdfireSoc::ColdfireSoc(Sim& _sim, Uart0& _uart0, BusTarget* _mbus, InterruptController& _interrupts)
		: m_interrupts(&_interrupts)
		, m_sim(&_sim)
		, m_uart0(&_uart0)
		, m_mbus(_mbus)
	{
	}

	bool ColdfireSoc::isUartOwned(const uint32_t _offset)
	{
		if(_offset == g_simUipcrOffset)
			return false;

		if(_offset >= Uart0::gUart0Base && _offset < Uart0::gUart0Base + Uart0::gUartModuleSize)
			return true;

		return _offset >= Uart0::gUart1Base && _offset < Uart0::gUart1Base + Uart0::gUartModuleSize;
	}

	bool ColdfireSoc::isMbusOwned(const uint32_t _offset)
	{
		return _offset >= gMbusBase && _offset < gMbusBase + gMbusSize;
	}

	bool ColdfireSoc::isInterruptOwned(const uint32_t _offset)
	{
		if(_offset == InterruptController::gIrqparOffset)
			return true;
		if(_offset == InterruptController::gAvrOffset)
			return true;
		return _offset >= InterruptController::gIcrBase
			&& _offset < InterruptController::gIcrBase + InterruptController::gIcrCount;
	}

	BusTarget& ColdfireSoc::select(const uint32_t _offset)
	{
		if(m_mbus && isMbusOwned(_offset))
			return *m_mbus;
		if(isUartOwned(_offset))
			return *m_uart0;
		return *m_sim;
	}

	uint32_t ColdfireSoc::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
	{
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = CF_BUS_SIZE_ILLEGAL;
				return 0u;
			}
			_status = CF_BUS_OK;
			return m_interrupts->readRegister(_offset);
		}

		return select(_offset).read(_offset, _size, _status);
	}

	void ColdfireSoc::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
	{
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = CF_BUS_SIZE_ILLEGAL;
				return;
			}
			_status = CF_BUS_OK;
			m_interrupts->writeRegister(_offset, uint8_t(_value & 0xffu));
			return;
		}

		select(_offset).write(_offset, _size, _value, _status);
	}
}
