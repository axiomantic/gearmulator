/* internalClient.cpp -- g2::InternalClient.
 *
 * The hub is reached directly and through nothing else. m_hub is the only
 * collaborator in this file: attach, detach and toDevice are called on it by
 * name, and no other endpoint appears anywhere below. That is what makes this
 * client a peer of the socket and usbip attachments rather than a path through
 * one of them.
 *
 * The whole allocation is the two vectors in the constructor's initialiser
 * list. Nothing below resizes, pushes back, news or copies a container; the
 * inbox is a ring index over storage that already exists.
 */

#include "internalClient.h"

#include <cstring>

namespace g2
{
	InternalClient::InternalClient(TransportHub& _hub, const size_t _maxFrameBytes, const size_t _inboxDepth)
		: m_hub(_hub)
		, m_maxFrameBytes(_maxFrameBytes)
		, m_inboxDepth(_inboxDepth)
		, m_frames(_inboxDepth)
		, m_payload(_inboxDepth * _maxFrameBytes)
	{
		/* attach is setup-time only and is not thread-safe, so it happens
		 * here, where the client is not yet reachable from another thread. */
		m_hub.attach(*this);
	}

	InternalClient::~InternalClient()
	{
		m_hub.detach(*this);
	}

	bool InternalClient::send(const ProtocolFrame _frame) noexcept
	{
		/* The verdict is the hub's and it is returned unchanged. This client
		 * adds no bound of its own on the outbound direction: the hub owns
		 * maxFrameBytes and the queue depth for what leaves, and a second
		 * refusal here would be a second place to keep that number. */
		return m_hub.toDevice(*this, _frame);
	}

	void InternalClient::onFrameFromDevice(const ProtocolFrame _frame) noexcept
	{
		/* A frame that cannot be copied whole is dropped whole. Truncating it
		 * into a slot that cannot hold it would hand the plugin a malformed
		 * message that looks well-formed. */
		if(_frame.size > m_maxFrameBytes)
		{
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		/* The occupancy is measured against `release`, not `head`. The slots
		 * between release and head were handed out by the last receive and are
		 * still borrowed by the plugin; overwriting one is exactly the
		 * borrow-lifetime defect this inbox must not have. */
		const uint64_t tail    = m_tail.load(std::memory_order_relaxed);
		const uint64_t release = m_release.load(std::memory_order_acquire);
		if(tail - release >= m_inboxDepth)
		{
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		const size_t   slot = static_cast<size_t>(tail % m_inboxDepth);
		uint8_t* const dst  = m_payload.data() + slot * m_maxFrameBytes;

		if(_frame.size != 0 && _frame.data != nullptr)
			std::memcpy(dst, _frame.data, _frame.size);

		m_frames[slot] = ProtocolFrame{ dst, _frame.size };

		m_tail.store(tail + 1, std::memory_order_release);
	}

	bool InternalClient::receive(ProtocolFrame& _out) noexcept
	{
		/* Release what the previous receive handed out. This is what makes
		 * "valid until the next receive" true rather than aspirational, and it
		 * happens before this call reads anything, so a caller that reads the
		 * previous frame up to the instant of this call sees intact bytes. The
		 * release happens even on an empty inbox: the previous borrow ended
		 * either way. */
		const uint64_t head = m_head.load(std::memory_order_relaxed);
		m_release.store(head, std::memory_order_release);

		const uint64_t tail = m_tail.load(std::memory_order_acquire);
		if(head == tail)
			return false;

		const size_t slot = static_cast<size_t>(head % m_inboxDepth);
		_out = m_frames[slot];

		m_head.store(head + 1, std::memory_order_relaxed);
		return true;
	}

	uint64_t InternalClient::droppedFrames() const noexcept
	{
		return m_dropped.load(std::memory_order_relaxed);
	}
}
