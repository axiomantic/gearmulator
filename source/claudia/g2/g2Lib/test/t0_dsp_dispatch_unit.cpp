/* t0_dsp_dispatch_unit.cpp -- how far one dispatch of a G2 DSP slot can run.
 *
 * dspJob moves the ESAI slot transfers between calls to DSP::exec(), so a slot
 * transfer can land no closer to its place in the frame than one dispatch. A
 * dispatch that runs a long straight line of code moves every transfer behind
 * it, and which slots then latch fresh data depends on the budget.
 *
 * The check builds a g2::DspSet, because the bound under test is the G2's own
 * construction path: a fixture that installed its own JitConfig would answer the
 * same whatever dspSet.cpp does.
 *
 * The program is a straight run of one-cycle instructions far longer than the
 * bound, closed by a jump back to its start. Each dispatch is measured in cycles
 * and the longest one is asserted below a quarter of a second-bus slot at the
 * frame budget.
 *
 * The known positive is the same program on a slot whose block length is set
 * back to unbounded. It must dispatch past the bound, or this instrument cannot
 * see a long dispatch and its pass would mean nothing.
 *
 * On a build where dsp56k::g_useJIT is FALSE this check fails and does not
 * skip: the interpreter dispatches one instruction at a time and would pass
 * against any configuration.
 */

#include "dspSet.h"

#include "g2/timebase.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/jitconfig.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
	int g_failures = 0;

	void countLogLine(const std::string&)
	{
	}

	/* Above Vba_End, so the JIT compiles ordinary blocks and not the
	 * one-instruction fast-interrupt form. */
	constexpr dsp56k::TWord kProgramStart = 0x0400;

	/* Far longer than any bound a fix could choose, so an unbounded dispatch
	 * shows as a dispatch of this whole run. */
	constexpr uint32_t kStraightRun = 1024;

	constexpr uint32_t kDispatches = 64;

	/* The second bus's frame: 8 slots. A slot transfer late by a quarter of a
	 * slot is late by 6 of its 24 bits. */
	constexpr uint64_t kSecondBusSlots = 8;

	uint64_t quarterSlotCycles()
	{
		const uint64_t frame = G2_DSP_CYCLES_PER_FRAME_NUM / G2_DSP_CYCLES_PER_FRAME_DEN;
		return frame / kSecondBusSlots / 4u;
	}

	bool writeInst(dsp56k::DSP& _dsp, const char* const _text, dsp56k::TWord& _pc)
	{
		const dsp56k::Assembler assembler;
		const auto r = assembler.assemble(_text);
		if(!r.success())
		{
			std::printf("FAIL could not assemble \"%s\"\n", _text);
			++g_failures;
			return false;
		}
		for(uint32_t i = 0; i < r.wordCount; ++i)
		{
			if(!_dsp.memWriteP(_pc, r.word[i]))
			{
				std::printf("FAIL could not write program word at $%x\n", _pc);
				++g_failures;
				return false;
			}
			++_pc;
		}
		return true;
	}

	bool loadProgram(dsp56k::DSP& _dsp)
	{
		dsp56k::TWord pc = kProgramStart;
		for(uint32_t i = 0; i < kStraightRun; ++i)
		{
			if(!writeInst(_dsp, "inc a", pc))
				return false;
		}
		return writeInst(_dsp, "jmp $400", pc);
	}

	/* The longest single dispatch over kDispatches, in cycles. */
	uint64_t longestDispatch(dsp56k::DSP& _dsp)
	{
		_dsp.setPC(kProgramStart);

		uint64_t longest = 0;
		for(uint32_t i = 0; i < kDispatches; ++i)
		{
			const uint64_t before = _dsp.getCycles();
			_dsp.exec();
			longest = std::max(longest, _dsp.getCycles() - before);
		}
		return longest;
	}
}

int main()
{
	Logging::setLogFunc(&countLogLine);

	std::printf("t0_dsp_dispatch_unit: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		std::printf("FAIL this build has g_useJIT = false, and an interpreter dispatches one "
			"instruction at a time whatever the configuration says.\n");
		return 1;
	}

	const uint64_t bound = quarterSlotCycles();

	/* ---------------- the known positive: the same program, block length
	 * unbounded. Read-modify-write, so every other field still arrives from
	 * dspSet.cpp. */
	{
		g2::DspSet set;
		dsp56k::DSP& dsp = set.dsp(1);

		dsp56k::JitConfig config = dsp.getJit().getConfig();
		config.maxInstructionsPerBlock = 0;
		dsp.getJit().setConfig(config);

		if(!loadProgram(dsp))
			return 1;

		const uint64_t longest = longestDispatch(dsp);
		std::printf("unbounded block length: longest dispatch %llu cycles, bound %llu\n",
			static_cast<unsigned long long>(longest), static_cast<unsigned long long>(bound));

		if(longest <= bound)
		{
			std::printf("FAIL the known positive dispatched no further than the bound, so this "
				"instrument cannot see a long dispatch\n");
			++g_failures;
		}
	}

	/* ---------------- the G2's own configuration, on every slot. */
	{
		g2::DspSet set;

		for(unsigned slot = 0; slot < set.dspCount(); ++slot)
		{
			dsp56k::DSP& dsp = set.dsp(slot);

			if(!loadProgram(dsp))
				return 1;

			const uint64_t longest = longestDispatch(dsp);
			std::printf("slot %u: longest dispatch %llu cycles, bound %llu\n", slot,
				static_cast<unsigned long long>(longest), static_cast<unsigned long long>(bound));

			if(longest == 0)
			{
				std::printf("FAIL slot %u: no dispatch spent a cycle\n", slot);
				++g_failures;
			}
			else if(longest > bound)
			{
				std::printf("FAIL slot %u: one dispatch ran %llu cycles, beyond a quarter of a "
					"second-bus slot (%llu)\n", slot, static_cast<unsigned long long>(longest),
					static_cast<unsigned long long>(bound));
				++g_failures;
			}
		}
	}

	if(g_failures != 0)
	{
		std::printf("t0_dsp_dispatch_unit: %d failure(s)\n", g_failures);
		return 1;
	}

	std::printf("t0_dsp_dispatch_unit: all cases passed\n");
	return 0;
}
