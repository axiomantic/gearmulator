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
 * The sub-budget is a share of what remains, and the divisor is derived. Each
 * slot asks for its share of want MINUS what the quantum has already spent,
 * divided by the slots the frame has left to run; the slot count comes from
 * frameSlotBound, which reads the enables, the word counts and the second-bus
 * window that decide how many slots the helpers will actually run. Every
 * sub-call overshoots its own sub-budget by up to one dispatch unit, and
 * charging that overshoot to the slots that follow is what keeps the whole
 * quantum inside 0 <= debt < maxDispatchCost. A tail flush after step 3
 * delivers whatever a shorter-than-bounded frame left over, so the quantum
 * spends want plus at most one dispatch unit whatever the divisor says.
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
 * THE IDLE ROUTE (SCH-35) RESTORES WHAT SCH-34 REMOVED. On real hardware
 * the ESAI gates audio transfers, not core execution. When the audio port
 * has no enabled transmitters AND no enabled receivers -- the reset state
 * of every boot -- both helpers return before their loops and the quantum's
 * budget would never reach the core. dspJob then runs the budget DIRECTLY,
 * frame-granular at frame position, behind the same run gate; the two
 * routes are exclusive per quantum and share one reconciliation.
 *
 * THE LIMIT: this file configures the interleave and its idle fallback and
 * nothing else. It makes a correct waveform observable to a compiled guest;
 * it produces none.
 */

#include "dspContext.h"
#include "esaiFrame.h"

