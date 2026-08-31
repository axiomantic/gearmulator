/* CallbackTimer.h -- the P4 instrument, task PLG-13. Design section 18.10.
 *
 * THE ONE COMPONENT IN THIS PROJECT PERMITTED TO READ A HOST CLOCK. Design
 * section 13.7 forbids the system-clock spellings everywhere in the
 * emulation sources; the SCH-26 lint's exclusion list names exactly one file,
 * g2Lib/perf/CallbackTimer.cpp, and that list is FILES and not patterns.
 * Every host-clock read lives in the .cpp for that reason -- this header
 * carries none, not even in a comment, because the lint searches text and
 * cannot tell a comment from a call.
 *
 * WHAT IT TIMES. processAudio, the Device subclass's whole callback. It
 * cannot name a context, it cannot separate the scheduler from the
 * resampler, and it says nothing about the emulated machine. It is not the
 * cycle debt and it cannot be substituted for it; design section 18.10's
 * instrument table states the two apart.
 *
 * OWNERSHIP. The Device subclass owns exactly one, by value, constructed
 * with it and before the Scheduler. ALLOCATES ONCE, in the constructor: the
 * ring and every counter live in this object from construction on, and no
 * method allocates (design section 13.10 rule 1). It is NOT part of the
 * Scheduler snapshot -- it has no emulated state, so it saves and restores
 * nothing, and a state file recorded on a fast machine loads identically on
 * a slow one.
 *
 * THREADING. begin() and end() are AUDIO THREAD ONLY. report() and reset()
 * are callable from ANY OTHER THREAD and never from the audio thread. The
 * percentile needs a sort, so the sort runs in report() and never in end():
 * end() writes one ring slot, updates the running count, sum and maximum,
 * and stores a sequence counter with release. report() loads the sequence
 * with acquire, copies the ring, re-loads the sequence, and retries if it
 * moved -- a seqlock, with the whole cost on the reader, which is not
 * real-time. A retry is possible and is not a failure; the reader is a
 * console harness or a test.
 *
 * reset() TAKES EFFECT AT THE NEXT END(), not at the call. The audio thread
 * owns the counters it clears, so reset() only arms the request and the next
 * end() performs the clear and then records its own duration; no counter is
 * written from the reporting thread.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace g2
{
	class CallbackTimer
	{
	public:
		struct Report
		{
			uint64_t callbacks;   /* total, since construction or reset()      */
			uint32_t meanUs;
			uint32_t p99Us;
			uint32_t maxUs;       /* since construction or reset()             */
		};

		/* windowCallbacks is the number of most-recent durations kept. It is
		 * a PLAIN RING OF THE LAST N, not reservoir sampling: reservoir
		 * sampling needs a random source, and a percentile that depends on
		 * one is not reproducible between two runs of the same measurement.
		 */
		explicit CallbackTimer(size_t windowCallbacks);

		void   begin() noexcept;   /* first statement of processAudio          */
		void   end()   noexcept;   /* last statement of processAudio           */

		/* ANY NON-AUDIO THREAD. The sort runs HERE and never in end().
		 * It then sorts the copy and returns. A retry is possible and is not
		 * a failure; the reader is a console harness or a test. */
		Report report() const noexcept;

		/* ANY NON-AUDIO THREAD. Takes effect at the next end(). */
		void   reset() noexcept;

	private:
		void performReset() noexcept;   /* audio thread only, from end()      */
		static uint32_t toMicros(uint64_t ticks) noexcept;

		/* Fixed capacity, allocated once with the object. No heap after the
		 * constructor. m_reportScratch is written only by report(), whose
		 * single-reader contract the design states (report() and reset()
		 * are never called from the audio thread and never concurrently). */
		std::unique_ptr<uint64_t[]> m_ring;
		std::unique_ptr<uint64_t[]> m_reportScratch;
		size_t m_ringCapacity = 0;
		size_t m_ringWrite = 0;         /* audio thread only                  */
		size_t m_ringFilled = 0;        /* audio thread only                  */

		uint64_t m_count = 0;           /* audio thread only                  */
		uint64_t m_tickSum = 0;         /* audio thread only                  */
		uint64_t m_maxTicks = 0;        /* audio thread only                  */

		std::atomic<uint64_t> m_beginTicks{0};

		/* Seqlock sequence. Even = stable, odd = end() mid-update. end()
		 * bumps it to odd before its body and to even again with release
		 * after; report() acquire-loads it, retries while odd, and retries
		 * again if a relaxed re-load after the ring copy disagrees. */
		std::atomic<uint32_t> m_seq{0};

		/* Set by reset() from a non-audio thread; consumed by the next
		 * end(), which performs the clear. */
		std::atomic<bool> m_resetArmed{false};
	};
}
