#include "uart0.h"

#include <cstddef>

namespace coldfire
{
	namespace
	{
		constexpr uint32_t kMode = 0x00u;
		constexpr uint32_t kStatusOrClock = 0x04u;
		constexpr uint32_t kCommand = 0x08u;
		constexpr uint32_t kBuffer = 0x0Cu;
		constexpr uint32_t kStrapOrAux = 0x10u;
		constexpr uint32_t kIntStatusOrMask = 0x14u;
		constexpr uint32_t kBaudMsb = 0x18u;
		constexpr uint32_t kBaudLsb = 0x1Cu;
		constexpr uint32_t kIntVector = 0x30u;
		constexpr uint32_t kInputPort = 0x34u;
		constexpr uint32_t kOutputSet = 0x38u;
		constexpr uint32_t kOutputReset = 0x3Cu;

		constexpr int kRxFifoDepth = 4;

		constexpr uint8_t kMiscResetModePointer = 0x01u;
		constexpr uint8_t kMiscResetReceiver = 0x02u;
		constexpr uint8_t kMiscResetTransmitter = 0x03u;
		constexpr uint8_t kMiscResetErrorStatus = 0x04u;

		constexpr uint8_t kEnable = 0x01u;
		constexpr uint8_t kDisable = 0x02u;
	}

	Uart0::Uart0(InterruptController* _interrupts)
		: m_interrupts(_interrupts)
	{
		if(m_interrupts)
			m_interrupts->setInternalVector(gUart0InterruptIndex, gUart0Vector);
	}

	void Uart0::setInterruptController(InterruptController* _interrupts)
	{
		m_interrupts = _interrupts;
		if(m_interrupts)
		{
			m_interrupts->setInternalVector(gUart0InterruptIndex, gUart0Vector);
			m_interrupts->setInternalPending(gUart0InterruptIndex, m_interruptAsserted);
		}
	}

	Uart0::UartLoc Uart0::locate(const uint32_t _offset) const
	{
		UartLoc loc;
		if(_offset >= gUart0Base && _offset < gUart0Base + gUartModuleSize)
			loc.inModule = true;
		else if(_offset >= gUart1Base && _offset < gUart1Base + gUartModuleSize)
		{
			loc.inModule = true;
			loc.uart1 = true;
		}
		if(loc.inModule)
			loc.local = _offset - (loc.uart1 ? gUart1Base : gUart0Base);
		return loc;
	}

	uint8_t Uart0::usr() const
	{
		uint8_t value = 0;
		if(m_txEnabled && !m_txHoldingValid)
			value |= 0x0Cu;
		if(m_rxFifoCount >= kRxFifoDepth)
			value |= 0x02u;
		if(m_rxFifoCount > 0)
			value |= 0x01u;
		return value;
	}

	uint8_t Uart0::readUart0(const uint32_t _local)
	{
		switch(_local)
		{
		case kMode:
		{
			const uint8_t value = m_modeUmr1 ? m_umr1 : m_umr2;
			m_modeUmr1 = false;
			return value;
		}
		case kStatusOrClock:
			return usr();
		case kBuffer:
		{
			if(m_rxFifoCount > 0)
			{
				const uint8_t byte = m_rxFifo[(m_rxFifoHead - m_rxFifoCount + kRxFifoDepth) % kRxFifoDepth];
				--m_rxFifoCount;
				recomputeInterrupt();
				return byte;
			}
			return 0x00u;
		}
		case kStrapOrAux:
			return 0x0Eu;
		case kIntStatusOrMask:
		{
			uint8_t value = 0;
			if(m_rxFifoCount > 0)
				value |= 0x02u;
			if(m_txEnabled && !m_txHoldingValid)
				value |= 0x01u;
			return value;
		}
		case kBaudMsb:
		case kBaudLsb:
			return 0x00u;
		case kIntVector:
			return m_uivr;
		case kInputPort:
			return 0x01u;
		default:
			return 0x00u;
		}
	}

