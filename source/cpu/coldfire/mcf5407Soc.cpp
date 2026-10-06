#include "mcf5407Soc.h"

namespace coldfire
{
	Mcf5407Soc::Mcf5407Soc(void* _irqUser, InterruptPresentFn _irqPresent, bool _engineStrap)
		: m_ownedIntc(_irqUser, _irqPresent)
		, m_ownedSim(_engineStrap)
		, m_ownedUart0(&m_ownedIntc)
		, m_interrupts(&m_ownedIntc)
		, m_sim(&m_ownedSim)
		, m_uart0(&m_ownedUart0)
	{
		m_ownedSim.setInterruptController(&m_ownedIntc);
	}

	Mcf5407Soc::Mcf5407Soc(Sim& _sim, Uart0& _uart0, BusTarget& _mbus, InterruptController& _interrupts)
		: m_interrupts(&_interrupts)
		, m_sim(&_sim)
		, m_uart0(&_uart0)
		, m_mbus(&_mbus)
	{
	}

	Mcf5407Soc::Mcf5407Soc(Sim& _sim, Uart0& _uart0, BusTarget* _mbus, InterruptController& _interrupts)
		: m_interrupts(&_interrupts)
		, m_sim(&_sim)
		, m_uart0(&_uart0)
		, m_mbus(_mbus)
	{
	}

	bool Mcf5407Soc::isUartOwned(const uint32_t _offset)
	{
		if(_offset == g_simUipcrOffset)
			return false;

		if(_offset >= Uart0::gUart0Base && _offset < Uart0::gUart0Base + Uart0::gUartModuleSize)
			return true;

		return _offset >= Uart0::gUart1Base && _offset < Uart0::gUart1Base + Uart0::gUartModuleSize;
	}

	bool Mcf5407Soc::isMbusOwned(const uint32_t _offset)
	{
		return _offset >= gMbusBase && _offset < gMbusBase + gMbusSize;
	}

	bool Mcf5407Soc::isInterruptOwned(const uint32_t _offset)
	{
		if(_offset == InterruptController::gIrqparOffset)
			return true;
		if(_offset == InterruptController::gAvrOffset)
			return true;
		return _offset >= InterruptController::gIcrBase
			&& _offset < InterruptController::gIcrBase + InterruptController::gIcrCount;
	}

	BusTarget& Mcf5407Soc::select(const uint32_t _offset)
	{
		if(m_mbus && isMbusOwned(_offset))
			return *m_mbus;
		if(isUartOwned(_offset))
			return *m_uart0;
		return *m_sim;
	}

	uint32_t Mcf5407Soc::read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status)
	{
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
				return 0u;
			}
			_status = MCF5407_BUS_OK;
			return m_interrupts->readRegister(_offset);
		}

		return select(_offset).read(_offset, _size, _status);
	}

	void Mcf5407Soc::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5407_bus_status& _status)
	{
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
				return;
			}
			_status = MCF5407_BUS_OK;
			m_interrupts->writeRegister(_offset, uint8_t(_value & 0xffu));
			return;
		}

		select(_offset).write(_offset, _size, _value, _status);
	}
}
