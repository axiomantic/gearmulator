// Task INT-1. Tier T1: this test needs the Clavia firmware artifacts and
// SKIPS with a reason when NMG2_ARTIFACTS does not resolve.
//
// Plan section 15 (INT-1), design sections 9.1, 18.3, 22.
// The expected banner and every address below has ONE home: plan section 6.6.
// Nothing here re-derives a string; each constant names the subsection and the
// firmware address that establishes it.
//
// WHAT THIS TEST DOES. It boots `CODE_30000400.bin` DIRECTLY at 0x30000400,
// skipping the boot loader, and reads display 0's 32 character cells out of
// main memory. Plan section 6.6.9 measures that in this configuration the only
// CS2 access before the banner is the OS's own CFI probe at 0x300042e6, which
// task BRD-8's flash model answers.
//
// HOW IT DECIDES A BANNER APPEARED, AND WHY THAT IS NOT A READ OF THE CELLS.
// The firmware clears all four displays to 0x20 SPACES before it composes
// anything, so any predicate whose TRUE case a screenful of 0x20 satisfies is
// empty, and one stood here. The signals this file measures instead are the
// WRITES arriving at the SDRAM store -- counted only for bytes the clear cannot
// produce -- and a LANDMARK COUNT of instruction fetches at the banner
// function's first instruction. Neither can be satisfied by a byte that was
// already in memory, and the landmark is POSITIVE, so a firmware that never
// reaches the banner drives it to zero instead of satisfying it. The assertion
// block near the bottom of this file records what each replaced.
//
// WHY IT DRIVES Board::onRead AND Board::onWrite RATHER THAN Board::busRead.
// board.h records the measurement: an earlier revision of the composition kept
// the callbacks private, the check drove busRead instead, and restoring the old
// "return 0u with a bus-OK status" body into onRead left that check fully
// GREEN. onRead and onWrite are the exact function pointers the Board hands to
// mcf5307_create, so a core driven through them takes the path the real core
// takes. Nothing in this file reaches the routing by any other door.
//
// WHY IT ONCE CREATED ITS OWN mcf5307_ctx, AND WHAT ENDED THAT. Task INT-7,
// plan section 1.3 rule 12: the section this replaces is STRUCK AND QUOTED
// rather than deleted, because it was CORRECT ON THE DAY IT WAS WRITTEN and a
// reader who finds no trace of it will re-derive it. It read:
//
//     "WHY IT CREATES ITS OWN mcf5307_ctx. The Board owns a core but publishes
//      no handle to it, so nothing outside the Board can call mcf5307_reset
//      (which is the only way to set the program counter), mcf5307_halted,
//      mcf5307_faulted or mcf5307_get_reg. Adding an accessor would be a write
//      to `g2Lib/board.h`, which is NOT on this task's Files: line. The core
//      this file creates is pointed at the SAME Board through the SAME two
//      callbacks, so it exercises the composition and not a copy of it."
//
// THAT WAS A SCOPE CONSTRAINT AND TASK BRD-28 ENDED IT. board.h now publishes
// `resetMcu`, `mcuReg`, `setMcuReg` and `mcuHalted`, and `faulted()` reads the
// core's own flag -- every accessor the struck section names as absent. So the
// second core became vestigial, and it was not free: Scheduler::runFrames calls
// Board::runMcu on the BOARD's core, nothing ever called Board::resetMcu, and
// that core was halted and faulted from the first instant and returned zero
// cycles on every call. BRD-33 advances the MCF5307 timers from the cycles
// runMcu actually ran, so the timers received zero cycles for the whole boot
// and every interrupt-driven behaviour was unreachable under the one test that
// boots real firmware.
//
// THIS FILE NOW RESETS AND OBSERVES THE BOARD'S OWN CORE, and Scheduler::
// runFrames is the ONLY thing that advances it.
//
// WHERE THE WINDOWS COME FROM, AND WHICH TWO ARE INVENTED BY THIS HARNESS.
// CS1, CS3, CS5 and the SDRAM come from memoryMap.h, which takes them from
// AGENTS.md section 2.2. CS2 is 0x12000000..0x127FFFFF, MEASURED from the boot
// loader's own CSAR2/CSMR2 writes and recorded at plan section 6.6.9. MBAR is
// 0x10000000, MEASURED from the loader's `movel #0x10000001,%d0 / movec
// %d0,%mbar` at loader offset 0x1E and recorded at plan section 6.6.3; because
// this test boots CODE directly and the OS never writes MBAR, the harness must
// supply it. CS0's and CS4's bases are recorded by NO authority; the two values
// below are this harness's own configuration and are labelled as such at their
// site. Plan section 1.3 rule 1 is why they are here and not in a header.

#include "gatedFixture.h"

#include "../board.h"
#include "../executor.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

