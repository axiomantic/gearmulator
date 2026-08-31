/* g2::TransportHub.
 *
 * The whole allocation is the two vectors in the constructor's initialiser
 * list. Nothing below resizes, pushes back, news or copies a container; the
 * endpoint table is a fixed member array and every queue is a ring index over
 * storage that already exists.
 */

#include "transportHub.h"

#include <cstring>

namespace g2
{
	TransportHub::TransportHub(const size_t maxFrameBytes, const size_t queueDepth)
		: m_maxFrameBytes(maxFrameBytes)
		, m_queueDepth(queueDepth)
		, m_stamped(kMaxEndpoints * queueDepth)
		, m_payload(kMaxEndpoints * queueDepth * maxFrameBytes)
	{
	}

	/* kMaxEndpoints means "not attached", and it is returned rather than a
	 * sentinel index so that every caller's bounds test is the same one. */
	size_t TransportHub::indexOf(const TransportEndpoint& endpoint) const noexcept
	{
		for(size_t e = 0; e < kMaxEndpoints; ++e)
		{
			if(m_endpoints[e].handle == &endpoint)
				return e;
		}
		return kMaxEndpoints;
	}

	void TransportHub::attach(TransportEndpoint& endpoint)
	{
		if(indexOf(endpoint) != kMaxEndpoints)
			return;

		for(size_t e = 0; e < kMaxEndpoints; ++e)
		{
			if(m_endpoints[e].handle != nullptr)
				continue;

			/* The position is the attachment order and it is fixed from here
			 * on. The counters are reset so that a position reused after a
			 * detach starts empty rather than inheriting the previous
			 * occupant's ring state. */
			m_endpoints[e].handle = &endpoint;
			m_endpoints[e].tail.store(0, std::memory_order_relaxed);
			m_endpoints[e].head.store(0, std::memory_order_relaxed);
			m_endpoints[e].release.store(0, std::memory_order_relaxed);
			return;
		}
	}

	void TransportHub::detach(TransportEndpoint& endpoint)
	{
		const size_t e = indexOf(endpoint);
		if(e == kMaxEndpoints)
			return;

		/* No other endpoint moves. Compacting the table here would silently
		 * change the drain order of every endpoint after this one, which is
		 * the one property the fixed order exists to hold still. */
		m_endpoints[e].handle = nullptr;
	}

	void TransportHub::fromDevice(const ProtocolFrame frame) noexcept
	{
		for(size_t e = 0; e < kMaxEndpoints; ++e)
		{
			if(m_endpoints[e].handle != nullptr)
				m_endpoints[e].handle->onFrameFromDevice(frame);
		}
	}

	bool TransportHub::toDevice(TransportEndpoint& endpoint,
		const ProtocolFrame frame) noexcept
	{
		const size_t e = indexOf(endpoint);
		if(e == kMaxEndpoints)
			return false;

		if(frame.size > m_maxFrameBytes)
			return false;

		Endpoint& q = m_endpoints[e];

		/* The producer's occupancy is measured against `release`, not `head`.
		 * The slots between release and head are drained but still borrowed by
		 * the last drainToDevice's caller, and overwriting one of them is
		 * exactly the borrow-lifetime defect this hub must not have. */
		const uint64_t tail    = q.tail.load(std::memory_order_relaxed);
		const uint64_t release = q.release.load(std::memory_order_acquire);
		if(tail - release >= m_queueDepth)
			return false;

		const size_t slot  = static_cast<size_t>(tail % m_queueDepth);
		const size_t index = e * m_queueDepth + slot;
		uint8_t* const dst = m_payload.data() + index * m_maxFrameBytes;

		if(frame.size != 0 && frame.data != nullptr)
			std::memcpy(dst, frame.data, frame.size);

		/* The stamp is written by the drain, not here: the frame index a frame
		 * carries is the quantum it crossed the boundary in, and it has not
		 * crossed yet. */
		m_stamped[index].frameIndex = 0;
		m_stamped[index].frame      = ProtocolFrame{ dst, frame.size };

		q.tail.store(tail + 1, std::memory_order_release);
		return true;
	}

	size_t TransportHub::drainToDevice(StampedFrame* const out,
		const size_t max) noexcept
	{
		/* Release what the previous drain lent out. This happens before this
		 * drain reads anything, so a caller that reads the previous pointers
		 * up to the instant of this call sees intact bytes. */
		for(size_t e = 0; e < kMaxEndpoints; ++e)
		{
			Endpoint& q = m_endpoints[e];
			q.release.store(q.head.load(std::memory_order_relaxed),
				std::memory_order_release);
		}

		/* An endpoint's index is its attachment order, so two attachments
		 * delivering into the same quantum reach the device in the same order
		 * on every run. */
		size_t written = 0;
		for(size_t e = 0; e < kMaxEndpoints && written < max; ++e)
		{
			Endpoint& q = m_endpoints[e];
			if(q.handle == nullptr)
				continue;

			const uint64_t tail = q.tail.load(std::memory_order_acquire);
			uint64_t       head = q.head.load(std::memory_order_relaxed);

			while(head != tail && written < max)
			{
				const size_t slot  = static_cast<size_t>(head % m_queueDepth);
				const size_t index = e * m_queueDepth + slot;

				m_stamped[index].frameIndex = m_frameIndex;
				out[written] = m_stamped[index];

				++written;
				++head;
			}

			q.head.store(head, std::memory_order_relaxed);
		}

		++m_frameIndex;
		return written;
	}
}
