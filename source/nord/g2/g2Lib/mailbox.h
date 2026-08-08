/* mailbox.h -- g2::Mailbox, the chain delay line. Task CHN-1.
 * Design sections 12.3 and 13.10.2.
 *
 * THE MAILBOX IS A DELAY LINE, NOT A SINGLE BUFFER. It is a ring of
 * hopFrames + 1 frames with one head index, and the whole of its behaviour is
 * the index relation of section 12.3:
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
 * THE STORED FRAME IS g2::Frame, AND NOTHING ELSE. The two library frame
 * types dsp56k::Audio::TxFrame and dsp56k::Audio::RxFrame are different,
 * non-convertible types, so a mailbox that stored one of them could not
 * accept the other. The conversion at each edge is SCH-4's fromEsaiFrame and
 * toEsaiFrame overload pair; the mailbox sits between them and touches only
 * g2::Frame.
 *
 * THE SURFACE IS IN TWO TIERS. Tier 1 -- read(), write() and writeSlot() --
 * is all a DSP callback may reach during the run phase. Tier 2 -- advance(),
 * ingressFrame() and egressFrame() -- belongs to ChainAdapter and to nothing
 * else. The two codec-facing accessors are declared private with ChainAdapter
 * a friend (see design section 12.3 steps 2 and 4), so Tier 1 stays the whole
 * surface a DSP callback can name.
 *
 * writeSlot(unsigned) returns the SlotWriteView that CHN-3 declares in
 * slotWriteView.h. CHN-1's declared surface is what CHN-12's broadcast
 * wiring compiles against.
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
	/* The class ChainAdapter is declared here, and nowhere else yet, so that
	 * Mailbox can name it as a friend. CHN-4 lays down chainAdapter.h and
	 * CHN-5 defines the class; both live in the same namespace, so this
	 * forward declaration binds to that definition. */
	class ChainAdapter;

	/* A delay line of hopFrames + 1 frames between two chain positions.
	 * Section 12.3 gives the phase rule and section 13.10.2 the class.
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

		/* The FOURTH accessor. Section 12.3 said "three accessors, and no
		 * others", which was true of a Line and false of a Broadcast.
		 * Returns a view that commits `slot` only, and nothing else. On a
		 * Line or a Ring the adapter never hands one out. */
		SlotWriteView writeSlot(unsigned slot) noexcept;

		/* ------------- Tier 2: the adapter's own surface, reachable by
		 * nothing else. */
		void advance() noexcept;              /* the swap point            */

		/* The structural query the CHN-1 check is keyed to: the ring depth is
		 * exactly hopFrames + 1. Not part of either phase surface, and
		 * reachable by a DSP callback without consequence -- it reads no
		 * audio data. */
		unsigned depth() const noexcept;

	private:
		/* The two PRIVILEGED codec-facing accessors. They exist because
		 * section 12.3 steps 2 and 4 deliberately break the run-phase
		 * invariant, in the defined way that section states, and neither is
		 * expressible through read() or write():
		 *   - step 2 writes the READ frame, and read() is const;
		 *   - step 4 reads the WRITE frame, and write() is reserved to the
		 *     producing DSP's transmit callback.
		 * ChainAdapter is the only caller of either. */
		friend class ChainAdapter;

		Frame&       ingressFrame() noexcept;       /* step 2: write read()   */
		const Frame& egressFrame() const noexcept;  /* step 4: read write()   */

		/* THE INDEX RELATION -- the whole behaviour of the type. The
		 * implementation is in mailbox.cpp and is stated as the three
		 * expressions above. */
		std::vector<Frame> m_ring;   /* size hopFrames + 1, allocated once  */
		unsigned           m_head = 0;
	};
}
