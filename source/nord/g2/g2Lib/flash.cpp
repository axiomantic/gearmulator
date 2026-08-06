#include "flash.h"
#include "baseLib/logging.h"
#include <algorithm>
#include <iostream>

namespace g2
{
	Flash::Flash(uint32_t _cs0Base, uint32_t _cs0Size, uint32_t _cs2Base, uint32_t _cs2Size)
		: m_cs0Base(_cs0Base)
		, m_cs0Size(_cs0Size)
		, m_cs2Base(_cs2Base)
		, m_cs2Size(_cs2Size)
		, m_cs0Data(_cs0Size, 0xFFu)
		, m_cs2Data(_cs2Size, 0xFFu)
	{
	}

	void Flash::loadCs0(const uint8_t* _data, size_t _size)
	{
		if (!_data || _size == 0)
			return;

		size_t copySize = std::min(_size, static_cast<size_t>(m_cs0Size));
		if (m_cs0Data.size() < copySize)
			m_cs0Data.resize(copySize, 0xFFu);

		std::copy(_data, _data + copySize, m_cs0Data.begin());
	}

	void Flash::loadCs0(const std::vector<uint8_t>& _data)
	{
		loadCs0(_data.data(), _data.size());
	}

	void Flash::loadCs2(const uint8_t* _data, size_t _size)
	{
		if (!_data || _size == 0)
			return;

		size_t copySize = std::min(_size, static_cast<size_t>(m_cs2Size));
		if (m_cs2Data.size() < copySize)
			m_cs2Data.resize(copySize, 0xFFu);

		std::copy(_data, _data + copySize, m_cs2Data.begin());
	}

	void Flash::loadCs2(const std::vector<uint8_t>& _data)
	{
		loadCs2(_data.data(), _data.size());
	}

	bool Flash::containsCs0(uint32_t _addr) const noexcept
	{
		return (m_cs0Size > 0) && (_addr >= m_cs0Base) && (_addr < m_cs0Base + m_cs0Size);
	}

	bool Flash::containsCs2(uint32_t _addr) const noexcept
	{
		return (m_cs2Size > 0) && (_addr >= m_cs2Base) && (_addr < m_cs2Base + m_cs2Size);
	}

	uint8_t Flash::read8(uint32_t _addr) const noexcept
	{
		if (containsCs0(_addr))
		{
			uint32_t offset = _addr - m_cs0Base;
			if (offset < m_cs0Data.size())
				return m_cs0Data[offset];
			return 0xFFu;
		}

		if (containsCs2(_addr))
		{
			uint32_t offset = _addr - m_cs2Base;
			if (offset < m_cs2Data.size())
				return m_cs2Data[offset];
			return 0xFFu;
		}

		return 0xFFu;
	}

	uint16_t Flash::read16(uint32_t _addr) const noexcept
	{
		uint8_t b0 = read8(_addr);
		uint8_t b1 = read8(_addr + 1);
		return static_cast<uint16_t>((static_cast<uint16_t>(b0) << 8) | b1);
	}

	uint32_t Flash::read32(uint32_t _addr) const noexcept
	{
		uint16_t w0 = read16(_addr);
		uint16_t w1 = read16(_addr + 2);
		return (static_cast<uint32_t>(w0) << 16) | w1;
	}

	void Flash::write8(uint32_t _addr, uint8_t _val) noexcept
	{
		(void)_val;
		if (containsCs0(_addr) || containsCs2(_addr))
		{
			LOG("Rejected write to read-only Flash at 0x" << HEX(_addr));
		}
	}

	void Flash::write16(uint32_t _addr, uint16_t _val) noexcept
	{
		(void)_val;
		if (containsCs0(_addr) || containsCs2(_addr))
		{
			LOG("Rejected 16-bit write to read-only Flash at 0x" << HEX(_addr));
		}
	}

	void Flash::write32(uint32_t _addr, uint32_t _val) noexcept
	{
		(void)_val;
		if (containsCs0(_addr) || containsCs2(_addr))
		{
			LOG("Rejected 32-bit write to read-only Flash at 0x" << HEX(_addr));
		}
	}
}
