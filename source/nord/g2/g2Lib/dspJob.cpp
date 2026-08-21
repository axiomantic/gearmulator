/* dspJob.cpp -- the DSP job body. Task SCH-11. Design 13.10.3, 13.4.6.
 *
 * THE ORDER INSIDE THE BODY IS LOAD-BEARING, and it is the whole of this
 * check. Design section 12.3 derives the sample offset from exactly this
 * order and from no other:
 *
 *   1.  receiveDspFrame(*c->audioEsai), and, ONLY when
 *       c->frameIndex % c->secondBusFrameDivider == 0,
 *       receiveDspFrame(*c->secondEsai).
 *   2.  the budget/want/debt block, runDspCycles as ctx.run. This is the
 *       only step that consumes emulated cycles.
 *   3.  transmitDspFrame(*c->audioEsai), and, on the SAME condition as
 *       step 1, transmitDspFrame(*c->secondEsai).
 *
 * THE INTERLEAVE (SCH-34) MOVES STEP 2's EXECUTION INTO STEPS 1 AND 3.
 * The two frame helpers gain a callback form that invokes a caller-supplied
 * callback after EACH execTX and EACH execRX. dspJob computes want at the
 * top -- using the same formula runQuantum uses -- and passes a callback
 * that subdivides want into per-slot sub-budgets and calls runDspCycles
 * for each. After both halves complete, dspJob reconciles ctx.debt using
 * the same floor-at-zero rule runQuantum uses. runQuantum is not called.
 *
 * THE WANT FORMULA IS RESTATED RATHER THAN IMPORTED. cycleDebt.h is on no
 * Files: line in SCH-34, and the formula is the same computation: want =
 * alloc(ctx.rate, &ctx.acc) - ctx.debt. The debt is reconciled after both
 * halves: ctx.debt = totalSpent - want, floored at zero.
 *
 * THE SUB-BUDGET FLOOR. When want < slot count, each slot gets a dispatch
 * of at least 1 and the sub-budgets sum to the slot count rather than want.
 * The debt carries the overrun, which is what the debt is for. When want >=
 * slot count, each slot gets want / n plus the remainder distributed across
 * the leading slots, and the sub-budgets sum to want.
 *
 * STEPS 1 AND 3 RUN EVEN WHEN STEP 2 RUNS NOTHING. The want <= 0 branch
 * pays a debt down and executes no emulated cycle, and the frame cadence
 * does not depend on it: the scheduler owns the cadence, so a long-dispatch
 * quantum still transmits what the stale transmit registers carry.
 *
 * NO EsaiClock IS CONSTRUCTED FOR EITHER PORT. The scheduler drives the ESAI
 * frame and no clock does.
 *
 * THE DEREFERENCES ARE NOT DECORATION. Both frame calls take dsp56k::Esai&
 * and DspContext holds dsp56k::Esai*, so the star is what makes the body
 * compile. secondBusFrameDivider is never 0: Scheduler::create returns
 * Status::BadDivider and no object for that value, so the modulo cannot
 * divide by zero.
 *
 * THE LIMIT: this file configures the interleave and nothing else. It makes
 * a correct waveform observable to a compiled guest; it produces none.
 */

#include "dspContext.h"
#include "esaiFrame.h"
#include "runDspCycles.h"
#include "cycleDebt.h"

#include <cstdint>
#include <functional>

namespace g2
{
	namespace
	{
		/* Subdivides want across n slots. Each slot gets at least 1 when
		 * want > 0, the remainder distributed across the leading slots.
		 * When want < n, the floor overrides: every slot gets 1 and the
		 * sub-budgets sum to n rather than want. */
		uint32_t subBudget(const int64_t _want, const uint32_t _n,
			const uint32_t _slot)
		{
			if(_want <= 0)
				return 0;

			const auto uwant = static_cast<uint32_t>(_want);
			if(uwant < _n)
				return 1u;

			const uint32_t base = uwant / _n;
			const uint32_t rem  = uwant % _n;
			return base + (_slot < rem ? 1u : 0u);
		}
	}

	void dspJob(JobContext* const jobCtx) noexcept
	{
		auto* const c = reinterpret_cast<DspContext*>(jobCtx);

		/* Compute want at the top, using the same formula runQuantum uses.
		 * The interleave cannot inherit want from runQuantum because
		 * runQuantum is invoked once per quantum and the callback runs
		 * inside the frame helpers, which are steps 1 and 3. */
		const int64_t budget = static_cast<int64_t>(alloc(c->rate, &c->acc));
		const int64_t want   = budget - c->debt;

		uint64_t totalSpent = 0;
		uint32_t slotIndex = 0;

		const std::function<void()> run = [&]() noexcept
		{
			const auto sub = subBudget(want, 8u, slotIndex);
			++slotIndex;
			if(sub > 0)
				totalSpent += runDspCycles(*c->dsp, sub);
		};

		/* 1. The receive half of the frame, interleaved with core
		 * execution. The second bus advances only inside the window
		 * ChainAdapter::advanceAll uses. */
		const bool secondBus = c->frameIndex % c->secondBusFrameDivider == 0;

		/* THE RUN GATE. A NULL pointer is NOT LANDED, and that direction
		 * is the whole of the gate. Reading NULL as "landed" would run
		 * a slot whose program memory is zero-filled -- and 0x000000 is
		 * a no-operation on this core, so that slot faults nowhere and
		 * writes no log line. */
		if(c->programLanded != nullptr && *c->programLanded)
		{
			receiveDspFrame(*c->audioEsai, run);
			if(secondBus)
				receiveDspFrame(*c->secondEsai, run);
		}
		else
		{
			receiveDspFrame(*c->audioEsai);
			if(secondBus)
				receiveDspFrame(*c->secondEsai);
		}

		/* 2. When the program has not landed, the interleave does not run.
		 * The frame helpers still advance the ESAI. When want <= 0, the
		 * callback runs nothing and the debt is paid down by the whole
		 * allocation, which is the same rule runQuantum uses. */

		/* 3. The transmit half of the frame, gated on the same window. */
		if(c->programLanded != nullptr && *c->programLanded)
		{
			transmitDspFrame(*c->audioEsai, run);
			if(secondBus)
				transmitDspFrame(*c->secondEsai, run);
		}
		else
		{
			transmitDspFrame(*c->audioEsai);
			if(secondBus)
				transmitDspFrame(*c->secondEsai);
		}

		/* Reconcile the debt using the same floor-at-zero rule runQuantum
		 * uses. */
		c->debt = static_cast<int64_t>(totalSpent) - want;
		if(c->debt < 0)
			c->debt = 0;

		/* THE LONG-DISPATCH QUANTUM COUNTER. When want <= 0, the callback
		 * runs nothing and the debt is paid down by the whole allocation,
		 * which is the long-dispatch condition runQuantum counts. */
		if(want <= 0)
		{
			c->debt -= budget;
			++c->longDispatchQuanta;
		}
	}
}
