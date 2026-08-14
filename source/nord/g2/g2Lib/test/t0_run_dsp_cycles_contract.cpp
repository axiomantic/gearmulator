/* t0_run_dsp_cycles_contract.cpp -- the check of task SCH-8.
 * Design sections 13.10.3 and 26.
 *
 * THE CONTRACT, NOT THE BOUND. SCH-9 covers the bound over 1,000 quanta; this
 * row covers the declared signature and the two cases that fix the shape of
 * the loop.
 *
 * WHY THIS ADAPTER EXISTS AT ALL. dsp56k::DSP::exec is
 *   ASMJIT_FORCE_INLINE void exec() noexcept
 * -- no argument, no return value, ONE dispatch unit -- so no cycle-bounded run
 * call exists in the library. DSP::exec neither returns a cycle count nor
 * takes a budget, so dsp.exec(want) does not compile.
 *
 * THE TEST IS BEFORE THE exec(), NEVER AFTER IT, and the two cases below are
 * what fix that:
 *
 *   A wantCycles of 0 EXECUTES NO exec() AT ALL. A test-after loop runs one
 *   dispatch unit for a want of 0, which is the whole difference between the
 *   two shapes. The case reads the instruction counter, the cycle counter and
 *   the program counter, so "no exec at all" is measured three ways.
 *
 *   A wantCycles of 1 against a block that costs MORE THAN 1 executes EXACTLY
 *   ONE exec(). A loop that ran a second dispatch unit would show twice the
 *   block's instruction count.
 *
 * THE BLOCK COST IS MEASURED, NEVER HARDCODED. The fixture runs one exec()
 * and records what it cost, then holds every later assertion against that
 * figure. A hardcoded cycle count would be a second definition of the
 * library's own instruction timing, and it would fail on the day the library
 * changed one of them for a reason that has nothing to do with this adapter.
 *
 * WHERE THE DEBUG ASSERTIONS FIT. runDspCycles asserts dsp56k::g_useJIT and
 * asserts that the narrowing fits. BOTH ARE DEBUG-ONLY AND NEITHER IS THIS
 * CHECK'S PREDICATE. The default build of this tree is Release and defines
 * NDEBUG, so neither is in the translation unit; the two driven cases pass or
 * fail in any build type, and this check's verdict is a failure counter and
 * the process exit status.
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
#include <type_traits>

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

	/* THE SYNTHETIC DSP. It is a real dsp56k::DSP running a scripted program,
	 * which is the only thing "a synthetic DSP" can mean here: DSP is final,
	 * exec() is neither virtual nor replaceable, and the cycle counter this
	 * adapter reads is written by the compiled block.
	 *
	 * NO EsaiClock IS CONSTRUCTED. PeripheralsNop is not a Peripherals56362,
	 * so nothing in this fixture starts one. */
	struct Fixture
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop peripheralsX;
		dsp56k::PeripheralsNop peripheralsY;
		dsp56k::DSP            dsp;
		dsp56k::Assembler      assembler;

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &peripheralsX, &peripheralsY)
		{
			Logging::setLogFunc(&countLogLine);

			/* ONE exec() MUST BE ONE BLOCK, so block linking is off. With
			 * linking on, one dispatch unit can run a chain of blocks and the
			 * "exactly one exec()" case below would measure a chain instead. */
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
		}

		/* A LOOP, NOT A SLED. The program is `nopCount` nop instructions and a
		 * jmp back to its own first word, so the program counter returns to
		 * where it started and the same block runs for every quantum however
		 * many quanta the check drives. A straight run of nop would walk off
		 * the end of the written region into whatever P memory holds. */
		bool writeLoop(const dsp56k::TWord start, const unsigned nopCount)
		{
			dsp56k::TWord pc = start;

			for(unsigned i = 0; i < nopCount; ++i)
			{
				if(!writeInstruction("nop", pc))
					return false;
			}

			char jump[32];
			snprintf(jump, sizeof(jump), "jmp $%x",
				static_cast<unsigned>(start));

			if(!writeInstruction(jump, pc))
				return false;

			dsp.setPC(start);
			return true;
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
	};
}

/* ================ THE DECLARED SIGNATURE
 *
 * The argument list, the return type and the noexcept, held by the whole
 * function type. Change any one of them and this fails to compile; leave the
 * function declared and undefined and main() fails to link. */

static_assert(std::is_same_v<decltype(g2::runDspCycles),
		uint32_t(dsp56k::DSP&, uint32_t) noexcept>,
	"g2::runDspCycles is uint32_t(dsp56k::DSP&, uint32_t) noexcept. The DSP is "
	"a REFERENCE, the budget is a uint32_t, the return is the cycles actually "
	"spent, and it does not throw.");

static constexpr uint32_t (*kRunDspCycles)(dsp56k::DSP&, uint32_t) noexcept
	= &g2::runDspCycles;

