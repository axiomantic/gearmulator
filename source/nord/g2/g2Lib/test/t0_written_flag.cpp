/* The rule a target build cannot see.
 *
 *   The transmit wrappers' written flag is not "the callback fired". The
 *   scheduler drives a transmit callback for every position on every quantum
 *   once its transmitters are enabled, so a two-state flag set by arrival
 *   alone would be set on every quantum, underrunFrames could never rise,
 *   and the assertion against it would be a green mirage.
 *
 *   The flag records which kind of delivery arrived: a good frame, a frame
 *   carrying a transmit underrun, or no frame at all. Its source is the
 *   emulated ESAI's frame-lifetime underrun latch, Esai::txUnderrunInFrame(),
 *   read at the instant the callback fires. audioWritten/secondWritten report
 *   the good case, so they are false for a stale delivery and false for no
 *   delivery alike.
 *
 * The wrappers must not read the M_TUE status bit. On a running machine it is
 * already gone by the time the callback runs: writeSlotToFrame ends by
 * triggering the transmit DMA; the DMA is serviced synchronously, reaches
 * Esai::writeTX, and writeTX clears M_TUE as soon as every enabled transmitter
 * has been written -- before writeSlotToFrame has even returned, and slots
 * before execTX delivers the frame that carries the stale slot. M_TUE is a
 * slot-lifetime status; the wrappers need a frame-lifetime one, and
 * txUnderrunInFrame() is it.
 *
 * The cases below plant the condition rather than the reading: a real transmit
 * underrun, produced by the peripheral's own transmit path from a TX register
 * that was not written in time (esaiUnderrunPlant.h). Setting the status
 * register by hand would prove only that the read discriminates, never that
 * the condition can occur. Whether the counters that feed on these flags then
 * rise is t0_esai_underrun_gate.
 *
 * The flags are per position and per bus.
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
}

int main()
{
	/* Two positions, so both the per-position and the per-bus separation are
	 * observable in one adapter. */
	static const unsigned kN = 2u;

	g2test::PositionEsai pos[2];
	g2::ChainAdapter adapter(kN, 1u, g2::ChainTopology::Ring, 1u);
	adapter.attachEsai(0u, pos[0].audioEsai, pos[0].secondEsai);
	adapter.attachEsai(1u, pos[1].audioEsai, pos[1].secondEsai);

	/* The wrappers are reached the way the DSP set reaches them: installed on
	 * the peripheral, and fired by the peripheral when a frame completes. */
	pos[0].audioEsai .setWriteTxCallback(adapter.audioTxCallback (0u));
	pos[0].secondEsai.setWriteTxCallback(adapter.secondTxCallback(0u));
	pos[1].audioEsai .setWriteTxCallback(adapter.audioTxCallback (1u));
	pos[1].secondEsai.setWriteTxCallback(adapter.secondTxCallback(1u));

	g2test::EsaiTransmitDriver audio0 (pos[0].audioEsai);
	g2test::EsaiTransmitDriver second0(pos[0].secondEsai);
	g2test::EsaiTransmitDriver audio1 (pos[1].audioEsai);

	audio0 .enable();
	second0.enable();
	audio1 .enable();

	/* enable() runs the section to a frame boundary, so flags are already
	 * touched. Start every case from a known-clear set. */
	adapter.advanceAll(0u);

	/* ------------- Case 1: a good delivery on the audio bus -> flag set.
	 *
	 * A frame whose every slot was written in time must set position 0's audio
	 * flag. It must touch nothing else: position 1's audio flag stays clear
	 * (per-position) and both second flags stay clear (per-bus). */
	{
		audio0.goodFrame(0x111111u);

		check(adapter.audioWritten(0u),
			"a good audio delivery sets position 0's audio flag");
		check(!adapter.audioWritten(1u),
			"position 0's audio delivery leaves position 1's audio flag clear");
		check(!adapter.secondWritten(0u),
			"an audio delivery does not set position 0's second-bus flag "
			"(per-bus separation)");
		check(!adapter.secondWritten(1u),
			"an audio delivery does not set position 1's second-bus flag");
	}

	/* ------------- Case 2: a delivery carrying a real underrun -> flag clear.
	 *
	 * The wrapper must actively clear a previously-set flag when the frame it
	 * receives carries an underrun, so a stale delivery overwrites a good flag
	 * rather than leaving it set. This is the case that discriminates the flag
	 * from "the callback fired": the callback fires either way, and the frame
	 * arrives either way. */
	{
		audio0.goodFrame(0x222222u);
		check(adapter.audioWritten(0u),
			"precondition (a good delivery sets the flag)");

		audio0.staleFrame(0x222222u);
		check(!adapter.audioWritten(0u),
			"a delivery carrying a REAL transmit underrun leaves position 0's "
			"audio flag CLEAR even though the callback fired");
	}

	/* ------------- Case 3: the flag tracks the kind of each delivery and never
	 * the mere arrival. */
	{
		audio0.goodFrame(0x333333u);
		check(adapter.audioWritten(0u), "good  -> set,   delivery 1");

		audio0.staleFrame(0x333333u);
		check(!adapter.audioWritten(0u), "stale -> clear, delivery 2");

		audio0.goodFrame(0x333333u);
		check(adapter.audioWritten(0u), "good  -> set,   delivery 3");
	}

	/* ------------- Case 4: the second bus, same rule, own flag.
	 *
	 * Position 0's second wrapper responds to the second Esai's latch and owns
	 * m_secondWritten, and it must not touch the audio storage. To show the
	 * audio flag is genuinely untouched (rather than merely false by default),
	 * it is first driven to a known true state, then the second-bus delivery
	 * must leave it true. */
	{
		audio0.goodFrame(0x444444u);
		check(adapter.audioWritten(0u),
			"precondition (audio flag true before the second-bus delivery)");

		second0.goodFrame(0x444444u);
		check(adapter.secondWritten(0u),
			"a good second-bus delivery sets position 0's second-bus flag");
		check(adapter.audioWritten(0u),
			"a second-bus delivery leaves the audio flag UNCHANGED (per-bus "
			"separation)");
		check(!adapter.secondWritten(1u),
			"position 0's second delivery leaves position 1's second flag clear "
			"(per-position separation)");

		second0.staleFrame(0x444444u);
		check(!adapter.secondWritten(0u),
			"a second-bus delivery carrying a REAL transmit underrun leaves "
			"position 0's second-bus flag clear even though the callback fired");
		check(adapter.audioWritten(0u),
			"the stale SECOND-bus delivery still leaves the audio flag alone");
	}

	/* ------------- Case 5: the ESAI's own status bit is not the source.
	 *
	 * Setting M_TUE by hand must change nothing, because the wrappers do not
	 * read it. */
	{
		audio0.goodFrame(0x555555u);
		check(adapter.audioWritten(0u), "precondition (flag set by a good delivery)");

		pos[0].audioEsai.writestatusRegister(1u << dsp56k::Esai::M_TUE);
		audio0.goodFrame(0x555555u);

		check(adapter.audioWritten(0u),
			"a hand-written M_TUE does NOT clear the flag: the wrapper reads "
			"txUnderrunInFrame(), and M_TUE cannot survive to this instant on a "
			"running machine");
	}

	if(failures != 0)
	{
		printf("t0_written_flag: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_written_flag: all cases passed\n");
	return 0;
}
