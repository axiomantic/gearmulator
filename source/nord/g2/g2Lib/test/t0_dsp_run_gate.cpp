/* t0_dsp_run_gate.cpp -- the check of task SCH-33. Design 13.10.3, 13.4.6.
 *
 * THE GATE FAILS CLOSED. A context whose programLanded pointer is NULL is a
 * slot whose program has NOT landed, so an unwired gate stops the machine. The
 * alternative -- read NULL as "landed" and run -- is refused because program
 * memory is zero-filled and 0x000000 is a no-operation on this core: a slot
 * released with an empty program walks that memory, faults nowhere and writes
 * no log line.
 *
 * WHY THE FIXTURE RUNS A REAL PROGRAM ON A REAL DSP: a cycle counter that
 * stands still is evidence only from a fixture that could have moved it. So
 * the fixture writes a loop into program memory and drives one context, with a
 * non-zero want, through both gate states.
 *
 * A BAND AND NEVER AN EQUALITY: runDspCycles tests the counter BEFORE each
 * exec(), so the last dispatch unit carries it past the want. The upper half
 * of the band is this fixture's own measured dispatch unit, taken from the
 * fixture for the reason t0_run_dsp_cycles states: the shipped configuration
 * leaves maxInstructionsPerBlock uncapped, so no bound read from the build has
 * a threshold.
 *
 * THE JOB IS ENTERED THROUGH THE JobContext* RECOVERY THE EXECUTOR USES, and
 * not through a private helper, so the path exercised is the one the Executor
 * reaches.
 */

#include "dspContext.h"
#include "esaiFrame.h"

#include "g2/timebase.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace g2
{
	/* SCH-33's Files: line names dspJob.cpp and this test and no header, so the
	 * declaration is forward-declared here, exactly as SCH-11's own check does. */
	void dspJob(JobContext*) noexcept;
}

/* THE MEMBER'S DECLARED TYPE, PINNED IN THIS TASK'S OWN FILE. A borrowed
 * pointer is what makes the gate readable without the context owning the flag;
 * a bool by value would be a copy that no producer could ever update. */
