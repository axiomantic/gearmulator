/* t0_slot_write_view.cpp -- the check of task CHN-3. Design 12.3, 13.10.2, 26.
 *
 * SlotWriteView is a borrowed write view over ONE slot of a mailbox's write
 * frame, with set, get and slot. It exists because a Broadcast bus has eight
 * producers sharing one 8-slot g2::Frame, and position k must commit slot k
 * without being able to touch the other seven -- which a bare Frame& cannot
 * express.
 *
 * THE PROPERTY THIS CHECK OWNS. "A view for slot k cannot reach slot j for
 * any j != k." The view's API names one slot and no other, so a runtime
 * probe of that property is: commit a value through the view and confirm
 * that every OTHER slot of the underlying frame is untouched. That probe is
 * run for every k in the frame, against a frame pre-filled with a
 * distinguishing pattern, and the pattern blocks the failure mode where a
 * careless implementation leaves the other slots zero by coincidence.
 *
 * THE ONE CLAUSE OF DESIGN 12.3. A Broadcast producer may read back, through
 * get(), the one slot its own view commits, and nothing else. The check
 * verifies get() returns exactly the value set() committed on that slot and
 * only that slot.
 *
 * LIFETIME. The view is valid only until the next advance() and is never
 * stored. This check never stores one: every view is used and released
 * within a block. CHN-1's Mailbox owns the advance-to-invalidity half, which
 * is its task's check, not this one.
 */

#include "slotWriteView.h"

#include "frame.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	void fail(const char* const what)
	{
		printf("FAIL %s\n", what);
		++failures;
	}

	void check(const bool condition, const char* const what)
	{
		if(!condition)
			fail(what);
	}

	void checkEqual(const int32_t observed, const int32_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %d, expected %d\n", what,
				static_cast<int>(observed), static_cast<int>(expected));
			++failures;
		}
	}

	/* A value that exercises the sign bit as well as the low bits, so a bug
	 * that shifts or sign-extends wrongly cannot hide in a small positive. */
	constexpr int32_t kCommitValue = -0x400001;   /* a negative Q23 value  */

	/* Every slot gets a DIFFERENT distinguishing pre-fill, so a view that
	 * reaches a sibling slot reads (and the test sees) that sibling's own
	 * sentinel rather than a coincidental zero. */
	void fillPattern(g2::Frame& frame)
	{
		for(unsigned i = 0; i < g2::Frame::kSlots; ++i)
			frame.slot[i] = static_cast<int32_t>(0x10000u * (i + 1)) + 1;
	}

	bool slotUntouched(const g2::Frame& frame, const unsigned k)
	{
		/* Every sibling j != k still holds its own sentinel. */
		for(unsigned j = 0; j < g2::Frame::kSlots; ++j)
		{
			if(j != k && frame.slot[j] !=
				static_cast<int32_t>(0x10000u * (j + 1)) + 1)
				return false;
		}
		/* And slot k itself was overwritten by the view (see the caller's
		 * separate check of the exact committed value). */
		return true;
	}
}

int main()
{
	/* -------------------------------------------------------------------
	 * THE OWNING CLAUSE, EVERY SLOT.
	 *
	 * For every k, a view over slot k must be able to commit slot k, read
	 * it back through get(), report k through slot(), and leave all seven
	 * siblings untouched. This is the exact assertion the Check: line names.
	 * ------------------------------------------------------------------- */
	for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
	{
		g2::Frame frame;
		fillPattern(frame);

		{
			g2::SlotWriteView view(frame, k);
			checkEqual(view.slot(), k, "view reports its own slot index");

			view.set(kCommitValue);
			checkEqual(view.get(), kCommitValue,
				"get() reads back exactly the value the view committed");

			/* The whole frame is observable here, so the probe is honest:
			 * it can see ANY sibling write. The committed slot holds the
			 * value; every other slot still holds its own sentinel. */
			checkEqual(frame.slot[k], kCommitValue,
				"the committed slot holds the committed value");
			if(!slotUntouched(frame, k))
				fail("a view for slot k reached a sibling slot j != k");
		}
		/* The view is released here, at the end of the block, and is never
		 * stored past its block. */
	}

	/* -------------------------------------------------------------------
	 * INDEPENDENCE OF TWO VIEWS.
	 *
	 * A Broadcast has eight producers sharing one frame; each commits its
	 * own slot in whatever order the executor runs them. Two views over two
	 * slots, committed in either order, must each observe their own slot's
	 * value and never the sibling's -- so the Broadcast case stays order
	 * independent, which is what makes the executor free to serialise or
	 * parallelise as it likes.
	 * ------------------------------------------------------------------- */
	for(unsigned a = 0; a < g2::Frame::kSlots; ++a)
	{
		for(unsigned b = 0; b < g2::Frame::kSlots; ++b)
		{
			if(a == b)
				continue;

			g2::Frame frame;
			fillPattern(frame);

			g2::SlotWriteView viewA(frame, a);
			g2::SlotWriteView viewB(frame, b);

			viewA.set(kCommitValue);
			viewB.set(kCommitValue + 1);

			checkEqual(viewA.get(), kCommitValue,
				"view A reads back its own committed value, order A-then-B");
			checkEqual(viewB.get(), kCommitValue + 1,
				"view B reads back its own committed value, order A-then-B");
			if(frame.slot[a] != kCommitValue || frame.slot[b] != kCommitValue + 1)
				fail("two views over two slots wrote into each other");
		}
	}

	/* -------------------------------------------------------------------
	 * get() OBSERVES ONLY THE READER'S OWN WORK: the value a view sees is
	 * the value that same view (or a later view over the SAME slot) wrote.
	 * A re-read through a fresh view over the same slot sees the committed
	 * value; a fresh view over a DIFFERENT slot sees that sibling's own
	 * sentinel, not the first view's commit. ------------------------------------------------------------------- */
	{
		g2::Frame frame;
		fillPattern(frame);

		g2::SlotWriteView writer(frame, 3);
		writer.set(kCommitValue);

		g2::SlotWriteView reader(frame, 3);
		checkEqual(reader.get(), kCommitValue,
			"a fresh view over the same slot observes the earlier commit");

		g2::SlotWriteView other(frame, 4);
		checkEqual(other.get(), static_cast<int32_t>(0x10000u * 5) + 1,
			"a view over a sibling slot observes the sibling's own value, "
			"not the first view's commit");
	}

	if(failures == 0)
	{
		printf("PASS t0_slot_write_view\n");
		return 0;
	}
	printf("%d FAILURES\n", failures);
	return 1;
}

