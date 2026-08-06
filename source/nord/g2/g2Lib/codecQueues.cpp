/* codecQueues.cpp -- the two bounded queues. Task SCH-15.
 * Design sections 13.10.4 and 13.6.
 *
 * Both are the same ring: one vector allocated at construction, a read index
 * and a count. The write index is derived, so the two ends can never disagree
 * about how full the queue is.
 */

#include "codecQueues.h"

namespace g2
{
	namespace
	{
		/* The write position, derived from the read index and the count. A
		 * capacity of zero would divide by zero here, so both constructors
		 * refuse one; see below. */
		size_t writeIndex(const size_t readIndex, const size_t count,
			const size_t capacity) noexcept
		{
			return (readIndex + count) % capacity;
		}
	}

	/* ---------------- CodecSource */

	CodecSource::CodecSource(const size_t capacityFrames)
		: m_ring(capacityFrames == 0 ? 1 : capacityFrames)
	{
		/* A CAPACITY OF ZERO IS REPLACED BY ONE RATHER THAN ACCEPTED. The
		 * modulo above would divide by zero, and neither constructor has an
		 * error channel. The Scheduler's own factory is where a bad
		 * configuration is refused with a status; this is the last defence and
		 * it must not be undefined behaviour. */
	}

	bool CodecSource::push(const Frame& frame) noexcept
	{
		if(m_count >= m_ring.size())
		{
			++m_overflow;
			return false;
		}

		m_ring[writeIndex(m_readIndex, m_count, m_ring.size())] = frame;
		++m_count;
		return true;
	}

	const Frame& CodecSource::front() const noexcept
	{
		if(m_count == 0)
			return m_silence;

		return m_ring[m_readIndex];
	}

	void CodecSource::pop() noexcept
	{
		if(m_count == 0)
		{
			++m_starved;
			return;
		}

		m_readIndex = (m_readIndex + 1) % m_ring.size();
		--m_count;
	}

	size_t CodecSource::size() const noexcept
	{
		return m_count;
	}

	size_t CodecSource::capacity() const noexcept
	{
		return m_ring.size();
	}

	uint64_t CodecSource::overflowFrames() const noexcept
	{
		return m_overflow;
	}

	uint64_t CodecSource::starvedFrames() const noexcept
	{
		return m_starved;
	}

	/* ---------------- CodecSink */

	CodecSink::CodecSink(const size_t capacityFrames)
		: m_ring(capacityFrames == 0 ? 1 : capacityFrames)
	{
	}

	bool CodecSink::push(const Frame& frame) noexcept
	{
		/* REFUSES. IT DOES NOT OVERWRITE. Overwriting the oldest frame would
		 * discard audio the host has already been told to expect, which moves
		 * the real latency while the reported figure stays constant. */
		if(m_count >= m_ring.size())
		{
			++m_dropped;
			return false;
		}

		m_ring[writeIndex(m_readIndex, m_count, m_ring.size())] = frame;
		++m_count;
		return true;
	}

	size_t CodecSink::pull(Frame* const out, const size_t frames) noexcept
	{
		if(out == nullptr)
			return 0;

		size_t taken = 0;

		while(taken < frames && m_count > 0)
		{
			out[taken] = m_ring[m_readIndex];
			m_readIndex = (m_readIndex + 1) % m_ring.size();
			--m_count;
			++taken;
		}

		/* THE PART THAT COULD NOT BE SUPPLIED READS AS SILENCE. The consumer
		 * receives the whole buffer it asked for. */
		for(size_t i = taken; i < frames; ++i)
			out[i] = Frame{};

		m_underflow += frames - taken;

		return taken;
	}

	size_t CodecSink::size() const noexcept
	{
		return m_count;
	}

	size_t CodecSink::capacity() const noexcept
	{
		return m_ring.size();
	}

	uint64_t CodecSink::droppedFrames() const noexcept
	{
		return m_dropped;
	}

	uint64_t CodecSink::underflowFrames() const noexcept
	{
		return m_underflow;
	}
}
