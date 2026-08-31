/* The whole allocation happens once, in the constructor: m_ring is a
 * std::vector sized hopFrames + 1. advance(), read(), write() and writeSlot()
 * allocate nothing, which is what makes the ring a real-time-safe swap point
 * rather than a growing structure.
 */

#include "mailbox.h"

#include <cassert>

namespace g2
{
	Mailbox::Mailbox(const unsigned hopFrames)
		: m_ring(hopFrames + 1u)   /* the one allocation, sized hopFrames + 1 */
	{
		assert(hopFrames >= 1u
			&& "a mailbox with a hop delay of zero cannot express the delay "
			"line - Scheduler::create rejects it before this is reached");
	}

	const Frame& Mailbox::read() const noexcept
	{
		return m_ring[(m_head + 1u) % depth()];
	}

	Frame& Mailbox::write() noexcept
	{
		return m_ring[m_head];
	}

	void Mailbox::advance() noexcept
	{
		const unsigned n = depth();

		/* The order is load-bearing: the copy comes first, against the
		 * pre-step head, and the step comes second. After the step, write()
		 * is the cell that just received the copy -- the underrun rule, so a
		 * position that does not write leaves its last transmitted frame in
		 * place -- and read() is the oldest cell, the frame written H quanta
		 * ago. */
		m_ring[(m_head + 1u) % n] = m_ring[m_head];
		m_head = (m_head + 1u) % n;
	}

	SlotWriteView Mailbox::writeSlot(const unsigned slot) noexcept
	{
		return SlotWriteView(write(), slot);
	}

	unsigned Mailbox::depth() const noexcept
	{
		return static_cast<unsigned>(m_ring.size());
	}

	/* The two friend-only codec-facing accessors. ingressFrame() writes the
	 * read frame; egressFrame() reads the write frame. Both are reachable by
	 * ChainAdapter and by nothing else. */
	Frame& Mailbox::ingressFrame() noexcept
	{
		return m_ring[(m_head + 1u) % depth()];
	}

	const Frame& Mailbox::egressFrame() const noexcept
	{
		return m_ring[m_head];
	}
}
