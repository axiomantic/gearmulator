/* codecQueues.h -- CodecSource and CodecSink. Task SCH-15.
 * Design sections 13.10.4 and 13.6.
 *
 * Two bounded queues of Frames between the Device and the chain.
 *
 * BOTH CAPACITIES ARE lookaheadFrames + B, where B is the largest host block
 * expressed in 96 kHz frames. They are the same because the two queues are
 * symmetric, and one argument covers both:
 *
 *   CodecSource   Scheduler::push delivers a whole block at once, BEFORE
 *                 runFrames for the same block consumes any of it.
 *   CodecSink     runFrames produces a whole block at once, BEFORE pull for
 *                 the same block takes any of it.
 *
 * The call order is fixed, so neither queue can be sized for one quantum. A
 * CodecSink sized lookaheadFrames + framesPerQuantum makes the scheduler run
 * one quantum for each host block and then stop -- a plugin that emits no
 * audio at all. That is the defect the L + B capacity closes.
 *
 * WHERE B COMES FROM: NOT from the Device, which cannot see it.
 * synthLib::Device has no prepareToPlay and no block-size accessor, and
 * synthLib::Plugin::setBlockSize keeps the value private and never forwards
 * it. Scheduler::Config::maxHostBlockFrames supplies it.
 *
 * NO ALLOCATION HAPPENS FOR EACH QUANTUM. Each queue allocates its whole ring
 * once, at construction, and never resizes it.
 *
 * THE STORAGE IS INTEGER THROUGHOUT. A Frame is eight int32_t of Q23, and
 * nothing in these two classes is a floating-point type. The determinism claim
 * is made at the 96 kHz Q23 integer boundary and a float inside it would end
 * that claim.
 *
 * Ownership   Scheduler owns both.
 * Lifetime    Constructed and destroyed with the Scheduler.
 * Threading   CodecSource::push is called from Scheduler::push. Every other
 *             method belongs to whichever thread owns the Scheduler at the
 *             time.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "frame.h"

namespace g2
{
	/* host input -> chain head */
	class CodecSource
	{
	public:
		explicit CodecSource(size_t capacityFrames);

		/* Returns false when full, and the frame is DROPPED, not queued. The
		 * capacity rule is what makes a false return unreachable in a correct
		 * build, so a false return is a DEFECT REPORT and not a condition to
		 * handle. overflowFrames() is how it is observed.
		 *
		 * WHAT A REFUSED FRAME COSTS, PLAINLY: it is host audio input and it
		 * is dropped. There is no retry and no recovery. The audible
		 * consequence is a gap of that many frames in the input path. The
		 * behaviour is stated so that nobody reads "unreachable" as
		 * "harmless". */
		bool         push(const Frame& frame) noexcept;

		/* A ZERO FRAME WHEN EMPTY, never stale data. Reading alone counts no
		 * starve; see starvedFrames() below. */
		const Frame& front() const noexcept;

		/* THE CONSUME STEP, and the one that counts a starve. The ingress
		 * phase reads front() and calls pop() once for each quantum, so pop()
		 * is where "a quantum consumed a frame" happens exactly once. Putting
		 * the counter in front() would double-count any diagnostic that
		 * peeked. */
		void         pop() noexcept;

		size_t       size() const noexcept;

		/* Present for symmetry with CodecSink::capacity(), and because the
		 * Scheduler's required log line for a short push names "the queue
		 * capacity" -- a value an earlier draft exposed nowhere, so the line
		 * it demanded could not be written. */
		size_t       capacity() const noexcept;

		/* Counts the frames push() refused. An earlier draft had no
		 * counterpart to starvedFrames() on the input side, so an overflow --
		 * the symmetric failure to a starve -- was unhandled AND uncounted. */
		uint64_t     overflowFrames() const noexcept;

		/* Counts the quanta that consumed a zero frame because the host had
		 * supplied none. Deterministic, and asserted zero by the
		 * reproducibility row. */
		uint64_t     starvedFrames() const noexcept;

	private:
		std::vector<Frame> m_ring;
		size_t             m_readIndex  = 0;
		size_t             m_count      = 0;
		uint64_t           m_overflow   = 0;
		uint64_t           m_starved    = 0;
		Frame              m_silence{};
	};

	/* chain tail -> host output */
	class CodecSink
	{
	public:
		explicit CodecSink(size_t capacityFrames);

		/* REFUSES WHEN FULL. IT DOES NOT OVERWRITE THE OLDEST FRAME.
		 *
		 * An earlier design draft declared "overwrites oldest when full" while
		 * the lookahead section said "when the queue is full, the scheduler
		 * stops". Those two cannot both stand, and the overwrite is the one
		 * that had to go: overwriting silently discards a frame the host has
		 * already been told to expect, which changes the real latency
		 * mid-session while the reported figure stays constant -- the exact
		 * failure the constant-latency requirement exists to prevent, and one
		 * no test in this design would see. Refusing makes it observable.
		 *
		 * The scheduler stops running quanta when this returns false. In a
		 * correct build it never returns false, because the capacity holds the
		 * lookahead plus a whole host block, so a false return is a DEFECT
		 * REPORT and the check that precedes it is a defect check and not a
		 * flow-control mechanism. */
		bool     push(const Frame& frame) noexcept;

		/* Takes up to `frames` frames and returns how many it took. THE PART
		 * IT COULD NOT SUPPLY READS AS SILENCE, because the consumer receives
		 * the whole buffer it asked for and must not be handed whatever was
		 * there before. */
		size_t   pull(Frame* out, size_t frames) noexcept;

		size_t   size() const noexcept;
		size_t   capacity() const noexcept;

		/* Counts the frames push() refused. droppedFrames() above zero is a
		 * DEFECT REPORT, not a tolerance. An earlier draft declared this
		 * counter and NO test read it, which made it decoration. */
		uint64_t droppedFrames() const noexcept;

		/* Counts the frames pull() could not supply -- the number by which a
		 * pull's return fell short of its request. This is the SINK's
		 * under-supply counter, and an earlier draft had none: the source
		 * counted both starvation and overflow while the sink counted only
		 * overflow, so the one quadrant an under-sized sink capacity actually
		 * lands in was the quadrant nothing watched. */
		uint64_t underflowFrames() const noexcept;

	private:
		std::vector<Frame> m_ring;
		size_t             m_readIndex = 0;
		size_t             m_count     = 0;
		uint64_t           m_dropped   = 0;
		uint64_t           m_underflow = 0;
	};

	/* The determinism boundary is INTEGER. These two assertions are here so
	 * that a later member of a floating-point type is a compile error and not
	 * a discovery. */
	static_assert(std::is_same_v<std::remove_extent_t<decltype(Frame::slot)>,
			int32_t>,
		"A queued frame is Q23 integer storage. No floating-point type may "
		"enter the determinism boundary.");
}
