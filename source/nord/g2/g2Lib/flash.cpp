// Task BRD-7. Design section 7.4.

#include "flash.h"

#include "baseLib/logging.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace g2
{
	namespace
	{
		// The constructor fills both images with 0xFF. That is the erased
		// state of an AMD-style flash and the state the boot loader sees on
		// a board that lost power to the chip while the rest of the system
		// held reset. The test fixture overwrites whatever bytes it cares
		// about; the rest stay at 0xFF.
		constexpr uint8_t kErasedByte = 0xFF;

		// Format the address as eight lowercase hex digits with a leading
		// "0x". The rejection messages all use this form so a log reader can
		// match them.
		std::string formatAddressHex(uint32_t _addr)
		{
			std::stringstream ss;
			ss << "0x" << std::hex << std::setfill('0') << std::setw(8) << _addr;
			return ss.str();
		}
	}

	Flash::Flash(uint32_t _cs0Base, uint32_t _cs0Size, uint32_t _cs2Base, uint32_t _cs2Size)
		: m_cs0Base(_cs0Base)
		, m_cs0Size(_cs0Size)
		, m_cs0(_cs0Size, kErasedByte)
		, m_cs2Base(_cs2Base)
		, m_cs2Size(_cs2Size)
		, m_cs2(_cs2Size, kErasedByte)
	{
	}

	Flash::~Flash() = default;

	void Flash::loadCs0(const uint8_t* _data, size_t _size)
	{
		if(_size > m_cs0.size())
			throw std::logic_error("loadCs0: data larger than CS0 size");
		std::copy(_data, _data + _size, m_cs0.begin());
	}

	void Flash::loadCs0(const std::vector<uint8_t>& _data)
	{
		loadCs0(_data.data(), _data.size());
	}

	void Flash::loadCs2(const uint8_t* _data, size_t _size)
	{
		if(_size > m_cs2.size())
			throw std::logic_error("loadCs2: data larger than CS2 size");
		std::copy(_data, _data + _size, m_cs2.begin());
	}

	void Flash::loadCs2(const std::vector<uint8_t>& _data)
	{
		loadCs2(_data.data(), _data.size());
	}

	bool Flash::containsCs0(uint32_t _addr) const
	{
		return _addr >= m_cs0Base && _addr < m_cs0Base + m_cs0Size;
	}

	bool Flash::containsCs2(uint32_t _addr) const
	{
		return _addr >= m_cs2Base && _addr < m_cs2Base + m_cs2Size;
	}

	uint8_t Flash::read8(uint32_t _addr) const
	{
		if(containsCs0(_addr))
			return m_cs0[_addr - m_cs0Base];
		if(containsCs2(_addr))
			return m_cs2[_addr - m_cs2Base];
		// Outside both images. Returning 0xFF matches an erased device and
		// is the same answer the AM29F family gives on a bus float.
		return kErasedByte;
	}

	uint16_t Flash::read16(uint32_t _addr) const
	{
		return static_cast<uint16_t>(
			(static_cast<uint32_t>(read8(_addr    )) << 8) |
			 static_cast<uint32_t>(read8(_addr + 1))        );
	}

	uint32_t Flash::read32(uint32_t _addr) const
	{
		return (static_cast<uint32_t>(read8(_addr    )) << 24) |
		       (static_cast<uint32_t>(read8(_addr + 1)) << 16) |
		       (static_cast<uint32_t>(read8(_addr + 2)) <<  8) |
		        static_cast<uint32_t>(read8(_addr + 3));
	}

	void Flash::write8(uint32_t _addr, uint8_t /*_value*/)
	{
		baseLib::logging::logToConsole("Rejected write to read-only Flash at " + formatAddressHex(_addr));
	}

	void Flash::write16(uint32_t _addr, uint16_t /*_value*/)
	{
		baseLib::logging::logToConsole("Rejected 16-bit write to read-only Flash at " + formatAddressHex(_addr));
	}

	void Flash::write32(uint32_t _addr, uint32_t /*_value*/)
	{
		baseLib::logging::logToConsole("Rejected 32-bit write to read-only Flash at " + formatAddressHex(_addr));
	}
}
