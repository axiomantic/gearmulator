#include "panelSram.h"

#include <cstring>

namespace g2
{
	PanelSram::PanelSram(MemoryMap& _memory)
		: m_bank(g_panelSramSize, 0u)
		, m_latches(_memory.target(Region::Cs5))
		, m_latchWindow(_memory.window(Region::Cs5))
	{
	}

	bool PanelSram::isLatch(const uint32_t _absolute, uint32_t& _latchOffset) const
	{
		if(m_latches == nullptr || m_latchWindow.size == 0u)
			return false;

		if(_absolute < m_latchWindow.base || _absolute - m_latchWindow.base >= m_latchWindow.size)
			return false;

		_latchOffset = _absolute - m_latchWindow.base;
		return true;
	}

	uint32_t PanelSram::read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status)
	{
		_status = MCF5407_BUS_OK;

		if(_size != 8 && _size != 16 && _size != 32)
		{
			_status = MCF5407_BUS_SIZE_ILLEGAL;
			return 0u;
		}

		const uint32_t count = uint32_t(_size) / 8u;

		/* The latch pass-through is decided on the first byte and the whole
		 * access is handed over, because a latch read is a register read and
		 * splitting it into bytes would model a bus cycle the core never
		 * performs. */
		uint32_t latchOffset = 0u;

		if(isLatch(g_panelSramCs4Window.base + _offset, latchOffset))
			return m_latches->read(latchOffset, _size, _status);

		uint32_t value = 0u;

		for(uint32_t i = 0; i < count; ++i)
		{
			value <<= 8;

			const uint32_t absolute = g_panelSramCs4Window.base + _offset + i;

			if(absolute >= g_panelSramBase && absolute < g_panelSramBase + g_panelSramSize)
				value |= m_bank[absolute - g_panelSramBase];
		}

		return value;
	}

	void PanelSram::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5407_bus_status& _status)
	{
		_status = MCF5407_BUS_OK;

		if(_size != 8 && _size != 16 && _size != 32)
		{
			_status = MCF5407_BUS_SIZE_ILLEGAL;
			return;
		}

		uint32_t latchOffset = 0u;

		if(isLatch(g_panelSramCs4Window.base + _offset, latchOffset))
		{
			m_latches->write(latchOffset, _size, _value, _status);
			return;
		}

		const uint32_t count = uint32_t(_size) / 8u;

		for(uint32_t i = 0; i < count; ++i)
		{
			const uint32_t absolute = g_panelSramCs4Window.base + _offset + i;
			const int      shift    = int(8u * (count - 1u - i));
			const uint8_t  byte     = uint8_t((_value >> shift) & 0xffu);

			if(absolute >= g_panelSramBase && absolute < g_panelSramBase + g_panelSramSize)
			{
				m_bank[absolute - g_panelSramBase] = byte;
				++m_bankWrites;
			}
			else
			{
				++m_holeWrites;
			}
		}
	}

	bool PanelSram::place(const uint32_t _absolute, const std::vector<uint8_t>& _image)
	{
		/* An empty image is refused rather than accepted as a no-op: every
		 * caller reads it from a file, and a missing file arrives here as an
		 * empty vector that would otherwise map nothing and report success. */
		if(_image.empty())
			return false;

		if(_absolute < g_panelSramBase)
			return false;

		if(size_t(_absolute - g_panelSramBase) + _image.size() > m_bank.size())
			return false;

		std::memcpy(m_bank.data() + (_absolute - g_panelSramBase), _image.data(), _image.size());

		return true;
	}
}
