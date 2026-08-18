// Task BRD-16. The HDI08 host-port adapter over `mc68k::Hdi08`.
//
// Plan section 13.3, BRD-16. Design sections 10.1, 10.2.
// Logbook: AGENTS.md section 3.1.
//
// WHAT THIS FILE IS. `mc68k::Hdi08` models the register-level host port: ICR
// at +0, CVR at +1, ISR at +2, IVR at +3, and the TXH/TXM/TXL transmit bytes
// at +5/+6/+7, and it assembles the three bytes into one 24-bit word when TXL
// is written (hdi08.cpp:221-245). It knows nothing above that: no port select,
// no broadcast, no download protocol. This class holds EIGHT of them BY VALUE
// and routes a CS1 access to the port, or the ports, that BRD-15's decode
// selects, including the broadcast.
//
// THIS IS AN ADAPTER AND NOT A SHIM. The registers and the word assembly
// already exist in `mc68k::Hdi08`; this class only owns the eight instances,
// owns the decode, and rides the board's BusTarget interface.
//
// THE G2 HAS EIGHT HDI08 PORTS, NOT TWO. The Nord Lead 2X precedent,
// `mc68k::Hdi08Periph<Base>`, cannot express this machine: its base is a
// COMPILE-TIME template argument and the G2's nine-entry base table is
// zero-filled in the image and built at boot by `set_hdi08_bases(expanded)`
// at 0x300391E8. So this class holds `mc68k::Hdi08` directly and instantiates
// no `Hdi08Periph`.
//
// THIS FILE CARRIES NO ADDRESS AND NO BASE TABLE. The adapter decodes
// whatever offset the board presents through `Hdi08Decode`, which carries no
// address either.

#pragma once

#include <array>

#include "hdi08Decode.h"
#include "memoryMap.h"

#include "mc68k/hdi08.h"

#include "dsp56kEmu/types.h"

/* `dsp56k::HDI08` IS FORWARD-DECLARED RATHER THAN INCLUDED. board.h includes
 * this header, so including dsp56kEmu/hdi08.h here would put the DSP-side
 * HDI08 into the include closure of every board consumer. The two free
 * functions at the foot of this file take it by reference and need no complete
 * type; a caller that acts on one includes that header itself. */
namespace dsp56k
{
	class HDI08;
}

namespace g2
{
	// The board-side face of the HDI08 array. It presents the BusTarget every
	// other CS1 device presents, so the MemoryMap attaches it where it attaches
	// the flash and the latches, and inside it routes byte writes to whichever
	// port BRD-15's decode selected.
	class Hdi08Adapter final : public BusTarget
	{
	public:
		// _decode carries the populated-port set for the machine being
		// modelled. It is copied in, so the adapter does not outlive it.
		explicit Hdi08Adapter(Hdi08Decode _decode);

		/* NEITHER COPYABLE NOR MOVABLE, AND THE COMPILER IS WHAT SAYS SO. Each
		 * port carries a callback holding a reference to its own element of
		 * m_ports, so a copy or a move would produce an adapter whose ports
		 * drive the registers of the ORIGINAL -- which compiles, runs, and is
		 * wrong in a way nothing reports. Deleting the four members turns that
		 * into a build error at the site that tried it. */
		Hdi08Adapter(const Hdi08Adapter&) = delete;
		Hdi08Adapter(Hdi08Adapter&&) = delete;
		Hdi08Adapter& operator=(const Hdi08Adapter&) = delete;
		Hdi08Adapter& operator=(Hdi08Adapter&&) = delete;

		// BusTarget. _offset is relative to the base of the CS1 window, so a
		// port and a register offset come out of BRD-15's decode and nothing
		// here knows an absolute address.
		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// The per-port `mc68k::Hdi08`, so a caller can install the callbacks
		// the DSP side and BRD-17 need. Bounds are not asserted: the caller
		// passes g_hdi08PortCount and the operator of this class is the board.
		mc68k::Hdi08& port(int _index);
		const mc68k::Hdi08& port(int _index) const;

		const Hdi08Decode& decode() const { return m_decode; }

	private:
		bool isLegalWidth(int _size) const;

		Hdi08Decode m_decode;
		std::array<mc68k::Hdi08, g_hdi08PortCount> m_ports;
	};

	/* THE TWO FUNCTIONS BELOW ACT ON THE DSP SIDE, NOT ON THE `mc68k::Hdi08`
	 * THE CLASS ABOVE HOLDS. A reader who conflates the two looks for a
	 * blocking wait in `mc68k` and finds none. `dsp56k::HDI08::writeRX` pushes
	 * host words into a ring whose push waits on a semaphore once the ring is
	 * full, and the G2 drives the MCU, the DSPs and the panel from ONE thread,
	 * so nothing is left to drain it and that wait never returns. Both bounds
	 * below exist to keep the push off it. */

	// The words one quantum may offer, derived from the ring capacity.
	uint32_t hdi08QuantumWordBudget(const dsp56k::HDI08& _dsp);

	// Moves at most the budget and at most the ring's free space, and returns
	// what it moved, which is the count the caller must re-offer next quantum.
	uint32_t hdi08MoveWordsForQuantum(dsp56k::HDI08& _dsp, const dsp56k::TWord* _words, uint32_t _count);
}