static_assert(std::is_same_v<decltype(g2::DspContext::programLanded),
	const bool*>,
	"DspContext::programLanded must be a borrowed const bool*.");

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

	/* SIGNED, because the debt is an int64_t and the failure this reports is a
	 * debt paid down BELOW zero. Rendering that through the unsigned form above
	 * would print it as a number near 2^64 and hide which side of zero it fell. */
	void checkEqualSigned(const int64_t observed, const int64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %lld, expected %lld\n", what,
				static_cast<long long>(observed),
				static_cast<long long>(expected));
			++failures;
		}
	}

	void checkOrder(const std::vector<std::string>& observed,
		const std::vector<std::string>& expected)
	{
		if(observed != expected)
		{
			printf("FAIL dspJob order:\n  observed:");
			for(const auto& s : observed)
				printf(" %s", s.c_str());
			printf("\n  expected:");
			for(const auto& s : expected)
				printf(" %s", s.c_str());
			printf("\n");
			++failures;
		}
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	constexpr dsp56k::TWord kProgramStart = 0x100;
	constexpr unsigned      kNopCount     = 7;

	struct Fixture
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop periphX;
		dsp56k::PeripheralsNop periphY;
		dsp56k::DSP            dsp;
		dsp56k::Assembler      assembler;
		dsp56k::Esai           audioEsai;    /* MemArea_X, the audio bus   */
		dsp56k::Esai           secondEsai;   /* MemArea_Y, ESAI_1          */

		std::vector<std::string> order;
		uint64_t audioTx = 0;
		uint64_t audioRx = 0;
		uint64_t secondTx = 0;
		uint64_t secondRx = 0;

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
			, audioEsai(periphX,  dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{
			Logging::setLogFunc(&countLogLine);

			/* ONE exec() MUST BE ONE BLOCK, or the measured dispatch unit below
			 * is the cost of a chain of them and the band's upper half is not
			 * the bound it claims to be. */
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);

			audioEsai.setWriteTxCallback(
				[this](uint64_t& frameIndex, const dsp56k::Audio::TxFrame&)
				{
					++frameIndex;
					order.emplace_back("a_tx");
					++audioTx;
				});
			audioEsai.setReadRxCallback(
				[this](uint64_t& frameIndex, dsp56k::Audio::RxFrame& frame)
				{
					++frameIndex;
					frame.resize(dsp56k::Audio::MaxSlotsPerFrame);
					order.emplace_back("a_rx");
					++audioRx;
				});
			secondEsai.setWriteTxCallback(
				[this](uint64_t& frameIndex, const dsp56k::Audio::TxFrame&)
				{
					++frameIndex;
					order.emplace_back("s_tx");
					++secondTx;
				});
			secondEsai.setReadRxCallback(
				[this](uint64_t& frameIndex, dsp56k::Audio::RxFrame& frame)
				{
					++frameIndex;
					frame.resize(dsp56k::Audio::MaxSlotsPerFrame);
					order.emplace_back("s_rx");
					++secondRx;
				});
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

		/* A LOOP, NOT A SLED, so a quantum of any length stays inside the
		 * written region. */
		bool writeLoop(const dsp56k::TWord begin)
		{
			dsp56k::TWord pc = begin;

			for(unsigned i = 0; i < kNopCount; ++i)
			{
				if(!writeInstruction("nop", pc))
					return false;
			}

			char jump[32];
			snprintf(jump, sizeof(jump), "jmp $%x",
				static_cast<unsigned>(begin));

			return writeInstruction(jump, pc);
		}

		uint64_t measureDispatchUnit(const dsp56k::TWord begin)
		{
			dsp.setPC(begin);

			const uint64_t before = dsp.getCycles();
			dsp.exec();
			return dsp.getCycles() - before;
		}

		void resetBaseline()
		{
			order.clear();
			audioTx = audioRx = secondTx = secondRx = 0;
		}

		void setWordCounts()
		{
			audioEsai.writeTransmitClockControlRegister(
				(7u << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			audioEsai.writeReceiveClockControlRegister(
				(7u << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
			secondEsai.writeTransmitClockControlRegister(
				(7u << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			secondEsai.writeReceiveClockControlRegister(
				(7u << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
		}

		void enableAll()
		{
			audioEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
			audioEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
			secondEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
			secondEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
		}
	};

	/* frameIndex 0 with a divider of 4 opens the second bus window, so both
	 * ports advance rather than the audio one alone. The debt is 0 rather than
	 * a carried spike, because a debt-consumed quantum takes runQuantum's
	 * want <= 0 branch and reaches no DSP, which would leave the gate with
	 * nothing to move. */
	g2::DspContext makeContext(Fixture& f, const bool* const programLanded)
	{
		g2::DspContext c{};
		c.base.fault = g2::JobFault::None;
		c.position   = 0u;
		c.rate       = { G2_DSP_CYCLES_PER_FRAME_NUM,
						 G2_DSP_CYCLES_PER_FRAME_DEN };
		c.acc        = 0u;
		c.debt       = 0;
		c.longDispatchQuanta = 0u;
		c.dsp        = &f.dsp;
		c.audioEsai  = &f.audioEsai;
		c.secondEsai = &f.secondEsai;
		c.frameIndex = 0u;
		c.secondBusFrameDivider = 4u;
		c.programLanded = programLanded;
		return c;
	}

	/* What the budget/want/debt block would ask of the DSP this quantum, from
	 * the context's own rate and accumulator. The probe copy is what keeps the
	 * context untouched by the computation. */
	int64_t wantOf(const g2::DspContext& c)
	{
		uint32_t acc = c.acc;
		return static_cast<int64_t>(alloc(c.rate, &acc)) - c.debt;
	}

	/* One quantum against a gate that must stay shut. */
	void driveClosed(const bool* const programLanded, const char* const what)
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();

		if(!f.writeLoop(kProgramStart))
			return;

		f.dsp.setPC(kProgramStart);
		f.resetBaseline();

		g2::DspContext ctx = makeContext(f, programLanded);

		check(wantOf(ctx) > 0, "the closed case drives a NON-ZERO want");

		/* The program counter is deliberately NOT asserted: the scripted
		 * program is a loop, so it lands back at its start whether the gate
		 * ran it or not, and a clause that cannot go red is not a check. */
		const uint64_t cycles       = f.dsp.getCycles();
		const uint64_t instructions = f.dsp.getInstructionCounter();

		g2::dspJob(&ctx.base);

		checkEqual(f.dsp.getCycles() - cycles, 0u, what);
		checkEqual(f.dsp.getInstructionCounter() - instructions, 0u,
			"a slot whose program has not landed executes no instruction");

		/* THE ACCUMULATOR IS PINNED TO OBSERVED BEHAVIOUR AND THE ASSERTION
		 * SAYS SO ITSELF, IN THE STRING A READER MEETS WHEN IT GOES RED. The
		 * expected value is not a literal: it is one allocation taken from this
		 * context's own rate, so the pin follows the timebase rather than
		 * restating a number measured once. */
		uint32_t oneAllocation = 0u;
		(void) alloc(ctx.rate, &oneAllocation);

		checkEqual(ctx.acc, oneAllocation,
			"PROVISIONAL, and pinned to OBSERVED behaviour rather than to a "
			"ruling -- plan row W3-407 carries the open design question SHOULD "
			"A SHUT GATE ADVANCE THE RATIONAL SAMPLE ACCUMULATOR?, and this "
			"assertion does not answer it. Post-SCH-34 the allocation at "
			"dspJob.cpp:103 stands AHEAD of the run gate, so a shut gate "
			"advances the accumulator by one whole allocation; that is what "
			"this line pins, and it can no more tell a correct timebase from "
			"an accidental one than the code can. IF THE RULING GOES THE OTHER "
			"WAY, THIS ASSERTION AND THE alloc() CALL IN dspJob MOVE TOGETHER "
			"AND NEITHER MOVES ALONE. a closed gate advances the accumulator "
			"by exactly one allocation");
		checkEqual(static_cast<uint64_t>(ctx.debt), 0u,
			"a closed gate carries no debt");
		checkEqual(ctx.longDispatchQuanta, 0u,
			"a closed gate is not a long-dispatch quantum");

		checkOrder(f.order, { "a_rx", "s_rx", "a_tx", "s_tx" });
		checkEqual(f.audioRx, 1u,
			"the audio receive still happens behind a closed gate");
		checkEqual(f.secondRx, 1u,
			"the second receive still happens behind a closed gate");
		checkEqual(f.audioTx, 1u,
			"the audio transmit still happens behind a closed gate");
		checkEqual(f.secondTx, 1u,
			"the second transmit still happens behind a closed gate");
	}

	/* One quantum whose carried debt has ALREADY consumed this quantum's whole
	 * allocation -- design section 13.4.6 rule 4, the long-dispatch quantum,
	 * driven here on the DSP side. `_extraAllocations` says how many whole
	 * allocations the carried debt stands ABOVE the budget: 0 puts want at
	 * exactly zero, 1 puts it one whole allocation below.
	 *
	 * WHY THIS CASE EXISTS AND WHY IT IS SEPARATE FROM THE TWO ABOVE. Every
	 * other case in this file drives want > 0, so none of them enters the
	 * `want <= 0` branch at all -- plan row W3-407 records that as the reason
	 * its Finding 5 survived a repair pass that used this very file as its
	 * check. The equivalent case on the MCU side is t0_mcu_debt's forced-idle
	 * workload, and this one copies its shape deliberately.
	 *
	 * THE EXPECTATION IS ARITHMETIC AND NOT A SECOND OPINION. The debt after
	 * an idle quantum is asserted against `debtBefore - budget`, where
	 * `debtBefore` is a value THIS FUNCTION wrote into the context and `budget`
	 * comes from design section 13.4.1's own `alloc()` against a PRIVATE copy
	 * of the accumulator. Neither figure is read back from the job, and
	 * `runQuantum` is not called: t0_mcu_debt's mutation M4 -- "the idle branch
	 * pays down 2x budget" -- is invisible to a check that mirrors one
	 * implementation against the other, because both sides move together.
	 * Independent per-quantum arithmetic is the only shape that sees it. */
	void driveIdle(const int64_t extraAllocations, const char* const name)
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();

		if(!f.writeLoop(kProgramStart))
			return;

		f.dsp.setPC(kProgramStart);
		f.resetBaseline();

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		char what[512];

		/* The allocation this quantum will make, taken from the timebase
		 * against a private copy of the context's accumulator so that reading
		 * it costs the context nothing. */
		uint32_t probeAcc = ctx.acc;
		const int64_t budget =
			static_cast<int64_t>(alloc(ctx.rate, &probeAcc));

		snprintf(what, sizeof(what),
			"%s: the fixture's rate allocates a NON-ZERO budget, so the debt "
			"this quantum pays down is a debt that MOVES", name);
		check(budget > 0, what);

		if(budget <= 0)
			return;

		ctx.debt = budget * (1 + extraAllocations);

		const int64_t debtBefore = ctx.debt;

		snprintf(what, sizeof(what),
			"%s: the carried debt drives the want <= 0 branch", name);
		check(wantOf(ctx) <= 0, what);

		const uint64_t cycles       = f.dsp.getCycles();
		const uint64_t instructions = f.dsp.getInstructionCounter();

		g2::dspJob(&ctx.base);

		/* THE COUNTER IS ASSERTED BEFORE THE DEBT, AND THAT ORDER IS THE
		 * ANTI-MIRAGE GUARD. A counter still at zero means the branch was
		 * never entered, and every equality below would then be comparing a
		 * quantum this case never drove. */
		snprintf(what, sizeof(what),
			"%s: an idle quantum is counted as EXACTLY one long dispatch", name);
		checkEqual(ctx.longDispatchQuanta, 1u, what);

		snprintf(what, sizeof(what),
			"%s: an idle quantum executes no emulated cycle", name);
		checkEqual(f.dsp.getCycles() - cycles, 0u, what);

		snprintf(what, sizeof(what),
			"%s: an idle quantum executes no instruction", name);
		checkEqual(f.dsp.getInstructionCounter() - instructions, 0u, what);

		/* THE PAY-DOWN, ONCE. g2::runQuantum's want <= 0 branch subtracts one
		 * whole allocation and no more, and this job reconciles the same debt
		 * under the same rule. */
		snprintf(what, sizeof(what),
			"%s: an idle quantum pays the debt down by ONE whole allocation "
			"(from %lld, by a budget of %lld)", name,
			static_cast<long long>(debtBefore),
			static_cast<long long>(budget));
		checkEqualSigned(ctx.debt, debtBefore - budget, what);

		/* cycleDebt.h:31's lower half, which the double pay-down breaks
		 * outright whenever the carried debt equals the budget exactly. */
		snprintf(what, sizeof(what),
			"%s: an idle quantum leaves the debt at or above zero", name);
		check(ctx.debt >= 0, what);

		/* The frame cadence does not depend on the branch: dspJob.cpp:40-43
		 * states that steps 1 and 3 run even when step 2 runs nothing. */
		checkOrder(f.order, { "a_rx", "s_rx", "a_tx", "s_tx" });
	}
}

int main()
{
	/* ---------------- CASE 1, the gate CLOSED, in both of its spellings. */
	driveClosed(nullptr,
		"a NULL programLanded advances the cycle counter by nothing");

	const bool notLanded = false;
	driveClosed(&notLanded,
		"a false programLanded advances the cycle counter by nothing");

	/* ---------------- CASE 2, the gate OPEN. */
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();

		if(!f.writeLoop(kProgramStart))
		{
			printf("t0_dsp_run_gate: %d failure(s)\n", failures);
			return 1;
		}

		const uint64_t maxDispatchCost =
			f.measureDispatchUnit(kProgramStart);

		/* THE GUARD READS THE MEASUREMENT AND NOT THE FAILURE COUNTER. A
		 * counter read here would abandon this case whenever case 1 failed,
		 * which is exactly the run in which a reader needs both. */
		if(maxDispatchCost == 0)
		{
			printf("FAIL the fixture measured no dispatch unit, so the band's "
				"upper half has no threshold\n");
			++failures;
			printf("t0_dsp_run_gate: %d failure(s)\n", failures);
			return 1;
		}

		f.dsp.setPC(kProgramStart);
		f.resetBaseline();

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		const int64_t  want   = wantOf(ctx);
		const uint64_t cycles = f.dsp.getCycles();

		check(want > 0, "the open case drives a NON-ZERO want");

		g2::dspJob(&ctx.base);

		const uint64_t spent = f.dsp.getCycles() - cycles;

		if(spent < static_cast<uint64_t>(want))
		{
			printf("FAIL an open gate spent %llu cycles, which is BELOW the "
				"want of %llu.\n",
				static_cast<unsigned long long>(spent),
				static_cast<unsigned long long>(want));
			++failures;
		}

		/* THE UPPER HALF IS PER SUB-CALL AND NOT PER QUANTUM. Step 2 stopped
		 * being one call at SCH-34: the interleave subdivides want across the
		 * frame's ESAI slots and every sub-call rounds up to its own dispatch
		 * unit independently, so the honest bound is want + k * maxDispatchCost
		 * for the k sub-calls the quantum made.
		 *
		 * k IS MEASURED AND NEVER WRITTEN DOWN. It is the dispatch count the
		 * job itself reports for the quantum that just ran. A literal k here
		 * would be the same defect this file's subject was repaired for -- a
		 * roster standing where the generating count belongs -- one file
		 * further out. */
		const uint64_t k = ctx.slotDispatches;
		const uint64_t upperHalf =
			static_cast<uint64_t>(want) + k * maxDispatchCost;

		if(spent >= upperHalf)
		{
			printf("FAIL an open gate spent %llu cycles, which is not below "
				"the want of %llu plus its %llu sub-dispatches of one %llu-"
				"cycle dispatch unit each (%llu).\n",
				static_cast<unsigned long long>(spent),
				static_cast<unsigned long long>(want),
				static_cast<unsigned long long>(k),
				static_cast<unsigned long long>(maxDispatchCost),
				static_cast<unsigned long long>(upperHalf));
			++failures;
		}

		/* THE DIVISOR MUST BOUND THE COUNT THAT GENERATES IT, and this is the
		 * clause that fails when it does not. A divisor BELOW the dispatch
		 * count hands the leading slots a share of want computed against too
		 * few slots, which is how a hardcoded 8 delivered 3.75x its want across
		 * a thirty-slot frame. */
		if(ctx.slotBudgetDivisor < ctx.slotDispatches)
		{
			printf("FAIL the sub-budget divisor was %u while the interleave's "
				"callback fired %u times, so the divisor does not bound the "
				"count that generates it.\n",
				ctx.slotBudgetDivisor, ctx.slotDispatches);
			++failures;
		}

		check(ctx.slotDispatches > 0,
			"an open gate with enabled ports delivers its budget through the "
			"interleave rather than the idle route");

		uint32_t acc = 0u;
		(void) alloc(ctx.rate, &acc);

		checkEqual(ctx.acc, acc,
			"an open gate consumes exactly one allocation");

		/* SIGNED, and printed signed: the debt is an int64_t and a spend below
		 * the want is exactly the case a reader is diagnosing here. An
		 * unsigned difference would report that case as a number near 2^64. */
		const int64_t expectedDebt = static_cast<int64_t>(spent) - want;

		if(ctx.debt != expectedDebt)
		{
			printf("FAIL an open gate carries the overshoot as debt: observed "
				"%lld, expected %lld\n",
				static_cast<long long>(ctx.debt),
				static_cast<long long>(expectedDebt));
			++failures;
		}

		/* THE INVARIANT, AND IT IS THE REAL CHECK THAT THE BAND ONLY
		 * APPROXIMATES. cycleDebt.h states 0 <= debt < maxDispatchCost at every
		 * quantum boundary, and it holds under the interleave only because each
		 * sub-budget is taken from what REMAINS of want: the overshoot of one
		 * sub-call shrinks the next one, so the whole quantum overshoots by the
		 * single crossing dispatch and no more. A quantum that satisfied the
		 * band above and violated this line would be delivering k dispatch
		 * units of overshoot into the next quantum's debt. */
		if(ctx.debt < 0 || ctx.debt >= static_cast<int64_t>(maxDispatchCost))
		{
			printf("FAIL the quantum ended with a debt of %lld, outside "
				"cycleDebt.h's 0 <= debt < maxDispatchCost for this fixture's "
				"dispatch unit of %llu.\n",
				static_cast<long long>(ctx.debt),
				static_cast<unsigned long long>(maxDispatchCost));
			++failures;
		}

		checkEqual(ctx.longDispatchQuanta, 0u,
			"an open gate with a whole allocation is not a long-dispatch "
			"quantum");

		checkOrder(f.order, { "a_rx", "s_rx", "a_tx", "s_tx" });
		checkEqual(f.audioRx, 1u,
			"the audio receive happens ahead of an open gate");
		checkEqual(f.secondRx, 1u,
			"the second receive happens ahead of an open gate");
		checkEqual(f.audioTx, 1u,
			"the audio transmit happens behind an open gate");
		checkEqual(f.secondTx, 1u,
			"the second transmit happens behind an open gate");
	}

	/* ---------------- CASE 3, the gate OPEN and the quantum IDLE, in both
	 * spellings of the want <= 0 condition. */
	driveIdle(0, "case 3a, a carried debt equal to one whole allocation "
		"(want == 0)");
	driveIdle(1, "case 3b, a carried debt of two whole allocations "
		"(want < 0)");

	if(failures != 0)
	{
		printf("t0_dsp_run_gate: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_dsp_run_gate: all cases passed (%llu library log line(s))\n",
		static_cast<unsigned long long>(g_logLines));
	return 0;
}
