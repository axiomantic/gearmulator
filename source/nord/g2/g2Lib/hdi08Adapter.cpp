#include "hdi08Adapter.h"

namespace g2
{
	Hdi08Adapter::Hdi08Adapter() = default;

	uint8_t Hdi08Adapter::decodePortMask(uint32_t _addr) noexcept
	{
		uint32_t a3a10 = (_addr >> 3) & 0xFFu;
		if (a3a10 == 0x00u)
		{
			// Broadcast mode: all ports selected
			return 0xFFu;
		}

		uint8_t selectedMask = 0;
		for (size_t i = 0; i < kPortCount; ++i)
		{
			// Active-low: bit i is cold (0) when selected
			if (((a3a10 >> i) & 1u) == 0u)
			{
				selectedMask |= static_cast<uint8_t>(1u << i);
			}
		}
		return selectedMask;
	}

	uint8_t Hdi08Adapter::read8(uint32_t _addr)
	{
		uint8_t mask = decodePortMask(_addr);
		auto localAddr = static_cast<mc68k::PeriphAddress>(_addr & 0x7u);
		for (size_t i = 0; i < kPortCount; ++i)
		{
			if ((mask >> i) & 1u)
			{
				return m_ports[i].read8(localAddr);
			}
		}
		return 0xFFu;
	}

	uint16_t Hdi08Adapter::read16(uint32_t _addr)
	{
		uint8_t mask = decodePortMask(_addr);
		auto localAddr = static_cast<mc68k::PeriphAddress>(_addr & 0x7u);
		for (size_t i = 0; i < kPortCount; ++i)
		{
			if ((mask >> i) & 1u)
			{
				return m_ports[i].read16(localAddr);
			}
		}
		return 0xFFFFu;
	}

	void Hdi08Adapter::write8(uint32_t _addr, uint8_t _val)
	{
		uint8_t mask = decodePortMask(_addr);
		auto localAddr = static_cast<mc68k::PeriphAddress>(_addr & 0x7u);
		for (size_t i = 0; i < kPortCount; ++i)
		{
			if ((mask >> i) & 1u)
			{
				m_ports[i].write8(localAddr, _val);
			}
		}
	}

	void Hdi08Adapter::write16(uint32_t _addr, uint16_t _val)
	{
		uint8_t mask = decodePortMask(_addr);
		auto localAddr = static_cast<mc68k::PeriphAddress>(_addr & 0x7u);
		for (size_t i = 0; i < kPortCount; ++i)
		{
			if ((mask >> i) & 1u)
			{
				m_ports[i].write16(localAddr, _val);
			}
		}
	}

	void Hdi08Adapter::write32(uint32_t _addr, uint32_t _val)
	{
		// A 68k longword store at base+4 makes four byte cycles at offsets 4, 5, 6, 7.
		// Offset 4 is unused. Offsets 5, 6, 7 are TXH, TXM, TXL.
		write8(_addr + 0, static_cast<uint8_t>((_val >> 24) & 0xFFu));
		write8(_addr + 1, static_cast<uint8_t>((_val >> 16) & 0xFFu));
		write8(_addr + 2, static_cast<uint8_t>((_val >> 8) & 0xFFu));
		write8(_addr + 3, static_cast<uint8_t>(_val & 0xFFu));
	}

	void Hdi08Adapter::exec(uint32_t _deltaCycles)
	{
		for (size_t i = 0; i < kPortCount; ++i)
		{
			m_ports[i].exec(_deltaCycles);
		}
	}
}
