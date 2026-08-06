/* tools/blockTableHarness.cpp -- the block-table harness. Task SCH-14.
 * Design section 13.4.6 consequence 2.
 */

#include "tools/blockTableHarness.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jitblock.h"
#include "dsp56kEmu/jitblockinfo.h"
#include "dsp56kEmu/jitcacheentry.h"
#include "dsp56kEmu/jitconfig.h"

#include "dsp56kBase/mmuarray.h"

#include <map>
#include <set>

namespace g2
{
	BlockTableReport walkBlockTable(const dsp56k::DSP& dsp,
		const dsp56k::JitConfig& config, const uint32_t firstPc,
		const uint32_t lastPc)
	{
		BlockTableReport report;

		/* AN EMPTY CACHE, AND THAT IS THE WHOLE POINT. JitBlock::getInfo ends
		 * a block early when it reaches code that already exists, so a walk
		 * with a populated cache would report the blocks of whichever run
		 * happened to compile first. An empty cache gives the table the
		 * program itself forms, which is the table this harness is asked
		 * about. A default-constructed MmuArray reports a size of 0 and
		 * getInfo tests the size before it indexes, so no element is ever
		 * read. */
		const dsp56k::MmuArray<dsp56k::JitCacheEntry> emptyCache;

		const std::set<dsp56k::TWord>            noVolatileP;
		const std::map<dsp56k::TWord, dsp56k::TWord> noLoopStarts;
		const std::set<dsp56k::TWord>            noLoopEnds;

		bool first = true;

		for(uint32_t pc = firstPc; pc < lastPc; )
		{
			dsp56k::JitBlockInfo info;
			info.reset();

			dsp56k::JitBlock::getInfo(info, dsp, pc, config, emptyCache,
				noVolatileP, noLoopStarts, noLoopEnds);

			/* A BLOCK OF NO WORDS WOULD NOT ADVANCE THE WALK. It means the
			 * analysis could make no block at this address, so the walk stops
			 * and the caller sees a wordsWalked shorter than it asked for. */
			if(info.memSize == 0)
				break;

			++report.blockCount;
			report.wordsWalked += info.memSize;

			if(info.cycleCount > report.largestCycleCount)
			{
				report.largestCycleCount   = info.cycleCount;
				report.largestCycleCountPc = pc;
			}

			if(first || info.cycleCount < report.smallestCycleCount)
				report.smallestCycleCount = info.cycleCount;

			if(info.instructionCount > report.largestInstructionCount)
			{
				report.largestInstructionCount   = info.instructionCount;
				report.largestInstructionCountPc = pc;
			}

			first = false;
			pc += info.memSize;
		}

		return report;
	}
}
