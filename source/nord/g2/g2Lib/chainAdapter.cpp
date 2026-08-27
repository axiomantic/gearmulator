/* chainAdapter.cpp -- the chain adapter's implementation. Task CHN-5.
 * Design sections 12.3 and 13.10.2.
 *
 * CHN-5 defines the whole public surface the CHN-4 header declares, and its
 * Check is exactly the properties a surface-task can own: the
 * constructor arguments are stored and readable back; the audio chain
 * reports precisely dspCount + 1 mailboxes at every second-bus topology; and
 * every method on the declared surface has a definition, so taking its
 * address links. Each deeper behaviour is owned by a later chain task, and
 * this file says, on the member that owns it, which task that is:
 *
 *   advanceAll's count-then-clear-then-advance accounting ... CHN-7
 *   the transmit wrappers' written-flag rule ................. CHN-6
 *   the counters' real storage and cadence ................... CHN-7, CHN-8
 *   the save-and-load round trip ............................. CHN-14
 *   the head/tail slot mappings and the Scheduler-driven four phase ... CHN-9
 *
 * Chains that assemble this surface take
 *   m_audio  = mailboxCount(ChainTopology::Line, dspCount)         mailboxes
 *   m_second = mailboxCount(secondBusTopology,     dspCount)      mailboxes
 * each a delay line of hopFrames + 1 frames, and the count follows section
 * 12.3's rule: it is computed from the topology and never written down.
 */

#include "chainAdapter.h"

#include "frame.h"
#include "dsp56kEmu/esai.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <type_traits>

namespace g2
{
	namespace
	{
		/* The two buses, used to pick which mailbox array a callback reaches.
		 * Kept local: nothing outside this translation unit needs the name. */
		enum class Bus
		{
			Audio,
			Second
		};

		/* One ESAI receive/transmit frame of the chain carries the slot count
		 * section 12.3 states. The head/tail special cases -- slots in at the
		 * head, out at the tail -- belong to the chain track; the uniform
		 * form is the one this surface carries. Frame::kSlots is the
		 * constant so the two cannot drift apart. */
		constexpr unsigned kChainSlots = Frame::kSlots;

		/* The receive/transmit register index each bus uses. The audio chain
		 * receives on M_RX0 (reg 0) and transmits on M_TX0 (reg 0); the
		 * second bus receives on M_RX0_1 (reg 0) and transmits on M_TX2_1
		 * (reg 2). frame.h carries the full table; these two are the two the
		 * adapter needs here. */
		constexpr unsigned kAudioReg   = 0u;
		constexpr unsigned kSecondReg  = 2u;
	}

	ChainAdapter::ChainAdapter(const unsigned dspCount,
	                           const unsigned hopFrames,
	                           const ChainTopology secondBusTopology,
	                           const unsigned secondBusFrameDivider)
		: m_audio(mailboxCount(ChainTopology::Line, dspCount), Mailbox(hopFrames))
		, m_second(mailboxCount(secondBusTopology, dspCount), Mailbox(hopFrames))
		, m_dspCount(dspCount)
		, m_hopFrames(hopFrames)
		, m_secondBusTopology(secondBusTopology)
		, m_secondBusFrameDivider(secondBusFrameDivider)
		/* CHN-6: one borrowed Esai pointer and one cleared written flag for
		 * each position on each bus. Sized to dspCount now, so a position's
		 * wrapper can index them from the first fire without resizing. */
		, m_audioEsai(dspCount, nullptr)
		, m_secondEsai(dspCount, nullptr)
		, m_audioWritten(dspCount, 0u)
		, m_secondWritten(dspCount, 0u)
		/* CHN-7: one counter per position per bus, so advanceAll can index
		 * them from the first fire without resizing. */
		, m_underrun(dspCount, 0u)
		, m_secondUnderrun(dspCount, 0u)
		/* CHN-8: one phase-error counter per position, shared across the
		 * two buses, so a wrapper can index it from the first fire without
		 * resizing. */
		, m_phaseError(dspCount, 0u)
	{
		assert(hopFrames >= 1u
			&& "a mailbox with a hop delay of zero cannot express the delay "
			"line - Scheduler::create rejects it before this is reached");
	}

