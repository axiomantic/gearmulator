// The CS1 decode. Tier T0: this test needs no firmware artifact.
//
// The addresses below are the values set_hdi08_bases(expanded) at 0x300391E8
// writes into the 9-entry table at 0x30116970 at boot. They are written out
// here as the expected input of a decode, not as a table the emulator carries:
// hdi08Decode.h holds no address at all, and the exhaustive case below drives
// all 256 line patterns. The recorded entries are the per-DSP addresses and the
// broadcast.
//
// The recorded values are absolute addresses. This test turns each one into a
// CS1 offset by subtracting the base of the CS1 window the MemoryMap carries,
// so the offset the decode sees is produced by the decode and not by arithmetic
// written here.

#include "hdi08Decode.h"
#include "memoryMap.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	// The CS1 window. The base is recorded and this fixture chooses the size,
	// because no authority records it. A3 to A10 span
	// 0x7F8, so the window is at least 0x800 bytes.
	g2::MemoryMap makeMap()
	{
		g2::MemoryMapConfig config;
		config.cs1 = {g2::g_cs1Base, 0x800u};
		return g2::MemoryMap(config);
	}

	struct RecordedEntry
	{
		int index;
		uint32_t address;
		uint8_t expectedPorts;
		const char* note;
	};

	// The expanded 8-DSP column. Each entry drives exactly one of A3 to A10
	// low, and the table index is not the address line number: the ordering is
	// deliberate so that index 0 and index N-1 stay on the two physical chips
	// that touch the codec.
	const RecordedEntry g_expandedTable[] =
	{
		{0, 0x110007b8u, 0x08u, "index 0 drives A6 low"},
		{1, 0x110003f8u, 0x80u, "index 1 drives A10 low"},
		{2, 0x110005f8u, 0x40u, "index 2 drives A9 low"},
		{3, 0x110006f8u, 0x20u, "index 3 drives A8 low"},
		{4, 0x11000778u, 0x10u, "index 4 drives A7 low"},
		{5, 0x110007d8u, 0x04u, "index 5 drives A5 low"},
		{6, 0x110007e8u, 0x02u, "index 6 drives A4 low"},
		{7, 0x110007f0u, 0x01u, "index 7 drives A3 low"},
		{8, 0x11000000u, 0xffu, "index 8 is the broadcast and drives every populated select low"},
	};

	// The base 4-DSP column. The base machine uses A3 to A6 only.
	const RecordedEntry g_baseTable[] =
	{
		{0, 0x110007b8u, 0x08u, "index 0 drives A6 low"},
		{1, 0x110007d8u, 0x04u, "index 1 drives A5 low"},
		{2, 0x110007e8u, 0x02u, "index 2 drives A4 low"},
		{3, 0x110007f0u, 0x01u, "index 3 drives A3 low"},
		{4, 0x11000780u, 0x0fu, "index 4 is the broadcast and drives the four populated selects low"},
	};
}

