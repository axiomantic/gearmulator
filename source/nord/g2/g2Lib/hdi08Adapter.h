#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include "mc68k/hdi08.h"

namespace g2
{
	/* BRD-16: Adapter holding eight mc68k::Hdi08 host ports by value.
	 * Decodes relative address offset bits A3..A10 for active-low one-cold port selection and broadcast.
	 */
	class Hdi08Adapter
	{
	public:
		static constexpr size_t kPortCount = 8;

		Hdi08Adapter();

		mc68k::Hdi08& getPort(size_t _index) { return m_ports[_index]; }
		const mc68k::Hdi08& getPort(size_t _index) const { return m_ports[_index]; }

		mc68k::Hdi08& operator[](size_t _index) { return m_ports[_index]; }
		const mc68k::Hdi08& operator[](size_t _index) const { return m_ports[_index]; }

		uint8_t read8(uint32_t _addr);
		uint16_t read16(uint32_t _addr);
		void write8(uint32_t _addr, uint8_t _val);
		void write16(uint32_t _addr, uint16_t _val);
		void write32(uint32_t _addr, uint32_t _val);

		void exec(uint32_t _deltaCycles);

		// Returns 8-bit mask where bit i is 1 if port i is selected by _addr
		static uint8_t decodePortMask(uint32_t _addr) noexcept;

	private:
		std::array<mc68k::Hdi08, kPortCount> m_ports;
	};
}