	/* ------------- accessors. ------------------------------------------------- */

	unsigned ChainAdapter::dspCount() const noexcept
	{
		return m_dspCount;
	}

	unsigned ChainAdapter::hopFrames() const noexcept
	{
		return m_hopFrames;
	}

	ChainTopology ChainAdapter::secondBusTopology() const noexcept
	{
		return m_secondBusTopology;
	}

	unsigned ChainAdapter::secondBusFrameDivider() const noexcept
	{
		return m_secondBusFrameDivider;
	}

	unsigned ChainAdapter::audioMailboxCount() const noexcept
	{
		return static_cast<unsigned>(m_audio.size());
	}

	unsigned ChainAdapter::secondBusMailboxCount() const noexcept
	{
		return static_cast<unsigned>(m_second.size());
	}

	/* ------------- CHN-6: the written flags and their Esai source. ---------- */

	void ChainAdapter::attachEsai(const unsigned position,
	                              dsp56k::Esai& audio,
	                              dsp56k::Esai& second)
	{
		if(position < m_audioEsai.size())
			m_audioEsai[position] = &audio;
		if(position < m_secondEsai.size())
			m_secondEsai[position] = &second;
	}

	/* ------------- CHN-6: the three values a written flag can hold.
	 *
	 * The flag used to be a bool, and that is the shape of the defect it now
	 * repairs. TWO different questions read it: the underrun rule asks whether
	 * this position delivered a GOOD frame in this quantum, and the phase-error
	 * rule asks whether it delivered AT ALL. A bool can answer only one of
	 * them, so a stale delivery had to be encoded as "no delivery" and the
	 * phase-error rule went blind on exactly the quanta the underrun rule
	 * fires on. Three states answer both, in the same byte the state image
	 * already carries. */
	namespace
	{
		constexpr uint8_t kNoDelivery    = 0u;
		constexpr uint8_t kGoodDelivery  = 1u;
		constexpr uint8_t kStaleDelivery = 2u;

		/* What one transmit wrapper writes into its position's flag. A null or
		 * unattached Esai reports kNoDelivery, which is what the committed
		 * conjunction did and is the right answer: a position with no
		 * peripheral behind it delivered nothing. */
		uint8_t deliveryState(const dsp56k::Esai* const _esai) noexcept
		{
			if(_esai == nullptr)
				return kNoDelivery;

			return _esai->txUnderrunInFrame() ? kStaleDelivery : kGoodDelivery;
		}
	}

	bool ChainAdapter::audioWritten(const unsigned position) const noexcept
	{
		return position < m_audioWritten.size() && m_audioWritten[position] == kGoodDelivery;
	}

	bool ChainAdapter::secondWritten(const unsigned position) const noexcept
	{
		return position < m_secondWritten.size() && m_secondWritten[position] == kGoodDelivery;
	}

	/* ------------- the swap point. -------------------------------------------- */

