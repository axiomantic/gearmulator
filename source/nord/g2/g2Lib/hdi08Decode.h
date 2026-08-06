// Task BRD-15. The CS1 decode.
//
// Plan section 13.3, BRD-15. Design section 10.3.
// Logbook: AGENTS.md section 3.1.
//
// WHAT THIS FILE IS. CS1 carries the HDI08 array. Address lines A3 to A10 are
// eight ACTIVE-LOW, ONE-COLD DSP chip selects: a select is driven low when its
// own address line is low. 0x7F8 drives every line high and selects none, and
// an offset of zero drives every line low and selects every populated port at
// once, which is the BROADCAST the firmware uses.
//
// THIS IS A DECODE AND NOT A LOOKUP TABLE, AND THAT IS THE WHOLE POINT.
// AGENTS.md section 3.1 records a 9-entry base table at 0x30116970 that is
// ZERO-FILLED in the firmware image and is built at boot by
// set_hdi08_bases(expanded) at 0x300391E8. The emulator must not hardcode that
// table. It decodes whatever address the firmware produces, which is also the
// only way the broadcast case can be expressed: a table of nine entries cannot
// answer an address that drives two lines low, and this decode can.
//
// WHICH PORTS EXIST IS CONFIGURATION. AGENTS.md sections 2.3 and 3.1: the base
// machine uses A3 to A6 and the expansion board adds A7 to A10. The populated
// set is therefore a constructor argument, and an address that selects an
// absent port selects nothing.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>

namespace g2
{
	// Eight selects on A3 to A10. AGENTS.md section 3.1.
	constexpr int g_hdi08PortCount = 8;
	constexpr int g_hdi08FirstAddressLine = 3;

	// The address bits the selects occupy. AGENTS.md section 3.1 records that
	// 0x7F8 "selects none", which is this mask with every line high.
	constexpr uint32_t g_hdi08SelectMask = 0x7f8u;

	// Address bits 2:0 carry the register offset inside the selected port.
	// The DSP56300 host port is eight bytes wide.
	constexpr uint32_t g_hdi08PortOffsetMask = 0x7u;

	// Every port of the base machine, which uses A3 to A6.
	constexpr uint8_t g_hdi08BasePorts = 0x0fu;
	// Every port of the expanded machine, which adds A7 to A10.
	constexpr uint8_t g_hdi08ExpandedPorts = 0xffu;

	struct Hdi08Selection
	{
		// Bit i is set when port i is selected. Zero means the address
		// selects no port, which 0x7F8 does by design.
		uint8_t ports = 0;

		// The register offset inside every selected port.
		uint32_t portOffset = 0;

		bool operator==(const Hdi08Selection& _other) const
		{
			return ports == _other.ports && portOffset == _other.portOffset;
		}
	};

	class Hdi08Decode
	{
	public:
		// _populatedPorts is a bit for every port the machine carries.
		explicit Hdi08Decode(uint8_t _populatedPorts);

		// _offset is relative to the base of the CS1 window, which the BRD-1
		// decode produced. This class carries no chip-select base.
		Hdi08Selection decode(uint32_t _offset) const;

		uint8_t populatedPorts() const { return m_populatedPorts; }

	private:
		uint8_t m_populatedPorts;
	};
}
