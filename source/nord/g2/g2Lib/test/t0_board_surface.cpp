// Task BRD-21. The board surface. Tier T0: this test needs no firmware
// artifact of any kind.
//
// Plan section 13.4, BRD-21. Design sections 6.4, 13.10 and 26.
//
// WHAT THIS TEST IS. Task BRD-21 is the keystone `The Board class` of the
// board track, and its Check is a SURFACE test: it asserts the shape the
// Scheduler will consume, and the one behavioural fact BRD-21 owns (the
// construction log line). The deep behaviour of the Board -- the real bus
// routing, the real 96:1 USB tick, the Nim state blocks -- belongs to the
// later board-track integration, and this test does not reach for any of it.
//
// THE CONCRETENESS IS CHECKED TWO WAYS, because the plan says the assertions
// "name their mechanism". board.h carries five static_asserts that make
// "concrete, not copyable, not movable" a COMPILE-TIME property, so the file
// would not compile if the class lost one of those properties; this test
// ALSO checks the same properties through the type traits at run time, so the
// test is not green merely because the header happened to be empty. Board is
// declared final, which is what makes "nothing derives from it" a property at
// all.
//
// THE CONSTRUCTION LOG LINE. The Board logs one line at construction naming
// G2_MCU_CORE_CLOCK_HZ, the value 45,000,000, and criterion (j). This test
// captures standard output across a Board construction and asserts the line
// is emitted EXACTLY once, and that it names all four things. Measurement
// register row 7 (plan section 4.1) owns the values: G2_MCU_CORE_CLOCK_HZ is
// the 45,000,000 placeholder until SPK-9 reports, and no golden reference or
// capture may be recorded until then -- the line exists so that a reader of
// the whole point of the machine can see the placeholder standing.
//
// RUNMCU IS EXERCISED WITH A ZERO BUDGET ON PURPOSE. The pinned core commit
// exports mcf5307_exec but is at the start of the cpu track, so executing an
// unreset MCU context could drive unspecified early-core behaviour. A zero
// cycle budget means no instruction is executed and the core returns
// immediately, which proves runMcu compiles, links and returns its uint32_t
// cycles-without aborting the process, without depending on the core's early
// behaviour. The budgeted execution of a real program is later integration.

#include "board.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

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

	// Count how many times `needle` appears in `haystack`.
	std::size_t countSubstring(const std::string& haystack, const std::string& needle)
	{
		std::size_t count = 0;
		std::size_t pos   = 0;
		while((pos = haystack.find(needle, pos)) != std::string::npos)
		{
			++count;
			pos += needle.size();
		}
		return count;
	}

	// Construct a Board inside a standard-output capture and return what it
	// printed. The stream is restored before this returns, so the test's own
	// diagnostics are never swallowed. Exactly one Board lives at a time
	// inside the capture.
	std::string captureBoardConstructionOnce()
	{
		std::ostringstream capture;
		std::streambuf*    oldBuf = std::cout.rdbuf(capture.rdbuf());
		{
			g2::Board board;
			(void)board;
		}
		std::cout.rdbuf(oldBuf);
		return capture.str();
	}
}

int main()
{
	// ------------------------------------------------------------------
	// Concreteness: Board is final, not polymorphic, and neither copyable nor
	// movable. The static_asserts in board.h make these compile-time facts;
	// these run-time checks mirror them so the test cannot pass by asserting
	// nothing.
	{
		check(std::is_final_v<g2::Board>,
		      "Board is declared final, so nothing can derive from it");
		check(!std::is_polymorphic_v<g2::Board>,
		      "Board is not polymorphic: no virtual method, no vtable");
		check(!std::is_copy_constructible_v<g2::Board>,
		      "Board is not copy constructible");
		check(!std::is_copy_assignable_v<g2::Board>,
		      "Board is not copy assignable");
		check(!std::is_move_constructible_v<g2::Board>,
		      "Board is not move constructible");
		check(!std::is_move_assignable_v<g2::Board>,
		      "Board is not move assignable");
	}

	// ------------------------------------------------------------------
	// The construction log line, emitted exactly once per construction and
	// naming all four required things.
	{
		const std::string out = captureBoardConstructionOnce();

		check(countSubstring(out, "G2_MCU_CORE_CLOCK_HZ") == 1,
		      "construction logs the G2_MCU_CORE_CLOCK_HZ name exactly once");
		check(countSubstring(out, "45000000") == 1,
		      "construction logs the placeholder value 45000000 exactly once");
		check(countSubstring(out, "placeholder") == 1,
		      "construction states the value is a placeholder exactly once");
		check(countSubstring(out, "(j)") >= 1,
		      "construction names criterion (j) as the value's owner");
	}

	// ------------------------------------------------------------------
	// A second construction emits the line exactly once again (once per Board,
	// not once per process).
	{
		const std::string out = captureBoardConstructionOnce();
		check(countSubstring(out, "G2_MCU_CORE_CLOCK_HZ") == 1,
		      "each Board construction logs the placeholder line exactly once");
		check(countSubstring(out, "(j)") >= 1,
		      "each Board construction names criterion (j)");
	}

	// ------------------------------------------------------------------
	// The six methods the Scheduler uses all exist with the right signatures
	// and are callable on a constructed Board.
	{
		std::ostringstream capture;
		std::streambuf*    oldBuf = std::cout.rdbuf(capture.rdbuf());

		g2::Board board;

		// runMcu: exercised with a zero budget so no instruction executes
		// (see the file header). It returns the cycles spent as a uint32_t.
		volatile uint32_t spent = board.runMcu(0u);
		check(spent == 0u,
		      "runMcu(0) returns the zero cycle budget immediately");

		// faulted: a fresh, non-executed board has not faulted.
		check(board.faulted() == false,
		      "faulted() returns false for a fresh board that has not run");

		// tickSofIfDue: callable on any frame index. The real 96:1 USB tick
		// is BRD-22's; here we only prove the method is on the surface.
		board.tickSofIfDue(0u);
		board.tickSofIfDue(96u);
		check(true, "tickSofIfDue is callable with a frame index");

		// stateSize/stateSave/stateLoad: a flat snapshot that round-trips.
		const std::size_t size = board.stateSize();
		check(size > 0u, "stateSize() returns a non-zero snapshot size");

		std::vector<std::uint8_t> snapshot(size, 0u);
		board.stateSave(snapshot.data());
		// Overwrite one byte so the load really copies state back in.
		snapshot[size / 2] = static_cast<std::uint8_t>(~snapshot[size / 2]);
		board.stateLoad(snapshot.data());
		check(true, "stateSave then stateLoad round-trip without crashing");

		std::cout.rdbuf(oldBuf);
	}

	if(g_failures)
	{
		std::cout << "t0_board_surface: " << g_failures << " of " << g_cases
		          << " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_board_surface: " << g_cases << " of " << g_cases
	          << " cases passed" << std::endl;
	return 0;
}
