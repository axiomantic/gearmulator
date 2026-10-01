/* The step order:
 *
 *   advanceAll closes the underrun accounting for the quantum that just
 *   ended, in this order, with 2 x dspCount written flags (one per position
 *   per bus):
 *     1. every quantum: for each position, if its audio-bus flag is clear,
 *        increment that position's underrunFrames.
 *     2. only when frameIndex % secondBusFrameDivider == 0: for each
 *        position, if its second-bus flag is clear, increment that
 *        position's secondBusUnderrunFrames.
 *     3. Clear the audio-bus flags always; clear the second-bus flags only
 *        on the same quanta as step 2.
 *     4. advance() the selected mailboxes: the audio bus every quantum, the
 *        second bus only on the window quanta.
 *   The order matters, because the flags describe the quantum that ended,
 *   not the one about to start.
 *
 * A flag is not kGoodDelivery for two reasons: no wrapper fired in the quantum,
 * or a wrapper fired and the frame it carried had underrun. Only the first
 * route is reachable here, because these cases fire the wrappers by hand with
 * a bare frame rather than driving the peripheral's transmit path; the
 * stale-frame route is exercised in t0_esai_underrun_gate.
 *
 * The flags are per-position and per-bus, so a divider of 2 makes the two
 * buses' cadences differ inside one run: window quanta are even frame
 * indices (0, 2, ...), non-window quanta are odd (1, 3, ...).
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

	/* One chain position's two real Esai objects, plus the DSP and memory
	 * an Esai needs to stand up. A real peripheral is behind every
	 * position, so the wrapper's
	 * "no underrun outstanding" reading is a real one and not a null-Esai
	 * default. */
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

/* The second-bus mailbox advance gate (step 4) is observed through the Ring
 * wiring: position 1's second transmit writes mailbox 0, and
 * position 0's second receive reads mailbox 0. A value written into the
 * mailbox's write frame must not reach position 0's receive on a non-window
 * advanceAll (where the second-bus mailboxes do not advance), and must reach
 * it on the next window advanceAll. */
static void secondBusMailboxAdvanceGate()
{
	static const unsigned kN = 2u;
	static const unsigned kDivider = 2u;
	static constexpr dsp56k::TWord kV = 0x123456u;   /* positive Q23, 24-bit */

	PositionEsai pos[2];
	g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
	adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
	adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

	uint64_t frameIndex = 0u;

	auto rx0  = adapter.secondRxCallback(0u);
	auto tx1  = adapter.secondTxCallback(1u);
	dsp56k::Audio::RxFrame rx;

	/* Position 1 transmits the value on its slot (TX2, register 2). The
	 * second bus writes mailbox (1 + 1) % 2 = 0. The Esai has transmitted
	 * nothing, so its underrun latch is clear and the write is a genuine
	 * delivery rather than a stale-frame one. */
	dsp56k::Audio::TxFrame tx;
	tx.resize(g2::Frame::kSlots);
	tx[1][2] = kV;
	pos[1].secondEsai.writestatusRegister(0u);
	tx1(frameIndex, tx);

	/* Before any advance, position 0's receive must not see the value: it
	 * reads the mailbox's read() frame, which is H = 1 frame behind. */
	rx0(frameIndex, rx);
	check(rx[1][0] == 0u,
		"position 0 does not yet see position 1's second-bus value before "
		"any advance");

	/* A non-window advanceAll (frameIndex 1, 1 % 2 != 0) must not advance the
	 * second-bus mailboxes, so the value still must not reach position 0. */
	adapter.advanceAll(1u);
	rx0(frameIndex, rx);
	check(rx[1][0] == 0u,
		"a non-window advanceAll leaves the second-bus mailbox unadvanced - "
		"position 0 still does not see the value");

	/* The next window advanceAll (frameIndex 2, 2 % 2 == 0) advances the
	 * second-bus mailboxes, and the value must now reach position 0. */
	adapter.advanceAll(2u);
	rx0(frameIndex, rx);
	check(rx[1][0] == kV,
		"a window advanceAll advances the second-bus mailbox - position 0 now "
		"sees the value");
}

