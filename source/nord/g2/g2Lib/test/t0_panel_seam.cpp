// The panel seam: the four members exist, are `noexcept`, and behave as an
// empty body with a zero-byte state, without disturbing the panel's BusTarget
// display buffer.
//
// The CS5 latch sits at 0x15000000; the panel display buffer sits on CS4,
// whose base and size no authority records. This fixture supplies both, and no
// shipped header carries a number. This test uses only the panel seam and the
// display buffer, so the CS5 latch plays no part here.

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

	// Case group 1. The seam is present and it is a zero-byte state.
	{
		Board board;
		checkEqual(board.panel().stateSize(), size_t(0),
			"the panel carries a zero-byte state today");

		checkEqual(board.panel().stateSize() == 0u, true,
			"a zero-byte state has a size that a stateSize caller can read");
	}

	// Case group 2. `tick` is an empty body. Advancing across a spread of
	// frame indices -- including the first quantum and the largest uint64_t --
	// must not disturb the zero-byte state or the display buffer the panel
	// serves as a BusTarget.
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

	// Case group 3. A save of a zero-byte block writes zero bytes. Filling the
	// destination with a sentinel and saving into it proves the body writes
	// nothing: a body that wrote even one byte would move a sentinel.
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
		// legal call, which lets the scheduler's snapshot path skip the
		// panel's block without special-casing it. The panel it saved from
		// must still serve what it held.
		mcf5307_bus_status status = MCF5307_BUS_OK;
		board.write(g_displayBase, 32, 0x4e4d4732u, status);
		board.panel().stateSave(nullptr);
		checkEqual(board.read(g_displayBase, 32, status), uint32_t(0x4e4d4732u),
			"a null destination leaves the display buffer the panel serves untouched");
		checkEqual(board.panel().stateSize(), size_t(0),
			"a null destination leaves the zero-byte state at zero");
	}

	// Case group 4. A load of a zero-byte block reads zero bytes and restores
	// no member. Loading from a scrubbed source over a panel that already
	// holds display contents must leave the display exactly as it was.
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

	// Case group 5. The scheduler's state path calls save on one object and
	// loads into another (or the same one). With a zero-byte state the round
	// trip must be an exact no-op: the destination panel's display keeps
	// whatever it held, and stateSize stays zero.
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