#include "dsp56kEmu/esai.h"
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

		/* The slot count this quantum's frame can cost, read from the same
		 * registers the two frame helpers read to decide their own loop
		 * counts. It is the divisor's only source.
		 *
		 * It is an upper bound and not an equality: a transmit frame costs
		 * getTxWordCount() + 1 slots except in the quantum after a transmitter
		 * enable, which costs one fewer because the guest's own control-register
		 * write already spent a slot. Bounding from above is the safe direction
		 * -- an over-estimate makes the interleave finer than the frame needs,
		 * while an under-estimate hands the leading slots a share computed
		 * against a frame that does not exist. */
		uint32_t frameSlotBound(const DspContext& _c, const bool _secondBus) noexcept
		{
			uint32_t slots = 0;

			if(_c.audioEsai->hasEnabledReceivers())
				slots += _c.audioEsai->getRxWordCount() + 1u;
			if(_c.audioEsai->hasEnabledTransmitters())
				slots += _c.audioEsai->getTxWordCount() + 1u;

			/* The second bus contributes its slots ONLY inside the window
			 * steps 1 and 3 advance it in, and the caller decides that window
			 * once so that one expression governs both. */
			if(_secondBus)
			{
				if(_c.secondEsai->hasEnabledReceivers())
					slots += _c.secondEsai->getRxWordCount() + 1u;
				if(_c.secondEsai->hasEnabledTransmitters())
					slots += _c.secondEsai->getTxWordCount() + 1u;
			}

			/* Never 0: it is a divisor, and an idle port takes the direct
			 * route rather than this one. */
			return slots > 0 ? slots : 1u;
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

		/* The second bus advances only inside the window
		 * ChainAdapter::advanceAll uses. It is decided ONCE, here, because the
		 * slot bound below and steps 1 and 3 must not be able to disagree
		 * about which ports this quantum touches. */
		const bool secondBus = c->frameIndex % c->secondBusFrameDivider == 0;

		/* The divisor is derived from the frame and is never a literal. The
		 * callback fires once per ESAI SLOT across all four frame halves and
		 * not once per DSP slot, so the count that generates it is whatever the
		 * enables, the word counts and the second-bus window make the helpers
		 * run. frameSlotBound reads the same registers the helpers read, so a
		 * change to any of them moves the divisor with it.
		 *
		 * The divisor is not load-bearing for correctness. Each sub-budget below
		 * is taken from what REMAINS of want and the tail flush after step 3
		 * delivers whatever the frame left undelivered, so the quantum spends
		 * want plus at most ONE dispatch unit whatever the divisor says. A wrong
		 * divisor can only make the interleave coarser or finer; it cannot make
		 * the quantum overspend, and it cannot violate the debt invariant. */
		c->slotBudgetDivisor = frameSlotBound(*c, secondBus);
		c->slotDispatches    = 0u;

		const std::function<void()> run = [&]() noexcept
		{
			const uint32_t slot = c->slotDispatches++;

			/* The share is of what remains, not of want. runDspCycles tests the
			 * cycle counter BEFORE each dispatch, so every sub-call returns at
			 * least its sub-budget and overshoots by up to one dispatch unit.
			 * Subdividing want itself would let those k overshoots accumulate
			 * into the debt; subdividing the remainder charges each overshoot
			 * to the slots that follow it, so only the dispatch that crosses
			 * want overshoots at all. */
			const int64_t remaining =
				want - static_cast<int64_t>(totalSpent);
			if(remaining <= 0)
				return;

			/* The slots still to come, from the same bound. It floors at 1 so
			 * a frame that outruns its bound gives its extra slots the whole
			 * remainder rather than dividing by zero. */
			const uint32_t left = c->slotBudgetDivisor > slot
				? c->slotBudgetDivisor - slot : 1u;

			totalSpent += runDspCycles(*c->dsp, subBudget(remaining, left, 0u));
		};

		/* THE IDLE ROUTE (SCH-35). On real hardware the ESAI gates audio
		 * transfers, not core execution. Both frame helpers return before
		 * their loops when their direction has no enabled channel, so an
		 * idle audio port leaves the interleave's callbacks unfired and
		 * the quantum's budget undelivered. The route is decided ONCE per
		 * quantum here, at the top, and a mid-quantum enable completes
		 * the chosen route: the enable writes align slot counters to the
		 * NEXT frame boundary, so no current-frame slot can appear. The
		 * AUDIO PORT ALONE gates -- secondEsai's enables decide nothing,
		 * which is the state a busy audio bus beside an idle second bus
		 * already survives -- and IDLE means no transmitters AND no
		 * receivers, not or: one enabled direction still runs its half's
		 * slots through the interleave below.
		 *
		 * THE TWO ROUTES ARE EXCLUSIVE PER QUANTUM. When the direct route
		 * fires it replaces this quantum's core execution entirely and
		 * all four helper calls run in their bare no-callback forms; a
		 * mixed quantum would deliver the budget twice against one want.
		 * Direct-run cycles join totalSpent BEFORE the single
		 * reconciliation, so ctx.debt reconciles over them exactly as it
		 * does over the interleave's per-slot spends.
		 *
		 * The direct run sits behind the SAME programLanded gate step 2
		 * has always sat behind. */
		const bool landed =
			c->programLanded != nullptr && *c->programLanded;
		const bool audioIdle =
			c->audioEsai->hasEnabledTransmitters() == 0 &&
			c->audioEsai->hasEnabledReceivers() == 0;

		/* 1. The receive half of the frame, interleaved with core
		 * execution when the interleave runs.
		 *
		 * THE RUN GATE. A NULL pointer is NOT LANDED, and that direction
		 * is the whole of the gate. Reading NULL as "landed" would run
		 * a slot whose program memory is zero-filled -- and 0x000000 is
		 * a no-operation on this core, so that slot faults nowhere and
		 * writes no log line. */
		if(landed && !audioIdle)
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

		/* 2. THE DIRECT ROUTE. Frame-granular, at frame position, the
		 * pre-SCH-34 shape restored for the idle-port quantum. A want <=
		 * 0 runs no cycle and the reconciliation below pays the debt
		 * down by the whole allocation, the same rule runQuantum uses. */
		if(landed && audioIdle && want > 0)
			totalSpent += runDspCycles(*c->dsp,
				static_cast<uint32_t>(want));

		/* 3. The transmit half of the frame, gated on the same window. */
		if(landed && !audioIdle)
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

		/* The tail flush. The slot bound is an upper bound, so a frame that
		 * costs fewer slots than it could -- the quantum after a transmitter
		 * enable -- ends the interleave with part of want undelivered. Those
		 * cycles are spent once, here, rather than dropped: an undelivered
		 * allocation floors the debt at zero and vanishes, which is a slow
		 * drift with no counter watching it.
		 *
		 * It cannot double-spend. It is guarded on the interleave having run
		 * this quantum and it asks only for the part of want that totalSpent
		 * does not already cover, so a quantum that delivered want reaches it
		 * with nothing to do. */
		if(landed && !audioIdle && want > static_cast<int64_t>(totalSpent))
			totalSpent += runDspCycles(*c->dsp, static_cast<uint32_t>(
				want - static_cast<int64_t>(totalSpent)));

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
