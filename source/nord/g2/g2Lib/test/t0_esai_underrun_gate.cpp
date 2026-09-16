/* The underrun gate is driven by a real transmit underrun, planted through the
 * emulated ESAI, and it rises.
 *
 * On a running machine the ESAI's M_TUE status bit is always clear at the
 * callback instant: Esai::writeSlotToFrame raises M_TUE for the stale slot and
 * then triggers the transmit DMA, and the DMA answers by writing TX, which
 * makes Esai::writeTX clear M_TUE again -- all before the frame containing the
 * stale slot reaches the callback at the frame boundary. A wrapper that read
 * M_TUE would therefore never see it set.
 *
 * Case 1 records the status register at the delivery instant and asserts M_TUE
 * is clear there -- and asserts the counter rose anyway. Those two assertions
 * together are the discriminator: against a build whose wrappers read M_TUE,
 * the first passes and the second fails.
 *
 * Cases 5 and 6 are phaseErrorFrames' known positive. That counter reads the
 * same written flags, and it needs "a delivery happened" while the underrun
 * gate needs "the delivery was good". Case 5 shows a second delivery inside one
 * quantum raises it; case 6 shows it still rises when the first delivery of the
 * quantum was a stale one, which is the case a gate built out of the underrun
 * flag alone would go blind on.
 */

#include "chainAdapter.h"
#include "esaiUnderrunPlant.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqualU64(const uint64_t got, const uint64_t want, const char* const what)
	{
		if(got != want)
		{
			printf("FAIL %s (got %llu, want %llu)\n", what,
				static_cast<unsigned long long>(got),
				static_cast<unsigned long long>(want));
			++failures;
		}
	}

	/* Records the ESAI status register at the instant the frame is delivered,
	 * then hands the frame to the adapter's own wrapper. Chaining rather than
	 * replacing is what lets one run observe both the bit and the counter. */
	struct DeliveryProbe
	{
		const dsp56k::Esai* esai = nullptr;
		g2::EsaiWriteTxCallback inner;
		uint32_t lastSr = 0;
		uint64_t deliveries = 0;

		void operator()(uint64_t& frameIndex, const dsp56k::Audio::TxFrame& in)
		{
			lastSr = static_cast<uint32_t>(esai->readStatusRegister());
			++deliveries;
			inner(frameIndex, in);
		}
	};

	constexpr uint32_t kMtue = 1u << dsp56k::Esai::M_TUE;
}

