// The panel.
//
// Why the buffer starts at zero: a boot loop polls the panel until it answers.
// Every offset of this window answers at every legal width, so no poll can spin
// for ever, and a freshly built panel reads zero everywhere, which is the
// quiescent report: no key down, no encoder moving, no button pressed.

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
