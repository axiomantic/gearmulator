// Task BRD-12. The panel.
//
// Plan section 13.3, BRD-12. Design sections 8.1, 8.2, 8.3.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1.
//
// THIS FILE CARRIES NO ADDRESS. The display window sits on CS4, no authority
// records CS4's base or its size, and both arrive as configuration.
//
// WHY THE BUFFER STARTS AT ZERO. A boot loop polls the panel until it answers.
// Every offset of this window answers at every legal width, so no poll can
// spin for ever, and a freshly built panel reads zero everywhere, which is the
// quiescent report: no key down, no encoder moving, no button pressed.
//
// WHAT THIS MODEL DOES NOT DO. It reads no banner and writes none. The banner
// is produced by Clavia's OS image running, so INT-1's t1_boot is what reads
// this buffer for it, and BRD-14's t1_rejected_config is what proves the panel
// model is not permissive. Both are T1 and gated.

#include "panel.h"

namespace g2
{
	namespace
	{
		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}
	}

	Panel::Panel(const uint32_t _displaySize)
		: m_display(_displaySize, 0u)
	{
	}

	uint32_t Panel::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
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
			if(index < m_display.size())
				value |= m_display[index];
		}

		return value;
	}

	void Panel::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
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
			if(index >= m_display.size())
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			m_display[index] = uint8_t((_value >> shift) & 0xffu);
		}
	}
}
