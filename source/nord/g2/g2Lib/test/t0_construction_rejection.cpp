/* t0_construction_rejection.cpp -- the construction rejections of task SCH-18.
 * Design sections 13.10 rule 4, 13.4.5, 13.6, 13.6.2. Plan section 14.5.
 *
 * `Scheduler::create` is the SINGLE rejection point, because every rejectable
 * value arrives through `Scheduler::Config`. Each case below asserts BOTH
 * halves of one row: a null return AND the exact `g2::Status`.
 *
 * NO CASE HERE IS A LANGUAGE assert() AND NO CASE CATCHES AN EXCEPTION. A
 * release build removes an assert(), and design section 13.10 rule 2 forbids
 * throwing, so the status out-param is the whole observable and this file runs
 * identically in a release build and a debug build.
 *
 * WHY BOTH HALVES OF EVERY ROW. A null return alone does not distinguish the
 * rows from each other, and a status alone does not establish that no object
 * was handed back. Either half on its own leaves a defect it cannot name.
 *
 * THE ORDER OF THE CHECKS INSIDE THE FACTORY IS PINNED HERE, AND ONE ROW
 * REQUIRES IT. A `dspCount` of 0 is a legal input to two rows at once: the DSP
 * count row rejects it, and `D_chain = (N - 1) * hopFrames` is unsigned, so at
 * a count of 0 the subtraction wraps and the lookahead bound is exceeded by the
 * same Config. The factory reports the FIRST failing row in the order the plan
 * writes the table, and the count row is the sixth of that table while the
 * lookahead row is the eighth, so the answer is `BadDspCount`.
 *
 * THAT SECOND ROW IS REACHED ONLY BECAUSE THE FACTORY ACCUMULATES THE SUM IN 64
 * BITS. In 32 bits the wrapped delay plus a small `L` wraps a SECOND time and
 * lands back UNDER the bound, so the bound would be satisfied rather than
 * exceeded and the two rows would not overlap at all. The width therefore
 * decides whether the ordering question exists, and the case below asserts the
 * premise at the width the factory uses rather than assuming it.
 *
 * WHAT THIS FILE DOES NOT ESTABLISH. It does not establish that the Config
 * defaults are the machine's shipped configuration. Two of them,
 * `lookaheadFrames` and `maxHostBlockFrames`, have no shipped constant to be
 * equal to and carry the smallest legal value instead. What the defaults buy
 * here is narrower and is all that is claimed: a case that perturbs ONE field
 * is rejected for that field and for nothing else.
 */

#include "board.h"
#include "executor.h"
#include "scheduler.h"
#include "status.h"

#include "g2/timebase.h"

#include "dsp56kEmu/dsp.h"

#include <cstdint>
#include <cstdio>
#include <memory>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const char* const _what)
	{
		++g_cases;

		if(_condition)
			return;

		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	/* The bound of design section 13.6.2, and the two terms that meet `L` under
	 * it. `D_codec` is 0 by construction and is written as a named term rather
	 * than omitted, exactly as section 20's formula writes it. */
	constexpr uint64_t kLookaheadBound = 16384;
	constexpr uint64_t kDelayCodec     = 0;

	/* The subtraction is `unsigned`, because ChainAdapter's `dspCount` and
	 * `hopFrames` members are, and the WIDENING HAPPENS AFTER IT. That order is
	 * the whole point: a count of 0 wraps here and the wrapped value survives
	 * into the sum, which is the premise the ordering case below rests on.
	 *
	 * Accumulating the sum in 32 bits instead would wrap a SECOND time and land
	 * back under the bound for a small lookahead, so the width is part of the
	 * claim rather than a formality. `Scheduler::create` accumulates in 64 bits
	 * for that reason and this helper matches it. */
	uint64_t chainDelay(const g2::Scheduler::Config& _config)
	{
		return static_cast<uint64_t>(_config.dspCount - 1u) * _config.hopFrames;
	}

	void checkRejected(g2::Executor& _executor, g2::Board& _board, const g2::Scheduler::Config& _config,
		const g2::Status _expected, const char* const _what)
	{
		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler = g2::Scheduler::create(_config, _executor, _board, status);

		char what[256];

		std::snprintf(what, sizeof(what), "%s: returns no object", _what);
		check(scheduler == nullptr, what);

		std::snprintf(what, sizeof(what), "%s: reports status %u (got %u)", _what,
			static_cast<unsigned>(_expected), static_cast<unsigned>(status));
		check(status == _expected, what);
	}

	/* The success half. It is CONDITIONAL ON THE BUILD for the reason SCH-17's
	 * check already records: in an interpreter build no Scheduler can be created
	 * at all, so an unconditional success assertion would pin a property that
	 * build cannot exercise. The rejection half above is unconditional. */
	void checkAccepted(g2::Executor& _executor, g2::Board& _board, const g2::Scheduler::Config& _config,
		const char* const _what)
	{
		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler = g2::Scheduler::create(_config, _executor, _board, status);

		char what[256];

		if(!dsp56k::g_useJIT)
		{
			std::snprintf(what, sizeof(what), "%s: returns no object in an interpreter build", _what);
			check(scheduler == nullptr, what);

			std::snprintf(what, sizeof(what), "%s: reports BadBackend in an interpreter build (got %u)", _what,
				static_cast<unsigned>(status));
			check(status == g2::Status::BadBackend, what);
			return;
		}

		std::snprintf(what, sizeof(what), "%s: returns an object", _what);
		check(scheduler != nullptr, what);

		std::snprintf(what, sizeof(what), "%s: reports Ok (got %u)", _what, static_cast<unsigned>(status));
		check(status == g2::Status::Ok, what);
	}
}