	/* CHN-7: advanceAll closes the underrun accounting for the quantum that
	 * just ended, in the FOUR-STEP ORDER of section 13.10.2, then swaps the
	 * selected mailboxes. There are 2 x dspCount written flags - one per
	 * position per bus - and the ORDER is load-bearing because the flags
	 * describe the quantum that ENDED, not the one about to start:
	 *
	 *   1. EVERY quantum: for each position, if its audio-bus flag is clear,
	 *      increment that position's underrunFrames.
	 *   2. ONLY when frameIndex % secondBusFrameDivider == 0: for each
	 *      position, if its second-bus flag is clear, increment that
	 *      position's secondBusUnderrunFrames. The second bus is not
	 *      expected to transmit outside its advance window, so examining its
	 *      flag there would count a non-event.
	 *   3. Clear the audio flags ALWAYS; clear the second-bus flags ONLY on
	 *      the same quanta as step 2, for the same reason.
	 *   4. advance() the selected mailboxes: the audio bus every quantum,
	 *      the second bus only on the window quanta. */
	void ChainAdapter::advanceAll(const uint64_t frameIndex) noexcept
	{
		const auto count = [this](const std::vector<uint8_t>& flags,
		                          std::vector<uint64_t>& counters) noexcept
		{
			/* A quantum counts as an underrun when the position did not deliver
			 * a GOOD frame on this bus: either nothing was delivered
			 * (kNoDelivery, including a position with no Esai attached) or what
			 * was delivered carried a transmit underrun (kStaleDelivery). The
			 * first disjunct is the one the committed code had; the second is
			 * the one it named and could not reach. */
			for(unsigned p = 0u; p < m_dspCount; ++p)
				if(!(p < flags.size() && flags[p] == kGoodDelivery))
					++counters[p];
		};

		/* Step 1: every quantum, count audio-bus underruns from clear
		 * audio-bus flags. */
		count(m_audioWritten, m_underrun);

		/* Step 2: only on the window quanta, count second-bus underruns. */
		const bool window = frameIndex % m_secondBusFrameDivider == 0u;
		if(window)
			count(m_secondWritten, m_secondUnderrun);

		/* Step 3: clear the audio flags always; clear the second-bus flags
		 * only on the window quanta. */
		std::fill(m_audioWritten.begin(), m_audioWritten.end(), 0u);
		if(window)
			std::fill(m_secondWritten.begin(), m_secondWritten.end(), 0u);

		/* Step 4: advance() the selected mailboxes - the audio bus every
		 * quantum, the second bus only on the window quanta. */
		for(auto& mailbox : m_audio)
			mailbox.advance();

		if(window)
			for(auto& mailbox : m_second)
				mailbox.advance();
	}

	/* ------------- the codec edges. -------------------------------------------- */

	void ChainAdapter::injectCodecSource(const Frame& src) noexcept
	{
		/* Section 12.3 step 2: write the codec stereo pair into slots 0 and 1
		 * of mailbox 0's READ frame, through ingressFrame() and not through
		 * read(), which is const. The head's DMA then places them at
		 * X:$001C04, not X:$001C00. Guarded so a degenerate 0-position
		 * adapter (a test fixture, not a machine) stays defined. */
		if(m_audio.empty())
			return;

		Frame& ingress = m_audio.front().ingressFrame();
		ingress.slot[0] = src.slot[0];
		ingress.slot[1] = src.slot[1];
	}

	void ChainAdapter::extractCodecSink(Frame& out) noexcept
	{
		/* Section 12.3 step 4: read slots 0 and 1 of the LAST audio mailbox's
		 * WRITE frame, through egressFrame() and not through write(), which
		 * only the producing DSP's transmit callback may call. The tail is
		 * the last mailbox of the Line: mailbox N, whose write frame holds
		 * the tail DSP's most recent transmit. The codec edges carry no
		 * delay of their own, which is what the hardware does. */
		if(m_audio.empty())
		{
			out.slot[0] = 0;
			out.slot[1] = 0;
			return;
		}

		const Frame& egress = m_audio.back().egressFrame();
		out.slot[0] = egress.slot[0];
		out.slot[1] = egress.slot[1];
	}

	/* ------------- the callback factories. ------------------------------------- */

	EsaiReadRxCallback ChainAdapter::audioRxCallback(const unsigned position)
	{
		return [this, position](uint64_t&, dsp56k::Audio::RxFrame& out) noexcept {
			/* Line: the receive callback of position k reads mailbox k's
			 * read() frame. The library frame is filled through the chain's
			 * single Rx conversion point, toEsaiFrame. CHN-9 owns the exact
			 * slot count a position expects (2 at the head, 8 elsewhere);
			 * this surface lays down the mailbox wiring. */
			if(this->m_audio.empty())
			{
				out.clear();
				return;
			}
			const unsigned index = position % static_cast<unsigned>(this->m_audio.size());
			toEsaiFrame(this->m_audio[index].read(), kAudioReg, out, kChainSlots);
		};
	}

