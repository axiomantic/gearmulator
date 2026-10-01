/* The index relation, and nothing else, is what makes D_chain derivable. The
 * mailbox's whole behaviour is these three expressions:
 *
 *     write()    ==  m_ring[m_head]
 *     read()     ==  m_ring[(m_head + 1) % n]
 *
 *     advance(): 1. m_ring[(m_head + 1) % n] = m_ring[m_head];   FIRST
 *                2. m_head = (m_head + 1) % n
 *
 * The reported plugin latency is derived from that relation: the frame
 * written in quantum q is returned by read() in quantum q + H.
 *
 * The underrun rule is the copy-first order, asserted as an observable. When a
 * producer writes nothing in a quantum, advance() still copies the head cell
 * into the next cell before it steps, so the last transmitted frame keeps
 * circulating and read() repeats the previous frame. Successive reads being
 * identical is the "repeat" half of the rule that no single frame equality can
 * express.
 *
 * The test touches only the public Tier-1 surface and never reaches into the
 * ring, so a reimplementation that produced the same index relation from other
 * private state passes.
 */

#include "mailbox.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	/* A distinct, deterministic frame for a quantum. The marker is unique
	 * per (q, slot) so that two different quanta can never alias, and it
	 * stays inside the positive int32 range that Q23 frames carry. */
	int32_t marker(const unsigned q, const unsigned slot)
	{
		return static_cast<int32_t>(0x200000u + q * 0x10000u + slot);
	}

	void fill(g2::Frame& dst, const unsigned q)
	{
		for(unsigned k = 0u; k < g2::Frame::kSlots; ++k)
			dst.slot[k] = marker(q, k);
	}

	bool frameEqual(const g2::Frame& a, const g2::Frame& b)
	{
		for(unsigned k = 0u; k < g2::Frame::kSlots; ++k)
			if(a.slot[k] != b.slot[k])
				return false;
		return true;
	}

	void checkFrame(const g2::Frame& observed, const g2::Frame& expected,
		const unsigned q, const char* const what)
	{
		if(!frameEqual(observed, expected))
		{
			printf("FAIL %s: read() at q=%u is not the expected frame\n",
				what, q);
			++failures;
		}
	}

	/* Case 1: the delay relation. The frame written in quantum q is returned
	 * by read() in quantum q + H. */
	void driveDelayCase(const unsigned H, const char* const what)
	{
		const unsigned N = 32u;
		g2::Mailbox mb(H);

		for(unsigned q = 0u; q < N; ++q)
		{
			/* Producer fills the write frame for this quantum. */
			fill(mb.write(), q);

			/* Consumer observes the read frame for this quantum. It must be
			 * the frame the producer wrote H quanta ago. The first H quanta
			 * have no such frame and are skipped, matching the delay-fill. */
			if(q >= H)
			{
				g2::Frame expected;
				fill(expected, q - H);
				checkFrame(mb.read(), expected, q, what);
			}

			mb.advance();
		}

		/* The newest write (q = N - 1) has not yet propagated past the final
		 * advance, so it is correctly not observed. */
	}

	/* Case 2: the underrun rule. A producer that writes nothing lets
	 * advance() copy the previous frame forward, so read() repeats the
	 * previous frame. Write one frame, then produce nothing for the rest of
	 * the run, and assert every read() from q = H onward is that frame and
	 * that successive reads are identical. */
	void driveUnderrunCase(const unsigned H, const char* const what)
	{
		const unsigned N = 32u;
		g2::Mailbox mb(H);

		/* Quantum 0: the one and only write of the run. The marker 999 is
		 * distinct from every marker used by the delay case, so a value
		 * meant to be X can never equal one meant to be a delay marker. */
		const unsigned kX = 999u;

		g2::Frame expected;
		fill(expected, kX);

		fill(mb.write(), kX);
		mb.advance();

		/* Quanta 1 .. N: the producer writes nothing. From q = H the frame
		 * written in quantum 0 must have arrived and be repeated forever,
		 * because advance() with a silent producer copies it forward. */
		g2::Frame previous{};
		bool havePrevious = false;

		for(unsigned q = 1u; q <= N; ++q)
		{
			/* The producer writes nothing this quantum. */
			if(q >= H)
			{
				checkFrame(mb.read(), expected, q, what);

				if(havePrevious && !frameEqual(mb.read(), previous))
				{
					printf("FAIL %s: at q=%u read() did not repeat the "
						"previous frame\n", what, q);
					++failures;
				}

				previous = mb.read();
				havePrevious = true;
			}

			mb.advance();
		}
	}
}

int main()
{
	driveDelayCase(1u, "delay H=1");
	driveDelayCase(2u, "delay H=2");
	driveUnderrunCase(1u, "underrun H=1");
	driveUnderrunCase(2u, "underrun H=2");

	if(failures != 0)
	{
		printf("t0_mailbox_index: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_mailbox_index: all cases passed\n");
	return 0;
}
