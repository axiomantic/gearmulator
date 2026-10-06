// The panel and the CS5 latches. Tier T0: this test needs no firmware
// artifact, and asserts only what the board does.
//
// One address here has a recorded source and the other does not. The CS5 latch
// sits at 0x15000000, and panel_id() at 0x3005BFFE drives it and takes bits
// 5:4. The panel display buffer sits on CS4, and no authority records CS4's
// base or its size, so this fixture supplies both.

#include "latches.h"
#include "memoryMap.h"
#include "model.h"
#include "panel.h"

#include <coldfire.h>

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

	// Every number in this block belongs to this fixture. The CS5 base is the
	// one exception and it arrives through the named constant memoryMap.h
	// carries for it.
	constexpr uint32_t g_latchWindowSize = 0x10u;
	constexpr uint32_t g_displayBase = 0x14000000u;
	constexpr uint32_t g_displaySize = 0x00000400u;

	// The second display base. Pairing it with the first is what proves the
	// CS4 base is read from the configuration rather than hardcoded.
	constexpr uint32_t g_otherDisplayBase = 0x18000000u;

	class Board
	{
	public:
		explicit Board(const uint32_t _displayBase = g_displayBase,
			const rg2::Model _model = rg2::Model::G2X)
			: m_panel(g_displaySize)
			, m_latches(g_latchWindowSize, _model)
		{
			m_panel.attachLatches(&m_latches);
			m_latches.attachPanel(&m_panel);

			rg2::MemoryMapConfig config;
			config.cs4 = {_displayBase, g_displaySize};
			config.cs5 = {rg2::g_cs5Base, g_latchWindowSize};

			m_map = new rg2::MemoryMap(config);
			m_map->attach(rg2::Region::Cs4, &m_panel);
			m_map->attach(rg2::Region::Cs5, &m_latches);
		}

		~Board() { delete m_map; }

		uint32_t read(const uint32_t _address, const int _size, cf_bus_status& _status)
		{
			_status = CF_BUS_OK;
			return rg2::memoryMapRead(m_map, _address, _size, &_status);
		}

		void write(const uint32_t _address, const int _size, const uint32_t _value, cf_bus_status& _status)
		{
			_status = CF_BUS_OK;
			rg2::memoryMapWrite(m_map, _address, _size, _value, &_status);
		}

		rg2::MemoryMap& map() { return *m_map; }
		rg2::Panel& panel() { return m_panel; }
		rg2::Latches& latches() { return m_latches; }

	private:
		rg2::Panel m_panel;
		rg2::Latches m_latches;
		rg2::MemoryMap* m_map = nullptr;
	};
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. The panel identifier latch presents a G2X.
	//
	// The map from the two bits to the model code: 0b00 is model code 0, a
	// plain G2; 0b11 is model code 1, the G2X; 0b10 is model code 2, the Rack
	// that never shipped; 0b01 is not written and the OS hangs on OS-HARDWARE
	// ERR at 0x3001B86C. This machine is 0b11.
	{
		Board board;
		cf_bus_status status = CF_BUS_OK;

		const uint32_t latch = board.read(0x15000000u, 8, status);

		checkEqual(status, CF_BUS_OK, "the CS5 latch answers a read");
		checkEqual((latch >> 4) & 0x3u, uint32_t(0x3u),
			"the panel latch at 0x15000000 returns bits 5:4 = 0b11, which is model code 1, the G2X");

		// A stub that returns zero gives panel bits 0b00, which boots and
		// presents a plain G2.
		check(latch != 0u,
			"the panel latch does not return zero, which would present a plain G2");

		// The other six bits have no recorded source. The model reads them
		// zero and says so, and this case holds that statement to account.
		checkEqual(latch, uint32_t(0x30u),
			"the six bits no authority records read zero, so the latch byte is 0x30");
	}

	// -----------------------------------------------------------------------
	// Case group 2. The identifier is a strap, so a write cannot change it.
	//
	// Clavia's service manual records the model as two 0-ohm resistors, R79
	// and R80, on the panel board. A model that let the firmware write over
	// the identifier would present a different machine one instruction later.
	{
		Board board;
		cf_bus_status status = CF_BUS_OK;

		board.write(0x15000000u, 8, 0x00u, status);
		checkEqual(status, CF_BUS_OK, "a write to the identifier latch completes");
		checkEqual((board.read(0x15000000u, 8, status) >> 4) & 0x3u, uint32_t(0x3u),
			"the identifier still reads 0b11 after a write of zero");

		board.write(0x15000000u, 8, 0xffu, status);
		checkEqual((board.read(0x15000000u, 8, status) >> 4) & 0x3u, uint32_t(0x3u),
			"the identifier still reads 0b11 after a write of all ones");
	}

	// -----------------------------------------------------------------------
	// Case group 2a. A selected model reaches the strap.
	//
	// The Engine is selected by UIPCR bit 0 and detect_model() tests that bit
	// before it calls panel_id(), so the strap an Engine presents is the plain
	// G2's and this fixture cannot tell the two apart. The SIM's own test holds
	// the bit that separates them.
	{
		struct Expectation
		{
			rg2::Model model;
			uint32_t  bits;
			const char* what;
		};

		const Expectation expectations[] =
		{
			{rg2::Model::G2,     0x0u, "a plain G2 straps 0b00"},
			{rg2::Model::G2X,    0x3u, "a G2X straps 0b11"},
			{rg2::Model::Rack,   0x2u, "a Rack straps 0b10"},
			{rg2::Model::Engine, 0x0u, "an Engine straps 0b00, because UIPCR decides before panel_id() runs"},
		};

		for(const Expectation& expectation : expectations)
		{
			Board board(g_displayBase, expectation.model);
			cf_bus_status status = CF_BUS_OK;

			checkEqual((board.read(0x15000000u, 8, status) >> 4) & 0x3u, expectation.bits,
				expectation.what);
		}
	}

	// -----------------------------------------------------------------------
	// Case group 3. Every other latch in the CS5 window is an output latch.
	//
	// It keeps what was written. No authority records what each one drives, so
	// the model carries no meaning for any of them and only the keeping is
	// asserted.
	{
		Board board;
		cf_bus_status status = CF_BUS_OK;

		checkEqual(board.read(0x15000001u, 8, status), uint32_t(0),
			"an output latch reads zero before anything is written to it");

		board.write(0x15000001u, 8, 0xa5u, status);
		checkEqual(status, CF_BUS_OK, "a write to an output latch completes");
		checkEqual(board.read(0x15000001u, 8, status), uint32_t(0xa5u),
			"an output latch returns the last value written to it");

		checkEqual(board.read(0x15000000u, 8, status), uint32_t(0x30u),
			"a write to one latch does not disturb the identifier latch");
	}

	// -----------------------------------------------------------------------
	// Case group 4. The CS4 base is configuration and it is live.
	//
	// Two boards differ only in where the display buffer sits. Each base is
	// asserted to answer in the board that carries it and to answer nothing in
	// the board that does not.
	{
		Board board(g_displayBase);
		Board other(g_otherDisplayBase);

		checkEqual(board.map().decode(g_displayBase), rg2::Region::Cs4,
			"the display buffer answers at the base its configuration gave it");
		checkEqual(other.map().decode(g_displayBase), rg2::Region::None,
			"a board configured elsewhere answers nothing at that base");
		checkEqual(other.map().decode(g_otherDisplayBase), rg2::Region::Cs4,
			"the other board answers at its own base");
		checkEqual(board.map().decode(g_otherDisplayBase), rg2::Region::None,
			"the first board answers nothing at the other base");
	}

	// -----------------------------------------------------------------------
	// Case group 5. The display write path keeps the last written contents.
	//
	// This test asserts only that the buffer returns what this test wrote into
	// it. It reads no banner, because a banner is produced by Clavia's OS
	// image running and this is a T0 check.
	{
		Board board;
		cf_bus_status status = CF_BUS_OK;

		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0),
			"the display buffer reads zero before anything is written to it");

		board.write(g_displayBase, 32, 0x4e4d4732u, status);
		checkEqual(status, CF_BUS_OK, "a 32-bit write to the display buffer completes");
		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0x4e4d4732u),
			"the display buffer returns the 32-bit value this test wrote");

		board.write(g_displayBase + 0x10u, 16, 0x1234u, status);
		checkEqual(board.read(g_displayBase + 0x10u, 16, status), uint32_t(0x1234u),
			"the display buffer returns the 16-bit value this test wrote");

		board.write(g_displayBase + 0x20u, 8, 0x5au, status);
		checkEqual(board.read(g_displayBase + 0x20u, 8, status), uint32_t(0x5au),
			"the display buffer returns the 8-bit value this test wrote");

		// The buffer is big-endian, like the part. A 32-bit write followed by
		// four byte reads is what says so.
		checkEqual(board.read(g_displayBase + 0u, 8, status), uint32_t(0x4eu),
			"byte 0 of the 32-bit write is its most significant byte");
		checkEqual(board.read(g_displayBase + 1u, 8, status), uint32_t(0x4du), "byte 1 follows");
		checkEqual(board.read(g_displayBase + 2u, 8, status), uint32_t(0x47u), "byte 2 follows");
		checkEqual(board.read(g_displayBase + 3u, 8, status), uint32_t(0x32u),
			"byte 3 of the 32-bit write is its least significant byte");

		checkEqual(board.read(g_displayBase + 0x10u, 16, status), uint32_t(0x1234u),
			"a later write elsewhere did not disturb an earlier one");
	}

	// -----------------------------------------------------------------------
	// Case group 6. The panel is quiescent and no poll can spin for ever.
	//
	// A boot loop polls a panel until it answers. This model answers every
	// offset of both windows, at every legal width, with a completed access.
	// A freshly built panel reads zero everywhere, which is no key down, no
	// encoder moving and no button pressed.
	{
		Board board;

		bool everyPollCompleted = true;
		bool everyPollIsQuiescent = true;
		const int widths[] = {8, 16, 32};

		for(uint32_t offset = 0; offset < g_displaySize; offset += 4u)
		{
			for(const int width : widths)
			{
				cf_bus_status status = CF_BUS_OK;
				const uint32_t value = board.read(g_displayBase + offset, width, status);
				if(status != CF_BUS_OK)
					everyPollCompleted = false;
				if(value != 0)
					everyPollIsQuiescent = false;
			}
		}

		check(everyPollCompleted,
			"every offset of the display window answers a poll at every legal width, so no boot loop spins for ever");
		check(everyPollIsQuiescent,
			"a freshly built panel reports no key down, no encoder moving and no button pressed");

		bool everyLatchPollCompleted = true;
		for(uint32_t offset = 0; offset < g_latchWindowSize; ++offset)
		{
			cf_bus_status status = CF_BUS_OK;
			board.read(rg2::g_cs5Base + offset, 8, status);
			if(status != CF_BUS_OK)
				everyLatchPollCompleted = false;
		}

		check(everyLatchPollCompleted,
			"every offset of the CS5 window answers a poll, so no latch poll spins for ever");
	}

	// -----------------------------------------------------------------------
	// Case group 7. Latches 1..7 drive LED rings (schematic ModularG2_Panel Sheet 5).
	//
	// Latch writes to CS5 offsets 1..7 update the corresponding 15-LED ring states.
	{
		Board board;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// Writing 8-bit pattern to Latch 1 (CS5 offset 1) updates LED ring 0
		board.write(rg2::g_cs5Base + 1u, 8, 0x5Au, status);
		checkEqual(status, CF_BUS_OK, "latch 1 write completes");
		checkEqual(uint32_t(board.panel().getLedRingState(0)), uint32_t(0x5Au),
			"latch 1 write updates LED ring 0 state query");
		checkEqual(uint32_t(board.latches().getLedRingState(0)), uint32_t(0x5Au),
			"latches getLedRingState(0) reflects latch 1 write");

		// Writing 16-bit pattern across latches 1 and 2 updates LED ring 0 with full 15-bit value
		board.write(rg2::g_cs5Base + 1u, 16, 0x7FFFu, status);
		checkEqual(status, CF_BUS_OK, "16-bit latch write completes");
		checkEqual(uint32_t(board.panel().getLedRingState(0)), uint32_t(0x7FFFu),
			"16-bit latch write updates LED ring 0 with 15-LED pattern");

		// Writing to Latch 7 (CS5 offset 7) updates LED ring 6
		board.write(rg2::g_cs5Base + 7u, 8, 0xA5u, status);
		checkEqual(status, CF_BUS_OK, "latch 7 write completes");
		checkEqual(uint32_t(board.panel().getLedRingState(6)), uint32_t(0xA5u),
			"latch 7 write updates LED ring 6 state query");
	}

	// -----------------------------------------------------------------------
	// Case group 8. Encoder deltas and latch reading (schematic ModularG2_Panel Sheet 5).
	//
	// Setting encoder deltas on the panel exposes the delta on CS5 latch reads.
	{
		Board board;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// Set encoder 0 delta to +5
		board.panel().setEncoderDelta(0, 5);
		checkEqual(int(board.panel().getEncoderDelta(0)), 5,
			"panel reports configured encoder 0 delta");

		// Reading Latch 1 (CS5 offset 1) returns the encoder delta
		uint32_t readVal = board.read(rg2::g_cs5Base + 1u, 8, status);
		checkEqual(status, CF_BUS_OK, "reading latch 1 completes");
		checkEqual(readVal, uint32_t(5u),
			"reading latch 1 returns encoder 0 delta");

		// Set encoder 3 delta to -4 (0xFC in two's complement)
		board.panel().setEncoderDelta(3, -4);
		readVal = board.read(rg2::g_cs5Base + 4u, 8, status);
		checkEqual(status, CF_BUS_OK, "reading latch 4 completes");
		checkEqual(uint32_t(uint8_t(readVal)), uint32_t(uint8_t(-4)),
			"reading latch 4 returns negative encoder 3 delta in two's complement");

		// Resetting encoder delta to 0 returns the latch to quiescent 0
		board.panel().setEncoderDelta(0, 0);
		readVal = board.read(rg2::g_cs5Base + 1u, 8, status);
		checkEqual(readVal, uint32_t(0u),
			"resetting encoder delta restores latch to quiescent state");
	}

	// -----------------------------------------------------------------------
	// Case group 9. CS4 matrix scan and button return row sensing.
	//
	// Writing walking zeros to CS4 (schematic ModularG2_MainBoard Sheet 3) scans
	// columns; pressed buttons pull corresponding return rows low for PADAT reads.
	{
		Board board;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// Initially, with no buttons pressed, all rows are idle (high in PADAT / active-low)
		checkEqual(board.panel().getActiveRowMask(), uint16_t(0u),
			"initially no button return rows are active");
		checkEqual(board.panel().getRowBits(), uint16_t(0xFFFFu),
			"PADAT return rows idle high with pull-ups");

		// Press button at row 2, column 0
		board.panel().setButtonPressed(2, 0, true);
		check(board.panel().isButtonPressed(2, 0), "button at (2,0) is pressed");

		// Scan column 0: 16-bit walking zero 0x7FFF (bit 15 is 0) written to CS4 base
		board.write(g_displayBase, 16, 0x7FFFu, status);
		checkEqual(status, CF_BUS_OK, "CS4 column 0 scan write completes");

		// Row 2 must now be sensed active
		check(board.panel().isRowActive(2), "row 2 is sensed active during column 0 scan");
		checkEqual(board.panel().getActiveRowMask(), uint16_t(1u << 2),
			"active row mask has bit 2 set");
		checkEqual(board.panel().getRowBits(), uint16_t(~(1u << 2)),
			"PADAT row bits has bit 2 driven low by pressed button");

		// Scan column 1: walking zero 0xBFFF (bit 14 is 0) written to CS4 base
		board.write(g_displayBase, 16, 0xBFFFu, status);
		checkEqual(status, CF_BUS_OK, "CS4 column 1 scan write completes");

		// Button at (2,0) is NOT on column 1, so row 2 is no longer active
		check(!board.panel().isRowActive(2), "row 2 is inactive during column 1 scan");
		checkEqual(board.panel().getActiveRowMask(), uint16_t(0u),
			"active row mask is zero during column 1 scan");
		checkEqual(board.panel().getRowBits(), uint16_t(0xFFFFu),
			"PADAT rows return to idle high during column 1 scan");

		// Press button at row 5, column 1 as well
		board.panel().setButtonPressed(5, 1, true);
		// Column 1 is still selected: row 5 should be sensed active
		board.write(g_displayBase, 16, 0xBFFFu, status);
		check(board.panel().isRowActive(5), "row 5 is sensed active during column 1 scan");
		checkEqual(board.panel().getActiveRowMask(), uint16_t(1u << 5),
			"active row mask has bit 5 set");

		// 32-bit walking zero write: 0xFFFF7FFF scans column 0 and senses row 2 again
		board.write(g_displayBase, 32, 0xFFFF7FFFu, status);
		checkEqual(status, CF_BUS_OK, "32-bit CS4 scan write completes");
		check(board.panel().isRowActive(2), "row 2 sensed active under 32-bit 0xFFFF7FFF scan");

		// Release button at (2,0)
		board.panel().setButtonPressed(2, 0, false);
		board.write(g_displayBase, 32, 0xFFFF7FFFu, status);
		check(!board.panel().isRowActive(2), "row 2 is inactive after button release");
	}

	if(g_failures)
	{
		std::cout << "t0_panel: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_panel: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
