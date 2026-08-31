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
 * Steps 1 and 3 run even when step 2 runs nothing. The want <= 0 branch of
 * cycleDebt.h pays a debt down and executes no emulated cycle, and the frame
 * cadence does not depend on it: the scheduler owns the cadence, so a
 * long-dispatch quantum still transmits what the stale transmit registers
 * carry.
 *
 * Step 2 instantiates the g2::runQuantum template and does not re-implement
 * it.
 *
 * No EsaiClock is constructed for either port. The scheduler drives the ESAI
 * frame and no clock does.
 *
 * secondBusFrameDivider is never 0: Scheduler::create returns
 * Status::BadDivider and no object for that value, so the modulo cannot divide
 * by zero.
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

		/* 2. One quantum of the cycle-debt rule; runDspCycles fills the
		 * ctx.run role. Adds no frame advance of its own. */
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
