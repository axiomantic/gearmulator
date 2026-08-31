/* The rule a target build cannot see.
 *
 *   The transmit wrappers' written flag is not "the callback fired". The
 *   scheduler drives a transmit callback for every position on every quantum
 *   once its transmitters are enabled, so a flag set by arrival alone would be
 *   set on every quantum, underrunFrames could never rise, and the assertion
 *   against it would be a green mirage.
 *
 *   The flag's source is the emulated ESAI's own M_TUE bit. The wrapper for
 *   position k sets the flag when the callback fires and M_TUE is clear in
 *   Esai::readStatusRegister() at that instant, and leaves it clear otherwise.
 *   M_TUE rises in writeSlotToFrame before the frame is delivered and is not
 *   cleared until the interrupt path runs after, so at the instant the
 *   callback runs the bit states exactly whether the frame it carries is
 *   stale. That is why the wrapper captures its position's Esai&.
 *
 * Each case constructs a real dsp56k::Esai, drives its status register
 * directly through writestatusRegister() to put M_TUE set or clear, then fires
 * the position's transmit wrapper and reads the flag back through
 * ChainAdapter::audioWritten / secondWritten, which is what proves the rule
 * can discriminate.
 *
 * The flags are per position and per bus, so the test asserts the separation
 * directly: firing position 0's audio wrapper with M_TUE clear sets audio[0]
 * and leaves audio[1], second[0] and second[1] clear.
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

	/* One chain position's two real Esai objects: the audio bus on MemArea_X
	 * and the second bus / ESAI_1 on MemArea_Y. */
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
	/* Two positions, so both the per-position and the per-bus separation are
	 * observable in one adapter. M_TUE is SrBit 14; writestatusRegister sets
	 * the whole status register, so a bare bit or a cleared 0 is exact. */
	static const unsigned kN = 2u;
	const unsigned kMtu = 1u << dsp56k::Esai::M_TUE;

	PositionEsai pos[2];
	g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, 1u);
	adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
	adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

	uint64_t frameIndex = 0u;
	dsp56k::Audio::TxFrame frame;

	/* ------------- Case 1: the audio wrapper, M_TUE clear -> flag set.
	 *
	 * Firing with the emulated ESAI's underrun bit clear must set position 0's
	 * audio flag. It must touch NOTHING else: position 1's audio flag stays
	 * clear (per-position) and both second flags stay clear (per-bus). */
	{
		auto tx0 = adapter.audioTxCallback(0u);
		pos[0].audioEsai.writestatusRegister(0u);   /* M_TUE clear */
		tx0(frameIndex, frame);

		check(adapter.audioWritten(0u),
			"audio wrapper with M_TUE clear sets position 0's audio flag");
		check(!adapter.audioWritten(1u),
			"position 0's audio flush leaves position 1's audio flag clear");
		check(!adapter.secondWritten(0u),
			"an audio flush does not set position 0's second-bus flag "
			"(per-bus separation)");
		check(!adapter.secondWritten(1u),
			"an audio flush does not set position 1's second-bus flag");
	}

	/* ------------- Case 2: the audio wrapper, M_TUE set -> flag clear.
	 *
	 * The wrapper must ACTIVELY clear a previously-set flag when the bit is
	 * set, so a stale callback overwrites a good flag rather than leaving it
	 * set. This is the case that discriminates the flag from "the callback
	 * fired": the callback fires either way. */
	{
		auto tx0 = adapter.audioTxCallback(0u);
		pos[0].audioEsai.writestatusRegister(0u);
		tx0(frameIndex, frame);
		check(adapter.audioWritten(0u),
			"precondition (M_TUE clear sets the flag)");

		pos[0].audioEsai.writestatusRegister(kMtu);   /* M_TUE set */
		tx0(frameIndex, frame);
		check(!adapter.audioWritten(0u),
			"audio wrapper with M_TUE set leaves position 0's audio flag CLEAR "
			"even though the callback fired");
	}

	/* ------------- Case 3: the audio wrapper's rule is M_TUE, not arrival.
	 *
	 * Two firings with the bit set in between: the first sets the flag, the
	 * second (bit clear) sets it again, and the third (bit set) clears it.
	 * The flag tracks the bit across each firing, never the mere arrival. */
	{
		auto tx0 = adapter.audioTxCallback(0u);

		pos[0].audioEsai.writestatusRegister(0u);
		tx0(frameIndex, frame);
		check(adapter.audioWritten(0u), "clear -> set, fire 1");

		pos[0].audioEsai.writestatusRegister(kMtu);
		tx0(frameIndex, frame);
		check(!adapter.audioWritten(0u), "set   -> clear, fire 2");

		pos[0].audioEsai.writestatusRegister(0u);
		tx0(frameIndex, frame);
		check(adapter.audioWritten(0u), "clear -> set, fire 3");
	}

	/* ------------- Case 4: the second wrapper, same rule, own flag.
	 *
	 * Firing position 0's second wrapper responds to the second Esai's M_TUE
	 * and owns m_secondWritten, and it must not touch the audio storage. To
	 * show the audio flag is genuinely untouched (rather than merely false by
	 * default), it is first driven to a known true state with an audio flush,
	 * then the second flush must leave it true. */
	{
		auto secondTx0 = adapter.secondTxCallback(0u);
		auto audioTx0  = adapter.audioTxCallback(0u);

		/* Drive the audio flag to a known true first, so the per-bus
		 * separation has something to be preserved against. */
		pos[0].audioEsai.writestatusRegister(0u);
		audioTx0(frameIndex, frame);
		check(adapter.audioWritten(0u),
			"precondition (audio flag true before the second-bus flush)");

		pos[0].secondEsai.writestatusRegister(0u);
		secondTx0(frameIndex, frame);
		check(adapter.secondWritten(0u),
			"second wrapper with M_TUE clear sets position 0's second-bus flag");
		check(adapter.audioWritten(0u),
			"a second-bus flush leaves the audio flag UNCHANGED (per-bus "
			"separation)");
		check(!adapter.secondWritten(1u),
			"position 0's second flush leaves position 1's second flag clear "
			"(per-position separation)");

		pos[0].secondEsai.writestatusRegister(kMtu);
		secondTx0(frameIndex, frame);
		check(!adapter.secondWritten(0u),
			"second wrapper with M_TUE set leaves position 0's second-bus flag "
			"clear even though the callback fired");
	}

	if(failures != 0)
	{
		printf("t0_written_flag: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_written_flag: all cases passed\n");
	return 0;
}
