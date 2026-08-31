// The board surface test.
//
// Concreteness is checked two ways. board.h carries five static_asserts that
// make "concrete, not copyable, not movable" a compile-time property; this
// test also checks the same properties through the type traits at run time, so
// the test is not green merely because the header happened to be empty.
//
// runMcu is exercised with a zero budget on purpose. Executing an unreset MCU
// context could drive unspecified early-core behaviour; a zero cycle budget
// executes no instruction and returns immediately, which proves runMcu
// compiles, links and returns its uint32_t without depending on the core's
// early behaviour.

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

		// runMcu: exercised with a zero budget so no instruction executes. It
		// returns the cycles spent as a uint32_t.
		volatile uint32_t spent = board.runMcu(0u);
		check(spent == 0u,
		      "runMcu(0) returns the zero cycle budget immediately");

		// faulted: a fresh, non-executed board has not faulted.
		check(board.faulted() == false,
		      "faulted() returns false for a fresh board that has not run");

		const std::size_t size = board.stateSize();
		check(size > 0u, "stateSize() returns a non-zero snapshot size");

		// tickSofIfDue: the snapshot is the only surface that observes the
		// recorded frame index, so two saves either side of a tick are what
		// prove the argument was kept.
		board.tickSofIfDue(0u);
		std::vector<std::uint8_t> atZero(size, 0u);
		board.stateSave(atZero.data());

		board.tickSofIfDue(96u);
		std::vector<std::uint8_t> at96(size, 0u);
		board.stateSave(at96.data());
		check(at96 != atZero,
		      "tickSofIfDue records the frame index it was given");

		// stateLoad must overwrite what the Board currently holds: the Board
		// is at frame 96, and loading the frame-zero snapshot back has to make
		// the next save reproduce the frame-zero bytes exactly.
		board.stateLoad(atZero.data());
		std::vector<std::uint8_t> reSaved(size, 0u);
		board.stateSave(reSaved.data());
		check(reSaved == atZero,
		      "stateLoad restores the snapshot, proven by re-saving the same bytes");

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