int main()
{
	std::printf("t0_construction_rejection: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* ONE Board and ONE Executor for every case. The Board creates the MCF5307
	 * core context and initialises the Nim runtime, so one per case would pay
	 * that for each row without exercising anything this file claims. */
	g2::SerialExecutor executor;
	g2::Board          board;

	/* ---------------- the configuration every case starts from.
	 *
	 * The default Config must itself be accepted: every case below perturbs one
	 * field of it and leaves the rest at their legal values. */
	checkAccepted(executor, board, g2::Scheduler::Config{}, "the default Config");

	/* ---------------- row 1: framesPerQuantum is fixed at 1.
	 *
	 * A value above 1 gives F transmit callbacks for each position with a
	 * single swap point outside the run phase, so each write overwrites the
	 * last and the mailbox drops F-1 of every F frames. */
	{
		const unsigned driven[] = { 0u, 2u, 4u };

		for(const unsigned frames : driven)
		{
			g2::Scheduler::Config config;
			config.framesPerQuantum = frames;

			char what[128];
			std::snprintf(what, sizeof(what), "framesPerQuantum of %u", frames);
			checkRejected(executor, board, config, g2::Status::BadFramesPerQuantum, what);
		}
	}

	/* ---------------- row 2: a hop of zero, WITH THE OVERRIDE TAKEN.
	 *
	 * The override is what makes this row independent of row 5 rather than
	 * shadowed by it. A hop of zero collapses the hop: every mailbox is a delay
	 * line of hopFrames + 1 frames, so a frame written in one quantum is taken
	 * back in that same quantum and no job may observe another job's state is
	 * broken inside the very object the override exists to exercise. */
	{
		g2::Scheduler::Config config;
		config.testOverride = true;
		config.hopFrames    = 0;

		checkRejected(executor, board, config, g2::Status::BadHopFrames, "hopFrames of 0 with the override taken");
	}

	/* ---------------- row 3: a divider of zero, WITH THE OVERRIDE TAKEN.
	 *
	 * The divider is a DIVISOR: dspContext.h's landed comment on that member
	 * states that create() returns BadDivider and no object for that value so
	 * the modulo cannot divide by zero. The override must not be able to hand a
	 * zero divisor to the window test in dspJob. */
	{
		g2::Scheduler::Config config;
		config.testOverride          = true;
		config.secondBusFrameDivider = 0;

		checkRejected(executor, board, config, g2::Status::BadDivider,
			"secondBusFrameDivider of 0 with the override taken");
	}

	/* ---------------- row 4: the divider must equal the build constant when the
	 * override is NOT taken. A Scheduler built with a divider of 1 against a
	 * build constant of 4 was legal, and a golden render taken through it passed
	 * while the manifest recorded 4. */
	{
		g2::Scheduler::Config config;
		config.testOverride          = false;
		config.secondBusFrameDivider = G2_SECOND_BUS_FRAME_DIVIDER + 1u;

		checkRejected(executor, board, config, g2::Status::BadDivider,
			"secondBusFrameDivider unequal to the build constant without the override");
	}

	/* ---------------- row 5: the hop must equal the build constant when the
	 * override is NOT taken. */
	{
		g2::Scheduler::Config config;
		config.testOverride = false;
		config.hopFrames    = G2_CHAIN_HOP_FRAMES + 1u;

		checkRejected(executor, board, config, g2::Status::BadHopFrames,
			"hopFrames unequal to the build constant without the override");
	}

	/* ---------------- rows 4 and 5, the companion case: WITH the override the
	 * same two values are ACCEPTED.
	 *
	 * Register rows 9 and 10 require tests at H = 1 and H = 2 and at divider 1
	 * and 4, which a strict equality would forbid. The override is the only way
	 * to satisfy both, so it is driven in the ACCEPTING direction here as well
	 * as in the rejecting direction above. */
	{
		g2::Scheduler::Config config;
		config.testOverride          = true;
		config.hopFrames             = 2;
		config.secondBusFrameDivider = 1;

		checkAccepted(executor, board, config, "hop 2 and divider 1 with the override taken");
	}

	/* ---------------- row 6: the DSP count is fixed at 8.
	 *
	 * The job array is exactly 8 and holds the DSP contexts only, so every other
	 * value is rejected -- including the 4-DSP machine BRD-15 records as a real
	 * configuration. */
	{
		const unsigned driven[] = { 0u, 4u, 9u };

		for(const unsigned count : driven)
		{
			g2::Scheduler::Config config;
			config.dspCount = count;

			char what[128];
			std::snprintf(what, sizeof(what), "dspCount of %u", count);
			checkRejected(executor, board, config, g2::Status::BadDspCount, what);
		}
	}

	/* ---------------- row 6 against row 8: THE ORDERING DECISION, PINNED.
	 *
	 * A dspCount of 0 satisfies two rows at once. `D_chain` is
	 * `(N - 1) * hopFrames` over unsigned members, so at a count of 0 the
	 * subtraction wraps and the lookahead bound is exceeded by the same Config.
	 * The decision taken by this task is that the factory reports the FIRST
	 * failing row in the order the plan writes the table, and the count row is
	 * the sixth of that table while the lookahead row is the eighth.
	 *
	 * The case below is deliberately not folded into the loop above. There it
	 * would read as one more count value; here it is the ordering rule. */
	{
		g2::Scheduler::Config config;
		config.dspCount = 0;

		check(chainDelay(config) + kDelayCodec + config.lookaheadFrames > kLookaheadBound,
			"a dspCount of 0 does put the unsigned chain delay above the lookahead bound");

		checkRejected(executor, board, config, g2::Status::BadDspCount,
			"dspCount of 0 reports the COUNT row and not the lookahead row");
	}

	/* ---------------- the same ordering rule, WITHOUT the accumulator width in
	 * the premise.
	 *
	 * The case above holds only while the factory accumulates in 64 bits: in 32
	 * bits the wrapped delay plus the default lookahead of 1 wraps back to 0,
	 * the bound is satisfied, and a factory that ran the bound first would still
	 * answer BadDspCount by falling through. A lookahead this far above the
	 * bound exceeds it in EITHER width, so this case pins the order and nothing
	 * else -- and the two together separate the order from the arithmetic. */
	{
		g2::Scheduler::Config config;
		config.dspCount        = 0;
		config.lookaheadFrames = 16386;

		check(static_cast<unsigned>(chainDelay(config)) + static_cast<unsigned>(kDelayCodec)
				+ config.lookaheadFrames > static_cast<unsigned>(kLookaheadBound),
			"a dspCount of 0 exceeds the lookahead bound in 32-bit arithmetic too, at this lookahead");

		checkRejected(executor, board, config, g2::Status::BadDspCount,
			"dspCount of 0 reports the COUNT row whatever width the bound is computed in");
	}

	/* ---------------- row 7: a rational with a zero denominator.
	 *
	 * Both rationals reach create() and both are checked. The invariant is not
	 * checked inside alloc(), which runs once for each context for each quantum
	 * and must carry no branch that cannot fire. */
	{
		g2::Scheduler::Config config;
		config.dspRate.den = 0;

		checkRejected(executor, board, config, g2::Status::BadRational, "dspRate with den == 0");
	}
	{
		g2::Scheduler::Config config;
		config.mcuRate.den = 0;

		checkRejected(executor, board, config, g2::Status::BadRational, "mcuRate with den == 0");
	}

	/* ---------------- row 8: the lookahead bound.
	 *
	 * Above 16,384 frames the framework silently truncates the reported latency
	 * and logs that audio will be out of sync, and no other test in this design
	 * would see it. The bound is on the sum being ABOVE 16,384, so the pair
	 * below straddles it: exactly at the bound, and one frame past it. */
	{
		g2::Scheduler::Config config;
		const uint64_t        headroom = kLookaheadBound - chainDelay(config) - kDelayCodec;

		config.lookaheadFrames = static_cast<unsigned>(headroom);
		checkAccepted(executor, board, config, "a lookahead exactly AT the bound");

		config.lookaheadFrames = static_cast<unsigned>(headroom) + 1u;
		checkRejected(executor, board, config, g2::Status::BadLookahead, "a lookahead one frame ABOVE the bound");
	}

	/* ---------------- row 9: the maximum host block.
	 *
	 * The plugin wrapper computes B at run time and the codec queues carry the
	 * capacity lookaheadFrames + B, so a B of 0 sizes them by the lookahead
	 * alone. */
	{
		g2::Scheduler::Config config;
		config.maxHostBlockFrames = 0;

		checkRejected(executor, board, config, g2::Status::BadMaxHostBlock, "maxHostBlockFrames of 0");
	}

	/* ---------------- row 10: the backend.
	 *
	 * SCH-17's rule, now reported through a status: create() succeeds only when
	 * backend == Backend::Jit AND g_useJIT is true. This case is unconditional,
	 * because the Interpreter enumerator is rejected on every build. */
	{
		g2::Scheduler::Config config;
		config.backend = g2::Backend::Interpreter;

		checkRejected(executor, board, config, g2::Status::BadBackend, "Backend::Interpreter");
	}

	if(g_failures != 0)
	{
		std::printf("t0_construction_rejection: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_construction_rejection: all %d cases passed\n", g_cases);
	return 0;
}
