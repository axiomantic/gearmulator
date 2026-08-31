// Tier T0: no firmware artifact of any kind is read, and every word this file
// puts into P memory is assembled here from text it authors itself.
//
// No assertion in this file is a language assert(). base.cmake sets
// CMAKE_BUILD_TYPE to Release when the caller names none, and Release carries
// -DNDEBUG, so an assert() here would be the predicate that compiles away in
// exactly the configuration a fresh clone and CI both build.
//
// What the funnel is for, and why "no crash" is not the test. g2::writePMem is
// the one function in the G2 that writes DSP P memory, and it always tells the
// just-in-time compiler that the word changed. Skipping that notification does
// not corrupt memory and does not fault on most hosts: the write lands, and the
// DSP goes on running the block the compiler built BEFORE the write. The
// firmware's new code is in memory and the old code is what executes. That is
// silent on Linux and Windows; on macOS the MMU path is force-disabled
// (dsp56kBase/mmuarray.h:67-69) and the same defect can present as a
// segmentation fault instead. A test that asserted only "no crash" would pass
// everywhere but macOS while the real defect shipped.
//
// So case group 2 is the test. It compiles a block, overwrites the block's
// first word THROUGH THE FUNNEL, re-executes the same address, and asserts the
// NEW code ran. The observable is the DSP's own instruction counter rather than
// a register value: it depends on nothing but `nop` and `jmp`, whose encodings
// this repository already exercises, and the two programs differ in length by
// construction so the expected counts are known without measuring them.
//
// An immediate-move's placement rule -- which byte of the destination an 8-bit
// literal lands in -- is a detail of the instruction set that a register-based
// observable would assert instead of asserting the funnel. The instruction
// counter is a property of the block that ran.

#include "pmemFunnel.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	// What an untouched P word reads. Every assertion of the form "the funnel's
	// word landed" must compare against a word that differs from this one, or
	// a funnel that wrote nothing satisfies it.
	constexpr dsp56k::TWord g_emptyPWord = 0;

	// The same memory geometry the scheduler track's DSP fixtures use, so the
	// funnel is exercised against the shape the G2 actually builds.
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
			// ONE exec() MUST BE ONE BLOCK. With block linking on, a single
			// dispatch unit runs a chain of blocks and the instruction counts
			// below would be the counts of a chain rather than of the block
			// the funnel rewrote.
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
		}

		// Assemble one instruction and return its single word. Every program
		// below is one word per instruction by construction; a text that
		// assembles to two words is a defect in this fixture's input and is
		// reported as one rather than silently truncated.
		bool assembleWord(const char* const _text, dsp56k::TWord& _word)
		{
			const dsp56k::AssembleResult result = assembler.assemble(_text);

			if(!result.success())
			{
				std::cout << "FAIL the fixture could not assemble \""
					<< _text << "\"" << std::endl;
				++g_failures;
				return false;
			}

			if(result.wordCount != 1)
			{
				std::cout << "FAIL \"" << _text << "\" assembles to "
					<< result.wordCount << " words, not 1" << std::endl;
				++g_failures;
				return false;
			}

			_word = result.word[0];
			return true;
		}

		// Run exactly one dispatch unit from _pc and return how many
		// instructions it executed.
		uint64_t executeOneBlock(const dsp56k::TWord _pc)
		{
			dsp.setPC(_pc);
			const uint64_t before = dsp.getInstructionCounter();
			dsp.exec();
			return dsp.getInstructionCounter() - before;
		}

		dsp56k::TWord readP(const dsp56k::TWord _address)
		{
			return memory.get(dsp56k::MemArea_P, _address);
		}
	};
}

