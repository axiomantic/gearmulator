/* The mailbox count is computed from the topology, never written down. The
 * audio bus is fixed to Line and therefore always has dspCount + 1 mailboxes;
 * the second bus takes its topology as a parameter, provisionally Ring, and
 * its count is a constant expression in the topology alone. Sizing the arrays
 * from the parameter -- rather than fixing the second bus to a ring -- keeps
 * an unmeasured value out of the structure.
 */

#pragma once

/* mailbox.h also brings g2::Frame and g2::SlotWriteView; frame.h pulls in
 * "dsp56kEmu/audio.h", which declares dsp56k::Audio's ReadRxCallback and
 * WriteTxCallback aliases that the two alias types below bind to. */
#include "mailbox.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/* The adapter stores only borrowed pointers to the per-position ESAIs (the
 * DSP set owns them), so a forward declaration is enough here;
 * chainAdapter.cpp includes the full dsp56kEmu/esai.h to dereference them. */
namespace dsp56k { class Esai; }

namespace g2
{
	/* The two alias types the four callback factories return:
	 *
	 *     EsaiReadRxCallback  = std::function<void(uint64_t&, RxFrame&)>
	 *     EsaiWriteTxCallback = std::function<void(uint64_t&, const TxFrame&)>
	 *
	 * The first argument is the mutable frame-index reference the library
	 * hands every callback. Every callback this adapter installs treats that
	 * reference as read-only and never writes it: the scheduler's own virtual
	 * frame index is the only authoritative one. */
	using EsaiReadRxCallback  = dsp56k::Audio::ReadRxCallback;
	using EsaiWriteTxCallback = dsp56k::Audio::WriteTxCallback;

	/* The three bus topologies. */
	enum class ChainTopology
	{
		Line,      /* open both ends:  N + 1 mailboxes             */
		Ring,      /* the tail feeds the head:  N mailboxes        */
		Broadcast  /* one shared 8-slot frame:  1 mailbox          */
	};

	/* The chain adapter that owns every Mailbox and drives the four phases
	 * once per virtual frame.
	 *
	 * Ownership   Scheduler owns exactly one ChainAdapter.
	 * Lifetime    Constructed before the DSP set, destroyed after it. The
	 *             callbacks it hands out borrow this object and must not
	 *             outlive it; the Scheduler's destruction order guarantees
	 *             that.
	 * Threading   Scheduler thread only. No lock, no atomic, by
	 *             construction. */
	class ChainAdapter
	{
	public:
		/* dspCount is the number of DSP positions; hopFrames is the hop
		 * delay H; secondBusTopology and secondBusFrameDivider are the second
		 * bus's topology and its frame divider, 4 today for a 24 kHz control
		 * rate. */
		ChainAdapter(unsigned dspCount, unsigned hopFrames,
		             ChainTopology secondBusTopology,
		             unsigned secondBusFrameDivider);

		/* The mailbox count for a topology: Line -> N + 1, Ring -> N,
		 * Broadcast -> 1.
		 *
		 * Defined inline, not merely declared: a constexpr function must be
		 * defined before any use of it in a constant expression, and the
		 * mailbox arrays are sized by exactly such a use. A declaration alone
		 * would compile and link while failing every constant-expression
		 * use. */
		static constexpr unsigned mailboxCount(ChainTopology t,
		                                       unsigned dspCount) noexcept
		{
			return t == ChainTopology::Line ? dspCount + 1u
			     : t == ChainTopology::Ring ? dspCount
			     :                            1u;   /* Broadcast */
		}

		/* audioMailboxCount is mailboxCount(Line, dspCount) = dspCount + 1 at
		 * every second-bus topology: the audio chain is fixed to Line and the
		 * second-bus topology parameter must not be able to move it. */
		unsigned       dspCount() const noexcept;
		unsigned       hopFrames() const noexcept;
		ChainTopology  secondBusTopology() const noexcept;
		unsigned       secondBusFrameDivider() const noexcept;
		unsigned       audioMailboxCount() const noexcept;
		unsigned       secondBusMailboxCount() const noexcept;

		/* The four phases, in this order, once for each virtual frame.
		 *
		 * advanceAll is the swap point and also closes the underrun accounting
		 * for the quantum that just ended. Audio mailboxes advance every
		 * quantum; second-bus mailboxes only when
		 * frameIndex % secondBusFrameDivider == 0. */
		void advanceAll(uint64_t frameIndex) noexcept;   /* 1 swap    */
		void injectCodecSource(const Frame&)   noexcept; /* 2 ingress */
		/*        3 run phase happens in the Scheduler, through the callbacks */
		void extractCodecSink (Frame& out)     noexcept; /* 4 egress  */

		/* ------------- The four callback factories.
		 *
		 * Each returns a callable that borrows this ChainAdapter and is
		 * installed on one DSP's ESAI at construction of the DSP set.
		 * Position is 0..dspCount-1. The mailbox each one reaches follows
		 * the wiring table: on a Line a receive reads mailbox k
		 * and a transmit writes mailbox k + 1; on a Ring the transmit writes
		 * (k + 1) mod N; on a Broadcast every position reads and writes the
		 * one mailbox (a transmit commits slot k only, through
		 * Mailbox::writeSlot, which is what keeps eight producers from
		 * overwriting one another). */
		EsaiReadRxCallback  audioRxCallback (unsigned position);
		EsaiWriteTxCallback audioTxCallback (unsigned position);
		EsaiReadRxCallback  secondRxCallback(unsigned position);
		EsaiWriteTxCallback secondTxCallback(unsigned position);

