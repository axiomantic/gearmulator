/* chainAdapter.h -- the chain adapter's header. Tasks CHN-4 and CHN-5.
 * Design sections 12.3 and 13.10.2.
 *
 * THIS HEADER LAYS DOWN THE DECLARATION, AND CHN-5 DEFINES THE CLASS.
 * The chain tasks split the one file: CHN-4 owns `ChainTopology` and the
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

/* CHN-5 needs the Mailbox type (whose delay line the adapter owns) and the
 * dsp56300 callback types the four callback factories return.
 *
 * Mailbox is pulled in by mailbox.h, which also brings g2::Frame and
 * g2::SlotWriteView; frame.h pulls in "dsp56kEmu/audio.h", which declares
 * dsp56k::Audio's ReadRxCallback and WriteTxCallback aliases that the two
 * alias types below bind to. */
#include "mailbox.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/* CHN-6: the transmit wrappers inspect the emulated ESAI's frame-lifetime
 * transmit-underrun latch through Esai::txUnderrunInFrame(). The adapter stores
 * only borrowed pointers to the per-position ESAs (the DSP set owns them,
 * section 13.10.2), so a forward declaration is enough here; chainAdapter.cpp
 * includes the full dsp56kEmu/esai.h when it dereferences them. */
namespace dsp56k { class Esai; }

namespace g2
{
	/* The two alias types the four callback factories return, bound to the
	 * real dsp56300 signatures at audio.h:116-117:
	 *
	 *     EsaiReadRxCallback  = std::function<void(uint64_t&, RxFrame&)>
	 *     EsaiWriteTxCallback = std::function<void(uint64_t&, const TxFrame&)>
	 *
	 * They are declared BEFORE the class, not after it as the design section
	 * 13.10.2 listing shows: the class's member declarations name them, and a
	 * type must be declared before it is named in a member signature. The
	 * design's prose ordering is illustrative, not compilable, and this
	 * header carries the compilable order.
	 *
	 * The first argument is the mutable frame-index reference the library
	 * hands every callback. Section 13.10.2's frame-index rule makes every
	 * callback this adapter installs treat that reference as READ-ONLY and
	 * never write it: the scheduler's own virtual frame index is the only
	 * authoritative one. */
	using EsaiReadRxCallback  = dsp56k::Audio::ReadRxCallback;
	using EsaiWriteTxCallback = dsp56k::Audio::WriteTxCallback;

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
	 * mailboxCount below. CHN-5 defines the whole public surface -- the
	 * phase methods, the per-position ESAI callbacks, the counters and
	 * the state trio -- in chainAdapter.cpp and adds their declarations here.
	 * The behavioural depth each later chain task adds is stated on the
	 * individual member, so that this task lays the surface down without
	 * pre-empting the task the plan names as the owner of a behaviour. */
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

		/* ------------- CHN-5 accessors.
		 *
		 * The four are the constructor arguments, stored and readable back.
		 * The CHN-5 Check constructs one adapter with each argument set to a
		 * distinct value and reads each back through these four, which is
		 * what makes "the constructor forwards its arguments" testable
		 * rather than stated.
		 *
		 * audioMailboxCount and secondBusMailboxCount report the two buses'
		 * mailbox counts, derived from the stored arguments. audioMailboxCount
		 * is mailboxes(Line, dspCount) = dspCount + 1 at EVERY second-bus
		 * topology, which is the property the CHN-5 Check asserts across a
		 * range of topology arguments: the audio chain is fixed to Line and
		 * the second-bus topology parameter must not be able to move it. */
		unsigned       dspCount() const noexcept;
		unsigned       hopFrames() const noexcept;
		ChainTopology  secondBusTopology() const noexcept;
		unsigned       secondBusFrameDivider() const noexcept;
		unsigned       audioMailboxCount() const noexcept;
		unsigned       secondBusMailboxCount() const noexcept;

		/* ------------- The four phases of section 12.3, in this order, once
		 * for each virtual frame.
		 *
		 * advanceAll is the swap point and ALSO closes the underrun
		 * accounting for the quantum that just ended. The full four-step
		 * order with the 2 x dspCount written flags is task CHN-7's (it adds
		 * the count-then-clear-then-advance sequence to this body); CHN-5
		 * lays down the swap itself -- audio mailboxes advance every quantum,
		 * second-bus mailboxes only when frameIndex % secondBusFrameDivider
		 * == 0 -- which is the cadence that does not move after CHN-7 lands.
		 *
		 * CHN-9 owns the four-phase procedure end to end, driven through a
		 * Scheduler; the codec edges below are where the run phase is entered
		 * and left from. */
		void advanceAll(uint64_t frameIndex) noexcept;   /* 1 swap    */
		void injectCodecSource(const Frame&)   noexcept; /* 2 ingress */
		/*        3 run phase happens in the Scheduler, through the callbacks */
		void extractCodecSink (Frame& out)     noexcept; /* 4 egress  */