	EsaiWriteTxCallback ChainAdapter::audioTxCallback(const unsigned position)
	{
		return [this, position](uint64_t&, const dsp56k::Audio::TxFrame& in) noexcept {
			/* CHN-8 PHASE-ERROR RULE (section 12.3, 13.10.2), the audio-bus
			 * half. phaseErrorFrames counts a transmit callback the scheduler
			 * did NOT ask for, on either bus. On the audio bus that condition
			 * is one thing: the position has ALREADY delivered on this bus in
			 * this quantum. "Already delivered" is exactly the position's
			 * audio written flag being NON-ZERO at this instant - advanceAll
			 * clears the audio flags every quantum, so a non-zero audio flag
			 * at callback time can only mean a previous audio transmit in this
			 * same quantum. The check runs BEFORE this callback updates the
			 * flag, so the lone, scheduler-driven delivery - the one callback
			 * the scheduler asks for - is not counted, while a second one is.
			 *
			 * NON-ZERO, NOT kGoodDelivery, AND THE DIFFERENCE IS LOAD-BEARING.
			 * This rule asks about ARRIVAL; the underrun rule asks about
			 * QUALITY. If this test demanded a good delivery, a quantum whose
			 * first delivery underran would report no phase error however many
			 * extra callbacks it carried - the underrun gate would blind this
			 * one in exactly the regime both exist to describe. */
			if(position < this->m_phaseError.size()
				&& position < this->m_audioWritten.size()
				&& this->m_audioWritten[position] != 0u)
			{
				++this->m_phaseError[position];
			}

			/* CHN-6 WRITTEN-FLAG RULE (section 12.3). The flag is NOT "the
			 * callback fired" - the scheduler drives a transmit callback for
			 * every position on every quantum, so a two-state arrival flag
			 * could never be clear and underrunFrames could never rise (a
			 * green mirage). It records WHICH KIND of delivery this was, and
			 * the source is the emulated ESAI's frame-lifetime underrun latch,
			 * Esai::txUnderrunInFrame(), read at this instant.
			 *
			 * IT USED TO READ THE M_TUE STATUS BIT HERE, AND THAT INPUT WAS
			 * UNREACHABLE. Not the counter -- underrunFrames was never silent;
			 * it counted the quanta in which no wrapper ran at all, which is
			 * its primary meaning and is what every pre-existing assertion on
			 * it tests. What could not reach it was the OTHER thing it was
			 * meant to catch: a frame that WAS delivered and was stale. The
			 * sentence that stood in this block claimed M_TUE "is
			 * not cleared until the interrupt path runs after" the delivery.
			 * That is false on a running machine and is quoted rather than
			 * deleted, because it is the premise the defect rested on:
			 * writeSlotToFrame raises M_TUE and then triggers the transmit
			 * DMA, which is serviced synchronously, reaches Esai::writeTX and
			 * clears M_TUE before writeSlotToFrame has even returned - slots
			 * before this callback runs at the frame boundary. Measured over
			 * one booted --impulse run with every slot forced to underrun:
			 * 2,080,110 raises of M_TUE, 2,079,950 of them cleared inside
			 * writeSlotToFrame, and this wrapper observed the bit set 7 times
			 * out of 224,495 callbacks. underrunFrames stayed 0.
			 *
			 * The latch is the same fact with the frame's lifetime instead of
			 * the slot's, so it is still standing when the frame it describes
			 * arrives here. The wrapper reaches its position's Esai through
			 * m_audioEsai, which attachEsai filled at construction. */
			if(position < this->m_audioWritten.size())
			{
				const dsp56k::Esai* esai =
					position < this->m_audioEsai.size() ? this->m_audioEsai[position] : nullptr;
				this->m_audioWritten[position] = deliveryState(esai);
			}

			/* Line: the transmit callback of position k writes mailbox k + 1's
			 * write() frame. The tail position N - 1 therefore writes mailbox
			 * N, which is the mailbox the egress phase reads. (CHN-9 owns the
			 * head/tail slot forms.) */
			if(this->m_audio.empty() || position + 1u >= this->m_audio.size())
				return;

			this->m_audio[position + 1u].write() = fromEsaiFrame(in, kAudioReg);
		};
	}

	EsaiReadRxCallback ChainAdapter::secondRxCallback(const unsigned position)
	{
		return [this, position](uint64_t&, dsp56k::Audio::RxFrame& out) noexcept {
			/* The second bus's topology is a parameter. On a Line or a Ring
			 * the receive callback of position k reads mailbox k; on a
			 * Broadcast every position reads the one shared mailbox. The
			 * wiring table is section 12.3's and CHN-12 tests them. */
			const unsigned n = static_cast<unsigned>(this->m_second.size());
			if(n == 0u)
			{
				out.clear();
				return;
			}

			const unsigned index = this->m_secondBusTopology == ChainTopology::Broadcast
				? 0u
				: position % n;
			toEsaiFrame(this->m_second[index].read(), kAudioReg, out, kChainSlots);
		};
	}

