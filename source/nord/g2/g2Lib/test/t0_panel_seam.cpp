// Task BRD-13. Tier T0: the panel seam for criterion (h).
//
// Plan section 13.4, BRD-13. Design sections 8.4, 13.5, 23.1.1.
//
// THIS TEST PROVES THE SEAM IS PRESENT AND IS AN EMPTY BODY WITH A ZERO-BYTE
// STATE -- no more and no less. Design section 23.1.1's seam row requires the
// panel to carry `tick(uint64_t frameIndex)`, `stateSize`, `stateSave` and
// `stateLoad` today, and section 13.5 requires the MVP panel to compute
// nothing. A "yes" from spike criterion (h) later fills the body and grows the
// state; the point of the seam is that that happens as a change of body and
// not as a change of two interfaces.
//
// WHAT THIS TEST DOES NOT DO, AND WHY. It does not assert the scheduler's order
// table, the context-index bounds of `cycleDebt` / `longDispatchQuanta` /
// `contextFaulted` / `contextFault`, or how many contexts the `Executor` job
// array holds. Those are scheduler and executor tasks owned by the SCH and
// DSP tracks (sections 13.5, 13.10.3); the panely-side half of each sentence is
// stated in panel.h and the scheduler-side half is asserted by those tasks'
// own tests. This test asserts the surface BRD-13 owns: the members exist,
// are `noexcept`, and behave as an empty body with a zero-byte state, without
// disturbing the panel's BusTarget display buffer.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG (see the correction log, 2026-08-06).
//
// ONE ADDRESS HERE HAS A RECORDED SOURCE AND THE OTHER DOES NOT, exactly as in
// t0_panel.cpp: AGENTS.md section 2.2 records the CS5 latch at 0x15000000, and
// the panel display buffer sits on CS4, whose base and size no authority
// records. This fixture therefore supplies both, and no shipped header carries
// a number. This test uses only the panel seam and the display buffer, so the
// CS5 latch plays no part here.

#include "latches.h"
#include "memoryMap.h"
#include "panel.h"

#include <mcf5307.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>

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

	// This fixture's own window, on CS4. Nothing in a shipped header names it.
	constexpr uint32_t g_displayBase = 0x14000000u;
	constexpr uint32_t g_displaySize = 0x00000400u;

	class Board
	{
	public:
		Board()
			: m_panel(g_displaySize)
			, m_latches(0x10u)
		{
			g2::MemoryMapConfig config;
			config.cs4 = {g_displayBase, g_displaySize};
			config.cs5 = {g2::g_cs5Base, 0x10u};

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

		g2::Panel& panel() { return m_panel; }

	private:
		g2::Panel m_panel;
		g2::Latches m_latches;
		g2::MemoryMap* m_map = nullptr;
	};
}