		/* The written-flag mechanism.
		 *
		 * Each transmit wrapper sets its position's written flag: true exactly
		 * when the callback fires and the emulated ESAI's M_TUE
		 * transmit-underrun bit is clear in Esai::readStatusRegister() at that
		 * instant. M_TUE rises in writeSlotToFrame before the frame is
		 * delivered and is not cleared until the interrupt path runs after, so
		 * at the instant the callback runs the bit states whether the frame it
		 * carries is stale.
		 *
		 * The DSP set calls attachEsai for every position at construction,
		 * before it produces the four callbacks - the position's wrappers need
		 * the Esai from the first fire.
		 *
		 * Nothing can set a flag outside ChainAdapter: a flag changes only
		 * when its own transmit wrapper fires. */
		void attachEsai(unsigned position, dsp56k::Esai& audio, dsp56k::Esai& second);
		bool audioWritten (unsigned position) const noexcept;
		bool secondWritten(unsigned position) const noexcept;

		/* underrunFrames counts, per position, quanta in which the audio bus's
		 * transmit wrapper was not satisfied; secondBusUnderrunFrames counts
		 * the same for the second bus on its window quanta only;
		 * phaseErrorFrames counts transmit callbacks the scheduler did not ask
		 * for. */
		uint64_t underrunFrames(unsigned position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned position) const noexcept;
		uint64_t phaseErrorFrames(unsigned position) const noexcept;

		/* stateSize/stateSave/stateLoad serialise the adapter's
		 * determinism-relevant state into the Scheduler snapshot.
		 *
		 * stateSize/stateSave/stateLoad serialise the adapter's
		 * determinism-relevant state into the Scheduler snapshot. CHN-14 owns
		 * the save-and-load round trip (mailbox contents and counters);
		 * CHN-5 declares and defines the trio so the surface links.
		 *
		 * stateLoad reports Status::Ok, or Status::BadStateImage for a null
		 * source and for an image whose geometry header describes a
		 * differently-shaped adapter.
		 *
		 * The refusal is total and it happens before any member moves. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* ------------- The reset.
		 *
		 * The geometry is not touched -- the position count, the hop, the
		 * topology and the divider are construction parameters and not state,
		 * so no ring is resized and no vector reallocates.
		 *
		 * The borrowed Esai pointers are not touched either. They name the DSP
		 * set's ports, the set outlives this object, and clearing them would
		 * leave the transmit wrappers pointing at nothing while every check
		 * here stayed green. */
		void reset() noexcept;

	private:
		/* The two buses' mailboxes. The audio bus is fixed to Line and holds
		 * mailboxCount(Line, m_dspCount); the second bus holds
		 * mailboxCount(m_secondBusTopology, m_dspCount). Each Mailbox is a
		 * delay line of m_hopFrames + 1 frames, allocated once here. */
		std::vector<Mailbox> m_audio;
		std::vector<Mailbox> m_second;

		unsigned       m_dspCount            = 0;
		unsigned       m_hopFrames           = 0;
		ChainTopology  m_secondBusTopology   = ChainTopology::Ring;
		unsigned       m_secondBusFrameDivider = 1;

		/* m_audioWritten and m_secondWritten hold one flag for each position
		 * and each bus, so 2 x dspCount flags and never dspCount. That is what
		 * lets the two buses be counted on different cadences.
		 *
		 * EACH FLAG HAS THREE VALUES, NOT TWO, and chainAdapter.cpp names
		 * them kNoDelivery / kGoodDelivery / kStaleDelivery. Two readers ask
		 * different questions of the same byte: CHN-7's underrun count asks
		 * whether the delivery was GOOD, and CHN-8's phase-error check asks
		 * whether there was a delivery AT ALL. A two-valued flag can answer
		 * only one of them, and encoding a stale delivery as "no delivery"
		 * would blind the phase-error rule on exactly the quanta the underrun
		 * rule fires on.
		 *
		 * m_audioEsai / m_secondEsai are the borrowed per-position Esai
		 * pointers (owned by the DSP set), populated by attachEsai before the
		 * four factories are produced. */
		std::vector<dsp56k::Esai*> m_audioEsai;
		std::vector<dsp56k::Esai*> m_secondEsai;
		std::vector<uint8_t>       m_audioWritten;
		std::vector<uint8_t>       m_secondWritten;

		/* One uint64_t for each position for each bus. advanceAll step 1
		 * increments m_underrun[position] on EVERY quantum when the
		 * position's audio-bus written flag is clear; step 2 increments
		 * m_secondUnderrun[position] ONLY on the second-bus window quanta
		 * (frameIndex % secondBusFrameDivider == 0). The two are separate
		 * storage, because the two buses advance at different rates, and
		 * both are sized to dspCount at construction so advanceAll indexes
		 * them without resizing. */
		std::vector<uint64_t> m_underrun;
		std::vector<uint64_t> m_secondUnderrun;

		/* One uint64_t for each position, shared across the two buses. The
		 * transmit wrappers increment it, never advanceAll: it counts the
		 * out-of-band execTX() callbacks and a duplicate delivery, which are
		 * events that happen in the wrappers. Sized to dspCount at
		 * construction so a wrapper indexes it without resizing. */
		std::vector<uint64_t> m_phaseError;
	};
}