	EsaiWriteTxCallback ChainAdapter::secondTxCallback(const unsigned position)
	{
		return [this, position](uint64_t& frameIndex, const dsp56k::Audio::TxFrame& in) noexcept {
			/* CHN-8 PHASE-ERROR RULE, the second-bus half (sections 12.3,
			 * 13.10.2). Increment AT MOST ONCE per callback - it is ONE
			 * unwanted callback, so one increment even when both conditions
			 * hold at once - when either condition below is met:
			 *   (a) the position has ALREADY delivered on this bus in this
			 *       quantum (its second written flag is set at this instant;
			 *       advanceAll clears the second flags only on the window
			 *       quanta, which is exactly the cadence that makes a set
			 *       flag mean "delivered this quantum" on this bus), or
			 *   (b) on the second bus only, this is outside the advance
			 *       window: frameIndex % secondBusFrameDivider != 0.
			 * The check runs BEFORE this callback updates the flag, so the
			 * lone scheduler-driven window delivery is not counted. */
			if(position < this->m_phaseError.size())
			{
				const bool alreadyDelivered =
					position < this->m_secondWritten.size()
					&& this->m_secondWritten[position] != 0u;
				const bool nonWindow =
					frameIndex % this->m_secondBusFrameDivider != 0u;
				if(alreadyDelivered || nonWindow)
					++this->m_phaseError[position];
			}

			/* CHN-6 WRITTEN-FLAG RULE, the second-bus half. Same condition as
			 * the audio wrapper - Esai::txUnderrunInFrame() at this instant
			 * decides whether the position's SECOND-bus delivery was good or
			 * stale - but it writes m_secondWritten, the per-bus storage that
			 * lets CHN-7 count the two buses on different cadences. The second
			 * bus is fed by a different Esai, so it needs the latch in its own
			 * right: reading the audio bus's would say nothing about it. */
			if(position < this->m_secondWritten.size())
			{
				const dsp56k::Esai* esai =
					position < this->m_secondEsai.size() ? this->m_secondEsai[position] : nullptr;
				this->m_secondWritten[position] = deliveryState(esai);
			}

			/* The second bus transmits on TX2 (frame.h's reg table), so the
			 * frame is read through the Tx conversion with kSecondReg.
			 *
			 *   Line     : position k writes mailbox k + 1.
			 *   Ring     : position k writes mailbox (k + 1) mod N.
			 *   Broadcast: every position writes the one mailbox, committing
			 *               slot (position) ONLY through Mailbox::writeSlot,
			 *               which is what prevents eight producers from
			 *               overwriting one another (sections 12.3, 13.10.2). */
			const unsigned n = static_cast<unsigned>(this->m_second.size());
			if(n == 0u)
				return;

			const g2::Frame frame = fromEsaiFrame(in, kSecondReg);

			if(this->m_secondBusTopology == ChainTopology::Broadcast)
			{
				if(position < Frame::kSlots)
					this->m_second.front().writeSlot(position).set(frame.slot[position]);
				return;
			}

			const unsigned index = this->m_secondBusTopology == ChainTopology::Line
				? position + 1u
				: (position + 1u) % n;          /* Ring */
			this->m_second[index].write() = frame;
		};
	}

	/* ------------- the counters. ----------------------------------------------- */

	/* CHN-7 gives underrunFrames and secondBusUnderrunFrames their real
	 * storage, fed by advanceAll's step 1 and step 2 (the two are separate
	 * vectors, because the two buses advance at different rates, which is
	 * exactly the separation CHN-8 asserts). CHN-8 gives phaseErrorFrames
	 * its real storage: one counter per position, incremented in the transmit
	 * wrappers (never in advanceAll) when a callback the scheduler did not
	 * ask for fires. */
	uint64_t ChainAdapter::underrunFrames(const unsigned position) const noexcept
	{
		return position < m_underrun.size() ? m_underrun[position] : 0u;
	}

