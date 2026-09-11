/* t0_dynamic_fast_interrupts.cpp -- dynamicFastInterrupts on every slot.
 *
 * It builds a g2::DspSet and not a hand-made dsp56k::DSP, because what is under
 * test is the G2's OWN construction path. A check that built its own DSP and
 * installed its own JitConfig would answer the same whatever dspSet.cpp does,
 * so it could never go red to green on this repair.
 *
 * The program counter is the observable, and the retired-instruction count is
 * not an alternative to it. An unrepaired machine retires one instruction for
 * every dispatch while the guest never leaves the poll, so a count assertion is
 * Satisfied by the defect. The count is printed below as evidence and is
 * asserted on nowhere.
 *
 * The run is bounded and not timed. The unrepaired behaviour is an infinite
 * spin, and a check that hangs reports nothing a reader can act on.
 *
 * On a build where dsp56k::g_useJIT is FALSE this check fails and does not
 * skip. DSP::exec() selects its backend at run time, so such a build would run
 * arm 1 through the interpreter, degenerate it into arm 3, and pass against
 * unrepaired code. A skip and a pass are the same observable to every reader
 * this row has; and dynamicFastInterrupts has no property an interpreter build
 * could assert instead, the way the backend rule has its construction refusal.
 *
 * A pass here says that a JIT-compiled guest can LEAVE a poll below the
 * fast-interrupt boundary. It says nothing about the ESAI sync-and-core phase
 * and nothing about the audio path.
 */

