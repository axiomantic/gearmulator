/* CallbackTimer.cpp
 *
 * This file is on the system-clock lint's exclusion list, and that is why it
 * lives at exactly this path. The lint searches the emulation sources for the
 * system-clock spellings -- the chrono family, clock_gettime, time(),
 * mach_absolute_time and QueryPerformanceCounter -- and fails on a hit; its
 * exclusion list names files, not patterns. A system-clock call anywhere
 * else, including anywhere else in this directory, still fails.
 *
 * The clock is std::chrono::steady_clock: a monotonic host clock, fit for
 * measuring elapsed host time and nothing else. No emulated value is
 * computed from it; nothing here feeds the Scheduler, the state, or any
 * buffer the machine renders.
 *
 * Allocates once, in the constructor: the ring and the report scratch are
 * both sized there and every later call writes into storage that already
 * exists. The scratch has exactly one reader at a time -- report() and
 * reset() are non-audio-thread-only, and concurrent report() calls are
 * undefined by contract -- so the scratch is reused rather than reallocated,
 * and no method allocates.
 */

#include "CallbackTimer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace g2
{
	namespace
	{
		uint64_t nowTicks() noexcept
		{
			return static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
		}
	}

	CallbackTimer::CallbackTimer(const size_t windowCallbacks)
		: m_ringCapacity(windowCallbacks)
	{
		/* The one allocation. */
		if(m_ringCapacity > 0)
		{
			m_ring.reset(new uint64_t[m_ringCapacity]);
			m_reportScratch.reset(new uint64_t[m_ringCapacity]);
		}
	}

	void CallbackTimer::begin() noexcept
	{
		m_beginTicks.store(nowTicks(), std::memory_order_relaxed);
	}

	void CallbackTimer::performReset() noexcept
	{
		m_ringWrite = 0;
		m_ringFilled = 0;
		m_count = 0;
		m_tickSum = 0;
		m_maxTicks = 0;
	}

	void CallbackTimer::end() noexcept
	{
		/* reset() takes effect at the next end(). The clear happens on the
		 * audio thread, at the head of the call whose duration is being
		 * recorded, so the counters it clears are this thread's own and no
		 * reporting thread ever writes them. */
		if(m_resetArmed.exchange(false, std::memory_order_acq_rel))
			performReset();

		/* Seqlock: odd marks the update in progress. report() retries
		 * whenever it reads odd, so the body below is never observed
		 * half-written. */
		const uint32_t seq = m_seq.load(std::memory_order_relaxed) + 1;
		m_seq.store(seq, std::memory_order_relaxed);

		const auto start = m_beginTicks.load(std::memory_order_relaxed);
		const uint64_t ticks = nowTicks() - start;

		m_tickSum += ticks;
		++m_count;
		if(ticks > m_maxTicks)
			m_maxTicks = ticks;

		if(m_ringCapacity > 0)
		{
			m_ring[m_ringWrite] = ticks;
			m_ringWrite = (m_ringWrite + 1) % m_ringCapacity;
			if(m_ringFilled < m_ringCapacity)
				++m_ringFilled;
		}

		/* Seqlock publish: even marks the state stable again. The release
		 * pairs with report()'s acquire load: everything written above is
		 * visible to a reader that then sees this value. */
		m_seq.store(seq + 1, std::memory_order_release);
	}

	CallbackTimer::Report CallbackTimer::report() const noexcept
	{
		Report r{};

		for(;;)
		{
			/* Acquire here pairs with end()'s release store: everything the
			 * audio thread wrote before it is visible after this load. */
			const uint32_t seq = m_seq.load(std::memory_order_acquire);
			if(seq & 1)
				continue;   /* end() mid-update; a retry is not a failure */

			const size_t filled = m_ringFilled;
			const uint64_t count = m_count;
			const uint64_t tickSum = m_tickSum;
			const uint64_t maxTicks = m_maxTicks;

			/* The ring copy is the seqlock body: end() may run while it
			 * copies, and the re-load below is what detects that. */
			if(filled > 0 && m_ringCapacity > 0)
				std::memcpy(m_reportScratch.get(), m_ring.get(),
					filled * sizeof(uint64_t));

			if(m_seq.load(std::memory_order_relaxed) != seq)
				continue;   /* torn copy; retrying is the contract */

			if(count > 0)
				r.meanUs = toMicros(tickSum / count);
			r.callbacks = count;
			r.maxUs = toMicros(maxTicks);

			if(filled > 0)
			{
				std::sort(m_reportScratch.get(), m_reportScratch.get() + filled);
				const auto idx = static_cast<size_t>(
					(static_cast<uint64_t>(filled - 1) * 99) / 100);
				r.p99Us = toMicros(m_reportScratch[idx]);
			}

			return r;
		}
	}

	void CallbackTimer::reset() noexcept
	{
		/* Arms only. The next end() performs the clear. */
		m_resetArmed.store(true, std::memory_order_release);
	}

	uint32_t CallbackTimer::toMicros(const uint64_t ticks) noexcept
	{
		/* steady_clock's period is 1ns on every platform this project
		 * targets; the conversion is stated once, here. */
		return static_cast<uint32_t>(ticks / 1000u);
	}
}