	uint64_t ChainAdapter::secondBusUnderrunFrames(const unsigned position) const noexcept
	{
		return position < m_secondUnderrun.size() ? m_secondUnderrun[position] : 0u;
	}

	uint64_t ChainAdapter::phaseErrorFrames(const unsigned position) const noexcept
	{
		return position < m_phaseError.size() ? m_phaseError[position] : 0u;
	}

	/* ------------- the state trio. --------------------------------------------- */

	/* CHN-9 STEP 2 (the absorbed CHN-14) gives the trio its real save-and-load
	 * round trip. The image carries EVERY member a later quantum can read:
	 *
	 *   a geometry header      audio mailbox count, second-bus mailbox count,
	 *                          ring depth, position count
	 *   each mailbox           its head index, then its whole ring
	 *   the three counters     underrun, second-bus underrun, phase error
	 *   the two written flags  the audio and the second-bus flag of each
	 *                          position
	 *
	 * THE WRITTEN FLAGS ARE IN THE IMAGE AND THEY ARE NOT AN OVER-SAVE. A flag
	 * describes the quantum that has not yet been closed by advanceAll, so a
	 * snapshot taken mid-quantum that dropped them would resume with every flag
	 * clear and count an underrun the run never had.
	 *
	 * THE GEOMETRY HEADER IS A GUARD AND NOT DECORATION. stateLoad against an
	 * image of a differently-shaped adapter would otherwise walk the cursor off
	 * the end of the buffer; the header lets it refuse instead, leaving this
	 * object exactly as it was.
	 *
	 * EVERY FIELD MOVES THROUGH memcpy AND NOTHING IS READ THROUGH A CAST
	 * POINTER. The destination is a caller-supplied void* with no alignment
	 * guarantee, so a typed store into it would be undefined for every field
	 * wider than a byte.
	 *
	 * stateLoad's RETURN TYPE IS NOW g2::Status, reconciled by SCH-21 step 4.
	 * The sentence that stood here -- "stateLoad's RETURN TYPE IS STILL void,
	 * which is the shape CHN-5 laid down ... Design section 13.10.2's g2::Status
	 * return is the reconciliation the header already records as outstanding" --
	 * described a shape that no longer exists and is quoted rather than
	 * overwritten. The two refusals below, the null source and the geometry
	 * mismatch, now have a channel to report on. */
	namespace
	{
		/* One frame of one ring. Frame is a flat array of int32_t with no
		 * invariant, so its whole object representation is its state. */
		static_assert(std::is_trivially_copyable_v<Frame>,
			"the mailbox image copies whole Frames, which requires them to be "
			"trivially copyable");

		constexpr size_t kHeaderFields = 4u;   /* the four geometry values */

		void put(uint8_t*& cursor, const uint64_t value) noexcept
		{
			std::memcpy(cursor, &value, sizeof(value));
			cursor += sizeof(value);
		}

		uint64_t get(const uint8_t*& cursor) noexcept
		{
			uint64_t value = 0;
			std::memcpy(&value, cursor, sizeof(value));
			cursor += sizeof(value);
			return value;
		}
	}

	size_t ChainAdapter::stateSize() const noexcept
	{
		const size_t depth  = m_hopFrames + 1u;
		const size_t frames = (m_audio.size() + m_second.size()) * depth;
		const size_t heads  = m_audio.size() + m_second.size();

		return kHeaderFields * sizeof(uint64_t)
			+ heads * sizeof(uint64_t)
			+ frames * sizeof(Frame)
			+ 3u * m_dspCount * sizeof(uint64_t)   /* the three counters   */
			+ 2u * m_dspCount * sizeof(uint8_t);   /* the two written flags */
	}

