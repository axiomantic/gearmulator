/* t0_block_table_harness.cpp -- the check of task SCH-14.
 * Design section 13.4.6 consequence 2, and 18.2.
 *
 * THIS IS THE SELF-TEST OF AN INSTRUMENT, AND IT ESTABLISHES NO
 * maxDispatchCost. It runs the harness against a synthetic DSP program
 * committed beside it, whose longest block is known by construction, so the
 * row asserts an exact figure. SPK-5 produces the number this project actually
 * uses and SCH-31 is the measurement that reads the real compiled kernel; this
 * row verifies that the instrument reads a block table correctly.
 *
 * IT NEEDS NO ARTIFACT. A fork contributor with no G2 ROM can run it.
 *
 * WHERE THE CAP COMES FROM, AND WHY IT IS NOT THE BUILD'S.
 *
 * The bound cannot be "the maxInstructionsPerBlock cap". Measurement register
 * row 2 records that field at the upstream default of 0, which means uncapped,
 * and that configuration ships. A bound of "no block exceeds uncapped" has no
 * threshold and passes for every possible measurement.
 *
 * So the cap this row asserts against is kLongestBlockCap, supplied by this
 * fixture. The scratch configuration the harness walks under carries a FINITE
 * kScratchInstructionLimit, so nothing here is walked uncapped either. The two
 * are deliberately different numbers: a scratch limit EQUAL to the asserted
 * cap would make the assertion hold by construction of the library -- the
 * walk would split every block at the limit -- and a check that the library
 * cannot fail is the class this row exists to close. With the limit above the
 * cap, a program whose longest block exceeds the cap really does report a
 * block above it, and THE NEGATIVE CASE BELOW DRIVES EXACTLY THAT.
 *
 * SCH-9, SCH-12 and SCH-13 take their bound from their own fixture for the
 * same reason, and each says so at its own site.
 */

