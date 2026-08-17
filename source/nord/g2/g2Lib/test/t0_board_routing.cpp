// Task INT-1, the board composition assigned by plan section 24.6 row W3-115.
// Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 24.6 row W3-115, INT-1's block, section 7.2.2 group G-M3.
// Design sections 5.2.1, 6.4.
//
// WHAT THIS TEST IS. W3-115's acceptance is that "the flash, panel, CS5
// latches, HDI08 adapter, memory map, SIM and UART0 [are] reachable from the
// `Board`'s bus callbacks". This test is that acceptance, and nothing else in
// INT-1 is in its scope: it reads no firmware, boots nothing, and asserts
// nothing about the banner.
//
// WHY EVERY CASE ASSERTS A VALUE AND NOT A STATUS ALONE. The defect this test
// exists to catch is a `Board` whose callbacks answer `0u` with a bus-OK
// status at EVERY address. A test that read an address and checked only that
// the access completed would pass against exactly that defect, because the
// broken board completes every access. So each unit is made to answer a value
// that NO OTHER UNIT in the composition produces, and each case asserts WHICH
// unit answered rather than that something did:
//
//     CS0    the flash boot image, first byte 0xA0 and last byte 0xB0
//     CS1    the HDI08 array, which reports the port a write selected
//     CS2    the flash main image, first byte 0xA2 and last byte 0xB2
//     CS4    the panel, which stores a written byte and reads it back
//     CS5    the latches, whose byte 0 carries the strapped panel identifier
//     MBAR   the SIM, which stores and returns a chip-select register
//     MBAR+  UART0, whose UIVR reads 0x0F at reset and which REJECTS a
//            16-bit access that the SIM would have accepted
//
// The flash images differ between CS0 and CS2 on purpose: one `Flash` object
// answers two windows, and a router that sent both windows to the same image
// would pass a test that used the same bytes for each.
//
// THE NEGATIVE HALF IS NOT OPTIONAL AND IT IS HALF THE CASES. A router that
// sent every address to every unit would satisfy every positive case above.
// So every window is probed one byte BELOW its base and one byte ABOVE its
// last byte, and the fixture leaves a gap between every pair of windows so
// that those two addresses fall in no window at all. An address in no window
// must report MCF5307_BUS_UNMAPPED, which is the answer memoryMap.cpp already
// gives and which this test does not re-decide.
//
// THE ONE ADDRESS WHOSE OWNER IS NOT OBSERVABLE, STATED RATHER THAN HIDDEN.
// MBAR+0x1D0 is UIPCR. sim.cpp's DIVERGENCE note assigns that one offset to
// the SIM because the firmware reads it as a model strap, and gives BRD-4
// "every other UART offset". Both models answer 0x0E there and both restrict
// it to byte access, so NO assertion can tell which one replied. The case
// below asserts the value the two agree on and says so; it is not evidence
// about the split, and the split is proved at MBAR+0x1F0 instead, where the
// two models genuinely differ.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG, so an assert() would be removed and a report
// built on one could never fire.