#include "dspSet.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/jitconfig.h"
#include "dsp56kEmu/types.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void fail(const char* const _what)
	{
		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	void checkEqual(const uint64_t _observed, const uint64_t _expected,
		const char* const _what)
	{
		if(_observed == _expected)
		{
			std::printf("ok   %s\n", _what);
			return;
		}

		std::printf("FAIL %s: observed $%llx, expected $%llx\n", _what,
			static_cast<unsigned long long>(_observed),
			static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	/* The count an isolated reproduction observed the defect over. One
	 * dispatch that happens to leave the program counter on the spin address is
	 * indistinguishable from a spin; ten consecutive ones are the measured
	 * state. */
	constexpr unsigned kDispatches = 10;

	/* The X location the spin polls, and the value that puts the tested bit in
	 * the FALL-THROUGH state of a brclr. The fixture names both in its text. */
	constexpr dsp56k::TWord kPollAddress = 0x20;
	constexpr dsp56k::TWord kPollBitSet  = 0x000001;

	/* The two entry addresses, read against the library's own boundary rather
	 * than against a number written here. */
	constexpr dsp56k::TWord kBelowVbaEnd = 0x40;
	constexpr dsp56k::TWord kAtVbaEnd    = dsp56k::Vba_End;

	constexpr unsigned kProgramWords = 4;

	static_assert(kBelowVbaEnd + kProgramWords <= dsp56k::Vba_End,
		"arm 1 and its landing must both sit below the fast-interrupt boundary");
	static_assert(kAtVbaEnd >= dsp56k::Vba_End,
		"arm 2 is the boundary control and must sit at or above the boundary");

	/* The slots the three arms drive. Separate slots so that no arm compiles
	 * over another's cached blocks. */
	constexpr unsigned kSlotSpin        = 0;
	constexpr unsigned kSlotBoundary    = 1;
	constexpr unsigned kSlotInterpreter = 2;

	struct Program
	{
		std::vector<dsp56k::TWord> words;
		std::vector<dsp56k::TWord> instructionOffsets;
	};

	/* Loads the committed assembly file. One instruction for each line; a
	 * semicolon starts a comment and a blank line is ignored. */
	bool loadProgram(const char* const _path, Program& _program)
	{
		std::ifstream file(_path);

		if(!file.is_open())
		{
			std::printf("FAIL the check could not open the committed program at "
				"%s\n", _path);
			++g_failures;
			return false;
		}

		const dsp56k::Assembler assembler;

		std::string line;

		while(std::getline(file, line))
		{
			const std::string::size_type comment = line.find(';');

			if(comment != std::string::npos)
				line.erase(comment);

			const std::string::size_type first = line.find_first_not_of(" \t\r\n");

			if(first == std::string::npos)
				continue;

			const std::string::size_type last = line.find_last_not_of(" \t\r\n");

			const std::string text = line.substr(first, last - first + 1);

			const dsp56k::AssembleResult result = assembler.assemble(text.c_str());

			if(!result.success())
			{
				std::printf("FAIL the check could not assemble \"%s\"\n",
					text.c_str());
				++g_failures;
				return false;
			}

			_program.instructionOffsets.push_back(
				static_cast<dsp56k::TWord>(_program.words.size()));

			for(uint32_t i = 0; i < result.wordCount; ++i)
				_program.words.push_back(result.word[i]);
		}

		/* The file must really carry the program the arms below are written
		 * against. An empty or truncated fixture would otherwise load nothing,
		 * leave the program counter wherever it was set, and the landing
		 * assertions would be reading an address no instruction produced. */
		checkEqual(_program.instructionOffsets.size(), 2u,
			"the committed program carries the spin and the landing, and "
			"nothing else");

		if(_program.instructionOffsets.size() != 2)
			return false;

		checkEqual(_program.words.size(), kProgramWords,
			"the committed program assembles to the word count the entry "
			"addresses below were chosen against");

		checkEqual(_program.instructionOffsets[0], 0u,
			"the spin is the FIRST instruction of the committed program, so an "
			"arm's entry address is the spin's address");

		return g_failures == 0;
	}

	bool loadInto(dsp56k::DSP& _dsp, const dsp56k::TWord _entry,
		const Program& _program)
	{
		/* memWriteP and not a write through dsp56k::Memory, because only this
		 * one notifies the just-in-time compiler that program memory moved. */
		for(size_t i = 0; i < _program.words.size(); ++i)
		{
			if(_dsp.memWriteP(_entry + static_cast<dsp56k::TWord>(i),
				_program.words[i]))
				continue;

			fail("the check could not write the committed program into P memory");
			return false;
		}

		if(_dsp.memWrite(dsp56k::MemArea_X, kPollAddress, kPollBitSet))
			return true;

		fail("the check could not put the polled bit into its fall-through "
			"state");
		return false;
	}

	/* READ-MODIFY-WRITE, AND linkJitBlocks is the only field this check
	 * overrides. With linking on, one dispatch runs a CHAIN of blocks and a
	 * self-linked spin has no end, so "ten dispatches" would bound nothing.
	 * Constructing a fresh JitConfig instead would carry the field under test at
	 * its upstream default whatever dspSet.cpp does, arm 1 would be red in both
	 * states, and this check would assert nothing. The read
	 * preserves every field it does not name, so dynamicFastInterrupts still
	 * arrives from dspSet.cpp and from nowhere else. */
	void disableBlockLinking(dsp56k::DSP& _dsp)
	{
		dsp56k::JitConfig config = _dsp.getJit().getConfig();
		config.linkJitBlocks = false;
		_dsp.getJit().setConfig(config);
	}

	enum class Backend { Inherited, Interpreter };

	struct ArmResult
	{
		dsp56k::TWord pc                = 0;
		unsigned      dispatchesOnEntry = 0;
		uint64_t      instructions      = 0;
	};

	ArmResult runArm(dsp56k::DSP& _dsp, const dsp56k::TWord _entry,
		const Backend _backend)
	{
		ArmResult result;

		const uint64_t before = _dsp.getInstructionCounter();

		_dsp.setPC(_entry);

		for(unsigned i = 0; i < kDispatches; ++i)
		{
			/* DSP::exec() is the entry the production path calls and the one
			 * that carries the backend selection. Only the interpreter control
			 * names its backend in the call. */
			if(_backend == Backend::Inherited)
				_dsp.exec();
			else
				_dsp.execInterpreter();

			if(_dsp.getPC().toWord() == _entry)
				++result.dispatchesOnEntry;
		}

		result.pc           = _dsp.getPC().toWord();
		result.instructions = _dsp.getInstructionCounter() - before;

		return result;
	}

	void report(const char* const _arm, const dsp56k::TWord _entry,
		const ArmResult& _result)
	{
		std::printf("%s: entry $%x, pc $%x after %u dispatches, %u of them "
			"ending on the entry address, %llu instruction(s) retired\n",
			_arm, _entry, _result.pc, kDispatches, _result.dispatchesOnEntry,
			static_cast<unsigned long long>(_result.instructions));
	}
}

int main(int argc, char** argv)
{
	std::printf("t0_dynamic_fast_interrupts: g_useJIT = %s\n",
		dsp56k::g_useJIT ? "true" : "false");

	if(argc < 2)
	{
		std::printf("FAIL the check needs the path of the committed program as "
			"its first argument\n");
		return 1;
	}

	if(!dsp56k::g_useJIT)
	{
		std::printf("FAIL this build has g_useJIT = false, so DSP::exec() runs "
			"the interpreter and the G2's JIT configuration is UNMEASURED here. "
			"This row does not skip: a skip and a pass are the same observable, "
			"and dynamicFastInterrupts has no property an interpreter build "
			"could assert in its place.\n");
		std::printf("t0_dynamic_fast_interrupts: 1 failure(s)\n");
		return 1;
	}

	Logging::setLogFunc(&countLogLine);

	Program program;

	if(!loadProgram(argv[1], program))
	{
		std::printf("t0_dynamic_fast_interrupts: %d failure(s)\n", g_failures);
		return 1;
	}

	const dsp56k::TWord landingOffset = program.instructionOffsets[1];

	g2::DspSet set;

	/* ---------------- ARM 1, the discriminating arm.
	 *
	 * The spin sits BELOW the fast-interrupt boundary, where JitBlock::emit
	 * compiles it under FastInterruptMode::Static unless the slot's JitConfig
	 * carries dynamicFastInterrupts. Static SKIPS the block's fall-through
	 * program-counter seed, and braIfBitTestMem writes the program counter only
	 * on its TAKEN path, so an unrepaired machine re-enters this same block for
	 * ever. */
	{
		dsp56k::DSP& dsp = set.dsp(kSlotSpin);

		disableBlockLinking(dsp);

		if(!loadInto(dsp, kBelowVbaEnd, program))
		{
			std::printf("t0_dynamic_fast_interrupts: %d failure(s)\n", g_failures);
			return 1;
		}

		const ArmResult result = runArm(dsp, kBelowVbaEnd, Backend::Inherited);

		report("arm 1, the spin below the boundary", kBelowVbaEnd, result);

		checkEqual(result.pc, kBelowVbaEnd + landingOffset,
			"ARM 1: through DSP::exec(), a bit-test spin BELOW the "
			"fast-interrupt boundary leaves its own address and stands on the "
			"landing it falls through to");
	}

	/* ---------------- ARM 2, the boundary control, AT Vba_End.
	 *
	 * The same program, the same entry, the same ten dispatches, at an address
	 * the fast-interrupt path does not reach. It passes in BOTH states. Without
	 * it, an arm-1 failure caused by a malformed fixture and an arm-1 failure
	 * caused by the defect print the same result. */
	{
		dsp56k::DSP& dsp = set.dsp(kSlotBoundary);

		disableBlockLinking(dsp);

		if(!loadInto(dsp, kAtVbaEnd, program))
		{
			std::printf("t0_dynamic_fast_interrupts: %d failure(s)\n", g_failures);
			return 1;
		}

		const ArmResult result = runArm(dsp, kAtVbaEnd, Backend::Inherited);

		report("arm 2, the same spin at the boundary", kAtVbaEnd, result);

		checkEqual(result.pc, kAtVbaEnd + landingOffset,
			"ARM 2: through DSP::exec(), the same spin at the fast-interrupt "
			"boundary leaves its own address");
	}

	/* ---------------- ARM 3, the interpreter control.
	 *
	 * Arm 1's program at arm 1's entry address, through the interpreter named
	 * in the call. It passes in BOTH states, which pins the defect to the JIT
	 * and not to the program. */
	{
		dsp56k::DSP& dsp = set.dsp(kSlotInterpreter);

		disableBlockLinking(dsp);

		if(!loadInto(dsp, kBelowVbaEnd, program))
		{
			std::printf("t0_dynamic_fast_interrupts: %d failure(s)\n", g_failures);
			return 1;
		}

		const ArmResult result = runArm(dsp, kBelowVbaEnd, Backend::Interpreter);

		report("arm 3, the interpreter control", kBelowVbaEnd, result);

		checkEqual(result.pc, kBelowVbaEnd + landingOffset,
			"ARM 3: through DSP::execInterpreter(), the spin below the "
			"fast-interrupt boundary leaves its own address");
	}

	if(g_failures != 0)
	{
		std::printf("t0_dynamic_fast_interrupts: %d failure(s) (%llu library log "
			"line(s))\n", g_failures,
			static_cast<unsigned long long>(g_logLines));
		return 1;
	}

	std::printf("t0_dynamic_fast_interrupts: all cases passed (%llu library log "
		"line(s))\n", static_cast<unsigned long long>(g_logLines));
	return 0;
}
