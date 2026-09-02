// The HDI08 host-port adapter over `mc68k::Hdi08`.
//
// The word completes when TXL is written: `mc68k::Hdi08` assembles it from
// TXH, TXM and TXL and fires its writeTx callback only when TXL lands. So a
// byte-at-a-time firmware path and a 32-bit store at register offset +4 both
// reach the same word, and this file routes byte writes to the selected
// port(s) without caring which path produced them.
//
// A 32-bit store at register offset 4 makes four byte cycles at offsets 4, 5,
// 6 and 7, of which offset 4 is unused. The bus-facing width of a ColdFire
// access is 8, 16 or 32, and this file decomposes 16- and 32-bit writes into
// big-endian byte cycles and hands each byte to the selected port's `write8`.
// Decomposing here and not in the MemoryMap is what lets a byte-at-a-time
// firmware path and a longword store share one code path and produce one
// word.

#include "hdi08Adapter.h"

#include "dsp56kEmu/hdi08.h"

#include <cassert>

namespace g2
{
	Hdi08Adapter::Hdi08Adapter(const Hdi08Decode _decode)
		: m_decode(_decode)
	{
		/* The port answers its own init request. Clearing INIT is the host
		 * interface's own hardware reporting that the initialisation it was
		 * asked for has finished; it does not depend on what is wired to the far
		 * side of the port, and a port with nothing behind it must still answer
		 * or a host polling the bit never leaves the loop.
		 * `mc68k::Hdi08::write8` stores the byte and clears nothing, so the
		 * callback is the seam that models the hardware.
		 *
		 * The callback holds a reference into m_ports, which is why this class
		 * deletes its copy and move. See hdi08Adapter.h. */
		for(mc68k::Hdi08& port : m_ports)
		{
			port.setInitHdi08Callback([&port]
			{
				port.icr(uint8_t(port.icr() & ~uint8_t(mc68k::Hdi08::Init)));
				port.isr(uint8_t(port.isr() | mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy));
			});
		}
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

	// -----------------------------------------------------------------------
	// Bounded, non-blocking control on the HDI08 transfer path.
	//
	// Two different HDI08 models live in this build tree and the two functions
	// below are about the other one. Everything above is `mc68k::Hdi08`, the
	// HOST side, which is callback-driven and has no blocking wait. These two
	// are about `dsp56k::HDI08`, the DSP side, in `dsp56300`.
	//
	// `dsp56k::HDI08::writeRX` pushes host words into
	// `RingBuffer<TWord, 8192, true>`, and `push_back` on a FULL ring waits on a
	// condition variable until a reader drains it. The G2 runs the MCU, the
	// eight DSPs and the panel from ONE thread, so there is no reader to drain
	// it and the wait never returns. That is a deadlock, not a slow path.
	//
	// The blocking primitive is the semaphore, not `waitNotFull()`. On this ring
	// `Lock` is true, and `RingBuffer::waitNotFull()` opens with
	// `if constexpr(Lock) return;` -- it is INERT here. The wait that blocks is
	// `m_writeSem.wait()` inside `push_back`, which is `SpscSemaphoreWithCount`
	// and is condition-variable backed. Bounding the call is therefore the only
	// control available from outside the class: there is no non-blocking push to
	// call instead.
	//
	// The count is returned rather than left on an object: a quantum's transfer
	// is one call, the caller needs the count to know what it must re-offer next
	// quantum, and a return value cannot be read stale.

	uint32_t hdi08QuantumWordBudget(const dsp56k::HDI08& _dsp)
	{
		// Derived from the ring, never written as a literal: a number here would
		// be a second definition of a buffer size `dsp56300` owns. Half the
		// capacity is the headroom rule -- the per-quantum bound must sit BELOW
		// the capacity, so that a quantum which moves its whole budget still
		// leaves the DSP room to be pushed to again before it has drained.
		return static_cast<uint32_t>(_dsp.rxData().capacity() / 2u);
	}

	uint32_t hdi08MoveWordsForQuantum(dsp56k::HDI08& _dsp, const dsp56k::TWord* _words,
		const uint32_t _count)
	{
		uint32_t moved = _count;

		const uint32_t budget = hdi08QuantumWordBudget(_dsp);
		if(moved > budget)
			moved = budget;

		// The budget alone does not make this non-blocking: a ring with less free
		// space than the budget would still block on the push that fills it. The
		// free-space clamp is what closes the deadlock; the budget is what stops
		// one quantum from monopolising the ring.
		const uint32_t freeSlots = static_cast<uint32_t>(_dsp.rxData().remaining());
		if(moved > freeSlots)
			moved = freeSlots;

		// A debug assertion only: a release build removes this line, so the bound
		// is checked with a runtime comparison in the test as well.
		assert(moved <= budget && moved <= freeSlots && "the bounded transfer would block");

		if(moved)
			_dsp.writeRX(_words, moved);

		return moved;
	}
}