int main()
{
	Fixture fixture;

	/* A block of 12 nop instructions and the jmp that closes the loop. The
	 * count is chosen only so that the block costs MORE THAN ONE cycle, which
	 * is what the want-of-1 case needs; the cost itself is measured below and
	 * never assumed. */
	if(!fixture.writeLoop(0x100, 12))
	{
		printf("t0_run_dsp_cycles_contract: %d failure(s)\n", failures);
		return 1;
	}

	/* ---------------- what one dispatch unit costs, MEASURED. */
	const uint64_t cyclesBefore      = fixture.dsp.getCycles();
	const uint64_t instructionBefore = fixture.dsp.getInstructionCounter();

	fixture.dsp.exec();

	const uint64_t blockCycles       = fixture.dsp.getCycles() - cyclesBefore;
	const uint64_t blockInstructions =
		fixture.dsp.getInstructionCounter() - instructionBefore;

	check(blockCycles > 1,
		"the scripted block costs MORE THAN ONE cycle. The want-of-1 case is "
		"meaningless against a block that costs exactly one.");
	check(blockInstructions > 0,
		"one exec() runs at least one instruction. A zero here means the "
		"cycle counter is not being written and every later case would pass "
		"by exercising nothing.");

	if(failures != 0)
	{
		printf("t0_run_dsp_cycles_contract: %d failure(s)\n", failures);
		return 1;
	}

	/* ---------------- A wantCycles OF 0 EXECUTES NO exec() AT ALL.
	 *
	 * This is the case that tells a test-before loop from a test-after one. A
	 * test-after loop runs one dispatch unit before it looks, so it would move
	 * all three of the observations below. */
	{
		const uint64_t      cycles       = fixture.dsp.getCycles();
		const uint64_t      instructions = fixture.dsp.getInstructionCounter();
		const dsp56k::TWord pc           = fixture.dsp.getPC().toWord();

		const uint32_t spent = kRunDspCycles(fixture.dsp, 0);

		checkEqual(spent, 0u, "a wantCycles of 0 spends no cycle");
		checkEqual(fixture.dsp.getCycles() - cycles, 0u,
			"a wantCycles of 0 advances the cycle counter by nothing");
		checkEqual(fixture.dsp.getInstructionCounter() - instructions, 0u,
			"a wantCycles of 0 EXECUTES NO exec() AT ALL. The instruction "
			"counter is what says so: a test-after loop would have run one "
			"whole dispatch unit.");
		checkEqual(fixture.dsp.getPC().toWord(), pc,
			"a wantCycles of 0 leaves the program counter where it was");
	}

	/* ---------------- A wantCycles OF 1 EXECUTES EXACTLY ONE exec().
	 *
	 * The block costs more than 1, so one dispatch unit already satisfies the
	 * budget and the loop must stop. A loop that ran a second unit would show
	 * twice the block's instruction count, and a loop that returned before it
	 * had spent the budget would return less than 1. */
	{
		const uint64_t cycles       = fixture.dsp.getCycles();
		const uint64_t instructions = fixture.dsp.getInstructionCounter();

		const uint32_t spent = kRunDspCycles(fixture.dsp, 1);

		checkEqual(spent, blockCycles,
			"a wantCycles of 1 returns what ONE dispatch unit cost, which is "
			"the cycles actually spent and never the budget");
		checkEqual(fixture.dsp.getCycles() - cycles, blockCycles,
			"a wantCycles of 1 advances the cycle counter by exactly one "
			"block");
		checkEqual(fixture.dsp.getInstructionCounter() - instructions,
			blockInstructions,
			"a wantCycles of 1 EXECUTES EXACTLY ONE exec(). Two dispatch "
			"units would show twice the block's instruction count.");
		check(spent >= 1u,
			"the return is AT LEAST the budget. The cycle-debt rule computes "
			"spent - want and floors the result at zero, and a return below "
			"the budget would be discarded there as though the context had "
			"idled.");
	}

	/* ---------------- the counter the loop reads is LIVE.
	 *
	 * dsp.getCycles() returns a const uint64_t&, so both reads inside the loop
	 * observe the counter and not a snapshot. An implementation that bound it
	 * to a value and re-tested the value would never terminate for a budget
	 * above one block, so a budget of several blocks is driven here and the
	 * check reports rather than hangs if it does not come back. */
	{
		const uint64_t want   = blockCycles * 3u + 1u;
		const uint64_t cycles = fixture.dsp.getCycles();

		const uint32_t spent = kRunDspCycles(fixture.dsp,
			static_cast<uint32_t>(want));

		check(spent >= want,
			"a budget of several dispatch units is met in full");
		check(spent < want + blockCycles,
			"the overshoot is at most ONE dispatch unit");
		checkEqual(fixture.dsp.getCycles() - cycles, spent,
			"the return is the cycle counter's own movement");
	}

	if(failures != 0)
	{
		printf("t0_run_dsp_cycles_contract: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_run_dsp_cycles_contract: all cases passed (one dispatch unit "
		"costs %llu cycles over %llu instructions; %llu library log line(s))\n",
		static_cast<unsigned long long>(blockCycles),
		static_cast<unsigned long long>(blockInstructions),
		static_cast<unsigned long long>(g_logLines));
	return 0;
}
