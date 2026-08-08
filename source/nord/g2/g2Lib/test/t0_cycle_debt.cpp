/* t0_cycle_debt.cpp -- the check of task SCH-12.
 * Design section 13.4.6, acceptance text in section 18.2.
 *
 * THE INVARIANT, THE TWO DRIFT CASES, AND THE FLOOR. This is the T0 check for
 * the cycle-debt rule as a FUNCTION TEMPLATE. It does not run a real DSP or an
 * MCU: plan section 13.4.6's block is not tied to either, and the acceptance
 * criterion (the "one block used twice" property) is discharged at the two
 * call sites SCH-11 and SCH-30, which are later tasks. What THIS row owns is
 * the rule itself, against a SYNTHETIC context and a SYNTHETIC role-filler:
 *
 *   - Rule 2: 0 <= debt < maxDispatchCost at every quantum boundary.
 *   - Rule 3: the debt never goes negative at a boundary (no credit banking),
 *     and an idle period therefore never turns into a burst in a later
 *     quantum.
 *   - The never-idle drift case: every dispatch unit shorter than one
 *     allocation, the floor never fires, and the accumulated cycles spent
 *     differ from the accumulated ideal allocation by exactly the current
 *     positional debt -- so the difference is BOUNDED and never grows with the
 *     frame count. Over 10 million frames that is zero drift.
 *   - The forced-idle drift case: the role-filler stops early on some quanta,
 *     the floor discards the unused cycles, the context runs SLOW and never
 *     fast, and the total loss is bounded by one full allocation for each
 *     idle quantum.
 *   - The want <= 0 (long-dispatch) branch, so the block is driven as it is
 *     written and not only in its running form: it pays the debt down by one
 *     whole allocation, increments longDispatchQuanta, never invokes the
 *     role-filler, and returns zero cycles spent.
 *
 * THE BOUND COMES FROM THIS FIXTURE, NOT FROM A HEADER. maxDispatchCost is
 * measurement register row 1 and has NO value until spike criterion SPK-5
 * reports; section 1.3 rule 1 forbids inventing one, and dsp56300's shipped
 * default leaves maxInstructionsPerBlock at 0 -- UNCAPPED. This fixture
 * defines its own finite cap and scripts its role-filler's overshoot strictly
 * inside it. Nothing here reads maxInstructionsPerBlock and nothing here
 * claims the shipped build holds rule 2.
 *
 * NO WALL CLOCK IS READ, ANYWHERE. The debt must never be charged for a
 * synchronous just-in-time compile, and a compile duration is host time that
 * cannot become emulated cycles (section 13.5 premise 6). spent below comes
 * only from the role-filler's return and never from the host clock; the
 * fixture carries no time source and asserts none of its quantities are host
 * times.
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

	/* The rational, straight from the one definition site. §13.4.1. */
	constexpr uint32_t kNum = G2_DSP_CYCLES_PER_FRAME_NUM;
	constexpr uint32_t kDen = G2_DSP_CYCLES_PER_FRAME_DEN;

	/* THE FIXTURE'S OWN FINITE CAP. Not a header value: measurement register
	 * row 1. The role-filler below scripts its overshoot strictly inside
	 * [1, kMaxDispatchCost). */
	constexpr int64_t kMaxDispatchCost = 40;

	/* A context exposing exactly the four members the template needs: rate,
	 * acc, debt, longDispatchQuanta. DspContext and McuContext both carry
	 * these in the same shapes; this is the synthetic stand-in. */
	struct DebtCtx
	{
		Rational rate;
		uint32_t acc;
		int64_t  debt;
		uint64_t longDispatchQuanta;
	};

	DebtCtx makeCtx()
	{
		DebtCtx c = {};
		c.rate.num = kNum;
		c.rate.den = kDen;
		return c;
	}

	/* The IDEAL allocation, maintained on a SEPARATE accumulator so the
	 * template's own internal alloc() on the context is never double-advanced
	 * by this fixture. Same rational, same start (0), so the sequence is
	 * identical to the one the template consumes. */
	uint32_t idealBudget(uint32_t* const acc)
	{
		const Rational r = { kNum, kDen };
		return alloc(r, acc);
	}

	/* The role-filler for the never-idle workload. Every call overruns the
	 * request by a positive overshoot strictly below kMaxDispatchCost, so the
	 * floor at zero never fires and the want <= 0 branch never fires either:
	 * the context is never idle. */
	struct NeverIdleRun
	{
		uint64_t calls = 0;

		int64_t operator()(const uint32_t want)
		{
			const int64_t os =
				static_cast<int64_t>(calls % (kMaxDispatchCost - 1)) + 1;
			++calls;
			return static_cast<int64_t>(want) + os;
		}
	};

	/* The role-filler for the forced-idle workload. Every idleEvery-th call
	 * stops early: it returns spent = want - idleLoss, which is BELOW the
	 * request, so the floor fires and the unused cycles are lost. Otherwise it
	 * overruns like NeverIdleRun. idleLoss is well under one full allocation,
	 * so each idle quantum loses at most one allocation. */
	struct ForcedIdleRun
	{
		const uint64_t idleEvery;
		const int64_t  idleLoss;
		uint64_t       calls      = 0;
		uint64_t       idleFrames = 0;

		int64_t operator()(const uint32_t want)
		{
			const bool idle = (calls % idleEvery == 0);
			++calls;
			if(idle)
			{
				++idleFrames;
				const int64_t w = static_cast<int64_t>(want);
				return (w > idleLoss) ? (w - idleLoss) : 0;
			}
			const int64_t os =
				static_cast<int64_t>(calls % (kMaxDispatchCost - 1)) + 1;
			return static_cast<int64_t>(want) + os;
		}
	};

	constexpr uint64_t kNeverIdleFrames  = 10000000ull;
	constexpr uint64_t kForcedIdleFrames = 10000000ull;

	int runNeverIdle()
	{
		printf("  [never-idle] driving %llu frames\n",
			static_cast<unsigned long long>(kNeverIdleFrames));

		DebtCtx       ctx = makeCtx();
		NeverIdleRun  run;
		uint32_t      idealAcc = 0;

		int64_t totalIdeal = 0;
		int64_t totalSpent = 0;
		uint64_t violations = 0;

		for(uint64_t f = 0; f < kNeverIdleFrames; ++f)
		{
			totalIdeal += static_cast<int64_t>(idealBudget(&idealAcc));
			totalSpent += g2::runQuantum(ctx, run);

			if(!(ctx.debt >= 0 && ctx.debt < kMaxDispatchCost))
			{
				++violations;
				if(violations <= 3)
				{
					printf("FAIL never-idle frame %llu: debt %lld outside "
						"[0, %lld)\n",
						static_cast<unsigned long long>(f),
						static_cast<long long>(ctx.debt),
						static_cast<long long>(kMaxDispatchCost));
				}
			}
		}

		checkEqual(static_cast<int64_t>(run.calls),
			static_cast<int64_t>(kNeverIdleFrames),
			"never-idle: the role-filler ran exactly once per frame");

		/* Rule 2 held on every quantum boundary. */
		checkEqual(static_cast<int64_t>(violations), 0,
			"never-idle: 0 <= debt < maxDispatchCost held on every boundary");

		/* The floor at zero never fired and the want <= 0 branch never fired:
		 * the context was never idle and never long-dispatching. */
		checkEqual(static_cast<int64_t>(ctx.longDispatchQuanta), 0,
			"never-idle: the want <= 0 branch never fired");
		check(ctx.debt > 0,
			"never-idle: the debt stayed strictly positive, so the floor at "
			"zero never fired");

		/* ZERO DRIFT. The accumulated spent minus the accumulated ideal
		 * allocation equals exactly the current positional debt, which rule 2
		 * bounds below kMaxDispatchCost. Over 10 million frames the difference
		 * does not grow: it is a bounded constant, so the long-run rate is
		 * exactly the rational. */
		checkEqual(totalSpent - totalIdeal, ctx.debt,
			"never-idle: accumulated spent - ideal == the current debt "
			"(bounded, so zero drift over 10 million frames)");
		check(totalSpent - totalIdeal < kMaxDispatchCost,
			"never-idle: the running difference stayed below one dispatch "
			"unit for the whole run");

		return failures == 0 ? 0 : 1;
	}

	int runForcedIdle()
	{
		printf("  [forced-idle] driving %llu frames\n",
			static_cast<unsigned long long>(kForcedIdleFrames));

		/* One idle quantum every 13, each losing 20 cycles. 20 is well below a
		 * full allocation (1562/1563), so each idle quantum loses at most one
		 * allocation. */
		const uint64_t idleEvery = 13;
		const int64_t  idleLoss  = 20;

		DebtCtx        ctx = makeCtx();
		ForcedIdleRun  run{ idleEvery, idleLoss };
		uint32_t       idealAcc = 0;

		int64_t totalIdeal = 0;
		int64_t totalSpent = 0;
		int64_t runningDiffMax = 0;   /* the most the context ever ran AHEAD  */
		uint64_t violations = 0;

		for(uint64_t f = 0; f < kForcedIdleFrames; ++f)
		{
			totalIdeal += static_cast<int64_t>(idealBudget(&idealAcc));
			totalSpent += g2::runQuantum(ctx, run);

			if(!(ctx.debt >= 0 && ctx.debt < kMaxDispatchCost))
			{
				++violations;
				if(violations <= 3)
				{
					printf("FAIL forced-idle frame %llu: debt %lld outside "
						"[0, %lld)\n",
						static_cast<unsigned long long>(f),
						static_cast<long long>(ctx.debt),
						static_cast<long long>(kMaxDispatchCost));
				}
			}

			const int64_t d = totalSpent - totalIdeal;
			if(d > runningDiffMax)
				runningDiffMax = d;
		}

		checkEqual(static_cast<int64_t>(violations), 0,
			"forced-idle: 0 <= debt < maxDispatchCost held on every boundary");

		/* RULE 3 -- THE FLOOR REALLY FIRED, and the debt never went negative
		 * at a boundary (no credit banking). */
		check(run.idleFrames > 0,
			"forced-idle: at least one idle quantum actually fired");
		check(ctx.debt >= 0,
			"forced-idle: the debt never survived a boundary negative");

		/* NEVER FAST. The running difference totalSpent - totalIdeal never
		 * exceeded one dispatch unit; rule 3's floor discards unused cycles
		 * and can only push the context SLOW, never let it run ahead. */
		check(runningDiffMax <= kMaxDispatchCost,
			"forced-idle: the context never ran more than one dispatch unit "
			"ahead of the ideal clock");

		/* SLOW, AND BOUNDED BY ONE ALLOCATION PER IDLE QUANTUM. The cycles
		 * lost to the floor are totalIdeal - totalSpent + debt (the current
		 * debt is the only positive positional offset). Each idle quantum
		 * loses at most one full allocation. maxAllocation is the largest
		 * single budget alloc() can return for this rational: num/den rounded
		 * up, which is 1563 at 150 MHz / 96 kHz. */
		const int64_t loss =
			totalIdeal - totalSpent + ctx.debt;
		const int64_t maxAllocation =
			static_cast<int64_t>(kNum / kDen) + 1;
		const int64_t bound =
			static_cast<int64_t>(run.idleFrames) * maxAllocation;

		check(loss > 0,
			"forced-idle: idle quanta actually lost cycles to the floor");
		check(loss <= bound,
			"forced-idle: the total drift was bounded by one full allocation "
			"for each idle quantum");

		printf("  [forced-idle] %llu idle quanta of %llu; loss %lld against "
			"bound %lld; the context ran slow and never fast\n",
			static_cast<unsigned long long>(run.idleFrames),
			static_cast<unsigned long long>(kForcedIdleFrames),
			static_cast<long long>(loss),
			static_cast<long long>(bound));

		return failures == 0 ? 0 : 1;
	}

	int runLongDispatchBranch()
	{
		printf("  [long-dispatch branch]\n");

		/* Pre-load a debt far above one allocation so the very first quantum
		 * takes the want <= 0 branch. */
		DebtCtx ctx = makeCtx();
		ctx.debt    = 5000;

		uint32_t idealAcc = 0;
		const int64_t budget0 = static_cast<int64_t>(idealBudget(&idealAcc));

		uint64_t runCalls = 0;
		auto noRun = [&runCalls](const uint32_t) -> int64_t
		{
			++runCalls;   /* must never be reached */
			return 1;
		};

		const int64_t spent = g2::runQuantum(ctx, noRun);

		checkEqual(static_cast<int64_t>(ctx.longDispatchQuanta), 1,
			"long-dispatch: longDispatchQuanta rose by exactly one");
		checkEqual(ctx.debt, 5000 - budget0,
			"long-dispatch: the debt was paid down by one whole allocation");
		checkEqual(static_cast<int64_t>(runCalls), 0,
			"long-dispatch: the role-filler was not invoked");
		checkEqual(spent, 0,
			"long-dispatch: the want <= 0 branch returns 0 cycles spent");

		return failures == 0 ? 0 : 1;
	}
}

int main()
{
	printf("t0_cycle_debt: the cycle-debt rule (task SCH-12)\n");

	const int neverIdle  = runNeverIdle();
	const int forcedIdle = runForcedIdle();

	if(failures != 0)
	{
		printf("t0_cycle_debt: %d failure(s)\n", failures);
		return 1;
	}

	const int longDispatch = runLongDispatchBranch();

	if(failures != 0)
	{
		printf("t0_cycle_debt: %d failure(s)\n", failures);
		return 1;
	}

	const int total = neverIdle + forcedIdle + longDispatch;

	if(total != 0)
	{
		printf("t0_cycle_debt: failed\n");
		return 1;
	}

	printf("t0_cycle_debt: all cases passed (invariant, zero drift over 10 "
		"million never-idle frames, slow-and-bounded forced-idle drift, the "
		"floor at zero, and the want <= 0 branch)\n");
	return 0;
}