int main()
{
	g2test::PositionEsai pos;
	g2::ChainAdapter adapter(1u, 1u, g2::ChainTopology::Ring, 1u);
	adapter.attachEsai(0u, pos.audioEsai, pos.secondEsai);

	auto audioProbe  = std::make_shared<DeliveryProbe>();
	auto secondProbe = std::make_shared<DeliveryProbe>();

	audioProbe->esai   = &pos.audioEsai;
	audioProbe->inner  = adapter.audioTxCallback(0u);
	secondProbe->esai  = &pos.secondEsai;
	secondProbe->inner = adapter.secondTxCallback(0u);

	pos.audioEsai.setWriteTxCallback(
		[audioProbe](uint64_t& i, const dsp56k::Audio::TxFrame& f) { (*audioProbe)(i, f); });
	pos.secondEsai.setWriteTxCallback(
		[secondProbe](uint64_t& i, const dsp56k::Audio::TxFrame& f) { (*secondProbe)(i, f); });

	g2test::EsaiTransmitDriver audio (pos.audioEsai);
	g2test::EsaiTransmitDriver second(pos.secondEsai);

	audio.enable();
	second.enable();

	/* enable() runs the section up to a frame boundary, so deliveries have
	 * already happened. Start the accounting from a clean quantum. */
	adapter.advanceAll(0u);
	adapter.reset();

	const uint64_t deliveriesAfterEnable = audioProbe->deliveries;
	check(deliveriesAfterEnable > 0u,
		"the driver reaches the transmit callback at all (an instrument that "
		"never delivers would make every counter below vacuously right)");

	/* ------------- Case 1: a real underrun raises the audio gate, and M_TUE
	 * is already clear when the frame carrying it is delivered. */
	{
		audio.staleFrame(0x222222u);

		checkEqualU64(audioProbe->deliveries, deliveriesAfterEnable + 1u,
			"the stale frame was delivered exactly once");
		check((audioProbe->lastSr & kMtue) == 0u,
			"M_TUE is CLEAR in readStatusRegister() at the instant the stale "
			"frame is delivered -- the bit is not a usable source here");
		check(!adapter.audioWritten(0u),
			"the audio written flag is CLEAR after a delivery carrying a real "
			"transmit underrun");

		adapter.advanceAll(1u);

		checkEqualU64(adapter.underrunFrames(0u), 1u,
			"underrunFrames(0) rises to 1 from a REAL planted transmit "
			"underrun");
		checkEqualU64(adapter.secondBusUnderrunFrames(0u), 1u,
			"the second bus underran too: its wrapper never fired this "
			"quantum, so its flag was clear");
	}

	/* ------------- Case 2: no plant, no alarm.
	 *
	 * A quantum whose delivered frame had every slot written in time must not
	 * raise the counter. Without this the gate could be a constant. */
	{
		const uint64_t before = adapter.underrunFrames(0u);

		for(unsigned q = 0; q < 4u; ++q)
		{
			audio.goodFrame(0x333333u);
			check(adapter.audioWritten(0u),
				"a frame with every slot written in time sets the audio "
				"written flag");
			adapter.advanceAll(2u + q);
		}

		checkEqualU64(adapter.underrunFrames(0u), before,
			"four clean quanta raise underrunFrames(0) by ZERO (no false "
			"alarm)");
	}

	/* ------------- Case 3: the gate rises again, so it is not a one-shot. */
	{
		const uint64_t before = adapter.underrunFrames(0u);

		audio.staleFrame(0x444444u);
		adapter.advanceAll(6u);

		checkEqualU64(adapter.underrunFrames(0u), before + 1u,
			"a second planted underrun raises underrunFrames(0) again");
	}

	/* ------------- Case 4: the second bus's own gate, driven the same way.
	 *
	 * The second bus is fed by a different Esai and a different flag array, so
	 * a fix that reached only the audio wrapper would leave this dead. The
	 * audio bus is kept clean in the same quanta so the two are told apart. */
	{
		/* Settle both buses into a clean quantum first. */
		audio.goodFrame(0x555555u);
		second.goodFrame(0x555555u);
		adapter.advanceAll(7u);

		const uint64_t audioBefore  = adapter.underrunFrames(0u);
		const uint64_t secondBefore = adapter.secondBusUnderrunFrames(0u);

		audio.goodFrame(0x666666u);
		second.staleFrame(0x666666u);

		check((secondProbe->lastSr & kMtue) == 0u,
			"M_TUE is CLEAR on the second bus at its delivery instant too");
		check(!adapter.secondWritten(0u),
			"the second-bus written flag is CLEAR after a delivery carrying a "
			"real transmit underrun");

		adapter.advanceAll(8u);

		checkEqualU64(adapter.secondBusUnderrunFrames(0u), secondBefore + 1u,
			"secondBusUnderrunFrames(0) rises from a REAL planted underrun on "
			"the second bus");
		checkEqualU64(adapter.underrunFrames(0u), audioBefore,
			"the audio bus stayed clean in that quantum, so its counter did "
			"NOT move (the two buses are separate gates)");
	}

	/* ------------- Case 5: phaseErrorFrames' known positive.
	 *
	 * Two audio deliveries inside one quantum is a delivery the scheduler did
	 * not ask for. Without this case the counter has never been shown able to
	 * leave zero -- and a counter that cannot is indistinguishable from a
	 * broken one. */
	{
		audio.goodFrame(0x777777u);
		adapter.advanceAll(9u);

		const uint64_t before = adapter.phaseErrorFrames(0u);

		audio.goodFrame(0x888888u);   /* the delivery the scheduler asked for */
		audio.goodFrame(0x888888u);   /* the one it did not                   */

		checkEqualU64(adapter.phaseErrorFrames(0u), before + 1u,
			"a SECOND audio delivery inside one quantum raises "
			"phaseErrorFrames(0) by exactly one");

		adapter.advanceAll(10u);
	}

	/* ------------- Case 6: a stale first delivery must not blind phaseError.
	 *
	 * The phase-error rule asks "has this position already delivered in this
	 * quantum", which is a question about arrival. The underrun rule asks
	 * whether the delivery was good. If the two shared one predicate, a
	 * quantum that underran would report no phase error however many extra
	 * callbacks it carried -- the underrun gate would blind the phase gate in
	 * exactly the regime both exist to describe. */
	{
		const uint64_t before = adapter.phaseErrorFrames(0u);

		audio.staleFrame(0x999999u);  /* first delivery of the quantum: stale */
		audio.goodFrame(0x999999u);   /* second delivery of the same quantum  */

		checkEqualU64(adapter.phaseErrorFrames(0u), before + 1u,
			"a second delivery still raises phaseErrorFrames(0) when the "
			"quantum's FIRST delivery carried an underrun");

		adapter.advanceAll(11u);
	}

	if(failures != 0)
	{
		printf("t0_esai_underrun_gate: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_esai_underrun_gate: all cases passed\n");
	return 0;
}
