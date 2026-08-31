// The HDI08 host-port adapter over `mc68k::Hdi08`.
//
// `mc68k::Hdi08` models the register-level host port: ICR at +0, CVR at +1,
// ISR at +2, IVR at +3, and the TXH/TXM/TXL transmit bytes at +5/+6/+7, and it
// assembles the three bytes into one 24-bit word when TXL is written. It knows
// nothing above that: no port select, no broadcast, no download protocol. This
// class holds eight of them by value and routes a CS1 access to the port, or
// the ports, the decode selects, including the broadcast.
//
// The G2 has eight HDI08 ports, not two. The Nord Lead 2X precedent,
// `mc68k::Hdi08Periph<Base>`, cannot express this machine: its base is a
// compile-time template argument, and the G2's nine-entry base table is
// zero-filled in the image and built at boot by `set_hdi08_bases(expanded)` at
// 0x300391E8. So this class holds `mc68k::Hdi08` directly and instantiates no
// `Hdi08Periph`.
//
// This file carries no address and no base table. The adapter decodes whatever
// offset the board presents through `Hdi08Decode`, which carries no address
// either.

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
	// port the decode selected.
	class Hdi08Adapter final : public BusTarget
	{
	public:
		// _decode carries the populated-port set for the machine being
		// modelled. It is copied in, so the adapter does not outlive it.
		explicit Hdi08Adapter(Hdi08Decode _decode);

		// BusTarget. _offset is relative to the base of the CS1 window, so a
		// port and a register offset come out of the decode and nothing here
		// knows an absolute address.
		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// The per-port `mc68k::Hdi08`, so a caller can install the callbacks
		// the DSP side needs. Bounds are not asserted: the caller passes
		// g_hdi08PortCount and the operator of this class is the board.
		mc68k::Hdi08& port(int _index);
		const mc68k::Hdi08& port(int _index) const;

		const Hdi08Decode& decode() const { return m_decode; }

	private:
		bool isLegalWidth(int _size) const;

		Hdi08Decode m_decode;
		std::array<mc68k::Hdi08, g_hdi08PortCount> m_ports;
	};
}
