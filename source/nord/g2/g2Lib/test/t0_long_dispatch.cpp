/* The long-dispatch counter, driven as the branch is written. It fires
 * exactly when one dispatch unit of the emulated
 * instruction stream costs more than one frame's allocation. When it fires,
 * the quantum's want == budget - debt is <= 0, so the want <= 0 branch runs
 * no emulated cycle, pays the carried debt down by one whole allocation, and
 * increments `longDispatchQuanta` by exactly one. The branch repeats for every
 * consecutive quantum the carried debt still overruns, and once the debt has
 * been paid below the next allocation the running branch resumes and the debt
 * is back inside its debt bound.
 *
 * THE spike. A single long dispatch unit -- one call that overruns its request
 * by several full allocations -- is exactly the "the previous quantum already
 * overran this quantum's whole budget" case. It leaves a debt equal to
 * the overshoot, and every quantum whose want is still <= 0 after it takes the
 * long branch and pays it down. The role-filler's later calls overrun by a
 * small amount strictly below kMaxDispatchCost, which is precisely the shape
 * that lets the debt return inside the bound once the spike is exhausted.
 *
 * The bound comes from this fixture, not from a header: maxDispatchCost is
 * unmeasured, and dsp56300's shipped default configuration leaves
 * maxInstructionsPerBlock at 0, uncapped. This row names a finite
 * kMaxDispatchCost it defines below and asserts the recovered debt against it.
 * Neither DspContext nor McuContext is involved: the check drives the
 * g2::runQuantum template through a synthetic context and role-filler.
 *
 * The count is double-checked against an independent branch model. The exact
 * number of long-dispatch quanta a spike produces is asserted once against a
 * hard-coded expectation (one full allocation is about 1562/1563, so a 1580
 * overshoot exhausts in one quantum, 4000 in two, 5000 in three) and once
 * against BranchModel, a pure re-derivation of the long-dispatch count and debt
 * bookkeeping that never calls g2::runQuantum. A regression that changed the
 * template's branch would then be caught by two independent witnesses rather
 * than passing because the code and an expectation copied from it drifted
 * together.
 *
 * No case here reads a clock. longDispatchQuanta detects one dispatch unit
 * longer than one frame's allocation, which says nothing about host wall-clock
 * time; CallbackTimer is what measures that.
 */

#include "cycleDebt.h"

