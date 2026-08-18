// Task BRD-24. The M-Bus controller. See mbus.h for what this unit is.
//
// PROVENANCE. The register map, the bit names and the START/STOP rules come
// from MCF5307UM chapter 15 and Table 15-1. What the model must PRODUCE comes
// from a disassembly of this machine's own firmware, which polls MBSR at
// MBAR+$28C in both the register-relative and the absolute form and never reads
// MBAR+$284 at all.
//
// WHERE MBAR IS, IS NOT THIS FILE'S QUESTION. The offsets below are
// MBAR-relative and the decode produces them, which is what keeps the inferred
// base out of this unit entirely.

#include "mbus.h"

namespace g2
{
	namespace
	{
		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}

		bool isRegister(const uint32_t _offset)
		{
			return _offset == MBus::g_madr
			    || _offset == MBus::g_mfdr
			    || _offset == MBus::g_mbcr
			    || _offset == MBus::g_mbsr
			    || _offset == MBus::g_mbdr;
		}

		std::string hex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for(int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}
	}

	MBus::MBus(BusSlave* const _slave) : m_slave(_slave)
	{
	}

	void MBus::logLine(const char* const _reason, const bool _isWrite, const int _size,
		const uint32_t _offset)
	{
		m_log.push_back(std::string("mbus: ") + _reason
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at offset " + hex32(_offset));
	}

	uint32_t MBus::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0u;
		}

		if(!isRegister(_offset))
		{
			logLine("UNMODELLED", false, _size, _offset);
			return 0u;
		}

		// Every M-Bus register is one byte wide, so a wider access reaches no
		// whole register and is refused rather than silently narrowed.
		if(_size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0u;
		}

		return readRegister(_offset);
	}

	void MBus::write(const uint32_t _offset, const int _size, const uint32_t _value,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		if(!isRegister(_offset))
		{
			logLine("UNMODELLED", true, _size, _offset);
			return;
		}

		if(_size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		writeRegister(_offset, uint8_t(_value & 0xffu));
	}

	uint8_t MBus::readRegister(const uint32_t _offset)
	{
		switch(_offset)
		{
		case g_madr:
			return m_madr;
		case g_mfdr:
			return m_mfdr;
		case g_mbcr:
			return m_mbcr;
		case g_mbsr:
			/* THE STATUS REGISTER IS COMPOSED AND NEVER STORED. A stored byte
			 * cannot report the bus busy after a START and idle after a STOP,
			 * which is what the firmware requires of it. */
			return uint8_t((m_busBusy ? g_mbb : 0u)
			             | (m_interrupt ? g_mif : 0u)
			             | (m_notAcknowledged ? g_rxak : 0u));
		case g_mbdr:
			return receive();
		default:
			return 0u;
		}
	}

	void MBus::writeRegister(const uint32_t _offset, const uint8_t _value)
	{
		switch(_offset)
		{
		case g_madr:
			m_madr = _value;
			break;
		case g_mfdr:
			m_mfdr = _value;
			break;
		case g_mbcr:
			writeControl(_value);
			break;
		case g_mbsr:
			/* MIF IS CLEARED BY WRITING ZERO TO IT and is the only bit of this
			 * register a write reaches in a master transfer. The firmware
			 * spells it `v & $FD`. */
			if((_value & g_mif) == 0u)
				m_interrupt = false;
			break;
		case g_mbdr:
			transmit(_value);
			break;
		default:
			break;
		}
	}

	void MBus::writeControl(const uint8_t _value)
	{
		const bool wasMaster = (m_mbcr & g_msta) != 0u;
		const bool isMaster  = (_value & g_msta) != 0u;

		m_mbcr = _value;

		/* MSTA 0 to 1 GENERATES A START AND 1 TO 0 GENERATES A STOP, and MBB
		 * follows the bus conditions rather than the register bit: the firmware
		 * clears MSTA and then requires MBB CLEAR, and later sets MSTA and
		 * requires MBB SET, which is why the transition and not the level is
		 * what this model keys on. */
		if(!wasMaster && isMaster)
		{
			m_busBusy      = true;
			m_addressPhase = true;
			return;
		}

		if(wasMaster && !isMaster)
		{
			m_busBusy      = false;
			m_addressPhase = false;

			if(m_slave)
				m_slave->stop();
		}
	}

	void MBus::transmit(const uint8_t _value)
	{
		// A data register write with no START behind it puts nothing on a bus
		// this module does not own.
		if((m_mbcr & g_msta) == 0u)
			return;

		if(m_addressPhase)
		{
			m_addressPhase = false;
			m_notAcknowledged = !(m_slave && m_slave->start(uint8_t(_value >> 1),
			                                                (_value & 0x01u) != 0u));
		}
		else
		{
			m_notAcknowledged = !(m_slave && m_slave->write(_value));
		}

		/* MIF IS SET AT THE END OF EVERY BYTE TRANSFER. This model has no clock,
		 * so the transfer completes within the write that started it. */
		m_interrupt = true;
	}

	uint8_t MBus::receive()
	{
		const uint8_t result = m_received;

		/* A DATA REGISTER READ HANDS BACK THE BYTE THE LAST TRANSFER CLOCKED IN
		 * AND STARTS THE NEXT ONE, which is why the firmware's first read after
		 * the address phase is a dummy whose value it discards.
		 *
		 * NO NOT-ACKNOWLEDGE IS EVER SENT. The firmware clears TXAK on every
		 * received byte with no index test, so nothing here may invent the NACK
		 * that would end the transfer. */
		if((m_mbcr & g_msta) != 0u && (m_mbcr & g_mtx) == 0u)
		{
			m_received  = m_slave ? m_slave->read() : 0xffu;
			m_interrupt = true;
		}

		return result;
	}
}
