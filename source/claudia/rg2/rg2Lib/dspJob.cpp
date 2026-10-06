/* dspJob.cpp -- the DSP job body.
 *
 * The order inside the body is load-bearing, and the sample offset is derived
 * from exactly this order: the receive half of the frame, then the one step
 * that consumes emulated cycles, then the transmit half. The three are labelled
 * 1, 2 and 3 below and each carries the reasoning for what it does.
 *
 * The interleave moves step 2's execution into steps 1 and 3. The frame helpers
 * take a callback form that fires after each execTX and each execRX; this file
 * computes want at the top, subdivides it per ESAI slot, and reconciles the debt
 * once after both halves. runQuantum is not called.
 *
 * Steps 1 and 3 run even when step 2 runs nothing: the scheduler owns the frame
 * cadence, so a long-dispatch quantum still transmits what the stale transmit
 * registers carry. No EsaiClock is constructed for either port -- the scheduler
 * drives the ESAI frame and no clock does.
 */

#include "dspContext.h"
#include "esaiFrame.h"

#include "dsp56kEmu/esai.h"
#include "runDspCycles.h"
#include "cycleDebt.h"

#include <cstdint>
#include <functional>

namespace rg2
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

			/* The second bus contributes its slots only inside the window
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

	/* The parameter is deliberately not `JobContext* const`. MSVC mangles a
	 * top-level const on a pointer parameter into the symbol, so a definition
	 * spelled that way does not link against a caller that declares
	 * `dspJob(JobContext*)`; clang and gcc ignore the const. */
	void dspJob(JobContext* jobCtx) noexcept
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
		 * ChainAdapter::advanceAll uses. It is decided once, here, because the
		 * slot bound below and steps 1 and 3 must not be able to disagree
		 * about which ports this quantum touches.
		 *
		 * The divider is never 0: Scheduler::create answers Status::BadDivider
		 * and builds no object for that value, so this modulo cannot divide by
		 * zero. */
		const bool secondBus = c->frameIndex % c->secondBusFrameDivider == 0;

		/* The divisor is derived from the frame and is never a literal. The
		 * callback fires once per ESAI slot across all four frame halves and
		 * not once per DSP slot, so the count that generates it is whatever the
		 * enables, the word counts and the second-bus window make the helpers
		 * run. frameSlotBound reads the same registers the helpers read, so a
		 * change to any of them moves the divisor with it.
		 *
		 * The divisor is not load-bearing for correctness. Each sub-budget below
		 * is taken from what remains of want and the tail flush after step 3
		 * delivers whatever the frame left undelivered, so the quantum spends
		 * want plus at most one dispatch unit whatever the divisor says. A wrong
		 * divisor can only make the interleave coarser or finer; it cannot make
		 * the quantum overspend, and it cannot violate the debt invariant. */
		c->slotBudgetDivisor = frameSlotBound(*c, secondBus);
		c->slotDispatches    = 0u;

		const std::function<void()> run = [&]() noexcept
		{
			const uint32_t slot = c->slotDispatches++;

			/* The share is of what remains, not of want. runDspCycles tests the
			 * cycle counter before each dispatch, so every sub-call returns at
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

		/* On real hardware the ESAI gates audio transfers and not core
		 * execution, so an idle port must not stop the core. Both frame
		 * helpers return before their loops when their direction has no
		 * enabled channel -- the reset state of every boot -- which would
		 * leave the interleave's callbacks unfired and the quantum's budget
		 * undelivered, so an idle audio port takes a direct route instead.
		 *
		 * The route is decided once per quantum here, at the top, and a
		 * mid-quantum enable completes the chosen route: the enable writes
		 * align slot counters to the next frame boundary, so no
		 * current-frame slot can appear. The audio port alone gates --
		 * secondEsai's enables decide nothing, which is the state a busy
		 * audio bus beside an idle second bus already survives -- and idle
		 * means no transmitters and no receivers, not one or the other: a
		 * single enabled direction still runs its half's slots through the
		 * interleave below.
		 *
		 * The two routes are exclusive per quantum. When the direct route
		 * fires it replaces this quantum's core execution entirely and
		 * all four helper calls run in their bare no-callback forms; a
		 * mixed quantum would deliver the budget twice against one want.
		 * Direct-run cycles join totalSpent before the single
		 * reconciliation, so ctx.debt reconciles over them exactly as it
		 * does over the interleave's per-slot spends.
		 *
		 * The direct run sits behind the same programLanded gate step 2
		 * sits behind. */
		const bool landed =
			c->programLanded != nullptr && *c->programLanded;
		const bool audioIdle =
			c->audioEsai->hasEnabledTransmitters() == 0 &&
			c->audioEsai->hasEnabledReceivers() == 0;

		/* 1. The receive half of the frame, interleaved with core
		 * execution when the interleave runs.
		 *
		 * The run gate: a NULL pointer is not landed, and that direction
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

		/* 2. The direct route: frame-granular, at frame position. A
		 * want <= 0 runs no cycle and the reconciliation below pays the
		 * debt down by the whole allocation, the same rule runQuantum
		 * uses. */
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

		/* The long-dispatch quantum counter, and nothing else. When want <= 0
		 * every route above runs nothing -- the callback returns on its
		 * remaining <= 0 guard, the direct route is gated on want > 0 and the
		 * tail flush on want > totalSpent -- so totalSpent is 0 and the
		 * reconciliation above already paid the debt down by exactly one whole
		 * allocation: with totalSpent == 0 it reads debt = -want = debt - budget,
		 * which is the arithmetic rg2::runQuantum's want <= 0 branch performs.
		 * A second `debt -= budget` here would pay the same quantum down twice
		 * and, because it would land after the floor at zero, would carry the
		 * debt below zero whenever the carried debt equalled the budget --
		 * breaking the lower half of cycleDebt.h's invariant. The floor stays
		 * the last write to the debt in every route through this function. */
		if(want <= 0)
			++c->longDispatchQuanta;
	}
}