		/* ------------- The four callback factories.
		 *
		 * Each returns a callable that borrows this ChainAdapter and is
		 * installed on one DSP's ESAI at construction of the DSP set.
		 * Position is 0..dspCount-1. The mailbox each one reaches follows
		 * section 12.3's wiring table: on a Line a receive reads mailbox k
		 * and a transmit writes mailbox k + 1; on a Ring the transmit writes
		 * (k + 1) mod N; on a Broadcast every position reads and writes the
		 * one mailbox (a transmit commits slot k only, through
		 * Mailbox::writeSlot, which is what keeps eight producers from
		 * overwriting one another).
		 *
		 * CHN-6 owns the written-flag rule that these transmit wrappers
		 * gain (the wrappers capture and inspect the emulated ESAI's
		 * transmit-underrun latch); CHN-5 lays down the wiring the wrappers
		 * carry. CHN-12 tests the wiring table for all three topologies. */
		EsaiReadRxCallback  audioRxCallback (unsigned position);
		EsaiWriteTxCallback audioTxCallback (unsigned position);
		EsaiReadRxCallback  secondRxCallback(unsigned position);
		EsaiWriteTxCallback secondTxCallback(unsigned position);

		/* ------------- CHN-6, the written-flag mechanism.
		 *
		 * Each transmit wrapper records WHICH KIND of delivery its position
		 * just received: a good frame, a frame carrying a transmit underrun,
		 * or - when no wrapper fires in a quantum, or no Esai is attached -
		 * no frame at all. The source is the emulated ESAI's frame-lifetime
		 * underrun latch, Esai::txUnderrunInFrame(), read at the instant the
		 * callback fires (section 12.3).
		 *
		 * IT READ THE M_TUE STATUS BIT HERE, AND THAT INPUT WAS UNREACHABLE.
		 * The claim that stood in this block -- "M_TUE rises in
		 * writeSlotToFrame before the frame is delivered and is not cleared
		 * until the interrupt path runs after" -- is false on a running
		 * machine, and it is quoted rather than deleted because it is the
		 * premise the defect rested on. writeSlotToFrame raises M_TUE and
		 * then triggers the transmit DMA, which is serviced synchronously and
		 * reaches Esai::writeTX, which clears the bit again before
		 * writeSlotToFrame returns - slots before the frame carrying the
		 * stale slot is delivered.
		 *
		 * THE COUNTER FED BY THESE FLAGS WAS NEVER SILENT, and no paragraph
		 * here should say it was. "No frame delivered this quantum" always
		 * reached underrunFrames and is what every assertion on that counter
		 * predating this change exercises. It is the OTHER value - "a frame
		 * arrived and it was stale" - that could not occur, and the latch is
		 * what makes it occur.
		 *
		 * attachEsai records the borrowed per-position Esai pointers a
		 * position's transmit wrappers inspect. The DSP set calls it for
		 * every position at construction, BEFORE it produces the four
		 * callbacks - the position's wrappers need the Esai from the first
		 * fire. It is CHN-6's seam so the rule is testable before CHN-7's
		 * advanceAll consumes the flags.
		 *
		 * audioWritten/secondWritten read one position's flag back and report
		 * the GOOD case only, so they are false for a stale delivery and for
		 * no delivery alike; they are the test and diagnostic surface of the
		 * rule. Nothing can SET a flag outside ChainAdapter - a flag changes
		 * only when its own transmit wrapper fires. */
		void attachEsai(unsigned position, dsp56k::Esai& audio, dsp56k::Esai& second);
		bool audioWritten (unsigned position) const noexcept;
		bool secondWritten(unsigned position) const noexcept;

		/* ------------- The counters.
		 *
		 * underrunFrames counts, per position, quanta in which the audio
		 * bus's transmit wrapper was not satisfied; secondBusUnderrunFrames
		 * counts the same for the second bus on its window quanta only;
		 * phaseErrorFrames counts transmit callbacks the scheduler did not
		 * ask for. CHN-5 declares and implements them so the surface links;
		 * the counting storage and the advanceAll cadence that feeds them
		 * are tasks CHN-7 and CHN-8 (a counter that cannot be driven above
		 * zero is a green mirage, so this task returns zero and lets the
		 * owning task replace the body). */
		uint64_t underrunFrames(unsigned position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned position) const noexcept;
		uint64_t phaseErrorFrames(unsigned position) const noexcept;

