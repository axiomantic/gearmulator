/* The property this check owns: a view for slot k cannot reach slot j for any
 * j != k. The runtime probe is to commit a value through the view and confirm
 * every other slot of the underlying frame is untouched. The frame is
 * pre-filled with a distinguishing pattern, which blocks the failure mode
 * where a careless implementation leaves the other slots zero by coincidence.
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
	/* For every k, a view over slot k must be able to commit slot k, read it
	 * back through get(), report k through slot(), and leave all seven
	 * siblings untouched. */
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

			/* The whole frame is observable here, so the probe can see any
			 * sibling write. */
			checkEqual(frame.slot[k], kCommitValue,
				"the committed slot holds the committed value");
			if(!slotUntouched(frame, k))
				fail("a view for slot k reached a sibling slot j != k");
		}
	}

	/* A Broadcast has eight producers sharing one frame; each commits its own
	 * slot in whatever order the executor runs them. Two views over two slots,
	 * committed in either order, must each observe their own slot's value and
	 * never the sibling's, which is what leaves the executor free to serialise
	 * or parallelise. */
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

	/* get() observes only the reader's own work: a re-read through a fresh
	 * view over the same slot sees the committed value; a fresh view over a
	 * different slot sees that sibling's own sentinel. */
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

