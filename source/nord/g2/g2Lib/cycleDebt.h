/* cycleDebt.h -- the cycle-debt rule. Task SCH-12.
 * Design section 13.4.6, acceptance text in section 18.2.
 *
 * THE ONE SHARED QUANTUM-LOOP BLOCK, DECLARED HERE SO THAT "ONE BLOCK USED
 * TWICE" NAMES SOMETHING. The budget/want/debt block below is what design
 * section 13.4.6 calls "the CYCLE part of one quantum, for one context". It
 * is a FUNCTION TEMPLATE, and exactly two call sites instantiate it -- the
 * DSP job (SCH-11, g2Lib/test/t0_dsp_job_order.cpp) and the MCU quantum
 * (SCH-30, g2Lib/test/t0_mcu_debt.cpp). Neither DspContext nor McuContext has
 * a `run` member, so the shared entity has to be this template and not a
 * method.
 *
 * THE TWO ROLE-FILLERS, AND NOTHING ELSE ACCEPTS THE ROLE. `Run` fills the
 * `ctx.run(want)` role of section 13.4.6, and it is invoked exactly once per
 * non-idle quantum as run(static_cast<uint32_t>(want)). It returns the cycles
 * actually spent. The role is filled by exactly two callables:
 *
 *   - g2::runDspCycles   (SCH-8)  an adapter this project writes, because
 *                                 dsp56k::DSP::exec has no budgeted call;
 *   - Board::runMcu      (BRD-21) a direct call that forwards mcf5307_exec,
 *                                 which already takes a cycle budget.
 *
 * Both return uint32_t, so `spent` below cannot be negative and the widening
 * to int64_t is exact. SCH-11 and SCH-30 share this one block; a third
 * implementation would not be "the same rule applied twice", and a reader who
 * wrote one from memory would drift from this block in some way nobody can
 * diff.
 *
 * THE INVARIANT, AND THE FIXTURE IT COMES FROM. At every quantum boundary
 *
 *     0 <= ctx.debt < maxDispatchCost(context)
 *
 * where maxDispatchCost is the cost of the longest single dispatch unit the
 * context's backend can issue. THE UPPER HALF IS NOT A VALUE THIS FILE
 * OWNS. maxDispatchCost is measurement register row 1 and has NO value until
 * spike criterion SPK-5 reports; section 1.3 rule 1 forbids inventing one, and
 * dsp56300's shipped default config leaves maxInstructionsPerBlock UNCAPPed,
 * so no bound read from the build exists today. Every test that names the
 * bound takes it from its own FIXTURE, this file only states the shape of the
 * debt and enforces no numeric cap.
 *
 * WHY THE WANT <= 0 BRANCH RETURNS 0. spent is the number of EMULATED cycles
 * the role-filler reported this quantum, and the want <= 0 branch executes no
 * emulated cycle at all, so it spent 0. THE RETURN IS EMPHASISED HERE BECAUSE
 * AN EARLIER REVISION LEFT IT UNSTATED (plan finding IM-6): runQuantum returns
 * the cycles spent this quantum -- spent in the running branch, 0 in the
 * want <= 0 branch. Neither call site may depend on it for its emulated
 * state -- it is a measurement, and the state that decides the next quantum is
 * carried in ctx.debt and ctx.acc, which this function mutates.
 *
 * THE DEBT IS NEVER CHARGED FOR A SYNCHRONOUS COMPILE. A just-in-time compile
 * costs host wall-clock time and produces zero emulated cycles, so there is no
 * emulated overshoot to carry and no wall clock may be consulted to convert
 * the compile duration into cycles (section 13.5 premise 6). spent comes
 * entirely from the role-filler's return, which is a count of emulated cycles
 * and nothing else. This TU includes <cstdint> and g2/timebase.h and nothing
 * that reads the host clock, and the test asserts the same about its own
 * fixture.
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
	 * THE BLOCK IS EXACTLY THE ONE DESIGN SECTION 13.4.6 STATES. budget comes
	 * from alloc() and the rational accumulator; want subtracts the carried
	 * debt; the want <= 0 branch runs nothing, pays the debt down by one whole
	 * allocation and counts a long-dispatch quantum; the running branch asks
	 * the role-filler for want cycles, carries whatever it actually spent, and
	 * floors the result at zero so an idle period can never turn into a burst
	 * in a later quantum. No floating point appears and no wall clock is read.
	 *
	 * Ctx must expose these members with these exact types:
	 *
	 *   Rational  rate;   the cycles-per-frame rational, section 13.4.1
	 *   uint32_t  acc;    the rational accumulator, section 13.4.1
	 *   int64_t   debt;   the cycle debt, section 13.4.6
	 *   uint64_t  longDispatchQuanta;   the rule 4 counter
	 *
	 * DspContext (SCH-6) and McuContext (SCH-30) both already carry exactly
	 * these, which is why the template compiles against both without a
	 * common base class.
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
			 * budget. Rule 4: this is the long-dispatch condition and it is
			 * counted. Run nothing and pay the debt down. */
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
