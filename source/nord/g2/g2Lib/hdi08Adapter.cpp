// Task BRD-16. The HDI08 host-port adapter over `mc68k::Hdi08`.
//
// Plan section 13.3, BRD-16. Design sections 10.1, 10.2.
// Logbook: AGENTS.md section 3.1.
//
// THE WORD COMPLETES WHEN TXL IS WRITTEN, AND THIS FILE DOES NOT ASSUME THE
// CPU ISSUED A LONGWORD. `mc68k::Hdi08` assembles the word from TXH, TXM and
// TXL and fires its writeTx callback only when TXL lands (hdi08.cpp:221-245).
// So a byte-at-a-time firmware path and a 32-bit store at register offset +4
// both reach the same word, and this file routes byte writes to the selected
// port(s) without caring which path produced them.
//
// THE ONE 68K LONGWORD STORE. A 32-bit store at register offset 4 makes four
// byte cycles at offsets 4, 5, 6 and 7, of which offset 4 is unused. The
// bus-facing width of a ColdFire access is 8, 16 or 32, and this file simply
// decomposes 16- and 32-bit writes into big-endian byte cycles and hands each
// byte to the selected port's `write8`. Decomposing here and not in the
// MemoryMap is what lets a byte-at-a-time firmware path and a longword store
// share one code path and produce one word.
//
// THIS FILE CARRIES NO ADDRESS AND NO BASE TABLE. See hdi08Adapter.h.

#include "hdi08Adapter.h"

namespace g2
{
	Hdi08Adapter::Hdi08Adapter(const Hdi08Decode _decode)
		: m_decode(_decode)
	{
	}

	bool Hdi08Adapter::isLegalWidth(const int _size) const
	{
		return _size == 8 || _size == 16 || _size == 32;
	}

	mc68k::Hdi08& Hdi08Adapter::port(const int _index)
	{
		return m_ports[_index];
	}

	const mc68k::Hdi08& Hdi08Adapter::port(const int _index) const
	{
		return m_ports[_index];
	}

	uint32_t Hdi08Adapter::read(const uint32_t _offset, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			return 0;
		}

		const Hdi08Selection selection = m_decode.decode(_offset);
		if(selection.ports == 0)
			return 0;

		// A read addresses the lowest-numbered selected port. Reads on the
		// host port configure and sense a single DSP interface; the broadcast
		// is a write-side concept.
		int portIndex = 0;
		while(((selection.ports >> portIndex) & 1u) == 0)
			++portIndex;

		const int byteCount = _size / 8;
		uint32_t value = 0;
		for(int i = 0; i < byteCount; ++i)
		{
			const uint8_t byte = m_ports[portIndex].read8(
				static_cast<mc68k::PeriphAddress>(selection.portOffset + i));

			// Big-endian: the byte at the lowest address is the most
			// significant.
			value |= uint32_t(byte) << (8 * (byteCount - 1 - i));
		}

		return value;
	}

	void Hdi08Adapter::write(const uint32_t _offset, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			return;
		}

		const Hdi08Selection selection = m_decode.decode(_offset);
		if(selection.ports == 0)
			return;

		const int byteCount = _size / 8;

		for(int i = 0; i < byteCount; ++i)
		{
			// Big-endian: the byte at the lowest address is the most
			// significant. For a 32-bit store at register offset 4 this walks
			// offsets 4, 5, 6 and 7, so TXL (offset 7) is written last and the
			// selected port completes the word, exactly as a byte-at-a-time
			// path would.
			const uint8_t byte = uint8_t(_value >> (8 * (byteCount - 1 - i)));

			for(int portIndex = 0; portIndex < g_hdi08PortCount; ++portIndex)
			{
				if(((selection.ports >> portIndex) & 1u) == 0)
					continue;

				m_ports[portIndex].write8(
					static_cast<mc68k::PeriphAddress>(selection.portOffset + i), byte);
			}
		}
	}
}