int main()
{
	Fixture fixture;

	dsp56k::TWord nopWord = 0;
	dsp56k::TWord jmpWord = 0;

	if(!fixture.assembleWord("nop", nopWord))
		return 1;

	// The block terminator. Its target is never taken: exec() runs one block,
	// and a block ends at its branch whatever the branch's target is.
	if(!fixture.assembleWord("jmp $0", jmpWord))
		return 1;

	check(nopWord != jmpWord,
		"the fixture's two instruction words differ, so a program built from "
		"them is distinguishable from one that is not");

	// The word case group 1 writes must not be the value empty P memory already
	// holds. `nop` on the DSP56300 encodes as 0x000000, which
	// is exactly what an untouched P word reads, so a group that wrote `nop`
	// asserted "the word landed" against a value that was already there: a
	// funnel that wrote NOTHING passed, and so did one that wrote to the
	// neighbouring address as well. A mutation run caught it. Case group 1
	// therefore writes the jmp, and this check is what keeps the choice from
	// silently reverting.
	check(jmpWord != g_emptyPWord,
		"the word case group 1 writes differs from what empty P memory reads, "
		"so 'the word landed' is not satisfied by an untouched address");

	// -----------------------------------------------------------------------
	// Case group 1. The funnel writes the word, and the word was not there
	// before.
	//
	// The before state is asserted because a funnel that wrote nothing would
	// be indistinguishable from a correct one if the target already held the
	// value.
	{
		constexpr dsp56k::TWord address = 0x400;

		checkEqual(fixture.readP(address), g_emptyPWord,
			"P memory holds nothing at the target before the funnel writes");

		g2::writePMem(fixture.dsp, address, jmpWord);

		checkEqual(fixture.readP(address), jmpWord,
			"the funnel's word lands at the address the caller named");

		// A funnel that wrote its word to every address, or to a fixed one,
		// would pass the assertion above and fail this one.
		checkEqual(fixture.readP(address + 1), g_emptyPWord,
			"the funnel writes ONE word and leaves its neighbour alone");
		checkEqual(fixture.readP(address - 1), g_emptyPWord,
			"the funnel writes ONE word and leaves its predecessor alone");
	}

	// -----------------------------------------------------------------------
	// Case group 2. A write through the funnel replaces code the compiler has
	// already built. This is the case the funnel exists for.
	//
	// Program v1 at 0x100 is three nop and the jmp that ends the block: FOUR
	// instructions. The funnel then overwrites the block's FIRST word with the
	// jmp, making program v2 exactly ONE instruction. Re-executing 0x100 must
	// run one instruction and not four.
	//
	// A funnel that writes P memory but does not call
	// notifyProgramMemWrite leaves the compiled v1 block in place, the
	// re-execution runs the STALE four-instruction block, and this case goes
	// red. That is the whole defect, made observable.
	{
		constexpr dsp56k::TWord programStart = 0x100;

		for(dsp56k::TWord i = 0; i < 3; ++i)
			g2::writePMem(fixture.dsp, programStart + i, nopWord);

		g2::writePMem(fixture.dsp, programStart + 3, jmpWord);

		const uint64_t v1Instructions = fixture.executeOneBlock(programStart);

		checkEqual(v1Instructions, uint64_t(4),
			"the first program is four instructions and the compiler built it");

		// The overwrite. ONE word, through the funnel, at an address that now
		// carries a compiled block.
		g2::writePMem(fixture.dsp, programStart, jmpWord);

		checkEqual(fixture.readP(programStart), jmpWord,
			"the overwriting word reached P memory");

		const uint64_t v2Instructions = fixture.executeOneBlock(programStart);

		checkEqual(v2Instructions, uint64_t(1),
			"the NEW code runs after the funnel write, not the code the "
			"compiler had already built");
	}

	// -----------------------------------------------------------------------
	// Case group 3. The same at a second, independent address, and in the
	// opposite direction.
	//
	// Group 2 shortened a block. A funnel that always invalidated the address
	// it was given but also, say, invalidated everything, would pass group 2.
	// This group LENGTHENS a block at a different address and asserts the
	// block built in group 2 is still gone rather than resurrected, so the two
	// groups do not share a single failure mode.
	{
		constexpr dsp56k::TWord programStart = 0x300;

		g2::writePMem(fixture.dsp, programStart, jmpWord);

		checkEqual(fixture.executeOneBlock(programStart), uint64_t(1),
			"the short program at the second address is one instruction");

		// Lengthen it: the first word becomes a nop, so the block now runs
		// nop, nop, jmp.
		g2::writePMem(fixture.dsp, programStart,     nopWord);
		g2::writePMem(fixture.dsp, programStart + 1, nopWord);
		g2::writePMem(fixture.dsp, programStart + 2, jmpWord);

		checkEqual(fixture.executeOneBlock(programStart), uint64_t(3),
			"the LENGTHENED program runs in full after the funnel write");

		// The block group 2 rewrote is untouched by this group's writes.
		checkEqual(fixture.executeOneBlock(0x100), uint64_t(1),
			"the first address still runs its own current code");
	}

	if(g_failures)
	{
		std::cout << "t0_pmem_funnel: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_pmem_funnel: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