#include "dsp56kBase/logging.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const std::string& _what)
	{
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// dsp56kEmu's Esai::writeSlotToFrame calls LOG() once per transmit slot whose
	// data was never written, and LOG() goes to the console through
	// Logging::g_logToConsole. The hook this filter installs is the emulator's own
	// Logging::setLogFunc, so NOTHING in the vendored tree is patched to get here.
	//
	// THE LIMIT, AND IT IS THE WHOLE POINT OF THIS BLOCK. The underruns are REAL
	// and they are EXPECTED in the boot regime: nothing drains the ESAIs until the
	// codec queues arrive with task SCH-22, so every frame the Scheduler turns
	// latches empty slots. This filter hides the REPETITION of that condition and
	// nothing else -- it does not stop the underruns, and a green quiet run is NOT
	// evidence that the ESAIs are being drained. Once SCH-22 lands, a run that
	// still reports them is reporting a defect, and the kept lines below are what
	// makes that visible without re-reading a suppressed log.
	//
	// EVERY OTHER LINE IS FORWARDED UNCHANGED, including any underrun line the
	// library ever emits with different wording. The match is one message text.
	// mc68k keeps its own sink and its own stream -- mc68k::logToConsole writes to
	// stderr and this filter never sees it -- so the core's diagnostics are
	// untouched here by construction rather than by intent.
	//
	// TO TURN IT OFF: set G2_LOG_ESAI_UNDERRUN in the environment. With it set this
	// file installs no log function at all, and the run's output is byte for byte
	// what the library produces on its own.
	const char* const g_underrunMessage = "ESAI transmit underrun";

	// The first few are kept so the message, its written mask and its enabled mask
	// stay readable. The Executor interface admits a parallel implementation, so
	// the counter is atomic rather than trusting today's SerialExecutor.
	constexpr uint64_t g_underrunLinesKept = 4;

	std::atomic<uint64_t> g_underrunLines{0};

	void filterLog(const std::string& _s)
	{
		if(_s.find(g_underrunMessage) != std::string::npos &&
		   g_underrunLines.fetch_add(1) >= g_underrunLinesKept)
			return;

		std::cout << _s << '\n';
	}

	void installLogFilter()
	{
		if(std::getenv("G2_LOG_ESAI_UNDERRUN"))
			return;

		Logging::setLogFunc(&filterLog);
	}

	// The count is REPORTED rather than discarded, so "the log was silenced" stays
	// a statement about volume and not about evidence.
	void reportSuppressedLogLines()
	{
		const uint64_t seen = g_underrunLines.load();

		if(seen <= g_underrunLinesKept)
			return;

		std::cout << "note " << (seen - g_underrunLinesKept) << " further \""
		          << g_underrunMessage << "\" lines were suppressed; set "
		             "G2_LOG_ESAI_UNDERRUN to see every one of them"
		          << std::endl;
	}

	// ---------------------------------------------------------------- section 6.6

	// The display buffer base. Plan section 6.6.4 clause 1: confirmed at
	// 0x30057040, `addil #808062392,%d0`, and 808062392 decimal is 0x302A0DB8.
	constexpr uint32_t g_displayBase = 0x302A0DB8u;

	// The per-display record stride and the line width, from the address
	// arithmetic at 0x30057020..0x30057048: base + display*298 + line*16 + col.
	constexpr uint32_t g_displayStride = 298u;
	constexpr uint32_t g_lineWidth     = 16u;

	// The size a byte access presents to Board::onRead, IN THE CORE'S UNIT.
	// mcf5307.h states it twice, once per callback typedef: `size` is a COUNT
	// OF BYTES and never a width in bits. This file used to pass 8 -- the
	// MemoryMap's unit -- and the callbacks forwarded it unconverted, which is
	// the defect that made the firmware execute zero instructions. It is named
	// rather than written as a bare 1 because a silent swap of one unit for
	// another is that same defect.
	constexpr int g_byte = 1;

	// The same unit for a 16-bit access. TCN2 is a 16-bit register and sim.cpp's
	// own table declares it two bytes wide, so a byte read of it would be
	// refused rather than answered.
	constexpr int g_word = 2;

	// The two expected lines. Plan section 6.6.1 is their one home.
	//
	// THIS PROJECT PRESENTS THE G2X STRAP. Plan section 24.6 row W3-367 records
	// the operator decision: AGENTS.md section 4.1 fixes this machine at panel
	// latch bits 5:4 = 0b11, model code 1, and BRD-12 builds it there, so the
	// firmware selects `Nord Modular G2X`. That string is SIXTEEN characters
	// stored, so line 0's last cell is a real `X` and not the pad byte the
	// base model's fifteen-character string produced.
	//
	// The comparison still refuses to trim, and both literals are still written
	// out in full rather than composed, so nothing here can agree with a wrong
	// firmware by deriving its expectation from the same place the firmware
	// got it.
	const std::string g_expectedLine0 = "Nord Modular G2X";
	const std::string g_expectedLine1 = "Version 1.62 Exp";

	// ------------------------------------------------- what "a banner" means here
	//
	// THE BYTE THE DISPLAY CLEAR WRITES. MEASURED: the OS clears all four
	// displays to spaces at instruction 4,345,856, and 0x20 is the byte it
	// stores. It is named because it is the ONE value a banner predicate must
	// refuse to be satisfied by. A predicate reading "any byte >= 0x20" stood
	// here and a blank screen satisfied it, since 0x20 >= 0x20; that predicate
	// reported TRUE on a run whose landmark count at the banner function was
	// zero, and four further assertions conjoined with it and inherited its
	// emptiness.
	constexpr uint8_t g_clearByte = 0x20u;

	// TRUE for a byte the display CLEAR cannot produce and a zero-filled RAM
	// cannot produce: a printable character that is not a space. Every character
	// of `Nord Modular G2` is one of these; nothing the clear writes is, and
	// nothing a freshly constructed Ram holds is.
	// THE G2 DISPLAY DOES NOT HOLD ASCII, AND ASSUMING IT DOES COSTS THREE
	// INVESTIGATIONS. Plan section 24.6 row W3-374 is the record.
	//
	// The display helper at 0x30056FEA is a TABLE-TRANSLATING copy: it uses each
	// source character as an index into a 256-byte per-display table at
	// 0x302A0DE2 and stores table[char], never char. The firmware initialises
	// that table to the identity and then deliberately remaps exactly FIVE
	// entries, at 0x30056BE8..0x30056C04:
	//
	//     'g' -> 0x08   'p' -> 0x09   'q' -> 0x0A   'y' -> 0x0B   'j' -> 0x0C
	//
	// Those are the five DESCENDERS, and 0x08..0x0C is the CGRAM alias range for
	// custom characters 0..4. Immediately afterwards the firmware uploads five
	// 8-row glyph bitmaps from 0x300EC4B8 into those slots -- rendering them
	// gives recognisable lowercase letterforms with descending tails.
	//
	// So a cell holding 0x09 is a correctly displayed 'p'. Any reader that
	// compares raw cells against ASCII mis-reads g, j, p, q and y on every G2
	// display, on every line.
	constexpr uint8_t g_cgramFirst = 0x08u;
	constexpr uint8_t g_cgramLast  = 0x0Cu;

	// The CGRAM slot for each remapped character, and its inverse. Both are
	// written out rather than derived from each other, so a typo in one does not
	// silently agree with the other.
	constexpr char cgramToAscii(const uint8_t _byte)
	{
		return _byte == 0x08u ? 'g'
		     : _byte == 0x09u ? 'p'
		     : _byte == 0x0Au ? 'q'
		     : _byte == 0x0Bu ? 'y'
		     : _byte == 0x0Cu ? 'j'
		     : char(_byte);
	}

	constexpr bool isCgramGlyph(const uint8_t _byte)
	{
		return _byte >= g_cgramFirst && _byte <= g_cgramLast;
	}

	// A cell is CONTENT if it is an ordinary printable byte, or one of the five
	// CGRAM glyphs. The second clause is what stops a line whose only printable
	// character is a descender from counting as blank.
	constexpr bool isDisplayContent(const uint8_t _byte)
	{
		return (_byte > g_clearByte && _byte < 0x7fu) || isCgramGlyph(_byte);
	}

	// The number of characters of the expected line 0 that are display CONTENT by
	// the rule above, and the number that are not. BOTH ARE COMPUTED FROM THE
	// LITERAL AND NEITHER IS WRITTEN AS A DIGIT ANYWHERE, so a change to the
	// literal cannot leave a stale number behind. That is not a hypothetical: the
	// first draft of this comment asserted the split by hand and got it wrong, and
	// the control below -- which compares the observed counts against exactly
	// these -- is what said so.
	uint32_t contentCharacters(const std::string& _line)
	{
		uint32_t n = 0;
		for(const char c : _line)
		{
			if(isDisplayContent(uint8_t(c)))
				++n;
		}
		return n;
	}

	uint32_t clearCharacters(const std::string& _line)
	{
		uint32_t n = 0;
		for(const char c : _line)
		{
			if(uint8_t(c) == g_clearByte)
				++n;
		}
		return n;
	}

	// The two hard halts, each a `bra` to itself. Plan sections 6.6.8 and
	// 6.6.3. They are DISTINCT failure modes at distinct addresses and a boot
	// that stops must be told apart by which one it stopped at.
	constexpr uint32_t g_haltFlashGate = 0x3001BB4Cu;  // "  FLASH FAILURE "
	constexpr uint32_t g_haltModelByte = 0x3001B86Cu;  // "OS-HARDWARE ERR"

	// The banner function and the instruction after the call that reaches it.
	// Plan section 6.6.2: `jsr 0x3001B7FC` sits at 0x3001B438, and execution
	// continues at 0x3001B43E with `jsr 0x30035670`.
	constexpr uint32_t g_bannerFunction = 0x3001B7FCu;
	constexpr uint32_t g_bannerFunctionEnd = 0x3001B8B0u;
	constexpr uint32_t g_afterBannerCall = 0x3001B43Eu;

	// The entry point and the initial stack pointer. The image loads at
	// 0x30000400 and crt0 is at that address; 0x30400000 is longword 0 of the
	// real reset vector, recorded on INT-1's own block.
	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	// The register indices of the mcf5307 C ABI. 17 is the program counter,
	// 18 is the vector base register. machine.nim's regFileGet/regFileSet
	// state the whole index space and why VBR is reachable through it.
	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	// ------------------------------------------------ the vector table, TASK INT-8
	//
	// MEASURED FROM THE OS IMAGE, and it is supplied here for the reason MBAR
	// is supplied here: booting CODE directly skips the code that would set it
	// up, and the machine needs it before the firmware gets there.
	//
	// CODE_30000400.bin at 0x30058218 does
	//
	//     movel  #0x30000000,%d0
	//     movel  %d0,0x302A159C
	//     movec  %d0,%vbr
	//     clrl   %d0
	//     moveal 0x302A159C,%a0
	//     movel  #0x300585CE,%d1
	//  L: movel  %d1,(%a0,%d0.l*4)
	//     addql  #1,%d0
	//     cmpil  #0xFF,%d0
	//     bles   L
	//
	// so the table is 256 longwords at 0x30000000, every one of them the same
	// handler, and VBR is that base. The 1 KiB it occupies is the gap below
	// the image, which loads at 0x30000400 and never reaches down into it.
	//
	// WHAT IS LOAD-BEARING HERE IS VBR AND NOT THE TABLE, AND THE MEASUREMENT
	// SAYS SO RATHER THAN THE OTHER WAY ROUND. The first exception this boot
	// takes is vector 25, the level 1 autovector, at program counter
	// 0x30011FE2 -- 0x46000 bytes of firmware short of the routine above. At
	// that instant the longword at 0x30000064 already reads 0x30001854 and NOT
	// the 0x300585CE this file writes, so the firmware has filled the table
	// itself by then through some path that is not the routine above. Omitting
	// the place() below leaves the whole run identical, assertion for
	// assertion; overriding VBR does not, and the core faults to 0xFFFFFFFF.
	//
	// SO THIS TABLE IS A FLOOR AND NOT A FIX, AND IT IS LABELLED AS ONE. It
	// holds for an exception taken before the firmware's own fill, which is a
	// window no run has yet entered. Nothing below asserts it, and nothing can:
	// its absence is invisible from outside.
	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	// -------------------------------------------------------------- the windows

	// MEASURED. Plan section 6.6.3: the loader writes `movel #0x10000001,%d0`
	// then `movec %d0,%mbar` at loader offset 0x1E, and the OS image contains no
	// `movec` to %mbar at all. Booting CODE directly means this harness supplies
	// it. The size is the SIM's own g_simSpaceSize, which covers UM Table B-1.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// General-purpose timer 2's counter. MCF5307 UM table 12-1 gives TCN2 at
	// MBAR + $18C, and sim.cpp's register table carries the same offset. It is
	// read HERE, through the bus, rather than from any Timer object, so the
	// value the assertion holds is the one the core itself would see.
	constexpr uint32_t g_tcn2Offset = 0x18Cu;

	// MEASURED. Plan section 6.6.9, from the loader's CSAR2 = $1200,
	// CSMR2 = $007F0001 at loader offsets 0x70 and 0x7c: the window is
	// 0x12000000..0x127FFFFF, and the OS never reprograms it.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

	// The CS3 window carrying the ISP1181 USB device. The base is memoryMap.h's
	// g_cs3Base; the size is 64 KiB derived from CSMR3 at 0x100000A8
	// (workspace logbook section 3.8), the same figure main.cpp and
	// t0_cs3_wire.cpp configure.
	constexpr uint32_t g_cs3Size = 0x00010000u;

	// INVENTED BY THIS HARNESS AND LABELLED AS SUCH. No authority records CS0's
	// or CS4's base (plan section 4.2 register row 18, still open). CS0 carries
	// the boot loader image, which loads at 0x00000000, so 0 is the one value
	// consistent with the image this harness does not execute. CS4's base is a
	// free choice: plan section 6.6.4 puts the panel HARDWARE on the CS5 latch
	// at 0x15000004, so the banner path does not read through CS4 at all, and
	// this window exists only so an access to it is decoded rather than logged
	// as unmapped. NEITHER NUMBER IS A MEASUREMENT AND NEITHER MAY BE COPIED
	// INTO A SHIPPED HEADER.
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;

	// The SDRAM the firmware executes from. The base is memoryMap.h's
	// g_sdramBase; the size is this harness's, chosen to cover the image
	// (0x30000400 + 1,220,560 bytes ends below 0x3012A000), the display buffer
	// at 0x302A0DB8, the model byte at 0x30119848 and the stack that grows down
	// from 0x30400000.
	constexpr uint32_t g_sdramSize = 0x00800000u;

	// The CS1 window carrying the HDI08 array, and the CS5 window carrying the
	// latches. Both bases come from memoryMap.h; both sizes are configuration.
	constexpr uint32_t g_cs1Size = 0x00010000u;
	constexpr uint32_t g_cs5Size = 0x00000010u;

	// ------------------------------------------------------------------- the RAM

	// The SDRAM, as a BusTarget the harness supplies.
	//
	// WHY IT IS HERE AND NOT ON THE BOARD. board.cpp attaches the seven units
	// plan section 24.6 row W3-115 names and leaves Region::Sdram with no target
	// on purpose; main memory is not one of those seven. A firmware image has to
	// live somewhere, so the harness supplies the store. It is a plain
	// big-endian byte array with no decode of its own: every address it answers
	// has already been decoded by the BRD-1 MemoryMap.
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		/* THE BANNER OBSERVATION LIVES HERE, ON THE WRITE, AND NOT ON THE BYTES
		 * THAT SURVIVE. Every SDRAM access the core makes arrives at this object,
		 * so this is the firmware's own write path and not a second door.
		 *
		 * WHY THE WRITE AND NOT THE RESULTING CELLS. The defect this replaces was
		 * an assertion satisfied by a value that was ALREADY THERE. A predicate
		 * that reads cells back asks whether a value is present, which is the same
		 * question in a different costume: it is green whenever the bytes happen to
		 * be right, whoever put them there and whether or not anybody did. A
		 * predicate over the write asks whether the firmware performed the
		 * transaction, and no pre-existing memory content can satisfy it. Today
		 * those two forms agree only by accident -- the harness's Ram is zero
		 * filled and the image lands far below the display buffer -- and an
		 * accident is not a mechanism.
		 *
		 * IT SEPARATES THE TWO BYTE VALUES THAT CAN ARRIVE rather than counting
		 * writes, because "the cells were written" is exactly what the display
		 * CLEAR does and exactly what must not count as a banner. */
		void watchCells(const uint32_t _offset, const uint32_t _length)
		{
			m_watchOffset   = _offset;
			m_watchLength   = _length;
			m_contentWrites = 0;
			m_clearWrites   = 0;
		}

		/* A LANDMARK COUNTER AT ONE ADDRESS. mcf5307.h states that an instruction
		 * fetch presents a COUNT OF 2 BYTES at the instruction's own address, which
		 * reaches a BusTarget as a 16-bit access at exactly that offset, so a
		 * 16-bit read here is how the core executing that instruction is seen.
		 *
		 * THE ONE THING THAT COULD OVER-COUNT is a 16-bit DATA read of the same
		 * code address, which nothing in the banner path does. The counter is not
		 * taken on trust: a second watch sits at the reset PC, where the core
		 * provably executes, and the assertion on it is what shows the mechanism
		 * sees real instruction fetches on the very run being reported. */
		void watchFetch(const uint32_t _offset) { m_fetches[_offset] = 0u; }

		uint32_t contentWrites() const { return m_contentWrites; }
		uint32_t clearWrites() const { return m_clearWrites; }

		uint32_t fetches(const uint32_t _offset) const
		{
			const auto it = m_fetches.find(_offset);
			return it == m_fetches.end() ? 0u : it->second;
		}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			if(_size == 16)
			{
				const auto it = m_fetches.find(_offset);
				if(it != m_fetches.end())
					++it->second;
			}

			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t value = 0u;

			for(uint32_t i = 0; i < count; ++i)
			{
				value <<= 8;
				const size_t index = size_t(_offset) + i;
				if(index < m_bytes.size())
					value |= m_bytes[index];
			}

			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			const uint32_t count = uint32_t(_size) / 8u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
				if(index >= m_bytes.size())
					continue;
				const int shift = int(8u * (count - 1u - i));
				const auto byte = uint8_t((_value >> shift) & 0xffu);
				m_bytes[index] = byte;
				observeWrite(index, byte);
			}
		}

		// Places an image at a window-relative offset. Used to put the OS image
		// where the loader would have put it.
		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

	private:
		// One byte that landed in the store, classified. A byte outside the watched
		// span is not a display cell and counts as neither.
		void observeWrite(const size_t _index, const uint8_t _byte)
		{
			if(m_watchLength == 0)
				return;
			if(_index < size_t(m_watchOffset) || _index >= size_t(m_watchOffset) + m_watchLength)
				return;

			if(isDisplayContent(_byte))
				++m_contentWrites;
			else if(_byte == g_clearByte)
				++m_clearWrites;
		}

		std::vector<uint8_t> m_bytes;

		uint32_t m_watchOffset   = 0;
		uint32_t m_watchLength   = 0;
		uint32_t m_contentWrites = 0;
		uint32_t m_clearWrites   = 0;

		std::map<uint32_t, uint32_t> m_fetches;
	};

	// ---------------------------------------------------------------- the harness

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;

		config.memory.cs0   = {g_cs0Base,     g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base, g_cs1Size};
		config.memory.cs2   = {g_cs2Base,     g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base, g_cs3Size};
		config.memory.cs4   = {g_cs4Base,     g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base, g_cs5Size};
		config.memory.mbar  = {g_mbarBase,    g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		return config;
	}

	// Reads one display line out of main memory, UNTRIMMED and with every byte
	// taken as it stands. It reads through Board::onRead, which is the path the
	// core takes, so a buffer that is correct only through some other door does
	// not satisfy it.
	std::string readDisplayLine(g2::Board& _board, const uint32_t _display, const uint32_t _line)
	{
		const uint32_t base = g_displayBase + _display * g_displayStride + _line * g_lineWidth;

		std::string out;
		out.reserve(g_lineWidth);

		for(uint32_t col = 0; col < g_lineWidth; ++col)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t byte = g2::Board::onRead(&_board, base + col, g_byte, &status);

			// Decode the five CGRAM glyphs back to the characters they render.
			// The cells hold display codes, not ASCII -- see W3-374 at
			// isDisplayContent above. Every other byte passes through untouched,
			// so a genuinely wrong cell still reads as whatever it actually is.
			out.push_back(cgramToAscii(uint8_t(byte & 0xffu)));
		}

		return out;
	}

	// The printable form of a line, so a failure report names the bytes that
	// were actually there instead of leaving a reader to guess at a trailing
	// space or a NUL.
	std::string escapedLine(const std::string& _line)
	{
		std::string out = "\"";
		for(const char c : _line)
		{
			const auto u = uint8_t(c);
			if(u >= 0x20 && u < 0x7f)
			{
				out.push_back(c);
				continue;
			}
			char buf[8];
			std::snprintf(buf, sizeof buf, "\\x%02X", unsigned(u));
			out += buf;
		}
		out += "\"";
		return out;
	}

	// Whether ONE HDI08 port stands, AT THIS INSTANT, with the handshake's two
	// flags both raised. HF0 is the host's own flag in ICR and HF2 is the DSP's
	// answer in ISR, so both must be set: HF0 alone is the firmware talking to
	// itself.
	//
	// icr() and isr() are the model's own register readers. The BusTarget-facing
	// read8 override is deliberately not used here: this is an inspection and it
	// must not look like a bus cycle.
	bool portHandshakeRaised(g2::Board& _board, const int _port)
	{
		auto& hdi08 = _board.hdi08().port(_port);

		const uint8_t icr = hdi08.icr();
		const uint8_t isr = hdi08.isr();

		return (icr & mc68k::Hdi08::Hf0) != 0 && (isr & mc68k::Hdi08::Hf2) != 0;
	}

	// The number of ports raised at the instant of the call. THIS IS A LEVEL READ
	// AND IT IS NOT AN ACCEPTANCE PREDICATE: plan section 24.6 row W3-394 measured
	// icr=0x0 isr=0x6 on all eight ports at the end of a 424,537-iteration run, on
	// a machine whose handshake had provably run. It is kept because the report
	// prints it beside the latch, where the pair is what shows the event is a
	// transient rather than a state.
	int handshakePortCount(g2::Board& _board)
	{
		int completed = 0;

		for(int port = 0; port < g2::g_hdi08PortCount; ++port)
		{
			if(portHandshakeRaised(_board, port))
				++completed;
		}

		return completed;
	}

	/* THE ACCEPTANCE PREDICATE, AND IT IS AN EDGE DETECTOR. Plan section 24.6 row
	 * W3-394: the firmware sets HF0, the DSP answers HF2, the routine returns and
	 * the firmware clears HF0, so the two flags stand together only inside a
	 * bounded window. A machine that never ran the handshake and a machine that
	 * ran it and finished present the SAME registers afterwards, which is why the
	 * terminal snapshot read zero for this whole project and was mistaken for a
	 * defect report each time.
	 *
	 * The latch is STICKY and PER PORT: once a port has been seen raised it stays
	 * recorded for the rest of the run. That is what the hardware promises -- the
	 * edge happens once -- so counting completions instead would assert a
	 * repetition nothing guarantees, and asserting the fetch landmark at the
	 * handshake routine alone would prove the firmware ASKED and not that any DSP
	 * ANSWERED.
	 *
	 * SAMPLED ONCE PER ITERATION OF THE DRIVE LOOP, at the same granularity the
	 * loop advances the machine, so no completion can open and close between two
	 * samples unobserved. */
	void latchHandshakePorts(g2::Board& _board, std::vector<bool>& _latched)
	{
		for(int port = 0; port < g2::g_hdi08PortCount; ++port)
		{
			if(portHandshakeRaised(_board, port))
				_latched[size_t(port)] = true;
		}
	}

	int latchedPortCount(const std::vector<bool>& _latched)
	{
		int completed = 0;

		for(const bool latched : _latched)
		{
			if(latched)
				++completed;
		}

		return completed;
	}

	/* THE ITERATION BOUND. It is a STOP so that a machine which never converges
	 * fails rather than hanging the suite; it is not a figure the firmware
	 * publishes. The previous value, 0xFDE8, was the firmware's own handshake
	 * retry count and it is demonstrably too small for a boot: plan section 24.6
	 * row W3-394 measured the real boot needing roughly 425,000 iterations to
	 * reach the patch browser. Sized above that, and the run costs roughly ninety
	 * seconds when it is reached. */
	constexpr uint32_t g_handshakeIterations = 500000u;

	/* THE BUDGET OF THE ONE OBSERVING Board::runMcu CALL, AND OF NOTHING ELSE.
	 * Task INT-7 deleted the per-iteration mcf5307_exec this used to size; the
	 * single sample taken after the drive is over is its only remaining reader,
	 * and that sample asks for a budget large enough that a running core cannot
	 * answer zero merely because it was asked for nothing. */
	constexpr uint32_t g_cyclesPerIteration = 4096u;

	/* ONE SCHEDULER FRAME PER ITERATION, AND IT IS NOW THE WHOLE DRIVE. Task
	 * INT-7: there is ONE core, the Board's, so the frame turns the DSP set,
	 * the chain, the panel AND the MCU. The mcf5307_exec calls that used to sit
	 * beside this were DELETED rather than repointed at Board::runMcu, because
	 * a second budget applied to the same core would double-count the cycles
	 * the scheduler already allocated -- and BRD-33 feeds those cycles to the
	 * timers, so double-counting them would falsify a timer tick.
	 *
	 * runFrames carries no cycle budget out of this file: it allocates the MCU's
	 * cycles from its own Config rational, so what the drive inherits from the
	 * loop is the ITERATION BOUND and nothing else. */
	constexpr size_t g_framesPerIteration = 1;

	/* HOW LONG THE FIRMWARE IS GIVEN AFTER THE FIRST PRINTABLE CHARACTER.
	 * The previous exit fired on the FIRST content write, which was correct only
	 * while the sole content the boot ever produced was the flash-failure string.
	 * With the flash gate cleared the firmware composes a real banner one
	 * character at a time, so stopping at the first one samples the machine
	 * mid-word -- the measured symptom was line 0 reading "Nord" and stopping.
	 * The loop now keeps running after first content and leaves only once the
	 * display has been quiet for this many iterations. */
	constexpr uint32_t g_bannerSettleIterations = 20000u;

	// What one boot produced. Everything the assertions below read comes from
	// here, so the run happens once and no assertion can re-run the machine and
	// quietly get a second answer.
	struct BootResult
	{
		bool     imageLoaded    = false;
		bool     readPathProven = false;

		// The writes the firmware made into display 0 line 0, separated into the
		// ones the display CLEAR cannot account for and the ones it can. See the
		// Ram observation comment for why this is a write count and not a
		// read-back.
		uint32_t contentWrites  = 0;
		uint32_t clearWrites    = 0;

		// The landmark counters. `entryFetches` is at the reset PC, where the core
		// provably executes, and it is the instrumentation's own control;
		// `bannerEntries` is at the banner function and is the measurement.
		uint32_t entryFetches   = 0;
		uint32_t bannerEntries  = 0;

		std::string line0;
		std::string line1;
		// The LATCHED count -- ports observed with HF0 and HF2 raised together at
		// any point during the drive -- and, beside it, the level read taken at
		// the end. The second is reported and never asserted; see
		// latchHandshakePorts.
		int      handshakePorts    = 0;
		int      handshakeAtEnd    = 0;
		uint32_t pcAtBanner     = 0;
		uint32_t pcAfterBanner  = 0;
		uint32_t pcLater        = 0;
		bool     halted         = false;
		bool     faulted        = false;
		uint32_t iterations     = 0;

		/* TASK INT-7. THE THREE SIGNALS THAT SAY THE BOARD'S OWN CORE IS THE
		 * ONE THAT RAN, and each of them reads zero-or-true when it is not.
		 *
		 * `mcuCycles` is ONE Board::runMcu return value, sampled once after the
		 * drive has finished. It is an OBSERVATION and not a second drive: the
		 * firmware is advanced by Scheduler::runFrames alone, and this call
		 * exists because runFrames discards what runMcu returns and publishes
		 * no cycle count of its own.
		 *
		 * `haltedAtBanner` is Board::mcuHalted() sampled WHILE THE FIRMWARE
		 * RUNS -- at the first iteration that observed display content -- and
		 * not at the end, because "the core has not halted yet" is a claim
		 * about the run and not about its aftermath. It starts TRUE so that a
		 * run which never reaches that point fails rather than passing on an
		 * unwritten field.
		 *
		 * `tcn2` is the SIM's general-purpose timer 2 counter read through the
		 * bus. BRD-33 advances the timers from the cycles runMcu actually ran,
		 * so a core that runs zero cycles leaves this at zero however long the
		 * scheduler turns. */
		uint32_t mcuCycles      = 0;
		uint32_t tcn2           = 0;
		uint32_t tcn2Latched    = 0;   /* the witness: see the latch site */
		bool     haltedAtBanner = true;
		std::vector<std::string> busLog;

		// The DSP cycle counters, one for each slot the Board owns, sampled once
		// at the end of the run. The CARDINALITY is carried separately from the
		// values because a per-slot property over an empty set is vacuously
		// true, so the count is what the loop bound is held to rather than read
		// from.
		unsigned              dspCount = 0;
		std::vector<uint64_t> dspCycles;
	};

	// WRITTEN OUT RATHER THAN READ FROM THE OBJECT UNDER TEST. DspSet holds a
	// fixed array and dspCount() returns its size, so a comparison against that
	// same accessor would agree with itself whatever the array became.
	constexpr unsigned g_expectedDspCount = 8u;

	bool everyDspRan(const BootResult& _r)
	{
		for(const uint64_t cycles : _r.dspCycles)
		{
			if(cycles == 0u)
				return false;
		}

		return true;
	}

	std::string dspCycleList(const BootResult& _r)
	{
		std::string out;

		for(size_t i = 0; i < _r.dspCycles.size(); ++i)
		{
			if(i != 0)
				out += ", ";
			out += std::to_string(_r.dspCycles[i]);
		}

		return "[" + out + "]";
	}

	bool runBoot(const std::string& _directory, BootResult& _result)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig());

		// The SDRAM, attached by the harness. See the Ram comment above.
		Ram ram(g_sdramSize);

		// The image goes where its name says it goes: 0x30000400, which is
		// offset 0x400 into the SDRAM window at 0x30000000.
		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit the configured SDRAM window" << std::endl;
			return false;
		}

		// TASK INT-8. The vector table, big-endian, 256 identical longwords.
		// It goes in through Ram::place beside the image, which is the harness's
		// other supply site, and it goes in BEFORE the watches below so that
		// none of it is counted as the firmware's own traffic.
		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);

			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
			{
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] =
						uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			}

			if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit the configured SDRAM window" << std::endl;
				return false;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);
		_result.imageLoaded = true;

		/* THE READ PATH IS PROVEN BEFORE THE MACHINE IS RUN, AND THIS IS THE ONE
		 * CHECK THAT MAKES A ZERO DISPLAY BUFFER MEAN SOMETHING.
		 *
		 * A buffer of zeroes is produced by two completely different failures: a
		 * firmware that never composed a banner, and a read path that cannot
		 * reach memory at all. They are indistinguishable from the buffer alone,
		 * and reporting the first when the second is true is exactly this
		 * project's signature defect. So sixteen bytes are read back at the
		 * entry point THROUGH Board::onRead and compared against the image file:
		 * a match exercises the whole chain -- the decode, the Sdram region, the
		 * window-relative offset and the big-endian byte order -- with a value
		 * that was NOT already there, because a freshly constructed Ram is
		 * zero-filled and the image's first bytes are not zero. */
		_result.readPathProven = true;

		for(uint32_t i = 0; i < 16u; ++i)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t byte = g2::Board::onRead(&board, g_entryPc + i, g_byte, &status);

			if(status != MCF5307_BUS_OK || uint8_t(byte & 0xffu) != code[i])
				_result.readPathProven = false;
		}

		/* THE WATCHES ARE INSTALLED HERE, AFTER THE READ-PATH PROOF AND BEFORE THE
		 * CORE RUNS, so that every count below is the FIRMWARE's and none of it is
		 * the harness's own traffic. The proof above reads sixteen BYTES at the
		 * reset PC; a byte read is 8 bits and the landmark counter takes only
		 * 16-bit accesses, so it could not have contaminated the count either way,
		 * but a count whose emptiness depends on that coincidence is not one this
		 * file is willing to report. */
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);
		ram.watchFetch(g_entryPc - g2::g_sdramBase);
		ram.watchFetch(g_bannerFunction - g2::g_sdramBase);

		/* TASK INT-7. THE BOARD'S OWN CORE IS RESET, AND NO SECOND CORE IS
		 * CREATED. The Board already pointed its core at Board::onRead and
		 * Board::onWrite, so this is the same composition the struck header
		 * section went out of its way to reach -- reached now through the
		 * handle BRD-28 published instead of through a copy. */
		board.resetMcu(g_entrySp, g_entryPc);

		/* TASK INT-8. VBR IS PLACED AFTER THE RESET AND NOT BEFORE IT, because
		 * the reset is what defines the machine's starting state and a value
		 * written ahead of it would depend on what the reset does NOT clear.
		 *
		 * THE RETURN IS CHECKED. setMcuReg answers FALSE for an index the core
		 * refuses, and a core that refused this one would leave the table based
		 * at zero with nothing said about it -- which is the failure this whole
		 * block exists to remove. */
		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index "
			          << g_regVbr << std::endl;
			return false;
		}

		/* THE SCHEDULER, TASK INT-3. It is declared AFTER the Board so that it is
		 * destroyed BEFORE it: it borrows the Board's DSP set, and it installs
		 * chain callbacks into ESAIs the Board owns. The Executor is declared
		 * before the Scheduler for the same reason.
		 *
		 * A NULL RETURN IS THE ONE REJECTION THAT CARRIES A REASON, so the status
		 * is reported here and the run loop is not entered. Every Config default
		 * is a legal value and the factory is the single rejection point, so this
		 * is the only place a reason exists to be printed. */
		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		// PHASE 1 -- run until the firmware composes display content, or until the
		// bound.
		std::vector<bool> handshakeLatched(size_t(g2::g_hdi08PortCount), false);

		uint32_t settleIterations = 0;
		bool     bannerLatched    = false;
		for(uint32_t i = 0; i < g_handshakeIterations; ++i)
		{
			_result.iterations = i + 1;

			scheduler->runFrames(g_framesPerIteration);

			// THE LATCH IS SAMPLED BEFORE THE HALT TEST, so the iteration that
			// halts the core still contributes its observation.
			latchHandshakePorts(board, handshakeLatched);

			/* TCN2 IS A SAWTOOTH AND NOT A MONOTONIC WITNESS, SO IT IS LATCHED
			 * FOR THE SAME REASON THE HANDSHAKE IS -- plan section 24.6 row
			 * W3-403. The firmware programs TMR = 0x7F3B, whose FRR bit is SET,
			 * and TRR = 0x32, so the counter runs 0 to 50 and RESTARTS AT ZERO
			 * on every reference match. A terminal read samples a value in
			 * 0..50, and READING ZERO IS A LEGAL HEALTHY VALUE -- the same
			 * reading the required-red mutation produces when the core never
			 * ran, so the old assertion could not tell them apart and passed by
			 * luck. */
			if(_result.tcn2Latched == 0u)
			{
				mcf5307_bus_status tcnStatus = MCF5307_BUS_OK;
				const uint32_t tcnNow =
					g2::Board::onRead(&board, g_mbarBase + g_tcn2Offset, g_word, &tcnStatus);
				if(tcnStatus == MCF5307_BUS_OK && tcnNow != 0u)
					_result.tcn2Latched = tcnNow;
			}

			if(board.mcuHalted())
				break;

			/* THE LOOP LEAVES ON THE SAME PREDICATE THE ASSERTION USES, and that
			 * matters as much as the assertion does: the previous exit condition
			 * was the vacuous one, so the machine stopped the moment the display
			 * CLEAR ran and the program counter was then sampled in the middle of
			 * a boot that had not reached the banner. */
			if(ram.contentWrites() > 0)
			{
				if(_result.pcAtBanner == 0)
				{
					_result.pcAtBanner = board.mcuReg(g_regPc);

					// TASK INT-7. Sampled here, mid-run, and not at the end.
					_result.haltedAtBanner = board.mcuHalted();
				}

				/* THE BANNER IS LATCHED WHEN IT APPEARS, FOR THE SAME REASON THE
				 * HANDSHAKE IS -- plan section 24.6 rows W3-394 and W3-395. BOTH
				 * ARE TRANSIENTS. The firmware composes the banner, then boots on
				 * and the PATCH BROWSER overwrites it: measured at the full bound,
				 * line 0 reads "-:-       No Cat" and line 1 is blank, with the
				 * banner long gone. A terminal sample of one run cannot satisfy
				 * the banner clause and the handshake clause together, because
				 * they are true at different times.
				 *
				 * So the FIRST composed banner is captured and kept. The settle
				 * counter still runs, because the banner is written a character
				 * at a time and a capture on the first content byte would catch
				 * a partial line -- that is the defect the settle window was
				 * added to fix. What changes is that reaching the settle no
				 * longer STOPS the drive: it freezes the banner and lets the
				 * machine run on so the handshake can be observed. */
				if(++settleIterations >= g_bannerSettleIterations && !bannerLatched)
				{
					bannerLatched = true;
					_result.line0 = readDisplayLine(board, 0, 0);
					_result.line1 = readDisplayLine(board, 0, 1);
				}
			}
		}

		/* A run that never composed a banner latched nothing, and the empty
		 * strings it leaves must fail the comparison rather than read as a
		 * pass. readDisplayLine is called here ONLY in that case, so the
		 * assertion reports what the display actually held. */
		if(!bannerLatched)
		{
			_result.line0 = readDisplayLine(board, 0, 0);
			_result.line1 = readDisplayLine(board, 0, 1);
		}


		_result.pcAfterBanner = board.mcuReg(g_regPc);

		// PHASE 2 -- the answer to plan section 24.6 row W3-129. A green read of
		// correct cells does NOT by itself show the firmware ran on, so the
		// machine is run further and its program counter is sampled again. Plan
		// section 6.6.5's blocking claim is REFUTED by row W3-144 -- the spin at
		// 0x30056E52 services its own work at 0x30056E7E and terminates without a
		// timer -- so a machine that reached the banner is expected to leave it,
		// and a machine that did not is expected to sit still. The two are told
		// apart below.
		for(uint32_t i = 0; i < 64u && !board.mcuHalted(); ++i)
		{
			scheduler->runFrames(g_framesPerIteration);
			latchHandshakePorts(board, handshakeLatched);

			/* TCN2 IS A SAWTOOTH AND NOT A MONOTONIC WITNESS, SO IT IS LATCHED
			 * FOR THE SAME REASON THE HANDSHAKE IS -- plan section 24.6 row
			 * W3-403. The firmware programs TMR = 0x7F3B, whose FRR bit is SET,
			 * and TRR = 0x32, so the counter runs 0 to 50 and RESTARTS AT ZERO
			 * on every reference match. A terminal read samples a value in
			 * 0..50, and READING ZERO IS A LEGAL HEALTHY VALUE -- the same
			 * reading the required-red mutation produces when the core never
			 * ran, so the old assertion could not tell them apart and passed by
			 * luck. */
			if(_result.tcn2Latched == 0u)
			{
				mcf5307_bus_status tcnStatus = MCF5307_BUS_OK;
				const uint32_t tcnNow =
					g2::Board::onRead(&board, g_mbarBase + g_tcn2Offset, g_word, &tcnStatus);
				if(tcnStatus == MCF5307_BUS_OK && tcnNow != 0u)
					_result.tcn2Latched = tcnNow;
			}
		}

		_result.pcLater = board.mcuReg(g_regPc);
		_result.halted  = board.mcuHalted();
		_result.faulted = board.faulted();
		_result.handshakePorts = latchedPortCount(handshakeLatched);
		_result.handshakeAtEnd = handshakePortCount(board);

		/* TASK INT-7. THE ONE SAMPLE OF Board::runMcu's RETURN VALUE, taken
		 * after the drive is over so that it cannot alter what any other
		 * counter above measured. See the BootResult comment for why it exists
		 * at all. */
		_result.mcuCycles = board.runMcu(g_cyclesPerIteration);

		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t tcn2 =
				g2::Board::onRead(&board, g_mbarBase + g_tcn2Offset, g_word, &status);
			_result.tcn2 = (status == MCF5307_BUS_OK) ? tcn2 : 0u;
		}

		_result.busLog = board.memory().log();

		_result.dspCount = board.dspSet().dspCount();

		for(unsigned i = 0; i < _result.dspCount; ++i)
			_result.dspCycles.push_back(board.dspSet().dsp(i).getCycles());

		_result.contentWrites = ram.contentWrites();
		_result.clearWrites   = ram.clearWrites();
		_result.entryFetches  = ram.fetches(g_entryPc - g2::g_sdramBase);
		_result.bannerEntries = ram.fetches(g_bannerFunction - g2::g_sdramBase);

		return true;
	}

	bool insideBanner(const uint32_t _pc)
	{
		return _pc >= g_bannerFunction && _pc < g_bannerFunctionEnd;
	}

	// ------------------------------------------------------- the positive control
	//
	// THE PREDICATE'S FALSE CASE IS OBSERVED AND ITS TRUE CASE IS CONSTRUCTED,
	// and the asymmetry is stated rather than hidden. The firmware rests before
	// the banner in the closed loop at 0x300505d4..0x300505e0, which makes no
	// MBAR access at all: it polls HDI08 port 3's ISR at 0x110007BA for RXDF,
	// waiting on a reply the bootstrapped DSPs have not sent. Driving the
	// scheduler beside the core turns those DSPs and does not end the wait, so
	// no run available to this file reaches a real banner and the TRUE case
	// cannot be observed. It is built instead: the
	// expected bytes are driven into the display buffer through Board::onWrite --
	// the exact static callback handed to mcf5307_create, so the same decode, the
	// same region and the same store the core's own writes reach -- and the
	// predicate is read back.
	//
	// WHAT THE CONTROLS PROVE. That the predicate answers FALSE for clear-shaped
	// content and TRUE for banner-shaped content arriving by the firmware's own
	// route, and that the landmark counter answers one address rather than its
	// neighbour. A control that only showed the predicate reads memory would be
	// worth nothing here, because the predicate it replaces read memory correctly
	// and was still empty; each control below therefore separates the two shapes
	// by an EXACT COUNT and not by a non-zero test.
	//
	// WHAT THEY DO NOT PROVE. Not that the real firmware, on a run that got
	// there, would take this route -- the read-path proof and the reset-PC
	// landmark, both measured on the real core in the same run, are what carry
	// that. Not that these are the bytes the firmware would compose: plan section
	// 6.6.1 is the authority for those and the two equality clauses are what hold
	// the firmware to them. And not that the banner function's landmark would
	// fire on a real entry, only that the counter is bound to that address.

	// One Board with the harness's SDRAM attached and no firmware in it.
	struct ControlRig
	{
		ControlRig() : board(makeConfig()), ram(g_sdramSize)
		{
			board.memory().attach(g2::Region::Sdram, &ram);
			ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);
			ram.watchFetch(g_bannerFunction - g2::g_sdramBase);
		}

		// Drives one line into display 0 line 0 a byte at a time through the
		// installed write callback.
		void writeLine0(const std::string& _line)
		{
			for(uint32_t col = 0; col < g_lineWidth && col < _line.size(); ++col)
			{
				mcf5307_bus_status status = MCF5307_BUS_OK;
				g2::Board::onWrite(&board, g_displayBase + col, g_byte, uint32_t(uint8_t(_line[col])), &status);
			}
		}

		g2::Board board;
		Ram       ram;
	};

	void runControls()
	{
		// CONTROL 1 -- CLEAR-SHAPED. Sixteen 0x20 spaces, which is byte for byte
		// what the firmware's display clear writes. The predicate must count zero
		// content here; the previous predicate counted this as a banner.
		{
			ControlRig rig;
			rig.writeLine0(std::string(g_lineWidth, char(g_clearByte)));

			check(rig.ram.contentWrites() == 0u && rig.ram.clearWrites() == g_lineWidth,
			      "CONTROL clear-shaped: sixteen 0x20 spaces written into display 0 line 0 "
			      "through Board::onWrite count 0 content writes and " + std::to_string(g_lineWidth) +
			      " clear writes; counted " + std::to_string(rig.ram.contentWrites()) + " and " +
			      std::to_string(rig.ram.clearWrites()));
		}

		// CONTROL 2 -- BANNER-SHAPED, the same cells by the same route.
		{
			ControlRig rig;
			rig.writeLine0(g_expectedLine0);

			const uint32_t expectedContent = contentCharacters(g_expectedLine0);
			const uint32_t expectedClear   = clearCharacters(g_expectedLine0);

			check(rig.ram.contentWrites() == expectedContent && rig.ram.clearWrites() == expectedClear,
			      "CONTROL banner-shaped: " + escapedLine(g_expectedLine0) + " written by the same "
			      "route counts " + std::to_string(expectedContent) + " content writes and " +
			      std::to_string(expectedClear) + " clear writes; counted " +
			      std::to_string(rig.ram.contentWrites()) + " and " +
			      std::to_string(rig.ram.clearWrites()));

			const std::string readBack = readDisplayLine(rig.board, 0, 0);

			check(readBack == g_expectedLine0,
			      "CONTROL banner-shaped: the same sixteen cells read back through Board::onRead "
			      "equal " + escapedLine(g_expectedLine0) + "; read " + escapedLine(readBack));
		}

		// CONTROL 3 -- the landmark counter answers ONE address. A 2-byte read at
		// the instruction before the banner function must not move it and one at
		// the banner function must, so the count after both is exactly 1.
		{
			ControlRig rig;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			g2::Board::onRead(&rig.board, g_bannerFunction - 2u, 2, &status);
			g2::Board::onRead(&rig.board, g_bannerFunction, 2, &status);

			const uint32_t counted = rig.ram.fetches(g_bannerFunction - g2::g_sdramBase);

			check(counted == 1u,
			      "CONTROL landmark: one 2-byte read at 0x3001B7FA and one at 0x3001B7FC leave "
			      "the counter at 0x3001B7FC reading exactly 1, so it answers that address and "
			      "not its neighbour; read " + std::to_string(counted));
		}
	}

	void report(const BootResult& _r)
	{
		std::cout << "boot: iterations=" << _r.iterations
		          << " readPathProven=" << (_r.readPathProven ? 1 : 0)
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0) << std::endl;
		std::cout << "boot: display 0 line 0 writes: content=" << _r.contentWrites
		          << " clear=" << _r.clearWrites << std::endl;
		std::cout << "boot: landmark 16-bit fetches: 0x30000400=" << _r.entryFetches
		          << " 0x3001B7FC=" << _r.bannerEntries << std::endl;
		std::cout << "boot: pcAtBanner=0x" << std::hex << _r.pcAtBanner
		          << " pcAfterBanner=0x" << _r.pcAfterBanner
		          << " pcLater=0x" << _r.pcLater << std::dec << std::endl;
		std::cout << "boot: line0=" << escapedLine(_r.line0) << std::endl;
		std::cout << "boot: line1=" << escapedLine(_r.line1) << std::endl;
		std::cout << "boot: handshakePorts=" << _r.handshakePorts
		          << " (latched) handshakeAtEnd=" << _r.handshakeAtEnd
		          << " (level read, reported only)" << std::endl;

		// TASK INT-7. The Board's own core, reported before it is asserted on.
		std::cout << "boot: boardCore mcuCycles=" << _r.mcuCycles
		          << " haltedAtBanner=" << (_r.haltedAtBanner ? 1 : 0)
		          << " tcn2=" << _r.tcn2 << std::endl;

		std::cout << "boot: dspCount=" << _r.dspCount << " dspCycles=";
		for(const uint64_t cycles : _r.dspCycles)
			std::cout << cycles << ' ';
		std::cout << std::endl;

		// Every access the decode refused, in full. A boot that stopped stopped
		// somewhere, and this is the trace that names where.
		std::cout << "boot: bus log lines=" << _r.busLog.size() << std::endl;
		size_t printed = 0;
		for(const auto& line : _r.busLog)
		{
			if(printed++ >= 40)
			{
				std::cout << "boot: (bus log truncated after 40 lines)" << std::endl;
				break;
			}
			std::cout << "boot:   " << line << std::endl;
		}
	}
}