#include "board.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

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

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << hex32(_expected)
		          << ">, got <" << hex32(_actual) << ">" << std::endl;
		++g_failures;
	}

	// -----------------------------------------------------------------------
	// THE FIXTURE LAYOUT, AND WHICH NUMBERS HAVE AN AUTHORITY BEHIND THEM.
	//
	// FOUR BASES ARE RECORDED and this fixture uses the shipped constants
	// rather than repeating their values: g_cs1Base, g_cs3Base, g_cs5Base and
	// g_sdramBase in memoryMap.h, from AGENTS.md section 2.2.
	//
	// CS0's, CS2's and CS4's BASES ARE RECORDED BY NO AUTHORITY, and NO
	// AUTHORITY RECORDS A SIZE FOR ANY WINDOW. memoryMap.h states both facts
	// and plan section 1.3 rule 1 makes them configuration that a caller
	// supplies. The values below are therefore THIS FIXTURE'S, chosen only so
	// that the windows are disjoint and separated by gaps, and they are NOT a
	// claim about the machine. The one exception is the MBAR window's SIZE,
	// which sim.h records as g_simSpaceSize from MCF5307 UM Appendix B.
	//
	// CS1's size is the 0x800 that BRD-15's own decode test uses, because the
	// HDI08 decode reads address lines inside that span.
	//
	// THE GAPS ARE LOAD-BEARING. Every window is followed by unmapped space,
	// so "one byte past the end" is an address that no window claims and the
	// negative cases below have somewhere to land.
	constexpr uint32_t g_cs0Base = 0x10000000u;
	constexpr uint32_t g_cs0Size = 0x100u;

	constexpr uint32_t g_cs1Size = 0x800u;

	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x100u;

	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x100u;

	constexpr uint32_t g_cs5Size = 0x100u;

	constexpr uint32_t g_mbarBase = 0x16000000u;

	// The distinctive bytes. Each one identifies exactly one unit, so a value
	// arriving from the wrong unit is visible rather than merely wrong.
	constexpr uint8_t g_cs0First = 0xA0u;
	constexpr uint8_t g_cs0Last  = 0xB0u;
	constexpr uint8_t g_cs2First = 0xA2u;
	constexpr uint8_t g_cs2Last  = 0xB2u;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;

		config.memory.cs0   = {g_cs0Base,        g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,    g_cs1Size};
		config.memory.cs2   = {g_cs2Base,        g_cs2Size};
		config.memory.cs4   = {g_cs4Base,        g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,    g_cs5Size};
		config.memory.mbar  = {g_mbarBase,       g2::g_simSpaceSize};

		// CS3 is the ISP1181 and the SDRAM is main memory. NEITHER is one of
		// the seven units W3-115 names, so this fixture leaves both windows
		// absent. An absent window has size zero and answers nowhere, which is
		// what makes the CS3 and SDRAM cases below assert UNMAPPED.

		config.hdi08 = g2::Hdi08Decode(g2::g_hdi08ExpandedPorts);

		return config;
	}

	// Fill an image whose first and last bytes are the distinctive ones and
	// whose interior is a third value, so that a read of the wrong offset
	// inside the right window is still visible.
	std::vector<uint8_t> makeImage(const uint32_t _size, const uint8_t _first, const uint8_t _last)
	{
		std::vector<uint8_t> image(_size, 0x5Au);
		image.front() = _first;
		image.back()  = _last;
		return image;
	}

	// -----------------------------------------------------------------------
	// EVERY ACCESS IN THIS FILE GOES THROUGH THE INSTALLED CALLBACK, AND THAT
	// IS A CORRECTION RATHER THAN A PREFERENCE.
	//
	// An earlier revision of this test called Board::busRead directly. It
	// passed all 65 cases -- and it ALSO passed with the old
	// "return 0u and report MCF5307_BUS_OK" body restored into Board::onRead,
	// which is the exact M3 defect this test exists to catch. The object hash
	// moved, so the mutation reached the build; the test simply never drove the
	// path the core drives.
	//
	// Board::onRead and Board::onWrite are the function pointers handed to
	// mcf5307_create. Driving THEM means a body that stops forwarding turns
	// this test red, which is the property the earlier revision lacked.
	uint32_t busRead(g2::Board& _board, const uint32_t _address, const int _size,
	                 mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, _size, &_status);
	}

	void busWrite(g2::Board& _board, const uint32_t _address, const int _size,
	              const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		g2::Board::onWrite(&_board, _address, _size, _value, &_status);
	}

	// A read whose status is asserted UNMAPPED. The value is not asserted:
	// memoryMap.cpp returns zero on an unmapped access and the STATUS is the
	// fact under test, because a zero value is exactly what the broken board
	// this test exists to catch also returns.
	void checkUnmapped(g2::Board& _board, const uint32_t _address, const std::string& _what)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		(void)busRead(_board, _address, 8, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_UNMAPPED), _what);
	}

	// A byte read asserted to complete AND to carry the value only one unit
	// produces. Both halves are required: the status alone would pass against
	// a board that answers OK everywhere, and the value alone would pass
	// against a unit that faulted and returned the right number by accident.
	void checkByte(g2::Board& _board, const uint32_t _address, const uint32_t _expected,
	               const std::string& _what)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		const uint32_t value = busRead(_board, _address, 8, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK), _what + " completes");
		checkEqual(value, _expected, _what);
	}
}

