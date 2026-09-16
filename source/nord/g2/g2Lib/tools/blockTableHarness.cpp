/* The block-table harness. */

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

		/* An empty cache, and that is the whole point. JitBlock::getInfo ends
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

			/* A block of no words would not advance the walk. It means the
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
