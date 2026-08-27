/* t0_chain_counters.cpp -- the check of task CHN-8. Design 12.3, 13.10.2.
 *
 * THE PROPERTIES THIS ROW OWNS, AND THAT A TARGET BUILD CANNOT SEE:
 *
 *   1. underrunFrames and secondBusUnderrunFrames are SEPARATE storage.
 *      Section 13.10.2 keeps two counters and not one because the two buses
 *      advance at different rates (with a divider above 1 the second bus
 *      transmits on one quantum in D), so a single shared counter could not
 *      be attributed to a bus. Asserted by driving one of the pair above
 *      zero at a single position and asserting the other stays zero there.
 *      A shared counter would report them equal at that position and fail.
 *
 *   2. ONE unwanted callback raises phaseErrorFrames(position) by exactly
 *      ONE even when both conditions hold at once. A transmit callback the
 *      scheduler did not ask for is one event, so the counter takes one
 *      increment whether one condition or both are true. The double
 *      condition is reachable on the second bus: the position has already
 *      delivered on that bus in this quantum AND this is a non-window
 *      quantum (frameIndex % secondBusFrameDivider != 0). A counter written
 *      as two independent tests of the two conditions would raise it by two
 *      and pass every other row in this plan.
 *
 * The underrun pair is driven through the CHN-6 written flags (real
 * emulated-ESAI transmit-underrun latch) and consumed by the CHN-7 advanceAll
 * cadence; the phase-error count is driven by firing the CHN-6 transmit
 * wrappers a second time. CHN-11 case 4 drives the SAME double condition
 * through the second bus inside the Scheduler; this row drives the wrapper
 * directly so the counter's own rule is pinned before the Scheduler exists.
 */

#include "chainAdapter.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

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

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	/* One chain position's two real Esai objects, mirroring the fixture the
	 * CHN-5/CHN-6/CHN-7 tests use, so the CHN-6 written-flag condition
	 * (a real peripheral behind every position, with no underrun
	 * outstanding) is real here too. */
	struct PositionEsai
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop periphX;
		dsp56k::PeripheralsNop periphY;
		dsp56k::DSP            dsp;
		dsp56k::Esai           audioEsai;
		dsp56k::Esai           secondEsai;

		PositionEsai()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
			, audioEsai(periphX, dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{}
	};
}

