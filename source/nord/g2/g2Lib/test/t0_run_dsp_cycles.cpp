/* The bound, not the contract. T0_run_dsp_cycles_contract fixes the declared
 * signature and the shape of the loop. This row drives the bound:
 *
 *     w  <=  runDspCycles(dsp, w)  <  w + maxDispatchCost
 *
 * over many quanta, across block lengths that straddle w.
 *
 * The lower bound matters because the debt rule computes `spent - want` and
 * floors the result at zero. A return below the budget would be discarded there
 * as though the context had idled -- a slow drift with no counter watching it.
 * It is asserted on every driven quantum rather than once.
 *
 * The upper bound comes from the fixture and not from a header. A bound taken
 * from the build would today be `maxInstructionsPerBlock`, which dsp56300
 * leaves at its upstream default of 0 -- uncapped -- and a bound of "below
 * uncapped" has no threshold and passes for every possible measurement. This
 * fixture measures its own largest block and holds the bound against that.
 *
 * dsp56k::DSP is final and exec() is neither virtual nor replaceable, so a
 * synthetic DSP is a real DSP running a scripted program. The fixture writes
 * loops of different lengths into P memory, measures what one dispatch unit of
 * each costs, and asserts that they straddle the budget before it drives them,
 * so "the block lengths straddle w" is a checked fact of this run and not a
 * comment.
 */

#include "runDspCycles.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <string>

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

	constexpr unsigned kQuantaPerProgram = 1000;
	constexpr unsigned kProgramCount     = 4;

	/* The four loop bodies. The lengths differ so that the measured costs
	 * differ; which of them lands above the budget and which below is
	 * asserted after the measurement and is never assumed here. */
	constexpr unsigned kNopCounts[kProgramCount] = { 3, 7, 12, 31 };

	struct Fixture
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop peripheralsX;
		dsp56k::PeripheralsNop peripheralsY;
		dsp56k::DSP            dsp;
		dsp56k::Assembler      assembler;

		dsp56k::TWord start[kProgramCount] = {};
		uint64_t      cost [kProgramCount] = {};

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &peripheralsX, &peripheralsY)
		{
			Logging::setLogFunc(&countLogLine);

			/* One exec() must be one BLOCK. With block linking on, one
			 * dispatch unit can run a chain of blocks and every measured cost
			 * below would be the cost of a chain. */
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
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

		/* A loop, not A sled, so that 1,000 quanta run the same block and the
		 * program counter never leaves the written region. */
		bool writeLoop(const dsp56k::TWord begin, const unsigned nopCount)
		{
			dsp56k::TWord pc = begin;

			for(unsigned i = 0; i < nopCount; ++i)
			{
				if(!writeInstruction("nop", pc))
					return false;
			}

			char jump[32];
			snprintf(jump, sizeof(jump), "jmp $%x",
				static_cast<unsigned>(begin));

			return writeInstruction(jump, pc);
		}

		/* What one dispatch unit of this program costs, measured. */
		uint64_t measure(const dsp56k::TWord begin)
		{
			dsp.setPC(begin);

			const uint64_t before = dsp.getCycles();
			dsp.exec();
			return dsp.getCycles() - before;
		}
	};
}

