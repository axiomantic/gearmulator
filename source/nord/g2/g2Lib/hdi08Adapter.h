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
// address either. The nine addresses of AGENTS.md section 3.1 appear nowhere
// in g2Lib, in any spelling.

#pragma once

#include <array>

#include "hdi08Decode.h"
#include "memoryMap.h"

#include "mc68k/hdi08.h"

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
}
