// Task BRD-22. The 1 kHz USB start-of-frame tick. Tier T0: this test needs no
// firmware artifact of any kind.
//
// Plan section 13.4, BRD-22. Design sections 9.4 and 13.10.5.
//
// WHAT THIS TEST IS. The Scheduler calls Board::tickSofIfDue(frameIndex) on
// EVERY frame, unconditionally, and passes the authoritative virtual frame
// index. The Board owns the test: it divides by 96 itself and issues one USB
// start-of-frame tick on each due frame. This test drives the method one
// quantum at a time and records the frame index of every tick the Board
// issued, so the assertion is over WHICH frames ticked and not only over how
// many ticks there were.
//
// WHY THE INDICES AND NOT THE COUNT. A count alone cannot separate a divisor
// that fires twice on one boundary and misses the next from one that fires
// once on each: the two produce the same total over a long run. The
// assertions below compare the whole recorded index sequence against the whole
// expected sequence, so a double and a miss no longer cancel.
//
// THE TWO NUMERIC CASES COME FROM THE PLAN AND THEY BRACKET THE BOUNDARY.
// 1,000 quanta tick 11 times and 960 quanta tick 10, because frame 0 satisfies
// the divisor and the multiples of 96 below 1,000 are 0, 96, ... 960. An
// earlier revision of the plan asserted 10 for 1,000, which a correct
// implementation fails and which only an implementation that skipped frame 0
// would pass. Both cases are asserted here so the boundary is pinned from
// both sides.
//
// HOW THE TICK IS OBSERVED, AND WHY THE TARGET LINKS NO LIBRARY. This
// executable compiles board.cpp together with this file and supplies its own
// definitions of the five mcf5307 C entry points board.cpp uses. That makes
// isp1181_tick observable without adding a test-only accessor to the shipped
// Board, and it is the reason tests_board.cmake names ../board.cpp as a source
// rather than linking g2Lib. Defining isp1181_tick while ALSO linking
// libmcf5307.a is a duplicate-symbol link error the moment anything in the
// link pulls the archive member that defines it, so the seam is taken by
// owning the whole link line instead.
//
// THE MODULUS APPEARS HERE AS THIS FILE'S OWN LITERAL. board.cpp derives it
// from G2_FRAME_RATE_HZ; this file writes 96 out. A change that moves one and
// not the other turns this test red, which is what makes the relation a
// mechanism rather than a shared constant nobody checks.

#include "board.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <mcf5307.h>

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

	// One recorded call of isp1181_tick: the device handle it was given, the
	// SOF frame count it was given, and the frame index the Board was being
	// driven with at the moment of the call.
	struct SofCall
	{
		const isp1181_ctx* ctx;
		uint32_t           sofFrames;
		uint64_t           atFrameIndex;
	};

	std::vector<SofCall> g_sofCalls;
	uint64_t             g_drivingFrameIndex = 0;

	// The token mcf5307_create hands back. Board only stores it and passes it
	// back to mcf5307_exec and mcf5307_destroy, so any non-null address does.
	int g_coreToken = 0;

	std::string indicesToString(const std::vector<uint64_t>& _indices)
	{
		std::string out = "[";
		for(std::size_t i = 0; i < _indices.size(); ++i)
		{
			if(i)
				out += ", ";
			out += std::to_string(_indices[i]);
		}
		return out + "]";
	}

	// Construct a Board, drive it one quantum at a time for `_quanta` frames
	// starting at frame 0, and return the frame indices at which it issued a
	// SOF tick.
	std::vector<uint64_t> sofIndicesOver(const uint64_t _quanta)
	{
		g_sofCalls.clear();

		g2::Board board;
		for(uint64_t i = 0; i < _quanta; ++i)
		{
			g_drivingFrameIndex = i;
			board.tickSofIfDue(i);
		}

		std::vector<uint64_t> indices;
		indices.reserve(g_sofCalls.size());
		for(const auto& call : g_sofCalls)
			indices.push_back(call.atFrameIndex);
		return indices;
	}

	// The frames a correct divisor ticks on, computed by STRIDING rather than
	// by testing a remainder, so this file does not restate the shipped
	// expression in the shipped expression's own shape.
	std::vector<uint64_t> expectedSofIndices(const uint64_t _quanta)
	{
		std::vector<uint64_t> indices;
		for(uint64_t i = 0; i < _quanta; i += 96)
			indices.push_back(i);
		return indices;
	}

	void checkCase(const uint64_t _quanta, const std::size_t _expectedCount)
	{
		const auto observed = sofIndicesOver(_quanta);
		const auto expected = expectedSofIndices(_quanta);

		check(expected.size() == _expectedCount,
		      std::to_string(_quanta) + " quanta: the expected sequence this file builds holds "
		      + std::to_string(_expectedCount) + " frames, and it holds "
		      + std::to_string(expected.size()));

		check(observed == expected,
		      std::to_string(_quanta) + " quanta: the Board ticked on exactly the expected frames; expected "
		      + indicesToString(expected) + ", observed " + indicesToString(observed));

		check(observed.size() == _expectedCount,
		      std::to_string(_quanta) + " quanta: exactly " + std::to_string(_expectedCount)
		      + " ticks were issued, and " + std::to_string(observed.size()) + " were");
	}
}

