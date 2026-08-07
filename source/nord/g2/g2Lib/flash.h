#pragma once

// Task BRD-7. Design section 7.4.
//
// Read-only flash model. CS0 holds the boot loader image; CS2 holds the main
// flash image. The model does not model erase, does not model write, and does
// not run Clavia's update procedure: writes are logged as rejected.
//
// The CS0 and CS2 bases and sizes come from the test fixture and no shipped
// header carries a number. AGENTS.md section 2.2 records them as unrecorded,
// section 1.3 rule 1 forbids writing them into a header, and SPK-13 reads
// CSAR0 to CSAR5 from the real firmware when a measured value is required.

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
		// fill value (0xFF). Throws std::logic_error when _size exceeds the
		// configured size.
		void loadCs0(const uint8_t* _data, size_t _size);
		void loadCs0(const std::vector<uint8_t>& _data);

		// Load the CS2 image. Same contract as loadCs0.
		void loadCs2(const uint8_t* _data, size_t _size);
		void loadCs2(const std::vector<uint8_t>& _data);

		// Address-range predicates.
		bool containsCs0(uint32_t _addr) const;
		bool containsCs2(uint32_t _addr) const;

		// Big-endian reads, matching the ColdFire byte order.
		uint8_t  read8 (uint32_t _addr) const;
		uint16_t read16(uint32_t _addr) const;
		uint32_t read32(uint32_t _addr) const;

		// Writes are logged and rejected. The flash model is read-only:
		// erase, write, and the Clavia update procedure are out of scope for
		// this task. The model carries contents from reset, so whatever image
		// was loaded is readable from the first cycle, but no call below ever
		// changes the underlying bytes.
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
	};
}
