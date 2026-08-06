/* t0_executor.cpp -- the check of task SCH-7.
 * Design sections 13.3 and 13.10.3.
 *
 * FOUR THINGS, AND THEY FAIL IN DIFFERENT WAYS ON PURPOSE.
 *
 * THE SURFACE. The four declarations SCH-7 names are held by their fully
 * qualified types: JobFn, Job, run and isSerial. A renamed method, a dropped
 * noexcept or a changed parameter list is a COMPILE error; a declared and
 * undefined method is a LINK error, because taking the address of a member
 * function odr-uses it.
 *
 * THE JOB ARRAY IS EXACTLY 8 and it holds the DSP contexts only. kJobCount is
 * asserted at compile time, and the order case below drives that many jobs.
 *
 * THE SERIAL EXECUTOR RUNS THE JOBS IN ORDER ON THE CALLING THREAD. The order
 * half is a trace. The calling-thread half is a thread_local marker that main
 * writes and every job reads: a job that ran on a worker thread reads the
 * initial value instead, so an executor that owned a thread FAILS this case
 * rather than passing it in silence. A discarded branch of this work gave the
 * Executor its own std::thread and a submit() that threw, and both contradict
 * the bit-exactness claim this design makes at the 96 kHz Q23 integer
 * boundary. NEITHER std::thread NOR <thread> APPEARS IN THIS FILE OR IN THE
 * FILES IT CHECKS.
 *
 * RUN IS NOT RE-ENTRANT, AND THE CHECK OF THAT IS A COUNTER RATHER THAN AN
 * ASSERTION. The default build of this tree is Release and defines NDEBUG, so
 * an assert() is not in the translation unit at all and a check whose predicate
 * is "the debug build caught it" passes against a tree in which the property
 * was never written. The executor therefore carries the depth counter IN EVERY
 * BUILD TYPE and exposes it, and this case reads the exposed value. The verdict
 * is the process exit status and a failure counter, in a release build as well
 * as a debug build.
 */

#include "executor.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	/* THE CALLING-THREAD WITNESS. A thread_local needs no <thread> and creates
	 * no thread. main writes the magic value; every job reads it. A job that
	 * ran anywhere but on the calling thread reads 0. */
	thread_local uint32_t callingThreadMarker = 0;

	constexpr uint32_t kMarker = 0xC0FFEEu;

	/* The trace every job appends to. The serial executor must produce
	 * 0, 1, 2 ... in the order the array carries. */
	unsigned traceLength = 0;
	unsigned trace[64]   = {};

	unsigned markerFailures = 0;

	/* The eight DSP contexts. This is the whole job array: the panel and the
	 * MCU run serially in the Scheduler and never enter the Executor. */
	g2::DspContext contexts[g2::kJobCount];

	/* THE POINTER RECOVERY IS THE POINT OF THE STANDARD-LAYOUT RULE. Job::ctx
	 * is a JobContext*, and the body recovers its DspContext from it with a
	 * reinterpret_cast and nothing else. A body that recovered the wrong
	 * object would append the wrong position and the order case would fail. */
	void recordingJob(g2::JobContext* const ctx) noexcept
	{
		if(callingThreadMarker != kMarker)
			++markerFailures;

		auto* const c = reinterpret_cast<g2::DspContext*>(ctx);

		if(traceLength < 64u)
			trace[traceLength++] = c->position;
	}

	/* THE RE-ENTRY CASE. This job calls run() on the executor that is running
	 * it. The call must be refused and counted, and the inner call must
	 * execute no job at all. */
	g2::SerialExecutor* reentryTarget    = nullptr;
	unsigned            innerJobRunCount = 0;
	unsigned            depthSeenInsideJob = 0xFFFFFFFFu;

	void innerJob(g2::JobContext*) noexcept
	{
		++innerJobRunCount;
	}

	void reenteringJob(g2::JobContext* const ctx) noexcept
	{
		auto* const c = reinterpret_cast<g2::DspContext*>(ctx);

		if(traceLength < 64u)
			trace[traceLength++] = c->position;

		depthSeenInsideJob = reentryTarget->depth();

		const g2::Executor::Job inner[1] = { { &innerJob, ctx } };
		reentryTarget->run(inner, 1u);
	}
}

/* ================ THE SURFACE */

static_assert(std::is_same_v<g2::Executor::JobFn,
		void (*)(g2::JobContext*) noexcept>,
	"Executor::JobFn is void(*)(JobContext*) noexcept. A JobFn that could "
	"throw would need an error channel the design does not have: a fault "
	"writes ctx->fault and returns.");

static_assert(std::is_same_v<decltype(g2::Executor::Job::fn),
		g2::Executor::JobFn>,
	"Executor::Job::fn is a JobFn.");
static_assert(std::is_same_v<decltype(g2::Executor::Job::ctx),
		g2::JobContext*>,
	"Executor::Job::ctx is a JobContext*, not a void*. The fault field has "
	"to live somewhere.");
static_assert(std::is_standard_layout_v<g2::Executor::Job>,
	"Job is a plain function pointer plus a typed context pointer. No "
	"std::function, so no allocation and no indirect ownership.");

static_assert(std::is_abstract_v<g2::Executor>,
	"Executor is an interface. run and isSerial are pure virtual.");
static_assert(std::has_virtual_destructor_v<g2::Executor>,
	"Executor is destroyed through a base pointer.");

static constexpr void (g2::Executor::*kRun)(const g2::Executor::Job*, size_t)
	noexcept = &g2::Executor::run;
