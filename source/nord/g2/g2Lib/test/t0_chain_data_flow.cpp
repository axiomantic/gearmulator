/* t0_chain_data_flow.cpp -- the chain carries a frame.
 *
 * A frame injected at slot i's AUDIO ESAI arrives at slot i + 1's AUDIO ESAI,
 * for every i, and slot i + 1's SECOND-BUS ESAI receives nothing from that
 * injection.
 *
 * Why the injection goes into transmit register 0 rather than any other. The
 * audio transmit wrapper converts from register index 0 and the second-bus one
 * from index 2 (frame.h's register table), while both receive wrappers read
 * index 0. The transmit side is therefore the only one that discriminates
 * between the two buses.
 *
 * Why the chain is stepped hopFrames times between injection and read-back. A
 * mailbox is a ring of hopFrames + 1 frames whose write cell is the head and
 * whose read cell is one past it, and advance() copies the head forward before
 * stepping it (mailbox.cpp). A value written at head h occupies h .. h + k
 * after k steps while read() sits at h + k + 1, so the two first coincide at
 * k == hopFrames. The hop depth is varied rather than fixed, so the step count
 * is taken from the adapter and not from a literal.
 *
 * Why every port is programmed before any traffic, and why every frame call's
 * return value is read. Esai::readRX answers 0 for a receiver whose RCR bit is
 * clear (esai.cpp), transmitDspFrame answers 0 with no enabled transmitter and
 * receiveDspFrame answers 0 with no enabled receiver
 * (esaiFrame.cpp). Those returns are the observable that separates a port that
 * ran from one that was never enabled.
 *
 * No DSP core runs and no program is landed. transmitDspFrame and
 * receiveDspFrame take an Esai& and drive execTX and execRX directly;
 * Peripherals56311::exec never advances an ESAI.
 */

#include "chainAdapter.h"
#include "dspSet.h"
#include "esaiFrame.h"

#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/types.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what,
		const unsigned round, const unsigned position)
	{
		if(condition)
			return;

		printf("FAIL [hop round %u, position %u] %s\n", round, position, what);
		++failures;
	}

	void checkEqual(const uint32_t observed, const uint32_t expected,
		const char* const what, const unsigned round, const unsigned position)
	{
		if(observed == expected)
			return;

		printf("FAIL [hop round %u, position %u] %s: observed 0x%06X, expected 0x%06X\n",
			round, position, what, observed, expected);
		++failures;
	}

	/* The clock control registers are programmed for a one-slot frame, which is
	 * what lets readRX(0) REPORT SLOT 0. receiveDspFrame issues
	 * getRxWordCount() + 1 calls to execRX and each latches its own slot into
	 * the read registers, so a wider frame leaves the last slot there while the
	 * injected sample sits in slot 0. */
	void enableTransmitter(dsp56k::Esai& _esai)
	{
		_esai.writeTransmitClockControlRegister(0);
		_esai.writeTransmitControlRegister(1u << dsp56k::Esai::M_TE0);
	}

	void enableReceiver(dsp56k::Esai& _esai)
	{
		_esai.writeReceiveClockControlRegister(0);
		_esai.writeReceiveControlRegister(1u << dsp56k::Esai::M_RE0);
	}
}

int main()
{
	static const unsigned kPositions = 8u;
	static const unsigned kSecondBusFrameDivider = 4u;

	/* The adapters outlive the set because they are declared before it. Every
	 * callback installed on the set's ESAIs borrows its adapter, and
	 * chainAdapter.h states that lifetime as the adapter's own contract. */
	g2::ChainAdapter adapters[] =
	{
		{kPositions, 1u, g2::ChainTopology::Ring, kSecondBusFrameDivider},
		{kPositions, 2u, g2::ChainTopology::Ring, kSecondBusFrameDivider},
		{kPositions, 3u, g2::ChainTopology::Ring, kSecondBusFrameDivider},
	};

	g2::DspSet set;

	uint64_t frameIndex = 0;

	for(unsigned round = 0; round < sizeof(adapters) / sizeof(adapters[0]); ++round)
	{
		g2::ChainAdapter& adapter = adapters[round];

		g2::attachChainCallbacks(adapter, set);

		/* After the install, NEVER BEFORE. Enabling a transmitter drives one
		 * execTX out of writeTransmitControlRegister, and the callback that
		 * fires is whichever one is installed at that instant. */
		for(unsigned i = 0; i < kPositions; ++i)
		{
			enableTransmitter(set.peripherals(i).getEsai());
			enableReceiver(set.peripherals(i).getEsai());
			enableReceiver(set.peripherals(i).getEsai1());
		}

		for(unsigned i = 0; i + 1u < kPositions; ++i)
		{
			dsp56k::Esai& source     = set.peripherals(i).getEsai();
			dsp56k::Esai& sinkAudio  = set.peripherals(i + 1u).getEsai();
			dsp56k::Esai& sinkSecond = set.peripherals(i + 1u).getEsai1();

			const dsp56k::TWord sample = 0x100000u + (round << 8) + i + 1u;

			check(sample != 0u,
				"the injected sample is non-zero (a zero compares equal against "
				"a default-zero read whether it crossed or not)", round, i);

			/* The written flag is the only reading here that separates an
			 * attached position from an unattached one, and it is read through
			 * the adapter the installer was handed. A transmit wrapper whose
			 * position holds no borrowed Esai pointer reads no M_TUE bit and
			 * pins the flag to zero, yet still performs the mailbox write -- so
			 * every arrival assertion below stays green while the ESAI attach is
			 * absent. Reading the flag CLEAR first and SET after makes this a
			 * transition rather than a standing truth, so a flag pinned high
			 * fails as loudly as one pinned low. Both readings are taken before
			 * the advance loop, because advanceAll clears the audio flags. */
			check(!adapter.audioWritten(i),
				"the transmitting position's audio written flag is clear before its "
				"transmit frame runs", round, i);

			source.writeTX(0u, sample);
			checkEqual(g2::transmitDspFrame(source), source.getTxWordCount() + 1u,
				"the injecting transmit frame ran", round, i);

			check(adapter.audioWritten(i),
				"the transmitting position's audio written flag is set once its "
				"transmit frame has run (the position's ESAI reached the adapter)", round, i);

			for(unsigned step = 0; step < adapter.hopFrames(); ++step)
				adapter.advanceAll(frameIndex++);

			checkEqual(g2::receiveDspFrame(sinkAudio), sinkAudio.getRxWordCount() + 1u,
				"the audio receive frame of the next slot ran", round, i);
			checkEqual(sinkAudio.readRX(0u), sample,
				"the frame injected at this slot's audio ESAI arrived at the next "
				"slot's audio ESAI", round, i);

			checkEqual(g2::receiveDspFrame(sinkSecond), sinkSecond.getRxWordCount() + 1u,
				"the second-bus receive frame of the next slot ran", round, i);
			checkEqual(sinkSecond.readRX(0u), 0u,
				"the next slot's second-bus ESAI received nothing from that "
				"injection", round, i);
		}
	}

	if(failures != 0)
	{
		printf("t0_chain_data_flow: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_chain_data_flow: all cases passed\n");
	return 0;
}
