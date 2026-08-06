// Task BRD-12. The panel.
//
// Plan section 13.3, BRD-12. Design sections 8.1, 8.2, 8.3.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1.
//
// THE PANEL DISPLAY BUFFER SITS ON CS4 AND NO AUTHORITY RECORDS CS4'S BASE.
// AGENTS.md open question 21, widened on 2026-08-05, carries CS4 with CS0 and
// CS2, and SPK-13 reads the base from CSAR4. Plan section 1.3 rule 1 therefore
// applies: the base AND the size of the display window are configuration, and
// this file carries no number for either.
//
// THE PANEL IS A REAL LAYER, NOT DECORATION. A stub that returns zero gives
// panel identifier bits 0b00, which boots and presents a plain G2; a stub that
// returns any other value can stop the machine with no message. The identifier
// lives in latches.h, on CS5, which is the one address in this task with a
// recorded source.
//
// KEY, KNOB AND ENCODER INPUT IS LATER WORK and is not in this milestone,
// because the editor and the host provide the same control surface. This model
// reports a QUIESCENT panel: no key down, no encoder moving, no button
// pressed. It answers every poll at every legal width, so no boot loop spins
// for ever.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	class Panel final : public BusTarget
	{
	public:
		explicit Panel(uint32_t _displaySize);

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

	private:
		// The display buffer. It starts at zero, which is the quiescent
		// report, and it keeps whatever is written into it. INT-1's t1_boot is
		// what reads this buffer for the OS banner; a T0 check asserts only
		// that the buffer returns what the test wrote.
		std::vector<uint8_t> m_display;
	};
}