int main()
{
	// ------------------------------------------------------------------
	// The composed board. Every unit below is reached ONLY through the
	// busRead / busWrite helpers above, which call the function pointers
	// mcf5307_create was given: no test case touches a unit's own read() or
	// write() directly, because doing so would test the unit and not the
	// routing, and none calls Board::busRead directly, because doing so would
	// skip the forwarding that the mutation record above shows was untested.
	g2::Board board(makeConfig());

	board.flash().loadCs0(makeImage(g_cs0Size, g_cs0First, g_cs0Last));
	board.flash().loadCs2(makeImage(g_cs2Size, g_cs2First, g_cs2Last));

	mcf5307_bus_status status = MCF5307_BUS_OK;

	// ==================================================================
	// CS0 -- the flash boot image.
	// ==================================================================
	{
		checkByte(board, g_cs0Base, g_cs0First,
		          "CS0 first byte reaches the flash boot image");
		checkByte(board, g_cs0Base + g_cs0Size - 1u, g_cs0Last,
		          "CS0 last byte reaches the flash boot image");

		checkUnmapped(board, g_cs0Base - 1u,
		              "one byte below CS0 reaches no unit");
		checkUnmapped(board, g_cs0Base + g_cs0Size,
		              "one byte above CS0 reaches no unit");

		// The flash is read-only and the model rejects a write. The value
		// must be unchanged afterwards, which proves the write reached the
		// flash model's rejection rather than a unit that accepted it.
		busWrite(board, g_cs0Base, 8, 0xFFu, status);
		checkByte(board, g_cs0Base, g_cs0First,
		          "a write to CS0 does not change the read-only flash");
	}

	// ==================================================================
	// CS2 -- the flash main image. The SAME Flash object as CS0, reached
	// through a DIFFERENT window, answering DIFFERENT bytes.
	// ==================================================================
	{
		checkByte(board, g_cs2Base, g_cs2First,
		          "CS2 first byte reaches the flash main image");
		checkByte(board, g_cs2Base + g_cs2Size - 1u, g_cs2Last,
		          "CS2 last byte reaches the flash main image");

		checkUnmapped(board, g_cs2Base - 1u,
		              "one byte below CS2 reaches no unit");
		checkUnmapped(board, g_cs2Base + g_cs2Size,
		              "one byte above CS2 reaches no unit");

		// The two windows must not be answering from the same image. This
		// is the case that a router mapping both windows to one image, or
		// mapping CS2's offset through CS0's base, would fail.
		mcf5307_bus_status cs0Status = MCF5307_BUS_OK;
		mcf5307_bus_status cs2Status = MCF5307_BUS_OK;
		const uint32_t cs0Value = busRead(board, g_cs0Base, 8, cs0Status);
		const uint32_t cs2Value = busRead(board, g_cs2Base, 8, cs2Status);
		check(cs0Value != cs2Value,
		      "CS0 and CS2 answer from different flash images");
	}

	// ==================================================================
	// CS5 -- the latches. Byte 0 carries the strapped panel identifier,
	// which is a value no other unit in this composition produces.
	// ==================================================================
	{
		checkByte(board, g2::g_cs5Base + g2::g_panelIdentifierOffset,
		          g2::g_panelIdentifierG2X,
		          "CS5 byte 0 reaches the latches and reads the panel identifier");

		checkUnmapped(board, g2::g_cs5Base - 1u,
		              "one byte below CS5 reaches no unit");
		checkUnmapped(board, g2::g_cs5Base + g_cs5Size,
		              "one byte above CS5 reaches no unit");

		// A latch byte that is not the strap stores what is written, so a
		// write and a read together prove the latches answered and that the
		// offset the router produced was window-relative.
		busWrite(board, g2::g_cs5Base + 1u, 8, 0xA5u, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "a write to a CS5 latch completes");
		checkByte(board, g2::g_cs5Base + 1u, 0xA5u,
		          "a CS5 latch returns the byte written through the board");

		// The last byte of the window is reachable, which pins the window's
		// upper boundary from the inside as well as from the outside.
		busWrite(board, g2::g_cs5Base + g_cs5Size - 1u, 8, 0x5Au, status);
		checkByte(board, g2::g_cs5Base + g_cs5Size - 1u, 0x5Au,
		          "CS5 last byte reaches the latches");
	}

	// ==================================================================
	// CS4 -- the panel. It reads zero everywhere until something is
	// written, so a non-zero read-back is what identifies it.
	// ==================================================================
	{
		busWrite(board, g_cs4Base, 8, 0xA4u, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "a write to the CS4 panel completes");
		checkByte(board, g_cs4Base, 0xA4u,
		          "CS4 first byte reaches the panel and reads back");

		busWrite(board, g_cs4Base + g_cs4Size - 1u, 8, 0xB4u, status);
		checkByte(board, g_cs4Base + g_cs4Size - 1u, 0xB4u,
		          "CS4 last byte reaches the panel and reads back");

		checkUnmapped(board, g_cs4Base - 1u,
		              "one byte below CS4 reaches no unit");
		checkUnmapped(board, g_cs4Base + g_cs4Size,
		              "one byte above CS4 reaches no unit");

		// The panel and the latches must be DIFFERENT objects. The panel's
		// byte 0 now holds 0xA4 while the latches' byte 0 holds the strap,
		// so a router that pointed both windows at one unit fails here.
		checkByte(board, g2::g_cs5Base + g2::g_panelIdentifierOffset,
		          g2::g_panelIdentifierG2X,
		          "writing the panel leaves the CS5 latch identifier untouched");
	}

	// ==================================================================
	// CS1 -- the HDI08 array. It is identified by WHICH PORT a write
	// selects, which no other unit in the composition can report.
	// ==================================================================
	{
		struct Capture
		{
			uint32_t word = 0;
			int count = 0;
		};

		// The captures live in THIS scope and the callbacks are installed on
		// the board's own adapter, so nothing here points at a dead object.
		std::array<Capture, g2::g_hdi08PortCount> captures;
		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			board.hdi08().port(p).setWriteTxCallback(
				[&captures, p](const uint32_t _word)
				{
					captures[p].word = _word;
					++captures[p].count;
				});
		}

		// 0x110007F0 is port 0's block and +4 is the longword register that
		// pushes a word. BRD-15's own decode test pins both. The address is
		// ABSOLUTE here: the board is what must turn it into the offset the
		// adapter expects, and that conversion is the thing under test.
		const uint32_t port0Word = 0x00BEAD00u;
		busWrite(board, 0x110007F4u, 32, port0Word, status);

		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "a CS1 longword write completes");
		checkEqual(uint32_t(captures[0].count), 1u,
		           "the CS1 write reached HDI08 port 0 exactly once");
		checkEqual(captures[0].word, port0Word,
		           "the CS1 write delivered its word to HDI08 port 0");

		// No other port saw it. This is what proves the board passed a
		// WINDOW-RELATIVE offset: an absolute address, or an offset computed
		// against the wrong base, decodes to a different port or to none.
		for(int p = 1; p < g2::g_hdi08PortCount; ++p)
		{
			checkEqual(uint32_t(captures[p].count), 0u,
			           "HDI08 port " + std::to_string(p)
			           + " saw nothing from the port 0 address");
		}

		checkUnmapped(board, g2::g_cs1Base - 1u,
		              "one byte below CS1 reaches no unit");
		checkUnmapped(board, g2::g_cs1Base + g_cs1Size,
		              "one byte above CS1 reaches no unit");

		// The first byte of CS1 is inside the window and must complete: the
		// HDI08 decode selects no port there and answers zero, and the
		// STATUS is what separates that from an address the router dropped.
		mcf5307_bus_status cs1Status = MCF5307_BUS_OK;
		(void)busRead(board, g2::g_cs1Base, 8, cs1Status);
		checkEqual(uint32_t(cs1Status), uint32_t(MCF5307_BUS_OK),
		           "CS1 first byte is inside the window and completes");
	}

	// ==================================================================
	// MBAR -- the SIM. A chip-select register stores and returns a value,
	// which identifies the SIM.
	// ==================================================================
	{
		// CSAR0 at MBAR+0x080, sixteen bits, read/write with no strap bits.
		const uint32_t csar0 = g_mbarBase + 0x080u;

		busWrite(board, csar0, 16, 0xA6A6u, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "a 16-bit write to the SIM CSAR0 completes");

		mcf5307_bus_status simStatus = MCF5307_BUS_OK;
		const uint32_t simValue = busRead(board, csar0, 16, simStatus);
		checkEqual(uint32_t(simStatus), uint32_t(MCF5307_BUS_OK),
		           "a 16-bit read of the SIM CSAR0 completes");
		checkEqual(simValue, 0xA6A6u,
		           "MBAR+0x080 reaches the SIM and returns the written CSAR0");

		checkUnmapped(board, g_mbarBase - 1u,
		              "one byte below MBAR reaches no unit");
		checkUnmapped(board, g_mbarBase + g2::g_simSpaceSize,
		              "one byte above MBAR reaches no unit");
	}

	// ==================================================================
	// MBAR+0x1C0 -- UART0, inside the MBAR window. This is the case that
	// proves the MBAR window is SPLIT between two units rather than
	// answered by one.
	// ==================================================================
	{
		// UIVR sits at UART0's base + 0x30 and resets to 0x0F. The SIM
		// models no register at that offset and would return 0x00, so the
		// VALUE alone distinguishes the two units.
		const uint32_t uivr = g_mbarBase + g2::Uart0::gUart0Base + 0x30u;

		checkByte(board, uivr, 0x0Fu,
		          "MBAR+0x1F0 reaches UART0 and reads the UIVR reset value");

		// UART0 restricts every one of its registers to byte access (UM
		// 14.3.7) and the SIM does NOT restrict this offset, because the SIM
		// models no register there at all. So a 16-bit access that is
		// REJECTED is a behaviour only UART0 produces, and it is independent
		// evidence of the same routing the value above asserts.
		mcf5307_bus_status wideStatus = MCF5307_BUS_OK;
		(void)busRead(board, uivr, 16, wideStatus);
		checkEqual(uint32_t(wideStatus), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
		           "a 16-bit read of a UART0 register is rejected by UART0");

		// UIVR is read/write, so the vector the firmware programs round
		// trips. 0x42 is the observed UART0 vector.
		busWrite(board, uivr, 8, g2::Uart0::gUart0Vector, status);
		checkByte(board, uivr, g2::Uart0::gUart0Vector,
		          "UART0's UIVR returns the vector written through the board");

		// The SIM register written earlier is UNCHANGED by all of that,
		// which proves the two MBAR units are separate objects and that the
		// split did not send the UART traffic to the SIM.
		mcf5307_bus_status simStatus = MCF5307_BUS_OK;
		const uint32_t simValue = busRead(board, g_mbarBase + 0x080u, 16, simStatus);
		checkEqual(simValue, 0xA6A6u,
		           "the UART0 writes left the SIM's CSAR0 untouched");

		// THE ONE ADDRESS WHOSE OWNER IS NOT OBSERVABLE. MBAR+0x1D0 is
		// UIPCR. sim.cpp's DIVERGENCE note gives that single offset to the
		// SIM and every other UART offset to BRD-4. Both models answer 0x0E
		// and both restrict it to byte access, so this case asserts the
		// value the two AGREE on. It is a consistency check and it is NOT
		// evidence about which unit replied; the split is proved above.
		checkByte(board, g_mbarBase + 0x1D0u, 0x0Eu,
		          "MBAR+0x1D0 answers the UIPCR strap value both models agree on");
	}

	// ==================================================================
	// The regions this composition deliberately leaves empty.
	// ==================================================================
	{
		// CS3 is the ISP1181 and the SDRAM is main memory. Neither is one of
		// the seven units W3-115 names, and this fixture attaches nothing to
		// either, so both must report UNMAPPED rather than answering zero
		// with a bus-OK status.
		checkUnmapped(board, g2::g_cs3Base,
		              "CS3 has no unit attached and reports unmapped");
		checkUnmapped(board, g2::g_sdramBase,
		              "the SDRAM has no unit attached and reports unmapped");

		// An address far from every window.
		checkUnmapped(board, 0x7F000000u,
		              "an address in no window at all reports unmapped");

		// Address zero is the one an unconfigured board would answer first,
		// and it must be unmapped rather than accepted.
		checkUnmapped(board, 0x00000000u,
		              "address zero reaches no unit");
	}

	// ==================================================================
	// A default-constructed Board answers nowhere. BRD-21's surface task
	// ships a Board with no configuration, and an unconfigured board must
	// report unmapped rather than accept every access -- which is the
	// exact defect this test exists to catch.
	// ==================================================================
	{
		g2::Board bare;
		checkUnmapped(bare, g_cs0Base,
		              "a default-constructed board answers no address");
		checkUnmapped(bare, g2::g_cs5Base,
		              "a default-constructed board answers no CS5 address");
	}

	if(g_failures)
	{
		std::cout << "t0_board_routing: " << g_failures << " of " << g_cases
		          << " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_board_routing: " << g_cases << " of " << g_cases
	          << " cases passed" << std::endl;
	return 0;
}