#include "g2/timebase.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const int64_t observed, const int64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %lld, expected %lld\n", what,
				static_cast<long long>(observed),
				static_cast<long long>(expected));
			++failures;
		}
	}

	/* The rational, straight from the one definition site. */
	constexpr uint32_t kNum = G2_DSP_CYCLES_PER_FRAME_NUM;
	constexpr uint32_t kDen = G2_DSP_CYCLES_PER_FRAME_DEN;

	/* The fixture's own finite cap, not a header value. A role-filler call
	 * overruns its request strictly inside [1, kMaxDispatchCost), and a
	 * recovered debt must be inside the same [0, kMaxDispatchCost). The spike
	 * overshoot is deliberately many allocations -- the long-dispatch
	 * condition -- and while it is being paid down the debt sits above this
	 * cap, returning inside the debt bound afterwards. */
	constexpr int64_t kMaxDispatchCost = 40;

	/* A context exposing exactly the four members the template needs: rate,
	 * acc, debt, longDispatchQuanta. DspContext and McuContext carry these in
	 * the same shapes; this is the synthetic stand-in. */
	struct LongCtx
	{
		Rational rate;
		uint32_t acc;
		int64_t  debt;
		uint64_t longDispatchQuanta;
	};

	LongCtx makeCtx()
	{
		LongCtx c = {};
		c.rate.num = kNum;
		c.rate.den = kDen;
		return c;
	}

	/* The ideal allocation, maintained on a separate accumulator so the
	 * template's own internal alloc() on the context is never double-advanced
	 * by this fixture. Same rational, same start (0), so the sequence is
	 * identical to the one the template consumes. */
	uint32_t idealBudget(uint32_t* const acc)
	{
		const Rational r = { kNum, kDen };
		return alloc(r, acc);
	}

	/* The spiked role-filler. Call 0 is the single long dispatch unit: it
	 * overruns the request by `spikeOvershoot`, which is several allocations.
	 * Every later call overruns by a small positive amount strictly below
	 * kMaxDispatchCost, so once the debt has been paid down the context
	 * resumes running and stays inside the debt bound. */
	struct SpikeRun
	{
		const int64_t spikeOvershoot;
		uint64_t      calls = 0;

		int64_t operator()(const uint32_t want)
		{
			if(calls == 0)
			{
				++calls;
				return static_cast<int64_t>(want) + spikeOvershoot;
			}
			const int64_t os =
				static_cast<int64_t>(calls % (kMaxDispatchCost - 1)) + 1;
			++calls;
			return static_cast<int64_t>(want) + os;
		}
	};

	/* The pure, independent model of the long-dispatch count and bookkeeping. It is
	 * derived from the spike overshoot and the frame-allocation sequence
	 * Without calling g2::runQuantum, so a regression in the template's branch
	 * is caught by a witness that cannot inherit the bug. `budgets` walks the
	 * Same allocation sequence (same rational, same starting accumulator) and
	 * skips the spike frame's own budget: the spike frame runs (its want is
	 * its budget), and only the carried debt that follows gates the subsequent
	 * quanta, so the model consumes budgets[1..]. */
	struct BranchModel
	{
		int64_t  debt;
		uint64_t longQuanta;

		BranchModel(const int64_t spike, const uint32_t* budgets,
			const uint64_t budgetCount) : debt(spike), longQuanta(0)
		{
			for(uint64_t i = 0; i < budgetCount; ++i)
			{
				const int64_t b = static_cast<int64_t>(budgets[i]);
				if(debt >= b)
				{
					debt -= b;
					++longQuanta;
				}
				else
				{
					break;
				}
			}
		}
	};

	/* One spike at frame 0, then enough running frames for the debt to return
	 * inside its bound. The assertions hold over the whole window. */
	constexpr uint64_t kFrames = 1 + 48;

	int runSpikeScenario(const int64_t spikeOvershoot,
		const uint64_t expectedLongQuanta, const char* const name)
	{
		printf("  [%s] spike of %lld cycles over one allocation\n", name,
			static_cast<long long>(spikeOvershoot));

		LongCtx  ctx = makeCtx();
		SpikeRun run{ spikeOvershoot };
		uint32_t idealAcc = 0;

		/* The frame-allocation sequence for the BranchModel. budgets[0] is the
		 * spike frame's own budget and is skipped by the model; the next seven
		 * are more than enough for any spike in this row. */
		uint32_t budgets[8] = {};
		for(uint64_t i = 0; i < 8; ++i)
			budgets[i] = idealBudget(&idealAcc);
		idealAcc = 0;

		uint64_t longFrames   = 0;
		uint64_t runCalls     = 0;
		uint64_t badBoundary  = 0;
		bool     ranAfterSpike = false;

		for(uint64_t f = 0; f < kFrames; ++f)
		{
			const int64_t  budget    = static_cast<int64_t>(idealBudget(&idealAcc));
			const uint64_t lqBefore  = ctx.longDispatchQuanta;
			const int64_t  debtBefore = ctx.debt;

			const int64_t spent = g2::runQuantum(ctx, run);

			const uint64_t lqDelta   = ctx.longDispatchQuanta - lqBefore;
			const int64_t  debtDelta = ctx.debt - debtBefore;

			if(lqDelta == 1)
			{
				/* The want <= 0 branch: exactly one quantum-count, and the debt
				 * is paid down by exactly one whole allocation. */
				++longFrames;
				checkEqual(static_cast<int64_t>(debtDelta), -budget,
					"each long-dispatch quantum pays the debt down by exactly "
					"one whole allocation");
				checkEqual(spent, 0,
					"each long-dispatch quantum runs no emulated cycle and "
					"returns 0 spent");
			}
			else
			{
				/* The running branch: the counter does not move, and only this
				 * branch may invoke the role-filler. */
				checkEqual(static_cast<int64_t>(lqDelta), 0,
					"a running quantum never moves longDispatchQuanta");
				++runCalls;
				if(lqBefore > 0)
					ranAfterSpike = true;
			}

			/* The debt bound holds afterwards, not during. Through the spike's
			 * pay-down the debt sits above the bound by
			 * construction; once the context has resumed running (a running
			 * quantum after the counter first moved) every boundary is inside
			 * [0, kMaxDispatchCost). */
			if(ranAfterSpike && lqDelta == 0)
			{
				if(!(ctx.debt >= 0 && ctx.debt < kMaxDispatchCost))
					++badBoundary;
			}
		}

		/* A dispatch unit longer than one allocation really fired, and it
		 * fired exactly the count the independent BranchModel derives from
		 * the same budget sequence -- and exactly the hard-coded expectation
		 * too. Both are independent of the template's own branch. */
		const BranchModel model(spikeOvershoot, budgets + 1, 7);
		check(longFrames > 0,
			"a dispatch unit longer than one allocation actually drove the "
			"want <= 0 branch");
		checkEqual(static_cast<int64_t>(longFrames),
			static_cast<int64_t>(expectedLongQuanta),
			"the observed long-dispatch count equals the hard-coded "
			"expectation");
		checkEqual(static_cast<int64_t>(longFrames),
			static_cast<int64_t>(model.longQuanta),
			"the observed count matches the independent branch model exactly");

		/* The counter is never coalesced: its final value is the count. */
		checkEqual(static_cast<int64_t>(ctx.longDispatchQuanta),
			static_cast<int64_t>(longFrames),
			"longDispatchQuanta totals one per long-dispatch quantum");

		/* The role-filler ran once per running frame and never inside a long
		 * quantum (already asserted per frame), and the debt returned inside
		 * the fixture's debt bound afterwards. */
		checkEqual(static_cast<int64_t>(runCalls),
			static_cast<int64_t>(kFrames - longFrames),
			"the role-filler ran exactly once per running frame and never in "
			"a long-dispatch quantum");
		check(ranAfterSpike,
			"the context resumed running after the spike was paid down");
		check(badBoundary == 0,
			"the debt stayed inside [0, maxDispatchCost) at every running "
			"boundary after recovery");

		printf("  [%s] %llu long-dispatch quanta, %llu running calls, final "
			"debt %lld inside the fixture bound\n", name,
			static_cast<unsigned long long>(longFrames),
			static_cast<unsigned long long>(runCalls),
			static_cast<long long>(ctx.debt));

		return failures == 0 ? 0 : 1;
	}
}

int main()
{
	printf("t0_long_dispatch: the long-dispatch counter\n");

	/* One whole allocation is about 1562/1563 at 150 MHz / 96 kHz. A spike of
	 * 1580 exceeds one allocation, so it exhausts in exactly one long-dispatch
	 * quantum; 4000 exceeds two in a row, so exactly two; 5000 exceeds three
	 * in a row, so exactly three. Each count is asserted against both the
	 * hard-coded value here and the independent BranchModel. */
	const int r1 = runSpikeScenario(1580, 1, "single-quantum spike");
	const int r2 = runSpikeScenario(4000, 2, "two-quantum spike");
	const int r3 = runSpikeScenario(5000, 3, "three-quantum spike");

	if(failures != 0)
	{
		printf("t0_long_dispatch: %d failure(s)\n", failures);
		return 1;
	}

	const int total = r1 + r2 + r3;
	if(total != 0)
	{
		printf("t0_long_dispatch: failed\n");
		return 1;
	}

	printf("t0_long_dispatch: all cases passed (the counter rises exactly one "
		"per long-dispatch quantum, each pays one whole allocation, and the "
		"debt returns inside the fixture's debt bound after recovery)\n");
	return 0;
}
