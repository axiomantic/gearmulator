/* g2::Mailbox, the chain delay line.
 *
 * The mailbox is a delay line, not a single buffer: a ring of hopFrames + 1
 * frames with one head index, and the whole of its behaviour is this index
 * relation:
 *
 *     write()   == m_ring[m_head]
 *     read()    == m_ring[(m_head + 1) % n]          n = m_ring.size()
 *
 *     advance(): 1. m_ring[(m_head + 1) % n] = m_ring[m_head];   FIRST
 *                2. m_head = (m_head + 1) % n
 *
 * The order inside advance() is load-bearing: the copy happens FIRST, against
 * the pre-step head, so a position whose producer wrote nothing leaves its
 * last transmitted frame in place (the underrun rule). A frame written in
 * quantum q is read in quantum q + H, which is the exact hop delay the chain
 * latency D_chain is derived from.
 *
 * The stored frame is g2::Frame and nothing else. The two library frame types
 * dsp56k::Audio::TxFrame and dsp56k::Audio::RxFrame are different,
 * non-convertible types, so a mailbox that stored one of them could not accept
 * the other. The conversion at each edge is the fromEsaiFrame/toEsaiFrame
 * overload pair; the mailbox sits between them.
 *
 * The surface is in two tiers. Tier 1 -- read(), write() and writeSlot() -- is
 * all a DSP callback may reach during the run phase. Tier 2 -- advance(),
 * ingressFrame() and egressFrame() -- belongs to ChainAdapter and to nothing
 * else. The two codec-facing accessors are private with ChainAdapter a friend,
 * so Tier 1 stays the whole surface a DSP callback can name.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "frame.h"
#include "slotWriteView.h"

namespace g2
{
	/* Forward-declared so that Mailbox can name ChainAdapter as a friend. */
	class ChainAdapter;

	/* A delay line of hopFrames + 1 frames between two chain positions.
	 *
	 * Ownership   ChainAdapter owns every Mailbox by value. Nothing else
	 *             holds a Mailbox, a pointer to one, or a reference that
	 *             outlives a quantum.
	 * Lifetime    Constructed with the ChainAdapter. Destroyed with it.
	 * Threading   Scheduler thread only. Not thread-safe and does not need to
	 *             be: within a quantum the run-phase invariant means no two
	 *             contexts touch the same Mailbox phase.
	 */
	class Mailbox
	{
	public:
		/* PRECONDITION: hopFrames >= 1. Not checked here. Scheduler::create
		 * rejects a Config whose hopFrames is 0 with Status::BadHopFrames
		 * before any Mailbox is constructed, so this constructor is
		 * unreachable with a bad value in a correct build. A debug build
		 * asserts it. */
		explicit Mailbox(unsigned hopFrames);

		/* ------------- Tier 1: the run-phase surface, reachable by a DSP
		 * callback. */
		const Frame& read() const noexcept;   /* run phase: consumers   */
		Frame&       write() noexcept;        /* run phase: producers   */

		/* Returns a view that commits `slot` only, and nothing else. On a Line
		 * or a Ring the adapter never hands one out. */
		SlotWriteView writeSlot(unsigned slot) noexcept;

		/* ------------- Tier 2: the adapter's own surface, reachable by
		 * nothing else. */
		void advance() noexcept;              /* the swap point            */

		/* The ring depth, exactly hopFrames + 1. Not part of either phase
		 * surface, and reachable by a DSP callback without consequence -- it
		 * reads no audio data. */
		unsigned depth() const noexcept;

	private:
		/* The two privileged codec-facing accessors. The ingress and egress
		 * steps deliberately break the run-phase invariant, and neither is
		 * expressible through read() or write():
		 *   - step 2 writes the READ frame, and read() is const;
		 *   - step 4 reads the WRITE frame, and write() is reserved to the
		 *     producing DSP's transmit callback.
		 * ChainAdapter is the only caller of either. */
		friend class ChainAdapter;

		Frame&       ingressFrame() noexcept;       /* step 2: write read()   */
		const Frame& egressFrame() const noexcept;  /* step 4: read write()   */

		std::vector<Frame> m_ring;   /* size hopFrames + 1, allocated once  */
		unsigned           m_head = 0;
	};
}
