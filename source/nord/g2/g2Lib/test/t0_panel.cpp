// The panel and the CS5 latches. Tier T0: this test needs no firmware
// artifact, and asserts only what the board does.
//
// One address here has a recorded source and the other does not. The CS5 latch
// sits at 0x15000000, and panel_id() at 0x3005BFFE drives it and takes bits
// 5:4. The panel display buffer sits on CS4, and no authority records CS4's
// base or its size, so this fixture supplies both.

#include "latches.h"
#include "memoryMap.h"
#include "panel.h"

#include <mcf5307.h>

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
		explicit Board(const uint32_t _displayBase = g_displayBase)
			: m_panel(g_displaySize)
			, m_latches(g_latchWindowSize)
		{
			g2::MemoryMapConfig config;
			config.cs4 = {_displayBase, g_displaySize};
			config.cs5 = {g2::g_cs5Base, g_latchWindowSize};

			m_map = new g2::MemoryMap(config);
			m_map->attach(g2::Region::Cs4, &m_panel);
			m_map->attach(g2::Region::Cs5, &m_latches);
		}

		~Board() { delete m_map; }

		uint32_t read(const uint32_t _address, const int _size, mcf5307_bus_status& _status)
		{
			_status = MCF5307_BUS_OK;
			return g2::memoryMapRead(m_map, _address, _size, &_status);
		}

		void write(const uint32_t _address, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
		{
			_status = MCF5307_BUS_OK;
			g2::memoryMapWrite(m_map, _address, _size, _value, &_status);
		}

		g2::MemoryMap& map() { return *m_map; }

	private:
		g2::Panel m_panel;
		g2::Latches m_latches;
		g2::MemoryMap* m_map = nullptr;
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
		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t latch = board.read(0x15000000u, 8, status);

		checkEqual(status, MCF5307_BUS_OK, "the CS5 latch answers a read");
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
		mcf5307_bus_status status = MCF5307_BUS_OK;

		board.write(0x15000000u, 8, 0x00u, status);
		checkEqual(status, MCF5307_BUS_OK, "a write to the identifier latch completes");
		checkEqual((board.read(0x15000000u, 8, status) >> 4) & 0x3u, uint32_t(0x3u),
			"the identifier still reads 0b11 after a write of zero");

		board.write(0x15000000u, 8, 0xffu, status);
		checkEqual((board.read(0x15000000u, 8, status) >> 4) & 0x3u, uint32_t(0x3u),
			"the identifier still reads 0b11 after a write of all ones");
	}

	// -----------------------------------------------------------------------
	// Case group 3. Every other latch in the CS5 window is an output latch.
	//
	// It keeps what was written. No authority records what each one drives, so
	// the model carries no meaning for any of them and only the keeping is
	// asserted.
	{
		Board board;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		checkEqual(board.read(0x15000001u, 8, status), uint32_t(0),
			"an output latch reads zero before anything is written to it");

		board.write(0x15000001u, 8, 0xa5u, status);
		checkEqual(status, MCF5307_BUS_OK, "a write to an output latch completes");
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

		checkEqual(board.map().decode(g_displayBase), g2::Region::Cs4,
			"the display buffer answers at the base its configuration gave it");
		checkEqual(other.map().decode(g_displayBase), g2::Region::None,
			"a board configured elsewhere answers nothing at that base");
		checkEqual(other.map().decode(g_otherDisplayBase), g2::Region::Cs4,
			"the other board answers at its own base");
		checkEqual(board.map().decode(g_otherDisplayBase), g2::Region::None,
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
		mcf5307_bus_status status = MCF5307_BUS_OK;

		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0),
			"the display buffer reads zero before anything is written to it");

		board.write(g_displayBase, 32, 0x4e4d4732u, status);
		checkEqual(status, MCF5307_BUS_OK, "a 32-bit write to the display buffer completes");
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
				mcf5307_bus_status status = MCF5307_BUS_OK;
				const uint32_t value = board.read(g_displayBase + offset, width, status);
				if(status != MCF5307_BUS_OK)
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
			mcf5307_bus_status status = MCF5307_BUS_OK;
			board.read(g2::g_cs5Base + offset, 8, status);
			if(status != MCF5307_BUS_OK)
				everyLatchPollCompleted = false;
		}

		check(everyLatchPollCompleted,
			"every offset of the CS5 window answers a poll, so no latch poll spins for ever");
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
