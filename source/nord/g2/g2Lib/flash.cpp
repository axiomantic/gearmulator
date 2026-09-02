// The read-only flash model.

#include "flash.h"

#include "baseLib/logging.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

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

		// The CFI command set, at byte offsets within the CS2 window. Offset
		// 0xAA is word address 0x55, where the JEDEC standard puts query-enter.
		constexpr uint32_t kCfiQueryEnterOffset  = 0x000000AAu;
		constexpr uint16_t kCfiQueryEnterCommand = 0x0098u;

		// 0xF0 is the AMD/Fujitsu reset. It is the ONLY exit command this model
		// honours, because the model answers a Primary Vendor Command Set ID of
		// 2 (AMD/Fujitsu Standard) and an AMD part does not reset on Intel's
		// 0xFF read-array command. Honouring 0xFF as well would answer for a
		// command set this device does not advertise, and would hide firmware
		// that took the ID-of-1 branch. A real part accepts the reset at any
		// address, so no offset is required here.
		constexpr uint16_t kCfiResetCommand = 0x00F0u;

		// What the CS2 device answers, per byte, while in query mode. The table
		// covers CFI word addresses 0x10 to 0x14, which is byte offsets 0x20 to
		// 0x29: the "QRY" signature in the low byte of each of the first three
		// words, then the Primary Vendor Command Set ID. Every other offset
		// falls through to the image, so query mode does not swallow the map.
		//
		// The ID is 2 -- an AMD-style part, which is an implication of this
		// file's own kErasedByte and not a firmware measurement. The firmware
		// accepts 1 or 2.
		//
		// The ID's high half is the byte at 0x26 and its low half the byte at
		// 0x28, which is how the firmware combines them. A strict JEDEC part
		// carries each datum in the LOW byte of its word, at 0x27 and 0x29,
		// which is where this table puts Q, R and Y. The two placements disagree
		// for the ID pair; the firmware's combination is what the probe branches
		// on, so it is what this table satisfies.
		constexpr uint32_t kCfiWindowFirst = 0x20u;
		constexpr uint32_t kCfiWindowLast  = 0x29u;


		constexpr uint8_t kCfiWindow[] = {
			0x00, 0x51,   // word 0x10, 'Q'
			0x00, 0x52,   // word 0x11, 'R'
			0x00, 0x59,   // word 0x12, 'Y'
			0x00, 0x02,   // word 0x13, the Primary Vendor Command Set ID at 0x26/0x27
			0x00, 0x00,   // word 0x14, the Extended Table pointer, absent
		};
	}

	Flash::Flash(uint32_t _cs0Base, uint32_t _cs0Size, uint32_t _cs2Base, uint32_t _cs2Size)
		: m_cs0Base(_cs0Base)
		, m_cs0Size(_cs0Size)
		, m_cs0(_cs0Size, kErasedByte)
		, m_cs2Base(_cs2Base)
		, m_cs2Size(_cs2Size)
		, m_cs2(_cs2Size, kErasedByte)
		, m_cs2QueryMode(false)
	{
	}

	Flash::~Flash() = default;

	bool Flash::loadCs0(const uint8_t* _data, size_t _size)
	{
		if(_size > m_cs0.size())
		{
			baseLib::logging::logToConsole("Refused CS0 image of "
				+ std::to_string(_size) + " bytes: CS0 is "
				+ std::to_string(m_cs0.size()) + " bytes");
			return false;
		}
		std::copy(_data, _data + _size, m_cs0.begin());
		return true;
	}

	bool Flash::loadCs0(const std::vector<uint8_t>& _data)
	{
		return loadCs0(_data.data(), _data.size());
	}

	bool Flash::loadCs2(const uint8_t* _data, size_t _size)
	{
		if(_size > m_cs2.size())
		{
			baseLib::logging::logToConsole("Refused CS2 image of "
				+ std::to_string(_size) + " bytes: CS2 is "
				+ std::to_string(m_cs2.size()) + " bytes");
			return false;
		}
		std::copy(_data, _data + _size, m_cs2.begin());
		return true;
	}

	bool Flash::loadCs2(const std::vector<uint8_t>& _data)
	{
		return loadCs2(_data.data(), _data.size());
	}

	bool Flash::containsCs0(uint32_t _addr) const
	{
		return _addr >= m_cs0Base && _addr < m_cs0Base + m_cs0Size;
	}

	bool Flash::containsCs2(uint32_t _addr) const
	{
		return _addr >= m_cs2Base && _addr < m_cs2Base + m_cs2Size;
	}

	bool Flash::cs2InQueryMode() const
	{
		return m_cs2QueryMode;
	}

	uint8_t Flash::read8(uint32_t _addr) const
	{
		if(containsCs0(_addr))
			return m_cs0[_addr - m_cs0Base];
		if(containsCs2(_addr))
		{
			const uint32_t offset = _addr - m_cs2Base;
			if(m_cs2QueryMode && offset >= kCfiWindowFirst && offset <= kCfiWindowLast)
			{
				const uint8_t v = kCfiWindow[offset - kCfiWindowFirst];
				return v;
			}
			return m_cs2[offset];
		}
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

	void Flash::write16(uint32_t _addr, uint16_t _value)
	{
		if(containsCs2(_addr))
		{
			const uint32_t offset = _addr - m_cs2Base;


			if(offset == kCfiQueryEnterOffset && _value == kCfiQueryEnterCommand)
			{
				m_cs2QueryMode = true;
				return;
			}

			if(_value == kCfiResetCommand)
			{
				m_cs2QueryMode = false;
				return;
			}
		}

		baseLib::logging::logToConsole("Rejected 16-bit write to read-only Flash at " + formatAddressHex(_addr));
	}

	void Flash::write32(uint32_t _addr, uint32_t /*_value*/)
	{
		baseLib::logging::logToConsole("Rejected 32-bit write to read-only Flash at " + formatAddressHex(_addr));
	}
}
