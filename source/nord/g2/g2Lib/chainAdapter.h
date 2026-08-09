/* chainAdapter.h -- the chain adapter's header. Tasks CHN-4 and CHN-5.
 * Design sections 12.3 and 13.10.2.
 *
 * THIS HEADER LAYS DOWN THE DECLARATION, AND CHN-5 DEFINES THE CLASS.
 * The two chain tasks split the one file: CHN-4 owns `ChainTopology` and the
 * inline `mailboxCount` constant expression that section 12.3 derives the
 * mailbox array sizes from, and CHN-5 defines the ChainAdapter class and its
 * whole public surface in chainAdapter.cpp. The class declaration lives here
 * because a C++ class definition must appear in the header; CHN-5 adds its
 * member declarations to this same class and writes the member definitions
 * into the .cpp.
 *
 * THE MAILBOX COUNT IS COMPUTED FROM THE TOPOLOGY, NEVER WRITTEN DOWN. The
 * audio bus is fixed to Line and therefore always has dspCount + 1 mailboxes
 * (section 2.4 proves the line from the DMA slot counts); the second bus
 * takes its topology as a parameter, provisionally Ring, and its count is a
 * constant expression in the topology alone. Sizing the arrays from the
 * parameter -- rather than fixing the second bus to a ring -- is the whole
 * point of this task, because fixing it would put a value the spike has not
 * measured into the structure (section 23.1).
 */

#pragma once

namespace g2
{
	/* The three bus topologies of section 12.3. */
	enum class ChainTopology
	{
		Line,      /* open both ends:  N + 1 mailboxes             */
		Ring,      /* the tail feeds the head:  N mailboxes        */
		Broadcast  /* one shared 8-slot frame:  1 mailbox          */
	};

	/* The chain adapter that owns every Mailbox and drives the four phases
	 * of section 12.3 once per virtual frame.
	 *
	 * Ownership   Scheduler owns exactly one ChainAdapter.
	 * Lifetime    Constructed before the DSP set, destroyed after it. The
	 *             callbacks it hands out borrow this object and must not
	 *             outlive it; the Scheduler's destruction order guarantees
	 *             that.
	 * Threading   Scheduler thread only. No lock, no atomic, by
	 *             construction.
	 *
	 * CHN-4 declares this header with the constructor line and the inline
	 * mailboxCount below. The rest of the public surface -- the four phase
	 * methods, the per-position ESAI callbacks, the three counters and the
	 * state trio -- is task CHN-5, which adds its declarations to this class
	 * and defines them in chainAdapter.cpp. */
	class ChainAdapter
	{
	public:
		/* dspCount is the number of DSP positions; hopFrames is the hop
		 * delay H of section 4 row 9; secondBusTopology and
		 * secondBusFrameDivider are the second bus's topology and its frame
		 * divider (4 today, from AGENTS.md section 2.2's recorded 24 kHz
		 * control rate). Defined in chainAdapter.cpp (CHN-5). */
		ChainAdapter(unsigned dspCount, unsigned hopFrames,
		             ChainTopology secondBusTopology,
		             unsigned secondBusFrameDivider);

		/* The mailbox count for a topology, derived from it. Section 12.3
		 * gives the rule and the reason for each of the three values:
		 * Line -> N + 1, Ring -> N, Broadcast -> 1.
		 *
		 * DEFINED INLINE, not merely declared: a constexpr function must be
		 * defined before any use of it in a constant expression, and the
		 * mailbox arrays are sized by exactly such a use (section 13.10.2).
		 * A declaration alone would compile and link a target while failing
		 * every constant-expression use, which is the exact failure the
		 * CHN-4 Check catches with static_assert. */
		static constexpr unsigned mailboxCount(ChainTopology t,
		                                       unsigned dspCount) noexcept
		{
			return t == ChainTopology::Line ? dspCount + 1u
			     : t == ChainTopology::Ring ? dspCount
			     :                            1u;   /* Broadcast */
		}
	};
}
