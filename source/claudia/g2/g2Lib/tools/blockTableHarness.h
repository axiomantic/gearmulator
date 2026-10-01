/* The block-table harness.
 *
 * It walks every entry of a compiled block table and reports the largest
 * encoded cycle count. That figure is what bounds the overshoot of
 * g2::runDspCycles and of Board::runMcu: one dispatch unit is one block, so the
 * largest block in the table is maxDispatchCost for the program that table
 * belongs to. This header establishes no maxDispatchCost; it is the instrument
 * that takes the reading.
 *
 * It uses the library's own block analysis. Dsp56k::JitBlock::getInfo is the
 * function the just-in-time compiler itself calls to decide where a block ends
 * and what it costs. A harness that re-derived block boundaries would be a
 * second definition of the compiler's own rule, and the two would disagree on
 * the day one of them changed. The harness passes the caller's own JitConfig,
 * so the table it walks is the table that configuration produces.
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

		/* The figure this harness exists to report, and the address of the
		 * block that carries it. */
		uint32_t largestCycleCount   = 0;
		uint32_t largestCycleCountPc = 0;

		/* The smallest, so that a caller can hold the largest against it
		 * rather than against a number written down once. */
		uint32_t smallestCycleCount = 0;

		/* The longest block in instructions. A cap on block length is stated in
		 * instructions -- JitConfig::maxInstructionsPerBlock is -- so a caller
		 * that asserts against a cap needs this and not the cycles. */
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
