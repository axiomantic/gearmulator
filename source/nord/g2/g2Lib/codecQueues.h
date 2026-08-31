/* CodecSource and CodecSink: two bounded queues of Frames between the Device
 * and the chain.
 *
 * Both capacities are lookaheadFrames + B, where B is the largest host block
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
 * B does not come from the Device, which cannot see it: synthLib::Device has
 * no prepareToPlay and no block-size accessor, and
 * synthLib::Plugin::setBlockSize keeps the value private and never forwards it.
 * Scheduler::Config::maxHostBlockFrames supplies it.
 *
 * No allocation happens for each quantum. Each queue allocates its whole ring
 * once, at construction, and never resizes it.
 *
 * The storage is integer throughout. A Frame is eight int32_t of Q23. The
 * determinism claim is made at the 96 kHz Q23 integer boundary and a float
 * inside it would end that claim.
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

		/* Returns false when full, and the frame is dropped, not queued. The
		 * capacity rule is what makes a false return unreachable in a correct
		 * build, so a false return is a defect report and not a condition to
		 * handle. OverflowFrames() is how it is observed.
		 *
		 * A refused frame is host audio input, dropped with no retry and no
		 * recovery: the audible consequence is a gap of that many frames in the
		 * input path. */
		bool         push(const Frame& frame) noexcept;

		/* A zero frame when empty, never stale data. Reading alone counts no
		 * starve; see starvedFrames() below. */
		const Frame& front() const noexcept;

		/* The consume step, and the one that counts a starve. The ingress phase
		 * reads front() and calls pop() once for each quantum, so pop() is
		 * where "a quantum consumed a frame" happens exactly once. Putting the
		 * counter in front() would double-count any diagnostic that peeked. */
		void         pop() noexcept;

		size_t       size() const noexcept;

		/* Present for symmetry with CodecSink::capacity(), and because the
		 * Scheduler's log line for a short push names the queue capacity. */
		size_t       capacity() const noexcept;

		/* Counts the frames push() refused: the symmetric failure to a
		 * starve. */
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

		/* Refuses when full. It does not overwrite the oldest frame:
		 * overwriting silently discards a frame the host has already been told
		 * to expect, which changes the real latency mid-session while the
		 * reported figure stays constant. Refusing makes it observable.
		 *
		 * The scheduler stops running quanta when this returns false. In a
		 * correct build it never returns false, because the capacity holds the
		 * lookahead plus a whole host block, so a false return is a defect
		 * report and the check that precedes it is a defect check and not a
		 * flow-control mechanism. */
		bool     push(const Frame& frame) noexcept;

		/* Takes up to `frames` frames and returns how many it took. The part it
		 * could not supply reads as silence, because the consumer receives the
		 * whole buffer it asked for and must not be handed whatever was there
		 * before. */
		size_t   pull(Frame* out, size_t frames) noexcept;

		size_t   size() const noexcept;
		size_t   capacity() const noexcept;

		/* Counts the frames push() refused. DroppedFrames() above zero is a
		 * defect report, not a tolerance. */
		uint64_t droppedFrames() const noexcept;

		/* Counts the frames pull() could not supply -- the number by which a
		 * pull's return fell short of its request. This is the quadrant an
		 * under-sized sink capacity lands in. */
		uint64_t underflowFrames() const noexcept;

	private:
		std::vector<Frame> m_ring;
		size_t             m_readIndex = 0;
		size_t             m_count     = 0;
		uint64_t           m_dropped   = 0;
		uint64_t           m_underflow = 0;
	};

	/* The determinism boundary is integer. These two assertions make a later
	 * member of a floating-point type a compile error. */
	static_assert(std::is_same_v<std::remove_extent_t<decltype(Frame::slot)>,
			int32_t>,
		"A queued frame is Q23 integer storage. No floating-point type may "
		"enter the determinism boundary.");
}
