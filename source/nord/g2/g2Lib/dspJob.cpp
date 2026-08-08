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
 * STEPS 1 AND 3 RUN EVEN WHEN STEP 2 RUNS NOTHING. The want <= 0 branch of
 * cycleDebt.h pays a debt down and executes no emulated cycle, and the frame
 * cadence does not depend on it: the scheduler owns the cadence, so a
 * long-dispatch quantum still transmits what the stale transmit registers
 * carry. T0_dsp_job_order drives exactly that quantum.
 *
 * STEP 2 INSTANTIATES SCH-12's g2::runQuantum template and does NOT
 * re-implement it. This file and t0_mcu_debt.cpp are the template's only two
 * call sites; deleting this one leaves it with one instantiation, which is
 * the half of SCH-12's acceptance criterion that this file owns.
 *
 * NO EsaiClock IS CONSTRUCTED FOR EITHER PORT. The scheduler drives the ESAI
 * frame and no clock does.
 *
 * THE DEREFERENCES ARE NOT DECORATION. Both frame calls take dsp56k::Esai&
 * and DspContext holds dsp56k::Esai*, so the star is what makes the body
 * compile. secondBusFrameDivider is never 0: Scheduler::create returns
 * Status::BadDivider and no object for that value, so the modulo cannot
 * divide by zero.
 */

#include "dspContext.h"
#include "esaiFrame.h"
#include "runDspCycles.h"
#include "cycleDebt.h"

#include <cstdint>

namespace g2
{
	void dspJob(JobContext* const jobCtx) noexcept
	{
		auto* const c = reinterpret_cast<DspContext*>(jobCtx);

		/* 1. The receive half of the frame. The second bus advances only
		 * inside the window ChainAdapter::advanceAll uses. */
		receiveDspFrame(*c->audioEsai);
		if(c->frameIndex % c->secondBusFrameDivider == 0)
			receiveDspFrame(*c->secondEsai);

		/* 2. One quantum of the cycle-debt rule. SCH-12's template owns the
		 * block; runDspCycles (SCH-8) fills the ctx.run role. Adds no frame
		 * advance of its own. */
		const auto run = [c](const uint32_t want) noexcept -> uint32_t
		{
			return runDspCycles(*c->dsp, want);
		};
		(void) runQuantum(*c, run);

		/* 3. The transmit half of the frame, gated on the same window. */
		transmitDspFrame(*c->audioEsai);
		if(c->frameIndex % c->secondBusFrameDivider == 0)
			transmitDspFrame(*c->secondEsai);
	}
}
