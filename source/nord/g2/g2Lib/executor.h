/* executor.h -- the Executor interface and the serial executor. Task SCH-7.
 * Design sections 13.3 and 13.10.3.
 *
 * ONE METHOD: run these eight jobs and return when all of them have returned.
 * Three things that sentence left out are here -- an error channel, a statement
 * about re-entry, and a guarantee that no allocation happens for each quantum.
 *
 * THE JOB IS A PLAIN FUNCTION POINTER PLUS A TYPED CONTEXT POINTER. No
 * std::function, so no allocation and no indirect ownership. The Scheduler
 * builds the job array once, at construction, and passes the same array every
 * quantum.
 *
 * THE ERROR CHANNEL IS THE CONTEXT AND NOT A RETURN VALUE. A job never throws
 * and never returns a value: a fault writes ctx->fault and returns
 * immediately, and the Scheduler reads every job's fault after run() returns.
 * The Executor itself neither reads nor reports a fault -- it has no state to
 * report one from.
 *
 * NO THREAD IS CREATED ANYWHERE IN THIS FILE OR IN serialExecutor.cpp, and
 * NEITHER <thread> NOR std::thread IS NAMED. The serial executor runs the jobs
 * in order ON THE CALLING THREAD. A discarded branch of this work gave the
 * Executor its own std::thread and a submit() that threw; both contradict the
 * bit-exactness claim this design makes at the 96 kHz Q23 integer boundary,
 * and both are refused here. A parallel implementation is a SEPARATE class
 * (PERF-6) that satisfies this interface and reports isSerial() false.
 *
 * NO FLOATING-POINT TYPE APPEARS ON THIS SURFACE. The determinism claim is
 * made at the Q23 integer boundary.
 *
 * Ownership   The caller owns the Executor and passes it to the Scheduler by
 *             reference. The Scheduler never destroys it.
 * Lifetime    Must outlive the Scheduler.
 * Threading   run() is called on the scheduler thread. A parallel
 *             implementation may use its own workers inside run(), but run()
 *             returns only when every job has returned, so no job outlives the
 *             call.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "dspContext.h"

namespace g2
{
	/* THE JOB ARRAY IS EXACTLY 8 AND IT HOLDS THE DSP CONTEXTS ONLY. The panel
	 * and the MCU both run serially in the Scheduler, outside the Executor.
	 *
	 * dspCount is therefore FIXED at 8 and SCH-18 rejects every other value,
	 * including the 4-DSP machine BRD-15 records as a real configuration. The
	 * narrow answer is chosen over the general one because nothing in this
	 * milestone path needs a variable count: the base machine presents eight
	 * responders and there is no "count what answers" fallback. WHEN A
	 * VARIABLE COUNT IS WANTED, every array follows dspCount in one change and
	 * this comment is the trigger to make it. */
	inline constexpr size_t kJobCount = 8;

	class Executor
	{
	public:
		virtual ~Executor() = default;

		using JobFn = void (*)(JobContext* ctx) noexcept;

		struct Job
		{
			JobFn       fn;
			JobContext* ctx;
		};

		/* Runs every job exactly once. Returns only when all have returned.
		 *
		 * Jobs are independent within one quantum: no job may observe another
		 * job's state. That is what makes a parallel implementation give the
		 * same answer as a serial one.
		 *
		 * run() IS NOT RE-ENTRANT. A job must never call run(). */
		virtual void run(const Job* jobs, size_t count) noexcept = 0;

		/* True for the serial executor. The determinism test runs the same
		 * workload through both and compares. */
		virtual bool isSerial() const noexcept = 0;
	};

	/* THE SERIAL EXECUTOR. It runs the jobs in order on the calling thread and
	 * it owns nothing.
	 *
	 * THE RE-ENTRY COUNTERS ARE CARRIED IN EVERY BUILD TYPE AND EXPOSED, and
	 * that is a decision with a measured reason rather than a convenience.
	 * Design section 13.10.3 says "debug builds assert this with a depth
	 * counter". THE DEFAULT BUILD OF THIS TREE IS Release AND DEFINES NDEBUG,
	 * so an assert() is not in the translation unit at all: a check whose
	 * predicate is "the debug build caught it" passes against a tree in which
	 * the property was never written. So the counter is always present, the
	 * re-entry is REFUSED rather than asserted, and t0_executor reads the
	 * exposed value in a release build as well as a debug build. SCH-18 states
	 * the same principle for the construction rejections and gives the same
	 * reason.
	 *
	 * A REFUSED RE-ENTRY RUNS NO JOB. Running the inner array would dispatch a
	 * job while another job was still on the stack, which breaks the
	 * independence premise that makes the serial and the parallel executor
	 * bit-identical. Returning without running is the only behaviour that
	 * keeps it. */
	class SerialExecutor final : public Executor
	{
	public:
		void run(const Job* jobs, size_t count) noexcept override;
		bool isSerial() const noexcept override;

		/* The live depth. 0 outside run(), 1 inside a dispatched job. */
		uint32_t depth() const noexcept;

		/* How many calls to run() were refused because one was already in
		 * progress. STICKY: nothing clears it. */
		uint64_t reentryCount() const noexcept;

	private:
		uint32_t m_depth      = 0;
		uint64_t m_reentries  = 0;
	};
}
