#pragma once

// Read-only flash model. CS0 holds the boot loader image; CS2 holds the main
// flash image. The model does not model erase, does not model write, and does
// not run Clavia's update procedure: writes are logged as rejected.
//
// No authority records the CS0 and CS2 bases and sizes, so the caller supplies
// them and no shipped header carries a number.

#include <cstdint>
#include <vector>

namespace g2
{
	class Flash
	{
	public:
		Flash(uint32_t _cs0Base, uint32_t _cs0Size, uint32_t _cs2Base, uint32_t _cs2Size);
		~Flash();

		// Load the CS0 image. Bytes past _size are left at the constructor's
		// fill value (0xFF).
		//
		// An image larger than the configured size is REFUSED: the call
		// returns false, logs the refusal with both sizes, and leaves the
		// contents untouched. The return is the error channel because the
		// rest of g2Lib reports through a return value rather than an
		// exception, and because a truncating load would put an image the
		// caller never asked for in front of the boot vector.
		[[nodiscard]] bool loadCs0(const uint8_t* _data, size_t _size);
		[[nodiscard]] bool loadCs0(const std::vector<uint8_t>& _data);

		// Load the CS2 image. Same contract as loadCs0.
		[[nodiscard]] bool loadCs2(const uint8_t* _data, size_t _size);
		[[nodiscard]] bool loadCs2(const std::vector<uint8_t>& _data);

		// Address-range predicates.
		bool containsCs0(uint32_t _addr) const;
		bool containsCs2(uint32_t _addr) const;

		// True while the CS2 device is in CFI query mode. The flag is a MEMBER
		// and not a file-local static: a static would be process-global, so two
		// Boards would share one mode.
		bool cs2InQueryMode() const;

		// Big-endian reads, matching the ColdFire byte order.
		uint8_t  read8 (uint32_t _addr) const;
		uint16_t read16(uint32_t _addr) const;
		uint32_t read32(uint32_t _addr) const;

		// Writes are logged and rejected. The flash model is read-only:
		// erase, write, and the Clavia update procedure are out of scope.
		// The model carries contents from reset, so whatever image
		// was loaded is readable from the first cycle, but no call below ever
		// changes the underlying bytes.
		//
		// Two 16-bit writes to CS2 are commands rather than rejected writes, and
		// they still change no byte. They select the CFI query mode: the
		// container header occupies the exact offsets the CFI probe reads, so a
		// model that STORED the signature would destroy the container the boot
		// loader parses. Real hardware separates the two readings of those
		// offsets only by mode.
		void write8 (uint32_t _addr, uint8_t  _value);
		void write16(uint32_t _addr, uint16_t _value);
		void write32(uint32_t _addr, uint32_t _value);

	private:
		uint32_t            m_cs0Base;
		uint32_t            m_cs0Size;
		std::vector<uint8_t> m_cs0;

		uint32_t            m_cs2Base;
		uint32_t            m_cs2Size;
		std::vector<uint8_t> m_cs2;

		bool                m_cs2QueryMode;
	};
}