		/* ------------- The state trio.
		 *
		 * stateSize/stateSave/stateLoad serialise the adapter's
		 * determinism-relevant state into the Scheduler snapshot. CHN-14 owns
		 * the save-and-load round trip (mailbox contents and counters);
		 * CHN-5 declares and defines the trio so the surface links.
		 *
		 * stateLoad's RETURN TYPE IS RECONCILED, AND THE DEVIATION THAT STOOD
		 * HERE IS QUOTED RATHER THAN DELETED. It read: "The design declares
		 * `Status stateLoad(const void*)`, but g2::Status is owned by task
		 * SCH-18 ... so stateLoad returns void here ... When SCH-18 has created
		 * status.h, the chain adapter's stateLoad is reconciled to return
		 * g2::Status." status.h exists, and SCH-21 step 4 -- which absorbed
		 * SCH-24 -- is the task that owns the correction.
		 *
		 * WHAT IT REPORTS. Status::Ok, or Status::BadStateImage for a null
		 * source and for an image whose geometry header describes a
		 * differently-shaped adapter. BOTH REFUSALS EXISTED ALREADY and both
		 * were SILENT: the body returned without touching a member and the
		 * caller could not tell that from a load that ran. The refusal is the
		 * reason the geometry header is in the image at all, so a channel to
		 * report it on is what makes the header a guard.
		 *
		 * THE REFUSAL IS TOTAL AND IT HAPPENS BEFORE ANY MEMBER MOVES. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* ------------- The reset. Task SCH-21 step 3, design section 13.10.5.
		 *
		 * IT ZEROES EVERY EMULATED MEMORY THIS OBJECT OWNS AND EVERY COUNTER
		 * IT KEEPS: every frame of every mailbox on both buses, every ring
		 * head, both written-flag arrays, and the three per-position counters.
		 * THE GEOMETRY IS NOT TOUCHED -- the position count, the hop, the
		 * topology and the divider are construction parameters and not state,
		 * so no ring is resized and no vector reallocates.
		 *
		 * THE BORROWED Esai POINTERS ARE NOT TOUCHED EITHER. They name the DSP
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

		/* ------------- CHN-6 private state: the written flags and the
		 * Esai pointers the transmit wrappers inspect.
		 *
		 * m_audioWritten and m_secondWritten hold ONE flag for each position
		 * and EACH bus, so 2 x dspCount flags and never dspCount (section
		 * 13.10.2). A position's audio transmit wrapper sets m_audioWritten
		 * and its second transmit wrapper sets m_secondWritten, which is
		 * what lets CHN-7 count the two buses on different cadences.
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
		 * pointers (owned by the DSP set), populated by attachEsai before
		 * the factories are produced. */
		std::vector<dsp56k::Esai*> m_audioEsai;
		std::vector<dsp56k::Esai*> m_secondEsai;
		std::vector<uint8_t>       m_audioWritten;
		std::vector<uint8_t>       m_secondWritten;

		/* ------------- CHN-7 private state: the two underrun counters.
		 *
		 * One uint64_t for each position for each bus. advanceAll step 1
		 * increments m_underrun[position] on EVERY quantum in which the
		 * position's audio-bus flag is not kGoodDelivery - so for a quantum
		 * that delivered nothing AND for one that delivered a stale frame;
		 * step 2 increments
		 * m_secondUnderrun[position] ONLY on the second-bus window quanta
		 * (frameIndex % secondBusFrameDivider == 0). The two are separate
		 * storage, because the two buses advance at different rates, and
		 * both are sized to dspCount at construction so advanceAll indexes
		 * them without resizing. The third counter, phaseErrorFrames, counts
		 * in the transmit wrappers rather than in advanceAll and is task
		 * CHN-8's. */
		std::vector<uint64_t> m_underrun;
		std::vector<uint64_t> m_secondUnderrun;

		/* ------------- CHN-8 private state: the phase-error counter.
		 *
		 * ONE uint64_t for each position, shared across the two buses
		 * (section 13.10.2 counts "a transmit callback the scheduler did not
		 * ask for, on either bus"). The transmit wrappers increment it, never
		 * advanceAll: it counts the out-of-band execTX() callbacks
		 * (esai.cpp:209) and a duplicate delivery, which are events that
		 * happen in the wrappers. Sized to dspCount at construction so a
		 * wrapper indexes it without resizing. */
		std::vector<uint64_t> m_phaseError;
	};
}
