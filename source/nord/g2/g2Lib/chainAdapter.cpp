/* chainAdapter.cpp -- the chain adapter's implementation. Task CHN-5.
 * Design sections 12.3 and 13.10.2.
 *
 * CHN-5 defines the whole public surface the CHN-4 header declares, and its
 * Check is exactly the properties a surface-task can own: the four
 * constructor arguments are stored and readable back; the audio chain
 * reports precisely dspCount + 1 mailboxes at every second-bus topology; and
 * every method on the declared surface has a definition, so taking its
 * address links. Each deeper behaviour is owned by a later chain task, and
 * this file says, on the member that owns it, which task that is:
 *
 *   advanceAll's count-then-clear-then-advance accounting ... CHN-7
 *   the transmit wrappers' written-flag rule ................. CHN-6
 *   the three counters' real storage and cadence ............. CHN-7, CHN-8
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

		/* One ESAI receive/transmit frame of the chain carries eight slots
		 * (section 12.3). The head/tail special cases -- two slots in at the
		 * head, two out at the tail -- are task CHN-9's; the uniform 8-slot
		 * form is the one this surface task lays down. Frame::kSlots is the
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

	/* ------------- the swap point. -------------------------------------------- */

	void ChainAdapter::advanceAll(const uint64_t frameIndex) noexcept
	{
		/* Step 4 of the four-step order of section 13.10.2: the audio bus
		 * advances every quantum, the second bus only on the window quanta.
		 * The count-then-clear-then-advance accounting order of section
		 * 13.10.2 that turns the per-position written flags into the three
		 * counters is task CHN-7's, and it prepends steps 1 to 3 to this
		 * body without moving this cadence. */
		for(auto& mailbox : m_audio)
			mailbox.advance();

		if(frameIndex % m_secondBusFrameDivider == 0u)
			for(auto& mailbox : m_second)
				mailbox.advance();
	}

	/* ------------- the two codec edges. ---------------------------------------- */

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
		 * the tail DSP's most recent transmit. The two codec edges carry no
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

	/* ------------- the four callback factories. -------------------------------- */

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
			/* Line: the transmit callback of position k writes mailbox k + 1's
			 * write() frame. The tail position N - 1 therefore writes mailbox
			 * N, which is the mailbox the egress phase reads. CHN-6 adds the
			 * written-flag rule to this wrapper; CHN-9 owns the head/tail slot
			 * forms. */
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
			 * wiring table is section 12.3's and CHN-12 tests all three. */
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
		return [this, position](uint64_t&, const dsp56k::Audio::TxFrame& in) noexcept {
			/* The second bus transmits on TX2 (frame.h's reg table), so the
			 * frame is read through the Tx conversion with kSecondReg.
			 *
			 *   Line     : position k writes mailbox k + 1.
			 *   Ring     : position k writes mailbox (k + 1) mod N.
			 *   Broadcast: every position writes the one mailbox, committing
			 *               slot (position) ONLY through Mailbox::writeSlot,
			 *               which is what prevents eight producers from
			 *               overwriting one another (sections 12.3, 13.10.2).
			 *
			 * CHN-6 adds the written-flag rule to this wrapper. */
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

	/* ------------- the three counters. ----------------------------------------- */

	/* CHN-5 declares and defines the counters so the surface links; the real
	 * storage and the advanceAll cadence that feed them are tasks CHN-7 and
	 * CHN-8. Returning zero here is deliberate: a counter that cannot be
	 * driven above zero is exactly the green mirage CHN-8 replaces, and it
	 * makes the differential obvious when the owning task lands. */
	uint64_t ChainAdapter::underrunFrames(unsigned) const noexcept { return 0u; }
	uint64_t ChainAdapter::secondBusUnderrunFrames(unsigned) const noexcept { return 0u; }
	uint64_t ChainAdapter::phaseErrorFrames(unsigned) const noexcept { return 0u; }

	/* ------------- the state trio. --------------------------------------------- */

	/* CHN-5 defines the trio so the surface links; CHN-14 owns the real
	 * save-and-load round trip (mailbox contents and counters) and the return
	 * type of stateLoad, which design section 13.10.2 declares as g2::Status
	 * but which SCH-18 has not created yet (see the header comment). */
	size_t ChainAdapter::stateSize() const noexcept { return 0u; }
	void   ChainAdapter::stateSave(void*) const noexcept {}
	void   ChainAdapter::stateLoad(const void*) noexcept {}
}