// The mcf5307 C entry points board.cpp uses, supplied here so that the tick is
// observable and no library is linked. The signatures are include/mcf5307.h's.
extern "C"
{
	void mcf5307_runtime_init(void)
	{
	}

	mcf5307_ctx* mcf5307_create(void*, mcf5307_read_fn, mcf5307_write_fn,
	                            mcf5307_iack_fn)
	{
		return reinterpret_cast<mcf5307_ctx*>(&g_coreToken);
	}

	void mcf5307_destroy(mcf5307_ctx*)
	{
	}

	uint32_t mcf5307_exec(mcf5307_ctx*, uint32_t)
	{
		return 0u;
	}

	void isp1181_tick(isp1181_ctx* const ctx, const uint32_t sofFrames)
	{
		g_sofCalls.push_back(SofCall{ctx, sofFrames, g_drivingFrameIndex});
	}
}

int main()
{
	// The plan's arithmetic, pinned against the plan's own two literals. The
	// count is floor((N - 1) / 96) + 1 because frame 0 is due.
	check((1000u - 1u) / 96u + 1u == 11u,
	      "floor((1000 - 1) / 96) + 1 is 11");
	check((960u - 1u) / 96u + 1u == 10u,
	      "floor((960 - 1) / 96) + 1 is 10");

	// The two cases the plan names.
	checkCase(1000, 11);
	checkCase(960, 10);

	// The boundary one frame either side of the first period, so that an
	// implementation which ticked on frame 96 but not on frame 0, or on both
	// 95 and 96, is separated from a correct one.
	checkCase(1, 1);
	checkCase(95, 1);
	checkCase(96, 1);
	checkCase(97, 2);

	// A Board that is never driven issues nothing. Without this case a tick
	// that fires on construction would be invisible to every case above.
	checkCase(0, 0);

	// Every tick advances the device by exactly one SOF frame, and every tick
	// of one run reaches the SAME device handle. The second assertion is what
	// separates a Board that owns one device from one that produces a fresh
	// device for each tick.
	{
		const auto indices = sofIndicesOver(1000);
		check(indices.size() == 11u, "the run under inspection issued 11 ticks");

		bool allOneFrame  = true;
		bool allSameDevice = true;
		for(const auto& call : g_sofCalls)
		{
			if(call.sofFrames != 1u)
				allOneFrame = false;
			if(call.ctx != g_sofCalls.front().ctx)
				allSameDevice = false;
		}
		// The emptiness clause is not decoration. Both loops above range over
		// g_sofCalls, so both flags stay true when no tick was issued at all,
		// and without this clause a Board that ticks NEVER passes both cases.
		check(!g_sofCalls.empty() && allOneFrame,
		      "every tick advanced the device by exactly one SOF frame, over a non-empty run");
		check(!g_sofCalls.empty() && allSameDevice,
		      "every tick of one run reached the same device handle, over a non-empty run");
	}

	std::cout << (g_failures ? "FAILED " : "PASSED ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
	return g_failures ? 1 : 0;
}
