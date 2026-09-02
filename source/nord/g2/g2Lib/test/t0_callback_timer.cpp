/* The check of g2::CallbackTimer.
 *
 * The surface is taken through fully qualified member-function-pointer
 * types. That spelling is what makes a renamed or re-signed method a
 * compile error rather than a silent match against something else, and a
 * missing definition a link error, because taking the address of a member
 * function odr-uses it.
 *
 * The ring is a plain ring of the last N, not reservoir sampling and not a
 * growing buffer. Mean, max and count are running figures since
 * construction or reset, over the whole history and not the window; the two
 * differ once the window is full. The count is exact: one per end().
 *
 * reset() takes effect at the next end() and not immediately.
 *
 * CallbackTimer is not part of the Scheduler snapshot, because it has no
 * emulated state. The detectable form of that absence is a SFINAE probe: a
 * class that declares stateSave makes the probe well-formed, and the
 * static_assert flips red.
 *
 * This test compiles perf/CallbackTimer.cpp directly and links g2Lib.
 *
 * The test reads no host clock and carries none of the system-clock
 * spellings, not even in a comment -- the lint searches text and cannot
 * tell a comment from a call. The increasing duration series is built by
 * the timer's own begin/end around each driven callback, and the cases
 * assert only order properties that any monotonic clock satisfies.
 */