int main()
{
	// BEFORE ANYTHING RUNS, so that no emitter escapes the filter by being
	// constructed early. See its definition for what it hides and how to stop it.
	installLogFilter();

	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		// THE CONTROLS RUN BEFORE THE BOOT so that a reader meets the proof that
		// the banner predicate discriminates before meeting the number it
		// produced. They need no firmware; they construct their own Board.
		runControls();

		BootResult result;

		if(!runBoot(directory, result))
			return false;

		report(result);

		// ----------------------------------------------------- the read path first
		//
		// It runs before the two Check clauses so that a failure here is read as
		// "the harness cannot see memory" and never as "the firmware printed
		// nothing". See the comment at its measurement site.
		check(result.readPathProven,
		      "sixteen bytes read at 0x30000400 through Board::onRead equal the "
		      "first sixteen bytes of CODE_30000400.bin, so the display read "
		      "path reaches memory and a zero buffer means the firmware wrote "
		      "nothing");

		// ------------------------------------------------- the two Check clauses

		// CLAUSE 1, the banner, compared UNTRIMMED and by EQUALITY. Plan section
		// 6.6.1 is the one home of both literals. A substring, a trim or a
		// non-empty test does not satisfy this and plan section 24.6 row W3-95
		// is why.
		check(result.line0 == g_expectedLine0,
		      "display 0 line 0 equals " + escapedLine(g_expectedLine0) +
		      " untrimmed; read " + escapedLine(result.line0));

		check(result.line1 == g_expectedLine1,
		      "display 0 line 1 equals " + escapedLine(g_expectedLine1) +
		      "; read " + escapedLine(result.line1));

		// CLAUSE 2, the handshake count, compared AS A NUMBER. It is the LATCHED
		// count: ports seen with HF0 and HF2 raised TOGETHER at any sample of the
		// drive. The bound and the iterations actually run are both named from
		// the values themselves, so neither can go stale against a literal.
		check(result.handshakePorts == int(g2::g_hdi08PortCount),
		      "HDI08 ports observed with HF0 and HF2 raised together at some point "
		      "within the " + std::to_string(g_handshakeIterations) +
		      "-iteration bound equals " + std::to_string(g2::g_hdi08PortCount) +
		      "; counted " + std::to_string(result.handshakePorts) + " latched over " +
		      std::to_string(result.iterations) + " iterations run, with " +
		      std::to_string(result.handshakeAtEnd) +
		      " still raised at the end (a level read, not the acceptance)");

		// ------------------------------ the answer to plan section 24.6 row W3-129
		//
		// W3-129 records that correct cells can be read off a machine that is
		// stuck, so a green comparison alone does not show the firmware ran on
		// past the banner. Row W3-144 then refuted the blocking premise: the spin
		// at 0x30056E52 calls the flush at 0x3005687C from INSIDE its own body at
		// 0x30056E7E, so it terminates with no timer, no scheduler and no
		// interrupt. THIS TEST THEREFORE ASSERTS PROGRESS RATHER THAN ACCEPTING
		// THE LIMITATION.
		//
		// WHAT USED TO STAND HERE, AND WHY EVERY LINE OF IT IS GONE. Five
		// assertions conjoined `bannerAppeared`, which was set by "any byte >=
		// 0x20 in display 0 line 0". The firmware clears all four displays to
		// 0x20 spaces at instruction 4,345,856, and 0x20 >= 0x20, so a BLANK
		// SCREEN set it. All five reported ok on a run whose landmark count at
		// the banner function was zero. The conjunction had been added to rescue
		// the last three from a different emptiness -- a core stopped at its entry
		// point sits at neither halt address and is trivially outside the banner
		// function -- and it rescued nothing, because the conjunct was empty too.
		// Two emptinesses in series look exactly like a check.
		//
		// THE SHAPE THE REPAIR TAKES. A conjunct is only worth its place if it can
		// be FALSE, so each signal below is measured rather than inferred from a
		// value that might already be sitting in memory:
		//
		//   - display content is counted ON THE WRITE, and only for bytes the
		//     clear cannot produce, so no pre-existing byte can satisfy it;
		//   - "the firmware got there" is a landmark count at the banner
		//     function's first instruction, which is POSITIVE: never entering the
		//     function drives it to zero rather than satisfying it, and that is
		//     the defect in "has LEFT the banner function" that no conjunction
		//     could have fixed;
		//   - the landmark counter is itself controlled, at the reset PC, on this
		//     same run.

		// THE INSTRUMENTATION FIRST, for the same reason the read path is checked
		// before the two Check clauses: a zero landmark count must be read as "the
		// firmware did not arrive" and never as "the counter did not look".
		check(result.entryFetches > 0,
		      "the landmark counter saw the core's own 16-bit instruction fetches at "
		      "the reset PC 0x30000400, so a zero count at any other landmark is the "
		      "firmware not arriving rather than the counter not looking; counted " +
		      std::to_string(result.entryFetches));

		// THE BANNER, COUNTED ON THE WRITE. A blank display cannot satisfy this:
		// the clear's 0x20 is excluded by construction and counted separately, so
		// the two shapes are told apart rather than merged.
		check(result.contentWrites > 0,
		      "the firmware wrote at least one printable non-space character into "
		      "display 0 line 0, which the display clear's 0x20 spaces cannot "
		      "satisfy; counted " + std::to_string(result.contentWrites) +
		      " content writes and " + std::to_string(result.clearWrites) +
		      " clear writes");

		// THE THREE PROGRESS CLAIMS. Each names one failure mode and each conjoins
		// a signal that a core which never ran drives to zero.
		check(result.bannerEntries > 0,
		      "execution entered the banner function 0x3001B7FC; counted " +
		      std::to_string(result.bannerEntries) + " fetches of its first instruction");

		check(result.bannerEntries > 0 && !insideBanner(result.pcLater),
		      "execution entered the banner function 0x3001B7FC AND is no longer "
		      "inside it, so the firmware returned past the banner rather than "
		      "never arriving or stopping within it");

		check(result.bannerEntries > 0 && !result.halted,
		      "execution entered the banner function 0x3001B7FC AND the core has not "
		      "halted");

		// The two hard halts plan sections 6.6.8 and 6.6.3 record, merged into one
		// claim. Kept as assertions rather than as report lines because a firmware
		// can reach either one with display 0 line 0 already holding text, in which
		// case no other assertion here names where it stopped; merged because two
		// separate lines each said "not at THIS address" and a run stuck anywhere
		// else collected a green tick from both.
		check(result.bannerEntries > 0 &&
		      result.pcLater != g_haltFlashGate && result.pcLater != g_haltModelByte,
		      "execution entered the banner function 0x3001B7FC AND the run ended at "
		      "neither hard halt (0x3001BB4C flash gate, 0x3001B86C model byte)");

		// --------------------------------------------- task INT-3, the scheduler drive
		//
		// THE CARDINALITY IS ASSERTED FIRST AND IT IS NOT REDUNDANT. The cycle
		// property below is quantified over the count, and a property over an
		// empty set is true without discriminating anything, so the count is
		// held to a number written here rather than read from the object the
		// loop bound already came from.
		//
		// WHAT THESE TWO DO NOT ESTABLISH. Not that any DSP ran the program it
		// was given, not that the firmware received the word it polls for, and
		// not that the boot left the loop at 0x300505d4..0x300505e0. A counter
		// above zero says the scheduler reached the DSP phase and says nothing
		// about what was executed there. The counter itself is dsp56kEmu's and
		// is written by the just-in-time backend alone, which is why
		// Scheduler::create refusing an interpreter build is what makes it
		// readable at all.
		check(result.dspCount == g_expectedDspCount,
		      "the Board owns " + std::to_string(g_expectedDspCount) +
		      " DSP slots; counted " + std::to_string(result.dspCount));

		check(everyDspRan(result),
		      "every DSP the Board owns executed at least one cycle, so the "
		      "scheduler turned them rather than leaving them bootstrapped and "
		      "still; counted " + dspCycleList(result));

		// ------------------------------------ task INT-7, the Board's own core runs
		//
		// THE THREE CLAUSES ARE NOT ONE CLAUSE SAID THREE WAYS. A core that is
		// halted returns zero cycles, so the first two would collapse into each
		// other if halt were the only way to reach zero -- but a core with a
		// zero budget also returns zero while never halting, and a running core
		// whose timers were never advanced still leaves TCN2 at zero. Each
		// clause therefore names a different way the drive can be absent.
		check(result.mcuCycles > 0,
		      "Board::runMcu executed a non-zero number of cycles on the Board's "
		      "own core, so the core the Scheduler drives is the one the firmware "
		      "runs on; counted " + std::to_string(result.mcuCycles) + " cycles");

		check(!result.haltedAtBanner,
		      "Board::mcuHalted() was FALSE while the firmware ran, sampled at the "
		      "first iteration that observed display content rather than after the "
		      "run ended");

		check(result.tcn2Latched > 0,
		      "the SIM's TCN2 at MBAR+$18C was observed non-zero AT SOME POINT during "
		      "the run, so the general-purpose timer received the cycles Board::runMcu "
		      "actually executed; latched " + std::to_string(result.tcn2Latched) +
		      ", terminal read " + std::to_string(result.tcn2) +
		      " (a terminal read alone proves nothing: TMR[FRR] is set, so the counter "
		      "restarts at zero on every reference match and zero is a legal healthy "
		      "sample)");

		return g_failures == 0;
	});

	reportSuppressedLogLines();

	std::cout << g2::test::summaryLine(counters) << std::endl;

	// A run that executed no gated test reports NOT VERIFIED and must not be
	// read as a pass, but it must not fail the suite either: an artifact-less
	// machine is a legitimate configuration. A run that DID execute and failed
	// is a failure.
	return g2::test::gatedExitCode(counters);
}
