// Task BRD-12. The CS5 latches.
//
// Plan section 13.3, BRD-12. Design sections 8.1, 8.2, 8.3.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1.
//
// THIS FILE CARRIES NO ADDRESS. The one recorded base, 0x15000000, lives in
// memoryMap.h as g_cs5Base, and the window size is configuration.
//
// THE PANEL IDENTIFIER IS A STRAP AND NOT A REGISTER. Clavia's service manual
// records the model as two 0-ohm resistors, R79 and R80, on the panel board.
// So a write reaches the identifier bits and changes nothing, and the model
// presents the same machine for the whole run.
//
// THE SIX BITS OUTSIDE 5:4 HAVE NO RECORDED SOURCE AND THIS MODEL READS THEM
// ZERO. AGENTS.md section 2.3 records the two identifier bits and records
// nothing about the rest of the byte. Reading them zero is a model decision
// and it is stated here rather than presented as a measurement.

#include "latches.h"

namespace g2
{
	namespace
	{
		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}
	}

	Latches::Latches(const uint32_t _windowSize)
		: m_latch(_windowSize, 0u)
	{
		if(m_latch.size() > g_panelIdentifierOffset)
			m_latch[g_panelIdentifierOffset] = g_panelIdentifierG2X;
	}

	uint32_t Latches::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			return 0;
		}

		// The part is big-endian. A byte beyond the window reads zero rather
		// than reading past the end of the model.
		const uint32_t bytes = uint32_t(_size) / 8u;
		uint32_t value = 0;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			value <<= 8;
			if(index < m_latch.size())
				value |= m_latch[index];
		}

		return value;
	}

	void Latches::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			return;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			if(index >= m_latch.size())
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			const uint8_t incoming = uint8_t((_value >> shift) & 0xffu);

			if(index == g_panelIdentifierOffset)
			{
				// The identifier bits are strapped. Everything else in the
				// first byte is an output latch like any other.
				m_latch[index] = uint8_t((m_latch[index] & g_panelIdentifierMask)
					| (incoming & uint8_t(~g_panelIdentifierMask)));
				continue;
			}

			m_latch[index] = incoming;
		}
	}
}
