// Task BRD-15. The CS1 decode.
//
// Plan section 13.3, BRD-15. Design section 10.3.
// Logbook: AGENTS.md section 3.1.
//
// THIS FILE CARRIES NO ADDRESS AND NO BASE TABLE. It computes the selected
// port set from the address the firmware produced, which is the only model
// that can answer the broadcast. AGENTS.md section 3.1 records that the
// 9-entry table at 0x30116970 is zero-filled in the image and is built at boot
// by set_hdi08_bases(expanded) at 0x300391E8, so a hardcoded table would model
// a value the firmware has not chosen yet.
//
// THE RULE, IN ONE LINE. A3 to A10 are eight active-low, one-cold selects, so
// port i is selected when address bit (3 + i) is LOW and the machine carries
// port i.
//
// The consequences fall out of that one line and neither is a special case.
// An offset of 0x7F8 drives every line high and selects nothing, which is what
// AGENTS.md section 3.1 records. An offset of zero drives every line low and
// selects every populated port, which is the broadcast the firmware uses. The
// base machine's broadcast at 0x11000780 drives A3 to A6 low and A7 to A10
// high, so it reaches exactly the four ports that machine carries.

#include "hdi08Decode.h"

namespace g2
{
	Hdi08Decode::Hdi08Decode(const uint8_t _populatedPorts)
		: m_populatedPorts(_populatedPorts)
	{
	}

	// A3 TO A10 IS AN EIGHT-BIT FIELD, AND THAT IS WHAT BOUNDS THE DECODE.
	//
	// This is a static_assert and not a run-time check on purpose. The default
	// build is Release and defines NDEBUG, so an assert() would be removed; a
	// static_assert survives every build and fails the COMPILE step. It ties
	// g_hdi08SelectMask to the eight-bit type below, so a change to the mask
	// cannot pass unnoticed.
	//
	// It also says why no AND with the mask appears in decode(): after the
	// shift, every address bit above A10 lands at bit 8 or higher and the
	// eight-bit type drops it. An AND written beside the cast would be an
	// operation that no address could make a difference to, and an operation
	// no input can reach is an operation no test can hold to account.
	static_assert((g_hdi08SelectMask >> g_hdi08FirstAddressLine) == 0xffu,
		"A3 to A10 must be a contiguous eight-bit field for the decode below to bound itself");

	Hdi08Selection Hdi08Decode::decode(const uint32_t _offset) const
	{
		const uint8_t lines = uint8_t(_offset >> g_hdi08FirstAddressLine);

		Hdi08Selection selection;
		// A line that is LOW selects its port. A port the machine does not
		// carry is never selected, whatever the address does.
		selection.ports = uint8_t(~lines) & m_populatedPorts;
		selection.portOffset = _offset & g_hdi08PortOffsetMask;
		return selection;
	}
}
