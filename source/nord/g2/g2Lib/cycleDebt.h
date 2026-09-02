/* The cycle-debt rule: the cycle part of one quantum, for one context.
 *
 * It is a function template rather than a method because neither DspContext
 * nor McuContext has a `run` member. `Run` fills the ctx.run(want) role and is
 * invoked exactly once per non-idle quantum as
 * run(static_cast<uint32_t>(want)), returning the cycles actually spent. Two
 * callables fill the role:
 *
 *   - g2::runDspCycles  an adapter this project writes, because
 *                       dsp56k::DSP::exec has no budgeted call;
 *   - Board::runMcu     a direct call that forwards mcf5307_exec, which
 *                       already takes a cycle budget.
 *
 * Both return uint32_t, so `spent` below cannot be negative and the widening
 * to int64_t is exact.
 *
 * The invariant at every quantum boundary is
 *
 *     0 <= ctx.debt < maxDispatchCost(context)
 *
 * where maxDispatchCost is the cost of the longest single dispatch unit the
 * context's backend can issue. That upper half is not a value this file owns:
 * it is unmeasured, and dsp56300's shipped default config leaves
 * maxInstructionsPerBlock uncapped, so no bound read from the build exists.
 * Every test that names the bound takes it from its own fixture; this file
 * states the shape of the debt and enforces no numeric cap.
 *
 * runQuantum returns the cycles spent this quantum -- spent in the running
 * branch, 0 in the want <= 0 branch, which executes no emulated cycle at all.
 * Neither call site may depend on the return for its emulated state: it is a
 * measurement, and the state that decides the next quantum is carried in
 * ctx.debt and ctx.acc, which this function mutates.
 *
 * The debt is never charged for a synchronous compile. A just-in-time compile
 * costs host wall-clock time and produces zero emulated cycles, so there is no
 * emulated overshoot to carry, and no wall clock may be consulted to convert
 * the compile duration into cycles. spent comes entirely from the
 * role-filler's return, which is a count of emulated cycles and nothing else.
 */

#pragma once

#include <cstdint>

#include "g2/timebase.h"

namespace g2
{
	/* One quantum of the cycle-debt rule for one context. Mutates the context
	 * (acc, debt, longDispatchQuanta) and returns the emulated cycles spent
	 * this quantum (0 in the want <= 0 branch).
	 *
	 * budget comes from alloc() and the rational accumulator; want subtracts
	 * the carried debt; the want <= 0 branch runs nothing, pays the debt down by one whole
	 * allocation and counts a long-dispatch quantum; the running branch asks
	 * the role-filler for want cycles, carries whatever it actually spent, and
	 * floors the result at zero so an idle period can never turn into a burst
	 * in a later quantum. No floating point appears and no wall clock is read.
	 *
	 * Ctx must expose four members with these exact types:
	 *
	 *   Rational  rate;   the cycles-per-frame rational
	 *   uint32_t  acc;    the rational accumulator
	 *   int64_t   debt;   the cycle debt
	 *   uint64_t  longDispatchQuanta;
	 *
	 * DspContext and McuContext both carry exactly these four, which is why
	 * the template compiles against both without a common base class.
	 *
	 * Run is invoked as run(static_cast<uint32_t>(want)) and returns the
	 * cycles actually spent (uint32_t in both role-fillers).
	 */
	template <typename Ctx, typename Run>
	int64_t runQuantum(Ctx& ctx, Run&& run) noexcept
	{
		const int64_t budget = static_cast<int64_t>(alloc(ctx.rate, &ctx.acc));
		const int64_t want   = budget - ctx.debt;

		if(want <= 0)
		{
			/* The previous quantum already overran this quantum's whole
			 * budget: the long-dispatch condition. Run nothing, count it, and
			 * pay the debt down. */
			ctx.debt -= budget;
			++ctx.longDispatchQuanta;
			return 0;
		}

		/* The role-filler returns uint32_t, so spent cannot be negative and
		 * the widening is exact. */
		const int64_t spent =
			static_cast<int64_t>(run(static_cast<uint32_t>(want)));

		ctx.debt = spent - want;   /* the signed carry-over               */
		if(ctx.debt < 0)
			ctx.debt = 0;          /* no credit banking                   */

		return spent;
	}
}
