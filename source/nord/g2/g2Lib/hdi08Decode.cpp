// The CS1 decode.
//
// The rule, in one line: A3 to A10 are eight active-low, one-cold selects, so
// port i is selected when address bit (3 + i) is low and the machine carries
// port i.
//
// Two consequences fall out of it and neither is a special case. An offset of
// 0x7F8 drives every line high and selects nothing. An offset of zero drives
// every line low and selects every populated port, which is the broadcast the
// firmware uses. The base machine's broadcast at 0x11000780 drives A3 to A6 low
// and A7 to A10 high, so it reaches exactly the four ports that machine carries.

#include "hdi08Decode.h"

namespace g2
{
	Hdi08Decode::Hdi08Decode(const uint8_t _populatedPorts)
		: m_populatedPorts(_populatedPorts)
	{
	}

	// A3 to A10 is an eight-bit field, and that is what bounds the decode.
	//
	// It is why no and with the mask appears in decode(): after the shift, every
	// address bit above A10 lands at bit 8 or higher and the eight-bit type
	// drops it. An and written beside the cast would be an operation that no
	// address could make a difference to.
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
