/* t0_construction_rejection.cpp -- the construction rejections.
 *
 * `Scheduler::create` is the SINGLE rejection point, because every rejectable
 * value arrives through `Scheduler::Config`. Each case below asserts BOTH
 * halves of one row: a null return AND the exact `g2::Status`.
 *
 * No case here is a language assert() and no case catches an exception. A
 * release build removes an assert(), and the design forbids
 * throwing, so the status out-param is the whole observable and this file runs
 * identically in a release build and a debug build.
 *
 * Why both halves of every row. A null return alone does not distinguish the
 * rows from each other, and a status alone does not establish that no object
 * was handed back. Either half on its own leaves a defect it cannot name.
 *
 * One config can fail two rows at once, and which one answers is a decision.
 * A `dspCount` of 0 is such a Config: the DSP count row rejects it, and
 * `D_chain = (N - 1) * hopFrames` is unsigned, so at a count of 0 the
 * subtraction wraps and the lookahead bound is exceeded by the same Config. The
 * factory reports the FIRST failing row in the order the table is written,
 * and the count row is the sixth of that table while the lookahead row is the
 * eighth, so the answer is `BadDspCount`.
 *
 * The overlap itself depends on the width the sum is accumulated at. In 32 bits
 * the wrapped delay plus a small `L` wraps a SECOND time and lands back UNDER
 * the bound, so the two rows would not overlap at all. The width is a premise of
 * the overlap rather than a detail of it, which is why it is written down beside
 * the value it governs rather than left to the reader.
 *
 * What this file does not establish. It does not establish that the Config
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
#include <vector>

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

	/* The lookahead bound, and the two terms that meet `L` under
	 * it. `D_codec` is 0 by construction and is written as a named term rather
	 * than omitted, exactly as section 20's formula writes it. */
	constexpr uint64_t kLookaheadBound = 16384;
	constexpr uint64_t kDelayCodec     = 0;

	/* The subtraction is `unsigned`, because ChainAdapter's `dspCount` and
	 * `hopFrames` members are, and the widening happens after it. That order is
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

	/* The success half. It is conditional on the build for the reason the backend
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

	/* ---------------- row 2: a hop of zero, with the override taken.
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

	/* ---------------- row 3: a divider of zero, with the override taken.
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

	/* ---------------- row 5 again, with the override left unnamed.
	 *
	 * Row 5 above writes `false` into the field, so it holds whatever the
	 * struct's initialiser says. This case drives the same hop of 2 and names
	 * nothing but the hop, so what it holds is the DEFAULT: a caller that never
	 * mentions the escape is held to the build constants.
	 *
	 * What this does not establish: it fixes the initialiser and not the field.
	 * A caller that names the override is a different Config and this case says
	 * nothing about it. */
	{
		g2::Scheduler::Config config;
		config.hopFrames = G2_CHAIN_HOP_FRAMES + 1u;

		checkRejected(executor, board, config, g2::Status::BadHopFrames,
			"hopFrames unequal to the build constant with the override left unnamed");
	}

	/* ---------------- row 6: the DSP count is fixed at 8.
	 *
	 * The job array is exactly 8 and holds the DSP contexts only, so every other
	 * value is rejected -- including the 4-DSP machine that is a real
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

	/* ---------------- row 6 against row 8: The ordering decision, PINNED.
	 *
	 * A dspCount of 0 satisfies two rows at once. `D_chain` is
	 * `(N - 1) * hopFrames` over unsigned members, so at a count of 0 the
	 * subtraction wraps and the lookahead bound is exceeded by the same Config.
	 * The factory reports the FIRST
	 * failing row in the order the table is written, and the count row is
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
	 * The case above rests on the 64-bit accumulation: in 32 bits the wrapped
	 * delay plus the default lookahead of 1 wraps back to 0 and the bound is
	 * satisfied, so at that lookahead the order and the arithmetic are not
	 * separable. The lookahead below is far enough above the bound to exceed it
	 * in EITHER width, which is why the value is chosen here rather than left at
	 * the default.
	 *
	 * What this does not establish: it fixes the order alone and says nothing
	 * about the width the factory accumulates at. */
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

	/* ---------------- the chain order, when the caller names one.
	 *
	 * Empty is the production value and means "derive it from the machine", so
	 * the default Config case above already covers the accepted-empty half. What
	 * is rejected is a named order that is not one: the wrong length, a slot
	 * that does not exist, or a slot named twice. The last is the one with no
	 * downstream symptom -- a duplicate leaves one slot unwired while two chain
	 * positions drive another, and nothing further along reports either.
	 *
	 * The accepted case is paired with them rather than left to the default
	 * Config, because a factory that rejected every named order would satisfy
	 * the refusals alone.
	 */
	{
		g2::Scheduler::Config config;

		std::vector<unsigned> order(config.dspCount);
		for(unsigned i = 0; i < config.dspCount; ++i)
			order[i] = i;

		config.chainOrder = order;
		checkAccepted(executor, board, config, "a chain order that is a permutation of the slots");

		config.chainOrder = order;
		config.chainOrder.pop_back();
		checkRejected(executor, board, config, g2::Status::BadChainOrder,
			"a chain order shorter than dspCount");

		config.chainOrder = order;
		config.chainOrder.push_back(0u);
		checkRejected(executor, board, config, g2::Status::BadChainOrder,
			"a chain order longer than dspCount");

		config.chainOrder = order;
		config.chainOrder.back() = config.dspCount;
		checkRejected(executor, board, config, g2::Status::BadChainOrder,
			"a chain order naming a slot that does not exist");

		config.chainOrder = order;
		config.chainOrder.back() = config.chainOrder.front();
		checkRejected(executor, board, config, g2::Status::BadChainOrder,
			"a chain order naming one slot twice");
	}

	/* ---------------- row 8: the lookahead bound.
	 *
	 * Above 16,384 frames the framework silently truncates the reported latency
	 * and logs that audio will be out of sync. The bound is on the sum being
	 * ABOVE 16,384, so the pair below straddles it: exactly at the bound, and
	 * one frame past it. */
	{
		g2::Scheduler::Config config;
		const uint64_t        headroom = kLookaheadBound - chainDelay(config) - kDelayCodec;

		config.lookaheadFrames = static_cast<unsigned>(headroom);
		checkAccepted(executor, board, config, "a lookahead exactly AT the bound");

		config.lookaheadFrames = static_cast<unsigned>(headroom) + 1u;
		checkRejected(executor, board, config, g2::Status::BadLookahead, "a lookahead one frame ABOVE the bound");
	}

	/* ---------------- row 8 at a chain delay that needs more than 32 bits.
	 *
	 * The override removes the equality row, which is the only thing bounding
	 * `hopFrames` at all, so a Config can reach the bound carrying a chain delay
	 * above 32 bits while every earlier row still holds -- the count is 8 and no
	 * subtraction wraps. The two premises below are asserted because they
	 * DISAGREE: the same Config is above the bound in 64 bits and lands back
	 * under it in 32.
	 *
	 * The hop is chosen so that `(N - 1) * hopFrames` lands just above 2^32 and
	 * the lookahead stays at its default. A large lookahead would carry the sum
	 * past the bound on its own and the chain term would then be free to be any
	 * value at all; at this hop the term itself is what crosses.
	 *
	 * What this does not establish: it fixes the width of the sum and the shape
	 * of the chain term, and nothing about the range of either input. The
	 * factory refuses no hop and no lookahead for being large on its own. */
	{
		g2::Scheduler::Config config;
		config.testOverride = true;
		config.hopFrames    = 0x24924925u;

		check(chainDelay(config) + kDelayCodec + config.lookaheadFrames > kLookaheadBound,
			"the chosen hop does put the 64-bit sum above the lookahead bound");

		check(static_cast<unsigned>(chainDelay(config)) + static_cast<unsigned>(kDelayCodec)
				+ config.lookaheadFrames <= static_cast<unsigned>(kLookaheadBound),
			"the same Config lands back UNDER the bound when the sum is accumulated in 32 bits");

		checkRejected(executor, board, config, g2::Status::BadLookahead,
			"a chain delay above 2^32 with the override taken");
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
	 * The backend rule, reported through a status: create() succeeds only when
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