int main()
{
	/* Two positions, so the per-position and per-bus separation are
	 * observable in one adapter. A divider of 2 makes the second-bus advance
	 * window land on even frame indices (0, 2, ...) and non-window quanta
	 * land on odd ones (1, 3, ...). */
	static const unsigned kN = 2u;
	static const unsigned kDivider = 2u;

	dsp56k::Audio::TxFrame frame;
	uint64_t frameIndex = 0u;

	/* ------------- Property 1a: audio above zero, second stays zero.
	 *
	 * Position 0 delivers on the SECOND bus only (its second flag is set,
	 * its audio flag is clear). A window advanceAll then counts the CLEAR
	 * audio flag into underrunFrames[0] and does NOT count the SET second
	 * flag into secondBusUnderrunFrames[0]. At position 0 the first is above
	 * zero and the second stays zero: the two cannot be one counter. */
	{
		PositionEsai pos[2];
		g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
		adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
		adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

		auto secondTx0 = adapter.secondTxCallback(0u);
		pos[0].secondEsai.writestatusRegister(0u);     /* no underrun outstanding */
		secondTx0(frameIndex, frame);                  /* window quantum */
		check(adapter.secondWritten(0u),
			"1a setup: position 0's second-bus flag is set");
		check(!adapter.audioWritten(0u),
			"1a setup: position 0's audio flag stays clear");

		adapter.advanceAll(0u);                        /* a window */
		check(adapter.underrunFrames(0u) == 1u,
			"1a: underrunFrames[0] rises to 1 from its clear audio flag");
		check(adapter.secondBusUnderrunFrames(0u) == 0u,
			"1a: secondBusUnderrunFrames[0] STAYS zero despite the window - "
			"its flag was set (separate storage)");
	}

	/* ------------- Property 1b: second above zero, audio stays zero.
	 *
	 * The reverse direction at the same position, on a fresh adapter:
	 * position 0 delivers on the AUDIO bus only, so a window advanceAll
	 * counts into secondBusUnderrunFrames[0] and not underrunFrames[0]. */
	{
		PositionEsai pos[2];
		g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
		adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
		adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

		auto audioTx0 = adapter.audioTxCallback(0u);
		pos[0].audioEsai.writestatusRegister(0u);      /* no underrun outstanding */
		audioTx0(frameIndex, frame);
		check(adapter.audioWritten(0u),
			"1b setup: position 0's audio flag is set");
		check(!adapter.secondWritten(0u),
			"1b setup: position 0's second-bus flag stays clear");

		adapter.advanceAll(0u);                        /* a window */
		check(adapter.secondBusUnderrunFrames(0u) == 1u,
			"1b: secondBusUnderrunFrames[0] rises to 1 from its clear second "
			"flag");
		check(adapter.underrunFrames(0u) == 0u,
			"1b: underrunFrames[0] STAYS zero despite the window - its flag "
			"was set (separate storage)");
	}

	/* ------------- Property 2: the double condition raises phaseErrorFrames
	 * by exactly ONE, not two.
	 *
	 * On the second bus, an unwanted callback is one where the position has
	 * ALREADY delivered on that bus in this quantum AND this is a non-window
	 * quantum. Drive it by firing position 0's second wrapper once in a
	 * window (frameIndex 0: first delivery, sets the flag, no error), then a
	 * SECOND time in a non-window (frameIndex 1: the flag is still set from
	 * the first delivery, so "already delivered" holds, AND 1 % 2 != 0, so
	 * "outside the window" holds). Both conditions true at once yield ONE
	 * increment. A counter written as two independent tests would raise by
	 * two. */
	{
		PositionEsai pos[2];
		g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
		adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
		adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

		auto secondTx0 = adapter.secondTxCallback(0u);
		pos[0].secondEsai.writestatusRegister(0u);     /* no underrun outstanding */

		/* First delivery, on the window (frameIndex 0): the one callback the
		 * scheduler asks for. No phase error - a lone, windowed delivery is
		 * not an unwanted callback. */
		frameIndex = 0u;
		secondTx0(frameIndex, frame);
		check(adapter.phaseErrorFrames(0u) == 0u,
			"2: a lone windowed second delivery raises phaseErrorFrames[0] by "
			"nothing");
		check(adapter.secondWritten(0u),
			"2: the first delivery set position 0's second flag (so "
			"'already delivered' can be detected)");

		/* Second delivery, on a NON-window (frameIndex 1): already delivered
		 * this quantum AND outside the window. EXACTLY one increment. */
		frameIndex = 1u;
		secondTx0(frameIndex, frame);
		check(adapter.phaseErrorFrames(0u) == 1u,
			"2: the double condition (already delivered AND non-window) raises "
			"phaseErrorFrames[0] by EXACTLY ONE, not two");
		check(adapter.phaseErrorFrames(1u) == 0u,
			"2: position 1's phase error stays zero (per-position)");
	}

	/* ------------- Supplementary: the audio-bus duplicate, the CHN-11 case 2
	 * half reachable without a Scheduler. A second audio delivery in one
	 * quantum raises phaseErrorFrames(position) by exactly one. */
	{
		PositionEsai pos[2];
		g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
		adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
		adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

		auto audioTx0 = adapter.audioTxCallback(0u);
		pos[0].audioEsai.writestatusRegister(0u);      /* no underrun outstanding */
		frameIndex = 0u;
		audioTx0(frameIndex, frame);
		check(adapter.phaseErrorFrames(0u) == 0u,
			"supp: a lone audio delivery raises phaseErrorFrames[0] by nothing");
		audioTx0(frameIndex, frame);                   /* second in one quantum */
		check(adapter.phaseErrorFrames(0u) == 1u,
			"supp: a second audio delivery in one quantum raises "
			"phaseErrorFrames[0] by exactly one");
	}

	if(failures != 0)
	{
		printf("t0_chain_counters: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_chain_counters: all cases passed\n");
	return 0;
}
