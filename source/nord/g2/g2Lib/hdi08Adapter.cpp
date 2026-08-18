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

#include "dsp56kEmu/hdi08.h"

#include <cassert>

namespace g2
{
	Hdi08Adapter::Hdi08Adapter(const Hdi08Decode _decode)
		: m_decode(_decode)
	{
		/* THE PORT ANSWERS ITS OWN INIT REQUEST, AND THAT IS WHY THIS SITS HERE
		 * RATHER THAN ON WHATEVER LATER OWNS THE DSP BEHIND THE PORT. Clearing
		 * INIT is the host interface's own hardware reporting that the
		 * initialisation it was asked for has finished; it does not depend on
		 * what is wired to the far side of the port, and a port with nothing
		 * behind it must still answer or a host polling the bit never leaves
		 * the loop. `mc68k::Hdi08::write8` stores the byte and clears nothing,
		 * so the callback is the seam that models the hardware.
		 *
		 * THE CALLBACK HOLDS A REFERENCE INTO m_ports, WHICH IS WHY THIS CLASS
		 * DELETES ITS COPY AND MOVE. See hdi08Adapter.h. */
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
	// Task BRD-17. Bounded, non-blocking control on the HDI08 transfer path.
	//
	// TWO DIFFERENT HDI08 MODELS LIVE IN THIS BUILD TREE AND THE TWO FUNCTIONS
	// BELOW ARE ABOUT THE OTHER ONE. Everything above is `mc68k::Hdi08`, the
	// HOST side, which is callback-driven and has no blocking wait. These two
	// are about `dsp56k::HDI08`, the DSP side, in `dsp56300`. A reader who
	// conflates the two will look for a blocking wait in `mc68k` and not find
	// one.
	//
	// WHY A BOUND AT ALL. `dsp56k::HDI08::writeRX` pushes host words into
	// `RingBuffer<TWord, 8192, true>`, and `push_back` on a FULL ring waits on a
	// condition variable until a reader drains it. The G2 runs the MCU, the
	// eight DSPs and the panel from ONE thread, so there is no reader to drain
	// it and the wait never returns. That is a deadlock, not a slow path.
	//
	// THE BLOCKING PRIMITIVE IS THE SEMAPHORE, NOT `waitNotFull()`. On this
	// ring `Lock` is true, and `RingBuffer::waitNotFull()` opens with
	// `if constexpr(Lock) return;` -- it is INERT here. The wait that actually
	// blocks is `m_writeSem.wait()` inside `push_back`, which is
	// `SpscSemaphoreWithCount` and is condition-variable backed. Bounding the
	// call is therefore the only control available from outside the class:
	// there is no non-blocking push to call instead.
	//
	// THE COUNT IS RETURNED RATHER THAN LEFT ON AN OBJECT. A quantum's transfer
	// is one call, the caller needs the count to know what it must re-offer next
	// quantum, and a return value cannot be read stale. BRD-17's `Files:` line
	// and the G-M3 file union both name this .cpp and neither names
	// hdi08Adapter.h, so these are free functions and not members.

	uint32_t hdi08QuantumWordBudget(const dsp56k::HDI08& _dsp)
	{
		// DERIVED FROM THE RING, NEVER WRITTEN AS A LITERAL. A number here would
		// be a second definition of a buffer size `dsp56300` owns, and it would
		// go stale silently the day that ring is resized. Half the capacity is
		// the headroom rule: BRD-17 requires the per-quantum bound to sit BELOW
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

		// THE BUDGET ALONE DOES NOT MAKE THIS NON-BLOCKING. A ring with less
		// free space than the budget would still block on the push that fills
		// it. The free-space clamp is the half that closes the deadlock; the
		// budget is the half that stops one quantum from monopolising the ring.
		const uint32_t freeSlots = static_cast<uint32_t>(_dsp.rxData().remaining());
		if(moved > freeSlots)
			moved = freeSlots;

		// A DEBUG ASSERTION AS WELL, AND IT IS NOT THE CHECK'S PREDICATE.
		// t0_hdi08_nonblocking asserts the same property with a runtime
		// comparison, because a release build removes this line and the bound
		// must be checked in a release build too.
		assert(moved <= budget && moved <= freeSlots && "the bounded transfer would block");

		if(moved)
			_dsp.writeRX(_words, moved);

		return moved;
	}
}