int main()
{
	g2::MemoryMap map = makeMap();
	const uint32_t cs1Base = map.window(g2::Region::Cs1).base;

	// -----------------------------------------------------------------------
	// Case group 0. The window this test decodes into is CS1, at the recorded
	// base.
	checkEqual(map.decode(0x11000000u), g2::Region::Cs1, "0x11000000 decodes to CS1");
	checkEqual(cs1Base, uint32_t(0x11000000u), "the CS1 window starts at the recorded base");

	// -----------------------------------------------------------------------
	// Case group 1. The recorded entries of the expanded machine.
	//
	// The eight per-DSP entries each select exactly one port, and the ninth is
	// the broadcast and selects all eight.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);

		for(const RecordedEntry& e : g_expandedTable)
		{
			const g2::Hdi08Selection selection = decode.decode(e.address - cs1Base);

			checkEqual(uint32_t(selection.ports), uint32_t(e.expectedPorts),
				std::string("expanded entry ") + std::to_string(e.index) + " at "
					+ hex32(e.address) + " selects " + hex32(e.expectedPorts) + ": " + e.note);
			checkEqual(selection.portOffset, uint32_t(0),
				std::string("expanded entry ") + std::to_string(e.index) + " addresses register 0 of its port");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 2. 0x7F8 selects none: it is the one pattern that drives every
	// line high.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);
		const g2::Hdi08Selection selection = decode.decode(0x7f8u);

		checkEqual(uint32_t(selection.ports), uint32_t(0),
			"an offset of 0x7F8 drives every select high and selects no port");
	}

	// -----------------------------------------------------------------------
	// Case group 3. The recorded entries of the base machine.
	//
	// The same decode with four ports populated. The base broadcast at
	// 0x11000780 drives A3 to A6 low and A7 to A10 high, which is exactly the
	// four selects the base machine carries.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08BasePorts);

		for(const RecordedEntry& e : g_baseTable)
		{
			const g2::Hdi08Selection selection = decode.decode(e.address - cs1Base);

			checkEqual(uint32_t(selection.ports), uint32_t(e.expectedPorts),
				std::string("base entry ") + std::to_string(e.index) + " at "
					+ hex32(e.address) + " selects " + hex32(e.expectedPorts) + ": " + e.note);
		}
	}

	// -----------------------------------------------------------------------
	// Case group 4. An address of the expanded table selects nothing on a base
	// machine.
	//
	// The four expansion addresses drive A7 to A10, and a base machine carries
	// no port on any of them. This is what says the populated set is read.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08BasePorts);

		const uint32_t expansionOnly[] = {0x110003f8u, 0x110005f8u, 0x110006f8u, 0x11000778u};

		for(const uint32_t address : expansionOnly)
		{
			checkEqual(uint32_t(decode.decode(address - cs1Base).ports), uint32_t(0),
				std::string("a base machine selects no port at ") + hex32(address)
					+ ", which is an expansion-board address");
		}

		checkEqual(uint32_t(decode.populatedPorts()), uint32_t(0x0fu),
			"a base machine reports four populated ports");
	}

	// -----------------------------------------------------------------------
	// Case group 5. It is a decode and not a lookup table.
	//
	// Every address here drives two or more lines low at once and appears in no
	// recorded table, which a lookup table could not answer; the firmware's own
	// broadcast is the same shape.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);

		checkEqual(uint32_t(decode.decode(0x7d0u).ports), uint32_t(0x05u),
			"an offset of 0x7D0 drives A3 and A5 low and selects ports 0 and 2");
		checkEqual(uint32_t(decode.decode(0x738u).ports), uint32_t(0x18u),
			"an offset of 0x738 drives A6 and A7 low and selects ports 3 and 4");
		checkEqual(uint32_t(decode.decode(0x3f0u).ports), uint32_t(0x81u),
			"an offset of 0x3F0 drives A3 and A10 low and selects ports 0 and 7");
	}

	// -----------------------------------------------------------------------
	// Case group 6. The exhaustive case.
	//
	// All 256 patterns of A3 to A10, on both machines. The expected set is
	// computed from the one-cold, active-low rule and not from any table.
	{
		const g2::Hdi08Decode expanded(g2::g_hdi08ExpandedPorts);
		const g2::Hdi08Decode base(g2::g_hdi08BasePorts);

		bool everyExpandedPatternIsRight = true;
		bool everyBasePatternIsRight = true;

		for(uint32_t pattern = 0; pattern < 256u; ++pattern)
		{
			const uint32_t offset = pattern << g2::g_hdi08FirstAddressLine;
			const uint8_t low = uint8_t(~pattern);

			if(expanded.decode(offset).ports != uint8_t(low & g2::g_hdi08ExpandedPorts))
				everyExpandedPatternIsRight = false;
			if(base.decode(offset).ports != uint8_t(low & g2::g_hdi08BasePorts))
				everyBasePatternIsRight = false;
		}

		check(everyExpandedPatternIsRight,
			"all 256 line patterns select the active-low, one-cold set on an expanded machine");
		check(everyBasePatternIsRight,
			"all 256 line patterns select the active-low, one-cold set on a base machine");
	}

	// -----------------------------------------------------------------------
	// Case group 7. Address bits 2:0 are the register offset inside the port
	// and they do not reach the selects.
	//
	// The DSP56300 host port is eight bytes wide, so local offsets run 0 to 7.
	// Letting those bits change the selected set would send one longword store
	// to four different ports.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);

		for(uint32_t local = 0; local < 8u; ++local)
		{
			const g2::Hdi08Selection selection = decode.decode(0x7f0u + local);

			checkEqual(uint32_t(selection.ports), uint32_t(0x01u),
				std::string("offset 0x7F0 plus ") + std::to_string(local) + " still selects port 0 alone");
			checkEqual(selection.portOffset, local,
				std::string("offset 0x7F0 plus ") + std::to_string(local)
					+ " addresses register " + std::to_string(local) + " of that port");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 8. Address bits above A10 do not reach the selects either.
	{
		const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);

		checkEqual(uint32_t(decode.decode(0x7f0u).ports),
			uint32_t(decode.decode(0x7f0u | 0x1800u).ports),
			"a bit above A10 does not change the selected set");
	}

	if(g_failures)
	{
		std::cout << "t0_cs1_decode: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_cs1_decode: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
