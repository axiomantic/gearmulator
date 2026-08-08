/* slotWriteView.h -- a borrowed write view over ONE slot of a Mailbox's
 * write frame. Task CHN-3. Design sections 12.3, 13.10.2, 26.
 *
 * WHY THE TYPE EXISTS. A Broadcast bus has eight producers sharing one
 * 8-slot g2::Frame, and position k must commit slot k without being able to
 * touch the other seven. A bare Frame& cannot express that restriction -- it
 * exposes all eight slots -- so the view exists to say "exactly this one
 * slot, and no other". CHN-1's Mailbox::writeSlot(unsigned) returns one of
 * these by value; this header declares the type it returns.
 *
 * THE ONE CLAUSE THE INVARIANT ALLOWS. Design 12.3's run-phase invariant
 * says a DSP callback reads only from read() and writes only to write().
 * SlotWriteView::get() is the one permitted exception: a Broadcast producer
 * may read back, through get(), the one slot its own view commits, and
 * nothing else. That read observes only the reader's own work in this
 * quantum, so it cannot leak a sibling producer's frame. The view cannot
 * reach any other slot, so the clause cannot be widened by accident.
 *
 * OWNERSHIP AND LIFETIME. The view is BORROWED. It is valid only until the
 * next advance() on the owning Mailbox, and it must never be stored. It
 * carries a Frame* and an index; it owns nothing.
 *
 * THREADING. Scheduler thread only, like every Mailbox accessor. Not
 * thread-safe and does not need to be.
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
		 * Broadcast position addresses exactly the slot it owns and
		 * Mailbox::writeSlot(unsigned) is called with a position index below
		 * kSlots from §13.10.2's wiring table. A debug build asserts it. */
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

		/* THE ONE CLAUSE OF DESIGN 12.3. Reads back the one slot this view
		 * commits, and nothing else. This is a read of the WRITE frame by a
		 * DSP callback, which the run-phase invariant forbids in its general
		 * form; 12.3 carries the one clause that permits it, stated at the
		 * top of this file. */
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