int main()
{
	/* Two positions, so both the per-position and the per-bus separation of
	 * the counting are observable in one adapter. A divider of 2 makes the
	 * second-bus advance window land on even frame indices. */
	static const unsigned kN = 2u;
	static const unsigned kDivider = 2u;

	PositionEsai pos[2];
	g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, kDivider);
	adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
	adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

	uint64_t frameIndex = 0u;
	dsp56k::Audio::TxFrame frame;

	/* ------------- Case 1: the first advanceAll, a window, nothing fired.
	 *
	 * Every flag is clear (no transmit wrapper has run), so every position
	 * underruns on both buses, and the window examines the second bus too:
	 * underrunFrames == [1, 1], secondBusUnderrunFrames == [1, 1]. This is
	 * the baseline the later cases are relative to. */
	adapter.advanceAll(0u);
	check(adapter.underrunFrames(0u) == 1u, "window 0: audio underrun 0 == 1");
	check(adapter.underrunFrames(1u) == 1u, "window 0: audio underrun 1 == 1");
	check(adapter.secondBusUnderrunFrames(0u) == 1u,
		"window 0: second-bus underrun 0 == 1");
	check(adapter.secondBusUnderrunFrames(1u) == 1u,
		"window 0: second-bus underrun 1 == 1");
	check(!adapter.audioWritten(0u), "window 0 clears audio flag 0");
	check(!adapter.audioWritten(1u), "window 0 clears audio flag 1");
	check(!adapter.secondWritten(0u), "window 0 clears second flag 0");
	check(!adapter.secondWritten(1u), "window 0 clears second flag 1");

	/* ------------- Set up for case 2: position 0 delivers on both buses.
	 *
	 * Firing position 0's audio and second wrappers with no underrun
	 * outstanding sets exactly audioWritten[0] and secondWritten[0];
	 * position 1's flags stay clear because no wrapper fires for it. */
	{
		auto audioTx0 = adapter.audioTxCallback(0u);
		auto secondTx0 = adapter.secondTxCallback(0u);
		pos[0].audioEsai.writestatusRegister(0u);
		pos[0].secondEsai.writestatusRegister(0u);
		audioTx0(frameIndex, frame);
		secondTx0(frameIndex, frame);
	}
	check(adapter.audioWritten(0u), "setup: audio flag 0 set");
	check(!adapter.audioWritten(1u), "setup: audio flag 1 clear");
	check(adapter.secondWritten(0u), "setup: second flag 0 set");
	check(!adapter.secondWritten(1u), "setup: second flag 1 clear");

	/* ------------- Case 2: a non-window advanceAll (frameIndex 1).
	 *
	 * Step 1 counts audio underruns from clear audio flags every quantum:
	 * position 0 delivered (flag set, not counted) so only position 1's audio
	 * underrun rises to 2. Step 2 does not examine the second bus on a
	 * non-window, so secondBusUnderrunFrames is unchanged. Step 3 clears the
	 * audio flags always, but retains the second-bus flags on a non-window. */
	adapter.advanceAll(1u);
	check(adapter.underrunFrames(0u) == 1u,
		"non-window 1: audio underrun 0 stays 1 (position 0 delivered)");
	check(adapter.underrunFrames(1u) == 2u,
		"non-window 1: audio underrun 1 rises to 2 (position 1 did not)");
	check(adapter.secondBusUnderrunFrames(0u) == 1u,
		"non-window 1: second-bus underrun 0 unchanged (second bus not "
		"examined, divider 2)");
	check(adapter.secondBusUnderrunFrames(1u) == 1u,
		"non-window 1: second-bus underrun 1 unchanged");
	check(!adapter.audioWritten(0u), "non-window 1 clears audio flag 0 ALWAYS");
	check(!adapter.audioWritten(1u), "non-window 1 clears audio flag 1 ALWAYS");
	check(adapter.secondWritten(0u),
		"non-window 1 RETAINS second-bus flag 0 (cleared only on window)");
	check(!adapter.secondWritten(1u), "non-window 1 second-bus flag 1 stays clear");

	/* ------------- Case 3: the next window advanceAll (frameIndex 2).
	 *
	 * Both audio flags are clear (case 2 cleared them), so both audio
	 * underruns rise again to [2, 3]. The second bus is now examined: second
	 * flag 0 is still set (case 2 retained it) so it is not counted, while
	 * second flag 1 is clear so secondBusUnderrunFrames[1] rises to 2. Step 3
	 * clears both flag sets here, because this is a window. */
	adapter.advanceAll(2u);
	check(adapter.underrunFrames(0u) == 2u, "window 2: audio underrun 0 == 2");
	check(adapter.underrunFrames(1u) == 3u, "window 2: audio underrun 1 == 3");
	check(adapter.secondBusUnderrunFrames(0u) == 1u,
		"window 2: second-bus underrun 0 stays 1 (its flag was set, never "
		"cleared on the non-window)");
	check(adapter.secondBusUnderrunFrames(1u) == 2u,
		"window 2: second-bus underrun 1 rises to 2");
	check(!adapter.secondWritten(0u), "window 2 clears second-bus flag 0");
	check(!adapter.secondWritten(1u), "window 2 clears second-bus flag 1");

	/* ------------- Case 4: step 4's mailbox-advance cadence, on its own
	 * freshly-constructed adapter so the ring heads start clean. */
	secondBusMailboxAdvanceGate();

	if(failures != 0)
	{
		printf("t0_advance_all: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_advance_all: all cases passed\n");
	return 0;
}