int main()
{
	Fixture fixture;

	/* ---------------- the four programs, and what each costs. */
	dsp56k::TWord next = 0x100;

	for(unsigned p = 0; p < kProgramCount; ++p)
	{
		fixture.start[p] = next;

		if(!fixture.writeLoop(next, kNopCounts[p]))
		{
			printf("t0_run_dsp_cycles: %d failure(s)\n", failures);
			return 1;
		}

		next += kNopCounts[p] + 4;   /* room for the jmp and a gap */
	}

	for(unsigned p = 0; p < kProgramCount; ++p)
		fixture.cost[p] = fixture.measure(fixture.start[p]);

	/* The costs must be distinct and rising, or "block lengths that straddle
	 * w" is not what this run drives. */
	for(unsigned p = 1; p < kProgramCount; ++p)
	{
		if(fixture.cost[p] <= fixture.cost[p - 1])
		{
			printf("FAIL program %u costs %llu, which is not above program "
				"%u's %llu. The four block lengths must differ.\n", p,
				static_cast<unsigned long long>(fixture.cost[p]), p - 1,
				static_cast<unsigned long long>(fixture.cost[p - 1]));
			++failures;
		}
	}

	/* The BUDGET. W is the third program's block cost, so two programs run a
	 * block SHORTER than the budget and one runs a block LONGER than it. */
	const uint64_t w = fixture.cost[2];

	check(fixture.cost[0] < w && fixture.cost[1] < w,
		"two of the scripted block lengths are BELOW the budget");
	check(fixture.cost[3] > w,
		"one of the scripted block lengths is ABOVE the budget");

	/* The BOUND'S upper half comes from this fixture. It is the largest
	 * dispatch unit the fixture itself can produce, and nothing here reads
	 * maxInstructionsPerBlock, which the shipped configuration leaves
	 * uncapped. */
	uint64_t maxDispatchCost = 0;

	for(unsigned p = 0; p < kProgramCount; ++p)
	{
		if(fixture.cost[p] > maxDispatchCost)
			maxDispatchCost = fixture.cost[p];
	}

	check(maxDispatchCost > 0,
		"the fixture measured a finite, non-zero largest dispatch unit");

	if(failures != 0)
	{
		printf("t0_run_dsp_cycles: %d failure(s)\n", failures);
		return 1;
	}

	/* ---------------- the bound, over 1,000 quanta for each program. */
	uint64_t quanta       = 0;
	uint64_t smallestSeen = 0xFFFFFFFFFFFFFFFFull;
	uint64_t largestSeen  = 0;

	for(unsigned p = 0; p < kProgramCount; ++p)
	{
		fixture.dsp.setPC(fixture.start[p]);

		for(unsigned q = 0; q < kQuantaPerProgram; ++q)
		{
			const uint64_t before = fixture.dsp.getCycles();

			const uint32_t spent = g2::runDspCycles(fixture.dsp,
				static_cast<uint32_t>(w));

			++quanta;

			if(spent < smallestSeen)
				smallestSeen = spent;
			if(spent > largestSeen)
				largestSeen = spent;

			if(spent < w)
			{
				printf("FAIL program %u quantum %u returned %llu, which is "
					"BELOW the budget of %llu. The debt rule would discard "
					"the shortfall as an idle.\n", p, q,
					static_cast<unsigned long long>(spent),
					static_cast<unsigned long long>(w));
				++failures;
				break;
			}

			if(spent >= w + maxDispatchCost)
			{
				printf("FAIL program %u quantum %u returned %llu, which is "
					"not below the budget of %llu plus the fixture's largest "
					"dispatch unit of %llu.\n", p, q,
					static_cast<unsigned long long>(spent),
					static_cast<unsigned long long>(w),
					static_cast<unsigned long long>(maxDispatchCost));
				++failures;
				break;
			}

			if(fixture.dsp.getCycles() - before != spent)
			{
				printf("FAIL program %u quantum %u returned %llu while the "
					"cycle counter moved by %llu.\n", p, q,
					static_cast<unsigned long long>(spent),
					static_cast<unsigned long long>(
						fixture.dsp.getCycles() - before));
				++failures;
				break;
			}
		}

		if(failures != 0)
			break;
	}

	checkEqual(quanta, kProgramCount * kQuantaPerProgram,
		"every quantum of every program ran");

	/* ---------------- A SCRIPTED RUN that LANDS exactly on w RETURNS w and
	 * Not LESS.
	 *
	 * The third program's block costs exactly w, so a budget of w is met by
	 * one dispatch unit with no overshoot at all, and a budget of three times
	 * w by exactly three. A test-after loop would return w - cost on the
	 * quantum that lands on the deadline, and the debt rule's floor would then
	 * hide it. */
	{
		fixture.dsp.setPC(fixture.start[2]);

		for(unsigned multiple = 1; multiple <= 3; ++multiple)
		{
			const uint64_t exact = w * multiple;

			const uint32_t spent = g2::runDspCycles(fixture.dsp,
				static_cast<uint32_t>(exact));

			checkEqual(spent, exact,
				"a run that lands exactly on the budget returns the budget "
				"and not less");
		}
	}

	/* ---------------- A want of 0 EXECUTES no exec(). */
	{
		fixture.dsp.setPC(fixture.start[0]);

		const uint64_t      cycles       = fixture.dsp.getCycles();
		const uint64_t      instructions = fixture.dsp.getInstructionCounter();
		const dsp56k::TWord pc           = fixture.dsp.getPC().toWord();

		const uint32_t spent = g2::runDspCycles(fixture.dsp, 0);

		checkEqual(spent, 0u, "a want of 0 spends no cycle");
		checkEqual(fixture.dsp.getInstructionCounter() - instructions, 0u,
			"a want of 0 EXECUTES NO exec()");
		checkEqual(fixture.dsp.getCycles() - cycles, 0u,
			"a want of 0 advances the cycle counter by nothing");
		checkEqual(fixture.dsp.getPC().toWord(), pc,
			"a want of 0 leaves the program counter where it was");
	}

	if(failures != 0)
	{
		printf("t0_run_dsp_cycles: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_run_dsp_cycles: all cases passed (%llu quanta at a budget of "
		"%llu; block costs %llu %llu %llu %llu; the fixture's largest "
		"dispatch unit is %llu; returns ranged %llu .. %llu; %llu library log "
		"line(s))\n",
		static_cast<unsigned long long>(quanta),
		static_cast<unsigned long long>(w),
		static_cast<unsigned long long>(fixture.cost[0]),
		static_cast<unsigned long long>(fixture.cost[1]),
		static_cast<unsigned long long>(fixture.cost[2]),
		static_cast<unsigned long long>(fixture.cost[3]),
		static_cast<unsigned long long>(maxDispatchCost),
		static_cast<unsigned long long>(smallestSeen),
		static_cast<unsigned long long>(largestSeen),
		static_cast<unsigned long long>(g_logLines));
	return 0;
}
