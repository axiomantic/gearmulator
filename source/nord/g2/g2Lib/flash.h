#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace g2
{
	/* Flash memory model for CS0 (Boot Flash) and CS2 (OS Flash).
	 * BRD-7: Read-only flash image loader for $30000000 OS flash and CS0 boot flash.
	 * Takes CS0 and CS2 base addresses and sizes as constructor parameters.
	 */
	class Flash
	{
	public:
		Flash(uint32_t _cs0Base, uint32_t _cs0Size, uint32_t _cs2Base, uint32_t _cs2Size);

		void loadCs0(const uint8_t* _data, size_t _size);
		void loadCs0(const std::vector<uint8_t>& _data);

		void loadCs2(const uint8_t* _data, size_t _size);
		void loadCs2(const std::vector<uint8_t>& _data);

		uint32_t cs0Base() const noexcept { return m_cs0Base; }
		uint32_t cs0Size() const noexcept { return m_cs0Size; }
		uint32_t cs2Base() const noexcept { return m_cs2Base; }
		uint32_t cs2Size() const noexcept { return m_cs2Size; }

		bool containsCs0(uint32_t _addr) const noexcept;
		bool containsCs2(uint32_t _addr) const noexcept;

		uint8_t read8(uint32_t _addr) const noexcept;
		uint16_t read16(uint32_t _addr) const noexcept;
		uint32_t read32(uint32_t _addr) const noexcept;

		void write8(uint32_t _addr, uint8_t _val) noexcept;
		void write16(uint32_t _addr, uint16_t _val) noexcept;
		void write32(uint32_t _addr, uint32_t _val) noexcept;

	private:
		uint32_t m_cs0Base;
		uint32_t m_cs0Size;
		uint32_t m_cs2Base;
		uint32_t m_cs2Size;

		std::vector<uint8_t> m_cs0Data;
		std::vector<uint8_t> m_cs2Data;
	};
}
