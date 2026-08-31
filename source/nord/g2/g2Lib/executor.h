/* The Executor interface and the serial executor.
 *
 * One method: run these eight jobs and return when all of them have returned.
 *
 * The job is a plain function pointer plus a typed context pointer. No
 * std::function, so no allocation and no indirect ownership. The Scheduler
 * builds the job array once, at construction, and passes the same array every
 * quantum.
 *
 * The error channel is the context and not a return value. A job never throws
 * and never returns a value: a fault writes ctx->fault and returns immediately,
 * and the Scheduler reads every job's fault after run() returns. The Executor
 * itself neither reads nor reports a fault.
 *
 * The serial executor runs the jobs in order on the calling thread, and creates
 * no thread. A worker thread would make job completion order depend on the host
 * scheduler, which ends the bit-exactness claim this design makes at the 96 kHz
 * Q23 integer boundary. A parallel implementation is a separate class that
 * satisfies this interface and reports isSerial() false.
 *
 * No floating-point type appears on this surface: the determinism claim is made
 * at the Q23 integer boundary.
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
	/* The job array is exactly 8 and holds the DSP contexts only. The panel and
	 * the MCU both run serially in the Scheduler, outside the Executor.
	 *
	 * dspCount is therefore fixed at 8 and every other value is rejected,
	 * including the 4-DSP machine, which is a real configuration. The narrow
	 * answer is chosen over the general one because nothing on this path needs
	 * a variable count: the base machine presents eight responders and there is
	 * no "count what answers" fallback. When a variable count is wanted, every
	 * array follows dspCount in one change and
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
		 * run() is not RE-ENTRANT. A job must never call run(). */
		virtual void run(const Job* jobs, size_t count) noexcept = 0;

		/* True for the serial executor. The determinism test runs the same
		 * workload through both and compares. */
		virtual bool isSerial() const noexcept = 0;
	};

	/* The serial executor. It runs the jobs in order on the calling thread and
	 * it owns nothing.
	 *
	 * The re-entry counters are carried in every build type and exposed, and
	 * that is a decision with a measured reason rather than a convenience.
	 * Design section 13.10.3 says "debug builds assert this with a depth
	 * counter". The DEFAULT BUILD of this TREE is Release and DEFINES NDEBUG,
	 * so an assert() is not in the translation unit at all: a check whose
	 * predicate is "the debug build caught it" passes against a tree in which
	 * the property was never written. So the counter is always present, the
	 * re-entry is REFUSED rather than asserted, and t0_executor reads the
	 * exposed value in a release build as well as a debug build.
	 *
	 * A refused re-entry runs no job. Running the inner array would dispatch a
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
