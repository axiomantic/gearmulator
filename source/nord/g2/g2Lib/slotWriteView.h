/* A borrowed write view over one slot of a Mailbox's write frame.
 *
 * A Broadcast bus has eight producers sharing one 8-slot g2::Frame, and
 * position k must commit slot k without being able to touch the other seven. A
 * bare Frame& cannot express that restriction -- it exposes all eight slots.
 *
 * The run-phase invariant says a DSP callback reads only from read() and
 * writes only to write(). SlotWriteView::get() is the one permitted exception:
 * a Broadcast producer may read back the one slot its own view commits. That
 * read observes only the reader's own work in this quantum, so it cannot leak
 * a sibling producer's frame, and the view cannot reach any other slot.
 *
 * The view is borrowed. It is valid only until the next advance() on the
 * owning Mailbox, and it must never be stored. It carries a Frame* and an
 * index; it owns nothing.
 *
 * Scheduler thread only, like every Mailbox accessor. Not thread-safe.
 */

#pragma once

#include "frame.h"

#include <cstdint>

namespace g2
{
	class SlotWriteView
	{
	public:
		/* Constructs a view over `slotIndex` of `frame`.
		 *
		 * PRECONDITION: slotIndex < Frame::kSlots. Not checked here: a
		 * Broadcast position addresses exactly the slot it owns, and
		 * Mailbox::writeSlot is called with a position index below kSlots. A
		 * debug build asserts it. */
		SlotWriteView(Frame& frame, const unsigned slotIndex) noexcept
			: m_frame(&frame)
			, m_slot(slotIndex)
		{
		}

		/* Commits the one slot this view owns. No other slot of the
		 * underlying frame is touched, reachable or named. */
		void set(const int32_t value) noexcept
		{
			m_frame->slot[m_slot] = value;
		}

		/* Reads back the one slot this view commits, and nothing else. This is
		 * a read of the write frame by a DSP callback, which the run-phase
		 * invariant forbids in its general form; the one permitting clause is
		 * stated at the top of this file. */
		int32_t get() const noexcept
		{
			return m_frame->slot[m_slot];
		}

		/* Which slot this view commits. */
		unsigned slot() const noexcept
		{
			return m_slot;
		}

	private:
		Frame*   m_frame;
		unsigned m_slot;
	};
}