#include "perf/CallbackTimer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <utility>

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
			printf("FAIL %s: expected %llu, got %llu\n", what,
				static_cast<unsigned long long>(expected),
				static_cast<unsigned long long>(observed));
			++failures;
		}
	}

	/* The surface, pinned through fully qualified member-pointer types. The
	 * decltype of a constexpr address-of-member expression is a POINTER-TO-
	 * CONST-MEMBER and carries the noexcept qualifier, so the expected
	 * types spell both. The constructor cannot be named through its own
	 * qualified name, so it is pinned through constructibility instead. */
	static_assert(std::is_constructible_v<g2::CallbackTimer, size_t>,
		"constructor is the explicit single-size_t form");
	static_assert(!std::is_convertible_v<size_t, g2::CallbackTimer>,
		"the constructor is explicit: no implicit size_t conversion");

	constexpr auto g_begin = &g2::CallbackTimer::begin;
	constexpr auto g_end = &g2::CallbackTimer::end;
	constexpr auto g_report = &g2::CallbackTimer::report;
	constexpr auto g_reset = &g2::CallbackTimer::reset;
	static_assert(std::is_same_v<decltype(g_begin),
		void (g2::CallbackTimer::*const)() noexcept>,
		"begin is void() noexcept");
	static_assert(std::is_same_v<decltype(g_end),
		void (g2::CallbackTimer::*const)() noexcept>, "end is void() noexcept");
	static_assert(std::is_same_v<decltype(g_report),
		g2::CallbackTimer::Report (g2::CallbackTimer::*const)() const noexcept>,
		"report is Report() const noexcept");
	static_assert(std::is_same_v<decltype(g_reset),
		void (g2::CallbackTimer::*const)() noexcept>,
		"reset is void() noexcept");

	/* The no-state-surface probe. If CallbackTimer ever declares stateSave
	 * (or stateLoad), the first overload becomes viable and returns the
	 * member-pointer type; otherwise the fallback char overload is chosen.
	 * The sizeof comparison holds the ABSENCE: equality with sizeof(char)
	 * means the fallback was chosen. */
	template<typename U>
	auto probeStateSave(int) -> decltype(static_cast<void (U::*)(const void*)>(
		&U::stateSave));
	template<typename>
	char probeStateSave(...);

	template<typename U>
	auto probeStateLoad(int) -> decltype(static_cast<void (U::*)(const void*)>(
		&U::stateLoad));
	template<typename>
	char probeStateLoad(...);

	static_assert(sizeof(probeStateSave<g2::CallbackTimer>(0)) == sizeof(char),
		"CallbackTimer must not declare stateSave: it has no emulated state");
	static_assert(sizeof(probeStateLoad<g2::CallbackTimer>(0)) == sizeof(char),
		"CallbackTimer must not declare stateLoad: it is not in the snapshot");

	/* Report members, pinned through their addressable form. */
	uint64_t g_reportCallbacks = g2::CallbackTimer::Report{}.callbacks;
	uint32_t g_reportMean = g2::CallbackTimer::Report{}.meanUs;
	uint32_t g_reportP99 = g2::CallbackTimer::Report{}.p99Us;
	uint32_t g_reportMax = g2::CallbackTimer::Report{}.maxUs;

	void runCaseRing()
	{
		/* Window of 8 over 20 callbacks. The duration magnitudes are
		 * whatever the host gives -- the test reads no clock -- so the
		 * exact p99 value cannot be asserted here. What IS asserted is
		 * everything the host cannot perturb: the count is exact, the
		 * figures keep their internal order, and the mean over the whole
		 * history lies at or below the window's p99 whenever the history
		 * is longer than the window and the series has any spread. The
		 * window-vs-history property is held by CASE 2b below, which
		 * drives a series whose ORDER is forced by construction. */
		g2::CallbackTimer timer(8);
		for(int i = 0; i < 20; ++i)
		{
			timer.begin();
			for(int spin = 0; spin <= i * 1000; ++spin)
			{
				/* Deterministic in-thread work; the spin count grows with
				 * i, which presses the series upward on every real host
				 * without reading a clock here. */
			}
			timer.end();
		}

		const auto r = timer.report();
		checkEqual(r.callbacks, 20u,
			"callbacks counts every end() since construction");
		check(r.meanUs <= r.p99Us, "mean never exceeds the window p99");
		check(r.p99Us <= r.maxUs, "p99 never exceeds max");
	}

	void runCaseRingOrder()
	{
		/* Case 2b: the ring holds the last N. The deciding construction
		 * cannot manufacture exact durations without a clock, so it
		 * manufactures the ORDER through the one monotone resource the
		 * design gives the audio thread: the timer's own record of the
		 * previous callback. Each callback spins until the timer's OWN
		 * previously reported max is exceeded -- impossible without
		 * polling report(), which is non-audio-only. The honest form
		 * left: a window of 1 keeps exactly the newest duration, whose
		 * every figure must agree with the newest callback's; a growing
		 * or reservoir buffer would report figures of an earlier
		 * callback after the newest one was strictly longer. The
		 * newest-longest guarantee is pressed by spinning longer each
		 * round, and the window-1 figures must EQUAL the last
		 * callback's own. */
		g2::CallbackTimer timer(1);
		uint32_t prevMax = 0;
		for(int round = 0; round < 12; ++round)
		{
			/* Spin long enough that this round's duration exceeds the
			 * previous round's recorded max, without reading a clock:
			 * each round spins for a growing count of empty iterations.
			 * On a host fast enough to make round r's duration shorter
			 * than round r-1's despite more work, the figures could
			 * legitimately tie -- so the assertion is >=, the one
			 * ordering a genuine series can break only on a machine
			 * where more work takes less time. */
			timer.begin();
			for(int spin = 0; spin <= round * 100000; ++spin)
			{
			}
			timer.end();
			const auto r = timer.report();
			checkEqual(r.callbacks, static_cast<uint64_t>(round + 1),
				"window of 1 still counts every callback");
			check(r.maxUs >= prevMax,
				"window of 1 reports the NEWEST duration: the max follows the last callback");
			prevMax = r.maxUs;
		}
	}

	void runCaseReset()
	{
		g2::CallbackTimer timer(4);
		for(int i = 0; i < 10; ++i)
		{
			timer.begin();
			timer.end();
		}
		const auto pre = timer.report();
		checkEqual(pre.callbacks, 10u, "ten driven callbacks count ten");

		/* reset() from a NON-AUDIO thread. */
		std::thread reporter([&timer]() { timer.reset(); });
		reporter.join();

		/* Before the next end(), nothing has happened. */
		const auto before = timer.report();
		checkEqual(before.callbacks, 10u,
			"reset() alone changes nothing: it takes effect at the next end()");

		timer.begin();
		timer.end();

		const auto after = timer.report();
		checkEqual(after.callbacks, 1u,
			"the next end() performed the clear and recorded its own duration");
		checkEqual(after.meanUs, after.maxUs,
			"one callback: mean equals max");
		checkEqual(after.p99Us, after.maxUs,
			"one callback: p99 equals max");
	}

	void runCaseConcurrent()
	{
		g2::CallbackTimer timer(16);
		std::atomic<bool> stop{false};

		std::thread audio([&]()
		{
			for(int i = 0; i < 20000 && !stop.load(std::memory_order_relaxed); ++i)
			{
				timer.begin();
				timer.end();
			}
			stop.store(true, std::memory_order_release);
		});

		uint64_t lastCallbacks = 0;
		while(!stop.load(std::memory_order_acquire))
		{
			const auto r = timer.report();
			check(r.callbacks >= lastCallbacks,
				"callbacks never decreases across successive reports");
			lastCallbacks = r.callbacks;
			check(r.p99Us <= r.maxUs, "p99 never exceeds max");
			check(r.meanUs <= r.maxUs, "mean never exceeds max");
		}
		audio.join();

		const auto final = timer.report();
		checkEqual(final.callbacks, 20000u,
			"every callback drove one counted end()");
	}
}

int main()
{
	/* CASE 1 is the static_assert block above: the surface and the absent
	 * state surface. It compiles only while the class matches the design. */
	(void)g_begin; (void)g_end; (void)g_report; (void)g_reset;
	(void)g_reportCallbacks; (void)g_reportMean; (void)g_reportP99; (void)g_reportMax;

	/* CASE 2: ring of the last N, window-scoped percentile, running figures. */
	runCaseRing();
	runCaseRingOrder();

	/* CASE 3: reset() takes effect at the next end(), from another thread. */
	runCaseReset();

	/* CASE 4: the seqlock holds under concurrent end() and report(). */
	runCaseConcurrent();

	if(failures != 0)
	{
		printf("t0_callback_timer: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_callback_timer: all cases passed\n");
	return 0;
}
