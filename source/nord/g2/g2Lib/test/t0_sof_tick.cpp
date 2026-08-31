// Tier T0: this test needs no firmware artifact of any kind.
//
// The Scheduler calls Board::tickSofIfDue(frameIndex) on EVERY frame,
// unconditionally, and passes the authoritative virtual frame index. The Board
// owns the test: it divides by 96 itself and issues one USB start-of-frame tick
// on each due frame. This test drives the method one quantum at a time and
// records the frame index of every tick the Board issued, so the assertion is
// over WHICH frames ticked and not only over how many ticks there were.
//
// A count alone cannot separate a divisor that fires twice on one boundary and
// misses the next from one that fires once on each: the two produce the same
// total over a long run. The assertions below compare the whole recorded index
// sequence against the whole expected sequence, so a double and a miss no
// longer cancel.
//
// The two numeric cases bracket the boundary. 1,000 quanta tick 11 times and
// 960 quanta tick 10, because frame 0 satisfies the divisor and the multiples
// of 96 below 1,000 are 0, 96, ... 960. Only an implementation that skipped
// frame 0 would answer 10 for 1,000, so both cases are asserted here.
//
// How the tick is observed, and why the target links no library. This
// executable compiles board.cpp together with this file and supplies its own
// definitions of the mcf5307 C entry points board.cpp uses. That makes
// isp1181_tick observable without adding a test-only accessor to the shipped
// Board, and it is the reason tests_board.cmake names ../board.cpp as a source
// rather than linking g2Lib. Defining isp1181_tick while ALSO linking
// libmcf5307.a is a duplicate-symbol link error the moment anything in the link
// pulls the archive member that defines it, so the seam is taken by owning the
// whole link line instead. The Board creates and destroys its USB device, so
// isp1181_create and isp1181_destroy are supplied here too.
//
// How this file models handle identity. The isp1181_create below hands back a
// DISTINCT, non-null handle on every call, drawn from a token pool this file
// never clears -- so no two handles in the whole process are equal. That is
// what separates the assertion "the tick reached the CORRECT handle" from the
// strictly weaker "the tick reached a CONSTANT handle": a permanently nil
// handle passes "constant" and fails "correct".
//
// The negative case drives a wrong handle through the same predicate. Two
// Boards are alive at once, each with its own device, and the ONE predicate
// everyTickOfBoardCarried is run twice against the first Board's ticks: once
// with that Board's own handle, which must hold, and once with the OTHER
// Board's handle, which must NOT. Without the second run the predicate is never
// observed to reject anything.
//
// The modulus appears here as this file's own literal. board.cpp derives it
// from G2_FRAME_RATE_HZ; this file writes 96 out. A change that moves one and
// not the other turns this test red.

#include "board.h"