#include "tools/blockTableHarness.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/jitconfig.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	/* THE CAP THIS FIXTURE SUPPLIES. Nothing reads it from a header. */
	constexpr uint32_t kLongestBlockCap = 16;

	/* The finite instruction limit the scratch configuration carries. It is
	 * ABOVE the cap on purpose; the file header states why. */
	constexpr uint32_t kScratchInstructionLimit = 64;

	/* What the committed program contains, by construction. */
	constexpr uint32_t kExpectedBlockCount            = 3;
	constexpr uint32_t kLongestBlockNopCount          = 12;
	constexpr uint32_t kShortestBlockNopCount         = 3;
	constexpr uint32_t kMiddleBlockNopCount           = 7;

	/* The exact figures of the longest block, against this revision of
	 * dsp56300. They are also RE-DERIVED below from the differences between
	 * the three blocks, so that a reader can see which half moved if the
	 * library ever changes an instruction's timing. */
	constexpr uint32_t kLongestBlockInstructions = kLongestBlockNopCount + 1;
	constexpr uint32_t kLongestBlockCycles       = 15;

	struct Fixture
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop peripheralsX;
		dsp56k::PeripheralsNop peripheralsY;
		dsp56k::DSP            dsp;
		dsp56k::Assembler      assembler;
		dsp56k::JitConfig      scratchConfig;

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &peripheralsX, &peripheralsY)
		{
			Logging::setLogFunc(&countLogLine);

			scratchConfig = dsp.getJit().getConfig();
			scratchConfig.linkJitBlocks           = false;
			scratchConfig.maxInstructionsPerBlock = kScratchInstructionLimit;
		}

		bool writeInstruction(const char* const text, dsp56k::TWord& pc)
		{
			const dsp56k::AssembleResult result = assembler.assemble(text);

			if(!result.success())
			{
				printf("FAIL the fixture could not assemble \"%s\"\n", text);
				++failures;
				return false;
			}

			dsp.memWriteP(pc, result.word[0]);
			++pc;

			if(result.wordCount > 1)
			{
				dsp.memWriteP(pc, result.word[1]);
				++pc;
			}

			return true;
		}

		/* Loads the committed assembly file. One instruction for each line; a
		 * semicolon starts a comment and a blank line is ignored. */
		bool loadProgram(const char* const path, const dsp56k::TWord begin,
			dsp56k::TWord& end)
		{
			std::ifstream file(path);

			if(!file.is_open())
			{
				printf("FAIL the fixture could not open the synthetic program "
					"at %s\n", path);
				++failures;
				return false;
			}

			dsp56k::TWord pc          = begin;
			unsigned      lineCount   = 0;
			std::string   line;

			while(std::getline(file, line))
			{
				const std::string::size_type comment = line.find(';');

				if(comment != std::string::npos)
					line.erase(comment);

				const std::string::size_type first =
					line.find_first_not_of(" \t\r\n");

				if(first == std::string::npos)
					continue;

				const std::string::size_type last =
					line.find_last_not_of(" \t\r\n");

				const std::string instruction = line.substr(first,
					last - first + 1);

				if(!writeInstruction(instruction.c_str(), pc))
					return false;

				++lineCount;
			}

			end = pc;

			/* The file must really have carried the program the assertions
			 * below name. An empty or truncated file would otherwise give a
			 * block count of zero and a largest cycle count of zero, and a
			 * "no block exceeds the cap" assertion would pass over nothing. */
			const unsigned expectedInstructions =
				kShortestBlockNopCount + kMiddleBlockNopCount
				+ kLongestBlockNopCount + kExpectedBlockCount;

			checkEqual(lineCount, expectedInstructions,
				"the committed synthetic program carries the instruction "
				"count this check was written against");

			return failures == 0;
		}

		/* Writes a scratch program whose one block is longer than the cap. */
		bool writeOverCapProgram(const dsp56k::TWord begin,
			const unsigned nopCount, dsp56k::TWord& end)
		{
			dsp56k::TWord pc = begin;

			for(unsigned i = 0; i < nopCount; ++i)
			{
				if(!writeInstruction("nop", pc))
					return false;
			}

			if(!writeInstruction("jmp $0", pc))
				return false;

			end = pc;
			return true;
		}
	};

	/* THE ROW'S OWN PREDICATE, AS ONE FUNCTION, so that the positive case and
	 * the negative case read the SAME predicate and the negative case really
	 * is the same assertion failing. */
	bool noBlockExceedsCap(const g2::BlockTableReport& report,
		const uint32_t cap)
	{
		return report.largestInstructionCount <= cap;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		printf("FAIL the check needs the path of the synthetic program as its "
			"first argument\n");
		return 1;
	}

	Fixture fixture;

	constexpr dsp56k::TWord kProgramBegin = 0x100;

	dsp56k::TWord programEnd = 0;

	if(!fixture.loadProgram(argv[1], kProgramBegin, programEnd))
	{
		printf("t0_block_table_harness: %d failure(s)\n", failures);
		return 1;
	}

	/* ---------------- the harness walks the committed program. */
	const g2::BlockTableReport report = g2::walkBlockTable(fixture.dsp,
		fixture.scratchConfig, kProgramBegin, programEnd);

	checkEqual(report.blockCount, kExpectedBlockCount,
		"the harness walked EVERY entry of the block table. The committed "
		"program is three runs of nop, each closed by a jmp, so it forms "
		"exactly three blocks.");

	checkEqual(report.wordsWalked, programEnd - kProgramBegin,
		"the blocks the harness reported cover the whole program and no "
		"more");

	/* THE EXACT FIGURES OF THE LONGEST BLOCK. */
	checkEqual(report.largestInstructionCount, kLongestBlockInstructions,
		"the longest block holds twelve nop and the jmp that closes it");
	checkEqual(report.largestCycleCount, kLongestBlockCycles,
		"the longest block's ENCODED CYCLE COUNT is the figure this row "
		"names");

	/* AND IT IS THE THIRD BLOCK, not whichever block happened to be walked
	 * last. */
	check(report.largestCycleCountPc > kProgramBegin,
		"the longest block is not the first block of the program");

	/* THE SAME FIGURE, RE-DERIVED. The three blocks differ only by their nop
	 * count, so the cycle count of the longest one is the shortest one's plus
	 * the difference in nop instructions. A library change to one instruction's
	 * timing moves the exact figure above and this derivation together, and a
	 * harness that mis-sums moves only the first. */
	checkEqual(report.smallestCycleCount
			+ (kLongestBlockNopCount - kShortestBlockNopCount),
		kLongestBlockCycles,
		"the longest block's cycle count is the shortest block's plus the "
		"nine extra nop instructions between them");

	/* ---------------- NO BLOCK EXCEEDS THE CAP THE FIXTURE SUPPLIES. */
	check(noBlockExceedsCap(report, kLongestBlockCap),
		"no block of the committed program exceeds the cap this fixture "
		"supplies");

	/* ---------------- THE NEGATIVE CASE.
	 *
	 * A scratch program whose longest block is ABOVE the fixture's cap, walked
	 * under the same scratch configuration. The row's predicate must be FALSE
	 * for it. Without this case, "no block exceeds the cap" is a sentence
	 * nobody has ever seen fail. */
	{
		const unsigned overCapNops = kLongestBlockCap + 4;

		dsp56k::TWord overCapEnd   = 0;
		const dsp56k::TWord overCapBegin = programEnd + 8;

		if(!fixture.writeOverCapProgram(overCapBegin, overCapNops, overCapEnd))
		{
			printf("t0_block_table_harness: %d failure(s)\n", failures);
			return 1;
		}

		const g2::BlockTableReport overCap = g2::walkBlockTable(fixture.dsp,
			fixture.scratchConfig, overCapBegin, overCapEnd);

		checkEqual(overCap.blockCount, 1u,
			"the over-cap program forms exactly one block");
		checkEqual(overCap.largestInstructionCount, overCapNops + 1,
			"the over-cap block holds every instruction that was written into "
			"it. The scratch configuration's finite instruction limit is ABOVE "
			"the fixture's cap on purpose, so the block is not split before it "
			"can break the cap.");

		check(!noBlockExceedsCap(overCap, kLongestBlockCap),
			"a program whose longest block is above the fixture's cap makes "
			"the row's own predicate FALSE. This is what proves the cap "
			"assertion can fail.");

		/* ---------------- THE HARNESS REALLY READS THE CONFIGURATION IT IS
		 * GIVEN.
		 *
		 * Every walk above runs under an instruction limit no block reaches,
		 * so a harness that ignored the configuration and used the library's
		 * own upstream default would report the same figures and every case so
		 * far would pass. This case walks the SAME program under a limit BELOW
		 * its natural block length: the table then has to split, and a harness
		 * that ignored the configuration reports one block of twenty-one
		 * instead of three blocks capped at eight.
		 *
		 * It is also what makes "the fixture sets a finite
		 * maxInstructionsPerBlock on the scratch configuration it compiles
		 * under" a measured statement rather than a line in a comment. */
		{
			constexpr uint32_t kSplitLimit = 8;

			dsp56k::JitConfig splitConfig = fixture.scratchConfig;
			splitConfig.maxInstructionsPerBlock = kSplitLimit;

			const g2::BlockTableReport split = g2::walkBlockTable(fixture.dsp,
				splitConfig, overCapBegin, overCapEnd);

			checkEqual(split.largestInstructionCount, kSplitLimit,
				"under an instruction limit of eight, no block the harness "
				"reports holds more than eight instructions");

			/* Twenty-one instructions at eight for each block is three
			 * blocks: eight, eight and five. */
			checkEqual(split.blockCount, 3u,
				"the twenty-one instructions split into three blocks at a "
				"limit of eight");
			checkEqual(split.wordsWalked, overCap.wordsWalked,
				"the split walk covers exactly the same words as the "
				"unsplit one");
		}
	}

	if(failures != 0)
	{
		printf("t0_block_table_harness: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_block_table_harness: all cases passed (%u blocks; the longest "
		"holds %u instructions and %u encoded cycles at $%x; the fixture's cap "
		"is %u instructions. THIS ESTABLISHES NO maxDispatchCost -- it "
		"verifies the instrument. %llu library log line(s))\n",
		report.blockCount, report.largestInstructionCount,
		report.largestCycleCount, report.largestCycleCountPc,
		kLongestBlockCap,
		static_cast<unsigned long long>(g_logLines));
	return 0;
}