static constexpr bool (g2::Executor::*kIsSerial)() const noexcept
	= &g2::Executor::isSerial;

/* THE JOB ARRAY IS EXACTLY 8. SCH-18 rejects every other dspCount, including
 * the 4-DSP machine BRD-15 records as a real configuration, because this
 * array, SCH-19's order table and SCH-20's context count are all fixed at 8. */
static_assert(g2::kJobCount == 8u,
	"The job array is exactly 8 and it holds the DSP contexts only.");

int main()
{
	callingThreadMarker = kMarker;

	g2::SerialExecutor executor;

	/* ---------------- the surface really is callable. */
	{
		g2::Executor* const e = &executor;

		check((e->*kIsSerial)(),
			"SerialExecutor::isSerial is true. The determinism test runs one "
			"workload through the serial and the parallel executor and "
			"compares, and this is what tells them apart.");

		/* A run of no jobs runs nothing and is not a re-entry. */
		(e->*kRun)(nullptr, 0u);

		checkEqual(traceLength, 0u, "a run of zero jobs executes no job");
		checkEqual(executor.reentryCount(), 0u,
			"a top-level run is not a re-entry");
		checkEqual(executor.depth(), 0u,
			"the depth counter returns to zero after run");
	}

	/* ---------------- eight jobs, in order, on the calling thread. */
	{
		traceLength    = 0;
		markerFailures = 0;

		g2::Executor::Job jobs[g2::kJobCount];

		for(unsigned k = 0; k < g2::kJobCount; ++k)
		{
			contexts[k]          = g2::DspContext{};
			contexts[k].position = k;
			jobs[k].fn           = &recordingJob;
			jobs[k].ctx          = &contexts[k].base;
		}

		executor.run(jobs, g2::kJobCount);

		checkEqual(traceLength, g2::kJobCount,
			"every job in the array ran exactly once");

		/* THE ORDER IS THE ASSERTION. An executor that ran the array
		 * backwards, that skipped one, or that ran one twice produces a
		 * different trace. */
		for(unsigned k = 0; k < traceLength && k < g2::kJobCount; ++k)
		{
			if(trace[k] != k)
			{
				printf("FAIL the serial executor ran position %u at index %u\n",
					trace[k], k);
				++failures;
			}
		}

		checkEqual(markerFailures, 0u,
			"every job ran ON THE CALLING THREAD. A job that ran on a worker "
			"thread reads the initial value of the thread_local marker.");

		checkEqual(executor.reentryCount(), 0u,
			"eight ordinary jobs are not a re-entry");
		checkEqual(executor.depth(), 0u,
			"the depth counter returns to zero after the array is done");
	}

	/* ---------------- run() is NOT re-entrant, and the counter says so in a
	 * release build.
	 *
	 * The inner call is refused: it runs no job and it raises the re-entry
	 * count by exactly one. The depth the job observes from inside its own
	 * dispatch is 1, which is what makes "the counter is live" a measured
	 * fact and not a comment. */
	{
		traceLength        = 0;
		markerFailures     = 0;
		innerJobRunCount   = 0;
		depthSeenInsideJob = 0xFFFFFFFFu;
		reentryTarget      = &executor;

		contexts[0]          = g2::DspContext{};
		contexts[0].position = 0;

		const g2::Executor::Job jobs[1] = { { &reenteringJob, &contexts[0].base } };

		executor.run(jobs, 1u);

		checkEqual(traceLength, 1u, "the outer job ran");
		checkEqual(depthSeenInsideJob, 1u,
			"a job observes a depth of exactly 1 while it is being dispatched");
		checkEqual(executor.reentryCount(), 1u,
			"the refused re-entry is counted, in a release build as well as a "
			"debug build");
		checkEqual(innerJobRunCount, 0u,
			"the refused re-entry executed no job at all");
		checkEqual(executor.depth(), 0u,
			"the depth counter returns to zero after the outer run");

		/* And the counter is sticky rather than a flag that the next run
		 * clears. */
		traceLength = 0;

		g2::Executor::Job plain[g2::kJobCount];

		for(unsigned k = 0; k < g2::kJobCount; ++k)
		{
			contexts[k]          = g2::DspContext{};
			contexts[k].position = k;
			plain[k].fn          = &recordingJob;
			plain[k].ctx         = &contexts[k].base;
		}

		executor.run(plain, g2::kJobCount);

		checkEqual(executor.reentryCount(), 1u,
			"a later ordinary run does not clear the re-entry count");
		checkEqual(traceLength, g2::kJobCount,
			"the executor still runs every job after a refused re-entry");
	}

	/* ---------------- run() returns only when every job has returned.
	 *
	 * A job that writes a stack object of main's is visible to main the moment
	 * run() returns. An implementation that deferred the work to a queue and
	 * returned would fail this and the order case together. */
	{
		traceLength = 0;

		g2::Executor::Job jobs[g2::kJobCount];

		for(unsigned k = 0; k < g2::kJobCount; ++k)
		{
			contexts[k]          = g2::DspContext{};
			contexts[k].position = k;
			jobs[k].fn           = &recordingJob;
			jobs[k].ctx          = &contexts[k].base;
		}

		executor.run(jobs, g2::kJobCount);

		checkEqual(traceLength, g2::kJobCount,
			"run() returned only after every job had returned");
	}

	if(failures != 0)
	{
		printf("t0_executor: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_executor: all cases passed\n");
	return 0;
}