#include <cstddef>
#include <cstdint>
#include <deque>
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

	// A single monotonic clock over every recorded call, whatever its kind. It
	// is what makes "the tick happened between the create and the destroy" an
	// assertion rather than an assumption about the order a reader imagines.
	uint64_t g_seq = 0;

	// One recorded call of isp1181_tick: the device handle it was given, the
	// SOF frame count it was given, the frame index the Board was being driven
	// with at the moment of the call, which Board was being driven, and where
	// the call sits in the recorded sequence.
	struct SofCall
	{
		const isp1181_ctx* ctx;
		uint32_t           sofFrames;
		uint64_t           atFrameIndex;
		int                fromBoard;
		uint64_t           seq;
	};

	// One recorded call of isp1181_create. The user pointer is recorded because
	// it is the Board that must own the device, and a create that passed
	// something else is a different defect from one that never happened.
	struct CreateCall
	{
		const isp1181_ctx* ctx;
		const void*        user;
		uint64_t           seq;
	};

	// One recorded call of isp1181_destroy.
	struct DestroyCall
	{
		const isp1181_ctx* ctx;
		uint64_t           seq;
	};

	std::vector<SofCall>     g_sofCalls;
	std::vector<CreateCall>  g_creates;
	std::vector<DestroyCall> g_destroys;

	uint64_t g_drivingFrameIndex = 0;
	int      g_drivingBoard      = 0;

	// The token pool isp1181_create draws its handles from. A deque is used
	// because push_back leaves references to existing elements valid, so every
	// handle stays live and distinct for the whole run.
	//
	// It is never cleared, and that is deliberate. Every handle this file hands
	// out is distinct across the WHOLE process, so a Board that cached a handle
	// belonging to an earlier run is caught instead of excused by an address
	// the allocator happened to reuse.
	std::deque<int> g_usbTokens;

	// The token mcf5307_create hands back. Board only stores it and passes it
	// back to mcf5307_exec and mcf5307_destroy, so any non-null address does.
	int g_coreToken = 0;

	// Clear the call records between runs. The token pool above is deliberately
	// not among them.
	void resetRecords()
	{
		g_sofCalls.clear();
		g_creates.clear();
		g_destroys.clear();
	}

	// TRUE when every tick recorded for Board `_tag` carried `_handle`, over a
	// NON-EMPTY set of ticks. The emptiness clause is not decoration: without
	// it the predicate is vacuously true for a Board that never ticked, and
	// both the positive and the negative case below would pass against a Board
	// that ticks never.
	//
	// This is the one predicate both cases run. The negative case exists to
	// watch it return false, which is the only thing that establishes it can.
	bool everyTickOfBoardCarried(const int _tag, const isp1181_ctx* const _handle)
	{
		std::size_t seen = 0;
		for(const auto& call : g_sofCalls)
		{
			if(call.fromBoard != _tag)
				continue;
			++seen;
			if(call.ctx != _handle)
				return false;
		}
		return seen != 0;
	}

	// How many times `_handle` was passed to isp1181_destroy.
	std::size_t destroyCountOf(const isp1181_ctx* const _handle)
	{
		std::size_t n = 0;
		for(const auto& call : g_destroys)
			if(call.ctx == _handle)
				++n;
		return n;
	}

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
		resetRecords();
		g_drivingBoard = 0;

		{
			g2::Board board;
			for(uint64_t i = 0; i < _quanta; ++i)
			{
				g_drivingFrameIndex = i;
				board.tickSofIfDue(i);
			}
		}

		std::vector<uint64_t> indices;
		indices.reserve(g_sofCalls.size());
		for(const auto& call : g_sofCalls)
			indices.push_back(call.atFrameIndex);
		return indices;
	}

	// Drive one Board of a multi-Board run, tagging every tick it issues so the
	// assertions can tell the two Boards' ticks apart.
	void driveBoard(const int _tag, g2::Board& _board, const uint64_t _quanta)
	{
		g_drivingBoard = _tag;
		for(uint64_t i = 0; i < _quanta; ++i)
		{
			g_drivingFrameIndex = i;
			_board.tickSofIfDue(i);
		}
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
	/* ANSWERS 1, WHICH IS "THE RUNTIME IS USABLE". mcf5307.h states the status
	 * is a truth value and not a POSIX error code, and 0 is reserved for a
	 * one-time latch that was abandoned. This fake has no latch and no runtime
	 * to stall, so 1 is the only answer it can honestly give. */
	int mcf5307_runtime_init(void)
	{
		return 1;
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

	/* The core entry points the Board's handle to its own core forwards to.
	 * Nothing here drives that handle, so each answers the value mcf5307.h
	 * defines for a context that can do nothing: no register holds a value, a
	 * write to one does not succeed, and a core that never ran is neither
	 * halted nor faulted. */
	void mcf5307_reset(mcf5307_ctx*, uint32_t, uint32_t)
	{
	}

	uint32_t mcf5307_get_reg(const mcf5307_ctx*, int)
	{
		return 0u;
	}

	int mcf5307_set_reg(mcf5307_ctx*, int, uint32_t)
	{
		return 0;
	}

	int mcf5307_halted(const mcf5307_ctx*)
	{
		return 0;
	}

	int mcf5307_faulted(const mcf5307_ctx*)
	{
		return 0;
	}

	/* board.cpp presents the Board's interrupt state to the core, so a target
	 * that compiles it on its own must supply this entry point too. Nothing in
	 * this test drives it: no case here programs an ICR or raises a source. */
	void mcf5307_set_irq(mcf5307_ctx*, int, uint8_t, int)
	{
	}

	/* A DISTINCT, NON-NULL handle on every call, and never a repeat. This is
	 * the whole reason the assertions below can say "correct" rather than only
	 * "constant" -- see the file header. */
	isp1181_ctx* isp1181_create(void* const user, isp1181_irq_fn,
	                            isp1181_tx_fn)
	{
		g_usbTokens.push_back(0);
		const auto handle =
			reinterpret_cast<isp1181_ctx*>(&g_usbTokens.back());
		g_creates.push_back(CreateCall{handle, user, g_seq++});
		return handle;
	}

	void isp1181_destroy(isp1181_ctx* const ctx)
	{
		g_destroys.push_back(DestroyCall{ctx, g_seq++});
	}

	void isp1181_tick(isp1181_ctx* const ctx, const uint32_t sofFrames)
	{
		g_sofCalls.push_back(SofCall{ctx, sofFrames, g_drivingFrameIndex,
		                             g_drivingBoard, g_seq++});
	}

	/* The two entry points the Board's CS3 window forwards to. Nothing in this
	 * test drives the bus, so each answers a benign value: a byte of zero on a
	 * read, and nothing kept on a write. They exist so that board.cpp, which
	 * calls them from the adapter, still links without a library. */
	uint8_t isp1181_read(isp1181_ctx*, const uint32_t)
	{
		return 0u;
	}

	void isp1181_write(isp1181_ctx*, const uint32_t, const uint8_t)
	{
	}

	/* The Board drains its transport hub into the device on every quantum
	 * boundary, so board.cpp references this entry point and a target that
	 * links no mcf5307 archive must supply it. It is a sink and not a recorder:
	 * nothing in this file drives the hub, so no frame ever reaches it.
	 *
	 * It answers 1, which is "an OUT buffer holds the packet". The Board reads
	 * this return and treats 0 as a NAK, which leaves its cursor where it was
	 * and offers the same packet again at the next quantum, so a sink that
	 * answered 0 would be retried forever rather than drained. */
	int isp1181_rx(isp1181_ctx*, int, const uint8_t*, size_t)
	{
		return 1;
	}

	/* The Board moves its handle off the Stub backend at construction, so
	 * board.cpp references this entry point too and a target that links no
	 * mcf5307 archive must supply it.
	 *
	 * It answers 1, which is "the handle moved". The Board reads the return
	 * only to detect a refusal, and a refusal is a state this file's fake
	 * device cannot be in: there is no backend here to refuse. Answering 0
	 * would make every Board in this file print the refusal line. */
	int isp1181_set_backend(isp1181_ctx*, int)
	{
		return 1;
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

	// Every tick advances the device by exactly one SOF frame, every tick
	// reaches the handle the Board's OWN isp1181_create returned, and that
	// handle is created once and destroyed once around them.
	{
		const auto indices = sofIndicesOver(1000);
		check(indices.size() == 11u, "the run under inspection issued 11 ticks");

		check(g_creates.size() == 1u,
		      "one Board construction called isp1181_create exactly once, and it called it "
		      + std::to_string(g_creates.size()) + " times");

		const isp1181_ctx* const owned =
			g_creates.size() == 1u ? g_creates.front().ctx : nullptr;

		check(owned != nullptr, "the handle the Board was given is not null");

		bool allOneFrame = true;
		for(const auto& call : g_sofCalls)
			if(call.sofFrames != 1u)
				allOneFrame = false;
		// The emptiness clause is not decoration. The loop above ranges over
		// g_sofCalls, so the flag stays true when no tick was issued at all,
		// and without this clause a Board that ticks NEVER passes the case.
		check(!g_sofCalls.empty() && allOneFrame,
		      "every tick advanced the device by exactly one SOF frame, over a non-empty run");

		// THE STRENGTHENED ASSERTION. It read "every tick of one run reached
		// the SAME device handle", which a permanently nil handle satisfies
		// exactly as well as a correct one -- the Board exposed no correct
		// handle to compare against, so constancy was all there was to assert.
		// It now names the handle isp1181_create returned for THIS Board.
		check(owned != nullptr && everyTickOfBoardCarried(0, owned),
		      "every tick reached the handle isp1181_create returned for this Board");

		check(g_destroys.size() == 1u,
		      "the Board destroyed exactly one USB device, and it destroyed "
		      + std::to_string(g_destroys.size()));
		check(owned != nullptr && destroyCountOf(owned) == 1u,
		      "the handle the Board was given was destroyed exactly once");

		// The ticks sit strictly inside the device's lifetime. Without this a
		// create that ran after the ticks, or a destroy that ran before them,
		// satisfies every assertion above.
		bool insideLifetime = !g_sofCalls.empty() && g_creates.size() == 1u
		                      && g_destroys.size() == 1u;
		if(insideLifetime)
			for(const auto& call : g_sofCalls)
				if(call.seq < g_creates.front().seq
				   || call.seq > g_destroys.front().seq)
					insideLifetime = false;
		check(insideLifetime,
		      "every tick happened after the create and before the destroy");
	}

	// TWO BOARDS ALIVE AT ONCE. Each owns its own device, and the NEGATIVE
	// case drives a wrong handle through the same predicate the positive case
	// trusts. This is the pair that makes "correct handle" mean more than
	// "constant handle": with one Board in the process there is only one
	// handle, and every wrong answer that is constant looks right.
	{
		resetRecords();
		const void* addrA = nullptr;
		const void* addrB = nullptr;
		{
			g2::Board a;
			g2::Board b;
			addrA = &a;
			addrB = &b;
			driveBoard(0, a, 200);
			driveBoard(1, b, 200);
		}

		check(g_creates.size() == 2u,
		      "two Boards called isp1181_create twice, and they called it "
		      + std::to_string(g_creates.size()) + " times");

		const isp1181_ctx* const handleA =
			g_creates.size() == 2u ? g_creates[0].ctx : nullptr;
		const isp1181_ctx* const handleB =
			g_creates.size() == 2u ? g_creates[1].ctx : nullptr;

		// Distinctness is ASSERTED and not assumed of the stub. If the two
		// handles were equal every negative case below would be vacuous, and
		// the test would report a strength it does not have.
		check(handleA != nullptr && handleB != nullptr && handleA != handleB,
		      "the two Boards were given two different non-null handles");

		// Each Board registered itself as its device's owner. The user pointer
		// is what the irq and tx callbacks are handed, so a Board that
		// registered null or its neighbour would deliver another Board's
		// interrupts. That defect is invisible in the tick record, which
		// carries only the handle.
		check(g_creates.size() == 2u && g_creates[0].user == addrA,
		      "Board A registered itself as its device's user pointer");
		check(g_creates.size() == 2u && g_creates[1].user == addrB,
		      "Board B registered itself as its device's user pointer");

		check(everyTickOfBoardCarried(0, handleA),
		      "positive: every tick of Board A reached Board A's own handle");
		check(everyTickOfBoardCarried(1, handleB),
		      "positive: every tick of Board B reached Board B's own handle");

		// THE NEGATIVE CASES. Same predicate, same recorded ticks, a WRONG
		// handle -- and the predicate must reject. A predicate nobody has
		// watched return false cannot be known to distinguish the correct
		// handle from any other, which is the exact weakness this test was
		// strengthened to remove.
		check(!everyTickOfBoardCarried(0, handleB),
		      "negative: Board A's ticks do NOT match Board B's handle");
		check(!everyTickOfBoardCarried(1, handleA),
		      "negative: Board B's ticks do NOT match Board A's handle");
		check(!everyTickOfBoardCarried(0, nullptr),
		      "negative: Board A's ticks do NOT match a nil handle");

		// Both devices are released, each exactly once. An unbalanced pair
		// leaks or double-destroys, and neither shows up in the tick record.
		check(g_destroys.size() == 2u,
		      "two Boards destroyed two USB devices, and they destroyed "
		      + std::to_string(g_destroys.size()));
		check(handleA != nullptr && destroyCountOf(handleA) == 1u,
		      "Board A's handle was destroyed exactly once");
		check(handleB != nullptr && destroyCountOf(handleB) == 1u,
		      "Board B's handle was destroyed exactly once");
	}

	std::cout << (g_failures ? "FAILED " : "PASSED ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
	return g_failures ? 1 : 0;
}
