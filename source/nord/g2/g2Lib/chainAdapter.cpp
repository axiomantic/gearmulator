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

	bool ChainAdapter::audioWritten(const unsigned position) const noexcept
	{
		return position < m_audioWritten.size() && m_audioWritten[position] != 0u;
	}

	bool ChainAdapter::secondWritten(const unsigned position) const noexcept
	{
		return position < m_secondWritten.size() && m_secondWritten[position] != 0u;
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
			for(unsigned p = 0u; p < m_dspCount; ++p)
				if(!(p < flags.size() && flags[p] != 0u))
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
			 * audio written flag being set at this instant - advanceAll clears
			 * the audio flags every quantum, so a set audio flag at callback
			 * time can only mean a previous audio transmit in this same
			 * quantum. The check runs BEFORE this callback updates the flag,
			 * so the lone, scheduler-driven delivery - the one callback the
			 * scheduler asks for - is not counted, while a second one is. */
			if(position < this->m_phaseError.size()
				&& position < this->m_audioWritten.size()
				&& this->m_audioWritten[position] != 0u)
			{
				++this->m_phaseError[position];
			}

			/* CHN-6 WRITTEN-FLAG RULE (section 12.3). The flag is NOT "the
			 * callback fired" - the scheduler drives a transmit callback for
			 * every position on every quantum, so an arrival flag could never
			 * be clear and underrunFrames could never rise (a green mirage).
			 * The source is the emulated ESAI's own M_TUE bit, read at this
			 * instant: M_TUE rises in writeSlotToFrame (esai.cpp:380-384)
			 * before the frame is delivered and is not cleared until the
			 * interrupt path (esai.cpp:108-112) runs after, so the bit states
			 * whether the frame this callback carries is stale. Set the flag
			 * true exactly when M_TUE is clear; actively clear it otherwise,
			 * so a stale callback overwrites a good flag rather than leaving
			 * it set. The wrapper reaches its position's Esai through
			 * m_audioEsai, which attachEsai filled at construction. */
			if(position < this->m_audioWritten.size())
			{
				const dsp56k::Esai* esai =
					position < this->m_audioEsai.size() ? this->m_audioEsai[position] : nullptr;
				const bool mTueClear = (esai != nullptr)
					&& !(esai->readStatusRegister() & (1u << dsp56k::Esai::M_TUE));
				this->m_audioWritten[position] = mTueClear ? 1u : 0u;
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
			 * the audio wrapper - M_TUE clear in readStatusRegister() at this
			 * instant sets the position's SECOND-bus flag, otherwise clears
			 * it - but it writes m_secondWritten, the per-bus storage that
			 * lets CHN-7 count the two buses on different cadences. */
			if(position < this->m_secondWritten.size())
			{
				const dsp56k::Esai* esai =
					position < this->m_secondEsai.size() ? this->m_secondEsai[position] : nullptr;
				const bool mTueClear = (esai != nullptr)
					&& !(esai->readStatusRegister() & (1u << dsp56k::Esai::M_TUE));
				this->m_secondWritten[position] = mTueClear ? 1u : 0u;
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

	/* CHN-5 defines the trio so the surface links; CHN-14 owns the real
	 * save-and-load round trip (mailbox contents and counters) and the return
	 * type of stateLoad, which design section 13.10.2 declares as g2::Status
	 * but which SCH-18 has not created yet (see the header comment). */
	size_t ChainAdapter::stateSize() const noexcept { return 0u; }
	void   ChainAdapter::stateSave(void*) const noexcept {}
	void   ChainAdapter::stateLoad(const void*) noexcept {}
}