	void ChainAdapter::stateSave(void* const dst) const noexcept
	{
		if(dst == nullptr)
			return;

		uint8_t* cursor = static_cast<uint8_t*>(dst);

		put(cursor, m_audio.size());
		put(cursor, m_second.size());
		put(cursor, m_hopFrames + 1u);
		put(cursor, m_dspCount);

		const auto saveBus = [&cursor](const std::vector<Mailbox>& bus) noexcept
		{
			for(const auto& mailbox : bus)
			{
				put(cursor, mailbox.m_head);

				for(const auto& frame : mailbox.m_ring)
				{
					std::memcpy(cursor, &frame, sizeof(frame));
					cursor += sizeof(frame);
				}
			}
		};

		saveBus(m_audio);
		saveBus(m_second);

		for(unsigned p = 0u; p < m_dspCount; ++p)
			put(cursor, m_underrun[p]);
		for(unsigned p = 0u; p < m_dspCount; ++p)
			put(cursor, m_secondUnderrun[p]);
		for(unsigned p = 0u; p < m_dspCount; ++p)
			put(cursor, m_phaseError[p]);

		for(unsigned p = 0u; p < m_dspCount; ++p)
			*cursor++ = m_audioWritten[p];
		for(unsigned p = 0u; p < m_dspCount; ++p)
			*cursor++ = m_secondWritten[p];
	}

	Status ChainAdapter::stateLoad(const void* const src) noexcept
	{
		if(src == nullptr)
			return Status::BadStateImage;

		const uint8_t* cursor = static_cast<const uint8_t*>(src);

		const uint64_t audioCount  = get(cursor);
		const uint64_t secondCount = get(cursor);
		const uint64_t depth       = get(cursor);
		const uint64_t positions   = get(cursor);

		/* THE REFUSAL IS TOTAL AND IT HAPPENS BEFORE ANY MEMBER MOVES. An image
		 * of a differently-shaped adapter carries a different cursor walk, so a
		 * partial load would leave this object in a state no run produced. */
		if(audioCount != m_audio.size()
			|| secondCount != m_second.size()
			|| depth != m_hopFrames + 1u
			|| positions != m_dspCount)
			return Status::BadStateImage;

		const auto loadBus = [&cursor](std::vector<Mailbox>& bus) noexcept
		{
			for(auto& mailbox : bus)
			{
				mailbox.m_head = static_cast<unsigned>(get(cursor));

				for(auto& frame : mailbox.m_ring)
				{
					std::memcpy(&frame, cursor, sizeof(frame));
					cursor += sizeof(frame);
				}
			}
		};

		loadBus(m_audio);
		loadBus(m_second);

		for(unsigned p = 0u; p < m_dspCount; ++p)
			m_underrun[p] = get(cursor);
		for(unsigned p = 0u; p < m_dspCount; ++p)
			m_secondUnderrun[p] = get(cursor);
		for(unsigned p = 0u; p < m_dspCount; ++p)
			m_phaseError[p] = get(cursor);

		for(unsigned p = 0u; p < m_dspCount; ++p)
			m_audioWritten[p] = *cursor++;
		for(unsigned p = 0u; p < m_dspCount; ++p)
			m_secondWritten[p] = *cursor++;

		return Status::Ok;
	}

	/* THE RESET. Design section 13.10.5's "zeroes every emulated memory",
	 * applied to the one object that owns the chain's own memory.
	 *
	 * A FRAME IS ZEROED IN PLACE RATHER THAN THE RING BEING REASSIGNED, so no
	 * allocation happens here and this call is legal on any thread the boot
	 * phase owns without a second look at the allocator.
	 *
	 * THE HEAD GOES TO 0 AS WELL AS THE FRAMES. A ring whose frames are all
	 * zero still carries a head, and two adapters reset from different heads
	 * would agree on every read and disagree on the next advance -- which is a
	 * difference no read of this object could show. */
	void ChainAdapter::reset() noexcept
	{
		const auto clearBus = [](std::vector<Mailbox>& _bus) noexcept
		{
			for(auto& mailbox : _bus)
			{
				for(auto& frame : mailbox.m_ring)
					frame = Frame{};

				mailbox.m_head = 0;
			}
		};

		clearBus(m_audio);
		clearBus(m_second);

		for(auto& flag : m_audioWritten)
			flag = 0;
		for(auto& flag : m_secondWritten)
			flag = 0;

		for(auto& c : m_underrun)
			c = 0;
		for(auto& c : m_secondUnderrun)
			c = 0;
		for(auto& c : m_phaseError)
			c = 0;
	}
}
