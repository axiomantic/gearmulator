/* The DSP job body.
 *
 * The order inside the body is load-bearing: the sample offset is derived from
 * exactly this order and from no other.
 *
 *   1.  receiveDspFrame(*c->audioEsai), and, ONLY when
 *       c->frameIndex % c->secondBusFrameDivider == 0,
 *       receiveDspFrame(*c->secondEsai).
 *   2.  the budget/want/debt block, runDspCycles as ctx.run. This is the
 *       only step that consumes emulated cycles.
 *   3.  transmitDspFrame(*c->audioEsai), and, on the SAME condition as
 *       step 1, transmitDspFrame(*c->secondEsai).
 *
 * The interleave moves step 2's execution into steps 1 and 3. The two frame
 * helpers gain a callback form that invokes a caller-supplied callback after
 * EACH execTX and EACH execRX. runQuantum is not called.
 *
 * The want formula is restated rather than imported, and is the same
 * computation: want = alloc(ctx.rate, &ctx.acc) - ctx.debt. The debt is
 * reconciled after both halves: ctx.debt = totalSpent - want, floored at zero.
 *
 * The sub-budget floor: when want < slot count, each slot gets a dispatch of at
 * least 1 and the sub-budgets sum to the slot count rather than want. The debt
 * carries the overrun, which is what the debt is for. When want >= slot count,
 * each slot gets want / n plus the remainder distributed across the leading
 * slots, and the sub-budgets sum to want.
 *
 * Steps 1 and 3 run even when step 2 runs nothing. The want <= 0 branch
 * pays a debt down and executes no emulated cycle, and the frame cadence
 * does not depend on it: the scheduler owns the cadence, so a long-dispatch
 * quantum still transmits what the stale transmit registers carry.
 *
 * No EsaiClock is constructed for either port. The scheduler drives the ESAI
 * frame and no clock does.
 *
 * THE DEREFERENCES ARE NOT DECORATION. Both frame calls take dsp56k::Esai&
 * and DspContext holds dsp56k::Esai*, so the star is what makes the body
 * compile. secondBusFrameDivider is never 0: Scheduler::create returns
 * Status::BadDivider and no object for that value, so the modulo cannot
 * divide by zero.
 *
 * The idle route: on real hardware the ESAI gates audio transfers, not core
 * execution. When the audio port has no enabled transmitters AND no enabled
 * receivers -- the reset state of every boot -- both helpers return before
 * their loops and the quantum's budget would never reach the core. dspJob then
 * runs the budget DIRECTLY, frame-granular at frame position, behind the same
 * run gate; the two routes are exclusive per quantum and share one
 * reconciliation.
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

		/* The idle route. Both frame helpers return before their loops
		 * when their direction has no enabled channel, so an idle audio
		 * port leaves the interleave's callbacks unfired and the
		 * quantum's budget undelivered. The route is decided ONCE per
		 * quantum here, at the top, and a mid-quantum enable completes
		 * the chosen route: the enable writes align slot counters to the
		 * NEXT frame boundary, so no current-frame slot can appear. The
		 * audio port alone gates -- secondEsai's enables decide nothing
		 * -- and IDLE means no transmitters AND no receivers, not or: one
		 * enabled direction still runs its half's slots through the
		 * interleave below.
		 *
		 * The two routes are exclusive per quantum. When the direct route
		 * fires it replaces this quantum's core execution entirely and
		 * all four helper calls run in their bare no-callback forms; a
		 * mixed quantum would deliver the budget twice against one want.
		 * Direct-run cycles join totalSpent BEFORE the single
		 * reconciliation. */
		const bool landed =
			c->programLanded != nullptr && *c->programLanded;
		const bool audioIdle =
			c->audioEsai->hasEnabledTransmitters() == 0 &&
			c->audioEsai->hasEnabledReceivers() == 0;

		/* 1. The receive half of the frame, interleaved with core
		 * execution when the interleave runs. The second bus advances
		 * only inside the window ChainAdapter::advanceAll uses. */
		const bool secondBus = c->frameIndex % c->secondBusFrameDivider == 0;

		/* The run gate. A NULL pointer is NOT landed, and that direction
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

		/* 2. The direct route, frame-granular at frame position. A
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

		/* Reconcile the debt using the same floor-at-zero rule runQuantum
		 * uses. */
		c->debt = static_cast<int64_t>(totalSpent) - want;
		if(c->debt < 0)
			c->debt = 0;

		/* The long-dispatch quantum counter. When want <= 0, the callback
		 * runs nothing and the debt is paid down by the whole allocation,
		 * which is the long-dispatch condition runQuantum counts. */
		if(want <= 0)
		{
			c->debt -= budget;
			++c->longDispatchQuanta;
		}
	}
}