	void Uart0::writeUart0(const uint32_t _local, const uint8_t _value)
	{
		switch(_local)
		{
		case kMode:
		{
			if(m_modeUmr1)
			{
				m_umr1 = _value;
				m_modeUmr1 = false;
			}
			else
				m_umr2 = _value;
			return;
		}
		case kStatusOrClock:
			m_ucsr = _value;
			return;
		case kCommand:
		{
			const uint8_t misc = (_value >> 4) & 0x07u;
			const uint8_t tc   = (_value >> 2) & 0x03u;
			const uint8_t rc   =  _value        & 0x03u;

			switch(misc)
			{
			case kMiscResetModePointer: m_modeUmr1 = true; break;
			case kMiscResetReceiver:
				m_rxEnabled = false;
				m_rxFifoCount = 0;
				m_rxFifoHead = 0;
				break;
			case kMiscResetTransmitter:
				m_txEnabled = false;
				m_txHoldingValid = false;
				break;
			case kMiscResetErrorStatus: break;
			default: break;
			}

			if(tc == kEnable) m_txEnabled = true;
			else if(tc == kDisable) { m_txEnabled = false; m_txHoldingValid = false; }

			if(rc == kEnable) m_rxEnabled = true;
			else if(rc == kDisable) { m_rxEnabled = false; m_rxFifoCount = 0; }

			recomputeInterrupt();
			return;
		}
		case kBuffer:
		{
			m_txHolding = _value;
			m_txHoldingValid = true;
			if(m_txEnabled && m_midiOut)
				m_midiOut(m_midiOutUser, _value);
			recomputeInterrupt();
			return;
		}
		case kStrapOrAux:
			m_uacr = _value;
			return;
		case kIntStatusOrMask:
			m_uimr = _value;
			recomputeInterrupt();
			return;
		case kBaudMsb:
			m_ubg1 = _value;
			return;
		case kBaudLsb:
			m_ubg2 = _value;
			return;
		case kIntVector:
			m_uivr = _value;
			return;
		case kOutputSet:
		case kOutputReset:
			return;
		default:
			logLine("UNMODELLED", true, 8, gUart0Base + _local);
			return;
		}
	}

	uint8_t Uart0::readReset(const uint32_t _local) const
	{
		switch(_local)
		{
		case kIntVector: return 0x0Fu;
		case kStrapOrAux: return 0x0Eu;
		default: return 0x00u;
		}
	}

	uint32_t Uart0::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if(!isByteAccess(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const UartLoc loc = locate(_offset);
		if(!loc.inModule)
		{
			_status = CF_BUS_UNMAPPED;
			logLine("UNMAPPED", false, _size, _offset);
			return 0;
		}

		if(loc.uart1)
			return readReset(loc.local);

		return readUart0(loc.local);
	}

	void Uart0::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if(!isByteAccess(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const UartLoc loc = locate(_offset);
		if(!loc.inModule)
		{
			_status = CF_BUS_UNMAPPED;
			logLine("UNMAPPED", true, _size, _offset);
			return;
		}

		if(!loc.uart1)
			writeUart0(loc.local, uint8_t(_value & 0xffu));
	}

	void Uart0::setMidiOut(const MidiOutFn _fn, void* _user)
	{
		m_midiOut = _fn;
		m_midiOutUser = _user;
	}

	void Uart0::receive(const uint8_t _byte)
	{
		if(!m_rxEnabled)
			return;

		if(m_rxFifoCount < kRxFifoDepth)
		{
			m_rxFifo[m_rxFifoHead] = _byte;
			m_rxFifoHead = (m_rxFifoHead + 1) % kRxFifoDepth;
			++m_rxFifoCount;
		}
		recomputeInterrupt();
	}

	void Uart0::transmitComplete()
	{
		m_txHoldingValid = false;
		recomputeInterrupt();
	}

	bool Uart0::interruptAsserted() const
	{
		const uint8_t uisr =
			((m_rxFifoCount > 0) ? 0x02u : 0x00u)
			| ((m_txEnabled && !m_txHoldingValid) ? 0x01u : 0x00u);
		return (uisr & m_uimr) != 0;
	}

	void Uart0::recomputeInterrupt()
	{
		setPending(interruptAsserted());
	}

	void Uart0::setPending(const bool _asserted)
	{
		if(_asserted == m_interruptAsserted)
			return;

		m_interruptAsserted = _asserted;

		if(m_interrupts)
			m_interrupts->setInternalPending(gUart0InterruptIndex, _asserted);
	}

	void Uart0::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
	{
		static const char* digits = "0123456789abcdef";
		std::string hex = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			hex += digits[(_offset >> shift) & 0xfu];
		m_log.push_back(std::string("uart0: ") + _reason
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at MBAR+" + hex);
	}
}