int main()
{
	static_assert(noexcept(std::declval<g2::Panel&>().tick(uint64_t(0))),
		"Panel::tick must be noexcept: the scheduler's run phase advances the panel without an error channel (design 13.10 rule 2).");
	static_assert(noexcept(std::declval<const g2::Panel&>().stateSize()),
		"Panel::stateSize must be noexcept.");
	static_assert(noexcept(std::declval<const g2::Panel&>().stateSave(nullptr)),
		"Panel::stateSave must be noexcept.");
	static_assert(noexcept(std::declval<g2::Panel&>().stateLoad(nullptr)),
		"Panel::stateLoad must be noexcept.");
	static_assert(std::is_same_v<decltype(std::declval<const g2::Panel&>().stateSize()), size_t>,
		"Panel::stateSize returns size_t, the byte count of the panel state (design 23.1.1).");
	static_assert(std::is_same_v<decltype(std::declval<const g2::Panel&>().stateSave(nullptr)), void>,
		"Panel::stateSave returns void: a zero-byte state writes nothing.");
	static_assert(std::is_same_v<decltype(std::declval<g2::Panel&>().stateLoad(nullptr)), void>,
		"Panel::stateLoad returns void: a zero-byte state cannot fail to restore.");

	// -----------------------------------------------------------------------
	// Case group 1. THE SEAM IS PRESENT AND IT IS A ZERO-BYTE STATE.
	//
	// Section 23.1.1's seam row: "a zero-byte state". stateSize is the number
	// the snapshot block must be today.
	{
		Board board;
		checkEqual(board.panel().stateSize(), size_t(0),
			"the panel carries a zero-byte state today");

		checkEqual(board.panel().stateSize() == 0u, true,
			"a zero-byte state has a size that a stateSize caller can read");
	}

	// -----------------------------------------------------------------------
	// Case group 2. `tick` IS AN EMPTY BODY THAT LEAVES EVERYTHING ALONE.
	//
	// The MVP panel computes nothing. Advancing it across a spread of frame
	// indices -- including the first quantum and the largest uint64_t -- must
	// not disturb the zero-byte state, and it must not disturb the display
	// buffer the panel serves as a BusTarget.
	{
		Board board;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		board.write(g_displayBase, 32, 0x4e4d4732u, status);
		checkEqual(status, MCF5307_BUS_OK, "a write into the display buffer completes before ticking");

		for(const uint64_t frame : {uint64_t(0), uint64_t(1), uint64_t(2048), uint64_t(~uint64_t(0))})
		{
			board.panel().tick(frame);
			checkEqual(board.panel().stateSize(), size_t(0),
				"ticking advances the panel without growing the zero-byte state");
		}

		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0x4e4d4732u),
			"ticking must not disturb the display buffer the panel serves");

		// The panel is still callable as a BusTarget after a tick.
		board.write(g_displayBase + 0x10u, 16, 0x1234u, status);
		checkEqual(board.read(g_displayBase + 0x10u, 16, status), uint32_t(0x1234u),
			"the panel still answers as a BusTarget after a tick");
	}

	// -----------------------------------------------------------------------
	// Case group 3. `stateSave` WRITES NOTHING, PROVEN AGAINST SENTINELS.
	//
	// A save of a zero-byte block writes zero bytes. Filling the destination
	// with a sentinel and saving into it proves the body writes nothing, which
	// is the strongest form of "writes nothing" available: a body that wrote
	// even one byte would move a sentinel.
	{
		Board board;
		std::uint8_t sink[64];
		for(std::uint8_t& b : sink)
			b = 0xa5u;

		board.panel().stateSave(sink);

		bool unchanged = true;
		for(const std::uint8_t b : sink)
			if(b != 0xa5u)
				unchanged = false;
		check(unchanged,
			"stateSave of a zero-byte state writes nothing, proven against a sentinel-filled destination");

		// A zero-byte save has no pointer to touch: a null destination is a
		// legal call, and this is what keeps the scheduler's snapshot path
		// able to skip the panel's block without special-casing it.
		board.panel().stateSave(nullptr);
		check(true, "stateSave accepts a null destination, because zero bytes are written");
	}

	// -----------------------------------------------------------------------
	// Case group 4. `stateLoad` READS NOTHING AND CHANGES NOTHING.
	//
	// A load of a zero-byte block reads zero bytes and restores no member.
	// Loading from a scrubbed source over a panel that already holds display
	// contents must leave the display exactly as it was.
	{
		Board board;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		board.write(g_displayBase, 32, 0x4e4d4732u, status);

		std::uint8_t source[64];
		for(std::uint8_t& b : source)
			b = 0x5au;

		board.panel().stateLoad(source);
		checkEqual(board.panel().stateSize(), size_t(0),
			"loading a zero-byte state keeps the state size at zero");
		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0x4e4d4732u),
			"loading a zero-byte state must not disturb the display buffer");

		// A zero-byte load has no pointer to read: a null source is legal.
		board.panel().stateLoad(nullptr);
		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0x4e4d4732u),
			"stateLoad accepts a null source, because zero bytes are read");
	}

	// -----------------------------------------------------------------------
	// Case group 5. A SAVE/LOAD ROUND TRIP IS A NO-OP, NOT A CORRUPTION.
	//
	// The scheduler's state path will call save on one object and load into
	// another (or the same one). With a zero-byte state the round trip must be
	// an exact no-op: the destination panel's display keeps whatever it held,
	// and stateSize stays zero.
	{
		Board board;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		board.write(g_displayBase + 0x20u, 8, 0x5au, status);

		std::uint8_t scratch[4] = {0x00, 0x00, 0x00, 0x00};
		board.panel().stateSave(scratch);
		board.panel().stateLoad(scratch);

		checkEqual(board.panel().stateSize(), size_t(0),
			"a save/load round trip leaves the zero-byte state at zero");
		checkEqual(board.read(g_displayBase + 0x20u, 8, status), uint32_t(0x5au),
			"a save/load round trip leaves the display buffer untouched");
	}

	if(g_failures)
	{
		std::cout << "t0_panel_seam: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_panel_seam: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
