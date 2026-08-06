/* tools/blockTableHarness.h -- the block-table harness. Task SCH-14.
 * Design section 13.4.6 consequence 2.
 *
 * IT WALKS EVERY ENTRY OF A COMPILED BLOCK TABLE AND REPORTS THE LARGEST
 * ENCODED CYCLE COUNT. That figure is what bounds the overshoot of
 * g2::runDspCycles and of Board::runMcu: one dispatch unit is one block, so
 * the largest block in the table is maxDispatchCost for the program that table
 * belongs to.
 *
 * ITS TIER FOLLOWS ITS INPUT, and that is why it is a tool and not a test.
 * Against the synthetic program committed with SCH-14's self-test it is T0 and
 * needs no artifact. Against the real compiled G2 kernel it is T1, because the
 * kernel comes out of the firmware; SCH-31 is that measurement and this file
 * makes no claim about it.
 *
 * THIS HEADER ESTABLISHES NO maxDispatchCost AND NOTHING HERE MAY BE READ AS
 * ONE. Measurement register row 1 owns that number and spike criterion SPK-5
 * produces it. This is the instrument that takes the reading.
 *
 * WHY IT USES THE LIBRARY'S OWN BLOCK ANALYSIS. dsp56k::JitBlock::getInfo is
 * the function the just-in-time compiler itself calls to decide where a block
 * ends and what it costs. A harness that re-derived block boundaries would be
 * a SECOND definition of the compiler's own rule, and the two would disagree
 * on the day one of them changed. The harness passes the caller's own
 * JitConfig, so the table it walks is the table that configuration produces.
 */

#pragma once

#include <cstdint>

namespace dsp56k
{
	class DSP;
	struct JitConfig;
}

namespace g2
{
	struct BlockTableReport
	{
		/* How many blocks the walk covered. */
		uint32_t blockCount = 0;

		/* How many words of P memory those blocks cover. A caller compares it
		 * with the length it asked for: a shorter figure means the walk
		 * stopped early. */
		uint32_t wordsWalked = 0;

		/* THE FIGURE THIS HARNESS EXISTS TO REPORT, and the address of the
		 * block that carries it. */
		uint32_t largestCycleCount   = 0;
		uint32_t largestCycleCountPc = 0;

		/* The smallest, so that a caller can hold the largest against it
		 * rather than against a number written down once. */
		uint32_t smallestCycleCount = 0;

		/* The longest block in INSTRUCTIONS. A cap on block length is stated
		 * in instructions -- JitConfig::maxInstructionsPerBlock is -- so a
		 * caller that asserts against a cap needs this and not the cycles. */
		uint32_t largestInstructionCount   = 0;
		uint32_t largestInstructionCountPc = 0;
	};

	/* Walks the blocks the given configuration forms for the program in
	 * [firstPc, lastPc) and reports the figures above.
	 *
	 * The DSP supplies the P memory and the opcode table and nothing else: the
	 * walk executes no instruction, changes no register and moves no program
	 * counter. */
	BlockTableReport walkBlockTable(const dsp56k::DSP& dsp,
		const dsp56k::JitConfig& config, uint32_t firstPc, uint32_t lastPc);
}
