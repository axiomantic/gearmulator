// A real `.pch2` delivered to booted, running firmware.
// Tier T1: it needs the Clavia-derived artifacts and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// t0_usb_ingress_byte proves a patch byte reaches the device register file the
// firmware reads, on a Board with no firmware in it. t1_boot and t1_egress boot
// the firmware and load no patch. So `--impulse`'s `arrival=-1` is a statement
// about an unpatched machine and says nothing about a patched one. This file is
// the join.
//
// The two questions it makes answerable.
//
//   1. Does MCU routine 0x30032A82 fire on a real patch load? Nothing in the
//      emulator names that address, so the only way to reach it is to let the
//      firmware run the path.
//
//   2. Audio. `arrival=-1` on an unpatched machine is not a claim about a
//      patched one.
//
// The instrument needs no production change. The MCF5307 core fetches every
// instruction word through the bus read callback -- `cpu.nim`'s
// `ctx.readFn(ctx.user, ctx.pc, 2, addr status)` -- and Board::onRead routes
// that to the MemoryMap, which routes it to the BusTarget attached at
// Region::Sdram. That target is this file's Ram. So a counter on 16-bit reads
// at one SDRAM offset is an instruction-fetch counter for that address, built
// entirely inside the test.
//
// The instrument's controls, both from the same population. A zero from a
// counter that never fires is not a measurement.
//
//   known positive   the most-visited address of the window: the argmax of a
//                    histogram this file keeps over every 16-bit read in the
//                    window. It is not named anywhere in this file; it is
//                    whatever address this run read most, counted by the same
//                    counter as every probe.
//   known negative   an address inside the vector TABLE. Vectors are read as
//                    32-bit longwords and never fetched as instruction words,
//                    so the same counter must read 0 there.
//
// The argmax, and not the address the core happened to sit at when the window
// opened: one instant of the machine is not a measure of how hard the counter
// can fire, and the window can open inside an interrupt handler at an address
// the window then reads once.
//
// The argmax is not certified to be an instruction -- it is the most-read
// 16-bit location, and a hot 16-bit data read wins it just as legitimately. Its
// job is to answer how large a count this counter can produce on this arm, so
// that a zero elsewhere has a scale to be read against.
//
// Because the known positive is the maximum, `knownPositive >= hitsTarget`
// holds by construction for every probe in this file. No such comparison is
// asserted below, and none would mean anything if it were.
//
// And the control that makes the answer an answer: the whole run happens twice
// on the same code path, once without a patch and once with one. A probe count
// that is non-zero in both runs says the routine fires anyway; non-zero only in
// the patched run is the patch load reaching it.
//
// Every verdict is an observable and not an assert(). A release build deletes
// assert(), so a predicate spelled as one is a predicate the shipped build does
// not have. Nothing below calls assert() and nothing catches an exception.
#include "gatedFixture.h"

#include "../board.h"
#include "../crc16.h"
#include "../frame.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"
#include "../transportHub.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "dsp56kBase/logging.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
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

	std::string hex32(const uint32_t _value)
	{
		static const char* const digits = "0123456789ABCDEF";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// The underruns are real and expected in the boot regime, because nothing
	// drains the ESAIs until the codec queues arrive. This hides the repetition
	// and nothing else. Set G2_LOG_ESAI_UNDERRUN to install no filter at all.
	const char* const g_underrunMessage = "ESAI transmit underrun";

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

	// ---------------------------------------------------- the machine placement

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase = 0x10000000u;

	constexpr uint32_t g_cs2Base   = 0x12000000u;
	constexpr uint32_t g_cs2Size   = 0x00800000u;
	constexpr uint32_t g_cs3Size   = 0x00010000u;
	constexpr uint32_t g_cs0Base   = 0x00000000u;
	constexpr uint32_t g_cs0Size   = 0x00020000u;
	constexpr uint32_t g_cs4Base   = 0x14000000u;
	constexpr uint32_t g_cs4Size   = 0x00010000u;
	constexpr uint32_t g_sdramSize = 0x00800000u;
	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;

	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;

	constexpr uint32_t g_bootQuantumBound   = 500000u;
	constexpr uint32_t g_bannerSettleQuanta = 20000u;

	// ------------------------------------------------- the CS3 peek instrument
	//
	// t0_usb_ingress_byte's instrument: the part's own peek command (0xD2)
	// issued at the CS3 command port and read back at the CS3 data port, with
	// the peek target selected by the endpoint-configuration command (0x20 +
	// the endpoint's configuration slot, which is not its number). It reads the
	// head byte of the OUT buffer the given endpoint delivers into, and answers
	// the model's benign 0x00 when that buffer holds nothing.
	//
	// t0_usb_ingress_byte runs it on a Board with no firmware in it. On a
	// booted machine the same reading answers a different question: whether the
	// firmware ever took the packet out.
	constexpr uint32_t g_dataPort    = g2::g_cs3Base + 0x00u;
	constexpr uint32_t g_commandPort = g2::g_cs3Base + 0x10u;

	constexpr int g_byteWidth = 1;

	constexpr uint8_t g_endpointConfigBase = 0x20u;
	constexpr uint8_t g_peekCommand        = 0xD2u;

	// The configuration slot order, which is not the endpoint number. ISP1362
	// Rev. 06 section 15.1.1 orders the sixteen `0x20`..`0x2F` slots control
	// OUT, control IN, then endpoints 1 to 14, so endpoint 0 is slot 0,
	// endpoint 1 is slot 2, endpoint 2 is slot 3 and endpoint 3 is slot 4. The
	// peek command answers about the buffer the last configuration command
	// selected, and that operand is one of these. Passing an endpoint number
	// straight through selects a buffer one place low for every endpoint above
	// 0, the read still succeeds, and the wrong answer arrives looking exactly
	// like the right one.
	constexpr int g_bufferSlotOfEndpoint[4] = {0, 2, 3, 4};

	int bufferSlotOfEndpoint(const int _endpoint)
	{
		if(_endpoint < 0 || _endpoint >= 4)
			return -1;
		return g_bufferSlotOfEndpoint[_endpoint];
	}

	// The synthetic object the known positive delivers. Its type byte is not
	// 0x00, so a reading of it cannot be confused with the benign answer; it is
	// also not 0x21, which is the type byte of the first object in every file
	// of the corpus, so a reading of it cannot be confused with the patch's
	// either. Its whole framed length is 3 + 15 = 18 bytes, well inside the
	// 64-byte capacity the model gives the protocol endpoint.
	constexpr uint8_t g_probeObjectType   = 0x4Au;
	constexpr size_t  g_probeObjectLength = 15u;

	uint8_t peekHeadByte(g2::Board& _board, const int _endpoint)
	{
		const int slot = bufferSlotOfEndpoint(_endpoint);

		if(slot < 0)
			return 0x00u;

		mcf5307_bus_status status = MCF5307_BUS_OK;

		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_endpointConfigBase) + uint32_t(slot), &status);
		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_peekCommand), &status);

		const uint32_t value = g2::Board::onRead(&_board, g_dataPort, g_byteWidth, &status);

		return uint8_t(value & 0xffu);
	}

	// The largest framed object in a `.pch2`, counting its 3-byte header, and
	// how many objects it holds. Both are computed from the file this run
	// loaded and neither is written here as a literal, so a different patch
	// reports its own figures.
	//
	// It also counts what the split costs and what the corpus cannot answer.
	// `_packets` is how many max-packet-size packets the whole container takes,
	// and `_exactMultiples` is how many of its framed objects have a length
	// that is an exact multiple of that packet size. The second figure decides
	// whether this file can say anything about the trailing zero-length packet
	// at all: the convention only ever applies to an exact multiple, so a
	// corpus containing none of them cannot exercise it, and saying so as a
	// computed number is the difference between a measurement and an
	// assumption. Both are computed from the file this run loaded.
	void measureObjects(const std::vector<uint8_t>& _file, unsigned& _count, size_t& _largest,
		uint8_t& _firstType, size_t _packetSize, unsigned& _packets, unsigned& _exactMultiples)
	{
		_count           = 0;
		_largest         = 0;
		_firstType       = 0;
		_packets         = 0;
		_exactMultiples  = 0;

		size_t at = 0;
		while(at < _file.size() && _file[at] != 0)
			++at;

		if(at >= _file.size())
			return;

		at += 1 + 2;                       // the NUL, then the 2-byte binary header
		const size_t end = _file.size() >= 2 ? _file.size() - 2 : 0;   // the stored CRC

		while(at + 3 <= end)
		{
			const size_t length = (size_t(_file[at + 1]) << 8) | size_t(_file[at + 2]);
			const size_t framed = 3 + length;

			if(at + framed > end)
				return;

			if(framed > _largest)
				_largest = framed;

			if(_count == 0)
				_firstType = _file[at];

			if(_packetSize != 0)
			{
				// A frame of N bytes costs ceil(N / packetSize) packets, and a
				// frame of zero bytes still costs one -- it is an empty packet,
				// not an absent one.
				_packets += unsigned(framed == 0 ? 1
				                   : (framed + _packetSize - 1) / _packetSize);

				if(framed != 0 && (framed % _packetSize) == 0)
					++_exactMultiples;
			}

			++_count;
			at += framed;
		}
	}

	// ------------------------------------------------------------ the probe set
	//
	// Two halves of one question, probed together because separately neither is
	// decisive. The prior pass measured the whole restart chain at zero on both
	// arms with a live known positive, so control never reaches FUN_3004c10c --
	// yet the packet leaves the endpoint buffer. Something drains it.
	//
	// Both halves were read out of CODE_30000400.bin with m68k-elf-objdump, and
	// each address below is an instruction boundary in that disassembly.
	//
	// HALF 1, THE DISPATCHER. FUN_3004c10c has exactly one longword reference in
	// the whole image, at 0x3001357E, inside the thin wrapper 0x30013568 which
	// reads a word length at msg+2 and a pointer at msg+4 and calls the worker.
	// That wrapper has exactly one caller, 0x3001213A, which is one arm of the
	// switch at 0x30012050:
	//
	//     moveb  (a0),d0        ; d0 = msg[0], the event code
	//     subql  #1,d0          ; index = code - 1
	//     moveq  #68,d1         ; codes 1..69 are in range
	//     cmpl   d0,d1
	//     bcsw   default
	//     movew  (0x30012072,pc,d0.l*2),d0
	//     jmp    (0x30012072,pc,d0.l)
	//
	// Decoding that 69-entry table, the entry at 0x300120BA holds 0x00C8 and so
	// sends index 36 -- code 0x25 -- to 0x3001213A. Code 0x25 is the ONLY code in
	// the table routed there. So the dispatch is not inferred from intent: it is
	// read off the table, and probing the dispatcher entry, the indexed jump and
	// that one arm separates "the 0x25 event is never raised" from "it is raised
	// and the table sends it elsewhere".
	//
	// HALF 2, THE CONSUMER. The 0x25 message is built by 0x30055DD0, which is the
	// only writer of a literal 0x25 into a message's byte 0 that also fills the
	// (2)=length, (4)=pointer layout the wrapper reads back. It enqueues onto
	// 0x302A26F8, one of the four queues the event loop at 0x30004674 drains
	// before calling the dispatcher. 0x30055DD0 has exactly one caller: the drain
	// at 0x300556FE, which reads the endpoint through 0x300544FA, accumulates
	// into a descriptor at [0x30280CF8], and raises 0x25 only when the fill count
	// reaches the length it parsed from the first two bytes.
	//
	// 0x300544FA writes 0x10+ep to 0x13000010 (ISP1181 command port, "read buffer
	// endpoint n") and streams from 0x13000000; 0x300545CC writes 0x70+ep ("clear
	// buffer"). Both are called from 0x300556FE with `pea 0x4` -- ENDPOINT INDEX
	// 4. The emulator's BoardConfig::usbProtocolEndpoint is 3. That disagreement
	// is why the consumer half is probed rather than argued about.
	struct ProbePoint
	{
		uint32_t    addr;
		const char* what;
	};

	constexpr ProbePoint g_probes[] = {
		// the consumer, in call order
		{0x30055D5Au, "drain call from the EP interrupt path"},
		{0x300556FEu, "DRAIN entry FUN_300556fe"},
		{0x30055728u, "read endpoint buffer (ep index 4)"},
		{0x30055740u, "parse length from first two bytes"},
		{0x30055776u, "MESSAGE COMPLETE (fill == length)"},
		{0x30055790u, "call the 0x25 raiser"},
		{0x30055DD0u, "RAISER entry FUN_30055dd0"},
		{0x30055DFEu, "write code 0x25 into msg[0]"},
		// the dispatcher, in call order
		{0x30012050u, "DISPATCHER entry FUN_30012050"},
		{0x30012068u, "indexed jump (code was in range 1..69)"},
		{0x3001213Au, "the 0x25 ARM (sole route in the table)"},
		{0x30013568u, "wrapper FUN_30013568 entry"},
		{0x3004C10Cu, "worker  FUN_3004c10c entry"},
		// The event loop that is the dispatcher's ONLY caller, in call order.
		// Derived, not guessed: a longword scan of the image for 0x30012050 finds
		// exactly one occurrence, 0x300046BC -- the operand of `jsr 0x30012050` at
		// 0x300046BA, inside FUN_30004674. The same scan for 0x30004674 finds
		// exactly one, 0x3001B54C, the operand of `jsr 0x30004674` at 0x3001B54A.
		// The loop body disassembles as four `jsr %a2@` with %a2 loaded by
		// `lea 0x30003914,%a2` at 0x3000467A -- so 0x30003914 is the dequeue's own
		// entry, not a pointer cell -- pushing the four queue handles 0x302A2700,
		// 0x302A26EC, 0x302A26F8 and 0x302A26F0 in that order, each short-circuited
		// by `bnes 0x300046B8`. 0x30004674 is the prologue and runs once; the
		// repeat target is 0x30004680, so that address counts iterations.
		{0x3001B54Au, "sole call site of the event loop"},
		{0x30004674u, "EVENT LOOP entry FUN_30004674 (prologue, once)"},
		{0x30004680u, "loop head -- one per iteration"},
		{0x30003914u, "DEQUEUE entry FUN_30003914"},
		{0x300046A2u, "dequeue call for queue 0x302A26F8 (the 0x25 queue)"},
		{0x300046B8u, "SOME queue returned non-empty -> dispatch"},
		{0x300046CCu, "all four queues empty -> idle branch"},
		{0x300047D6u, "queue construction FUN_300047d6 (creates all seven)"},
		// The init function that contains BOTH the queue construction and the loop
		// launch, and the ladder that bisects it. Derived: the sole longword ref to
		// 0x300047D6 is 0x3001B104 (operand of `jsr 0x300047d6` at 0x3001B102) and
		// the sole ref to 0x30004674 is 0x3001B54C (operand of the jsr at 0x3001B54A).
		// Scanning back, the nearest preceding rts is 0x3001B098 and the next
		// linkw is `4e56 fffc` at 0x3001B09C; the nearest following unlk/rts is
		// 0x3001B556/0x3001B558. So 0x3001B102 and 0x3001B54A sit in ONE function,
		// 0x3001B09C, with no return between them -- and that function has exactly
		// one caller, the jsr whose operand is at 0x3001BC10. The queue
		// construction ran once and the loop launch never did, so the run stops
		// somewhere in this span. Each rung below is a `jsr` site read straight off
		// the disassembly, in address order.
		{0x3001B09Cu, "INIT entry FUN_3001b09c"},
		{0x3001B102u, "rung: jsr queue construction (anchor, must be 1)"},
		{0x3001B158u, "rung: jsr 0x30029e76"},
		{0x3001B1C8u, "rung: jsr (a3) in the table-walk block"},
		{0x3001B25Eu, "rung: jsr 0x30057e4e"},
		{0x3001B2BCu, "rung: jsr (a4) in the alternate block"},
		{0x3001B360u, "rung: jsr 0x300383f0 (both branches rejoin here)"},
		{0x3001B384u, "rung: jsr 0x30012018"},
		{0x3001B3CCu, "rung: jsr 0x3001c660"},
		{0x3001B3FEu, "rung: jsr 0x3001b55a"},
		{0x3001B412u, "rung: jsr 0x30055bb0"},
		{0x3001B480u, "rung: jsr 0x30055f28"},
		{0x3001B4C2u, "rung: jsr 0x3001b674"},
		{0x3001B526u, "rung: jsr 0x3005e68c"},
		{0x3001B544u, "rung: jsr 0x3005e568 (the rung before the loop launch)"},
		// ---- the rung that never returns, opened up ------------------------
		// FUN_30055F28 is reached by `jsr 0x30055f28` at 0x3001B480. Its three
		// arguments are pushed at 0x3001B474..0x3001B47A in the order
		//   clrl -(sp) ; pea 0x3e8 ; pea 0x30132ac0
		// so the LAST push is the buffer pointer and it lands at fp@(8). The
		// callee reads `movel %fp@(8),%d0` (0x30055F34) and stores d0 into
		// 0x302A0DA8 at 0x30055F48, `movew %fp@(14),%d5` (=0x03E8=1000), and
		// `moveal %fp@(16),%a2` (=0, the null callback that makes both
		// `tstl %a2` guards fall through). Verified off the disassembly of the
		// call site, not inherited: the push order decides which cell is the
		// pointer, and reading it backwards would have named the wrong address.
		//
		// The straight line then is: memset the 0x202-byte buffer, seed four
		// state words, poke the DSP host port, write 100 into the buffer's first
		// word, and spin on that word until it reads zero.
		{0x30055F28u, "FUN_30055f28 entry (the rung that never returns)"},
		{0x30055F56u, "jsr 0x300d8ce0 -- the pre-spin call"},
		// FUN_300D8CE0 is NOT an arming call. Disassembled in full it is a
		// memset: it fills a1 (=arg1, the buffer) with byte arg2 (=0) for arg3
		// (=0x202) bytes, longword-at-a-time with a byte tail, and returns arg1
		// in d0. It has 62 longword references across the image, which is what a
		// libc-shaped routine looks like. Probed anyway, entry AND exit, because
		// "entered and never left" is a finding no amount of reading can rule out.
		{0x300D8CE0u, "memset FUN_300d8ce0 ENTRY"},
		{0x300D8D42u, "memset EXIT join (sole path to the epilogue)"},
		{0x300D8D4Au, "memset rts"},
		// The real arming call. `pea 0x80 ; jsr 0x3005c164` at 0x30055F76.
		// FUN_3005C164 reads the byte at 0x302AA5D4 -- the dspCount byte this
		// file already probes -- subtracts one, uses it to index the pointer
		// table at 0x30116970, writes a longword to that object's +4, spins on
		// bit 7 of its +1, then writes 0xD5 to +1. That is a host-port command
		// write to a DSP. Its entry, its busy-poll and its rts are all probed:
		// if the poll at 0x3005C1B8 is hot the stall is here and not at 0x30055FBA.
		{0x30055F7Au, "jsr 0x3005c164 -- the DSP host-port poke"},
		{0x3005C164u, "FUN_3005c164 ENTRY (DSP host port write)"},
		{0x3005C1B8u, "FUN_3005c164 busy-poll on bit 7 of the DSP object"},
		{0x3005C1D6u, "FUN_3005c164 rts"},
		{0x30055F86u, "STORE 100 into *0x30132AC0 (movew 0x302a0da0,%a0@)"},
		{0x30055FB4u, "spin arm: moveal 0x302a0da8,%a0"},
		// The instruction the spin falls through to. It is the ONLY exit from
		// `bnes 0x30055fba`, so a zero here is the hang stated positively.
		{0x30055FBEu, "SPIN EXIT -- jsr %a3@ (0x30056062); zero = never left the spin"},
		{0x30056062u, "FUN_30056062 entry (the post-spin analysis, a3)"},
		// The other dereference sites of 0x302A0DA8. Derived by scanning the
		// image for the longword 0x302A0DA8: it occurs exactly nine times, at
		// 0x30055F4A, 0x30055F82, 0x30055FB6, 0x30056006, 0x30056042, 0x30056070,
		// 0x300560C6, 0x300561EE and 0x3005620A. Those are OPERAND addresses; the
		// instruction begins two bytes earlier. 0x30055F48 is the store, the
		// other eight are loads, and six of them lie past the spin.
		{0x30056004u, "deref 0x302a0da8 -- clrw %a0@+ after the sample loop"},
		{0x30056040u, "deref 0x302a0da8 -- movew %d5,%a0@"},
		{0x3005606Eu, "deref 0x302a0da8 -- FUN_30056062's own read of the word"},
		{0x300560C4u, "deref 0x302a0da8 -- indexed read at +2"},
		{0x300561ECu, "deref 0x302a0da8 -- late site in FUN_30056062"},
		{0x30056208u, "deref 0x302a0da8 -- last site in FUN_30056062"},
		// ---- the agent that is supposed to zero the word --------------------
		// Found by following the OTHER user of the same 514-byte buffer. The
		// longword 0x30132AC0 occurs twice in the image: at 0x3001B47C (the pea
		// before the jsr to FUN_30055f28) and at 0x3001B280, a
		// `pea 0x30132ac0 ; pea 0x3 ; jsr 0x30056254` on the init's alternate
		// branch. FUN_30056254 caches that pointer in 0x30119E88 (a cell with
		// exactly two references, 0x30056260 and 0x30056582) and then drives
		// MBAR+0x288/0x28C -- MBCR and MBSR of the MCF5307 M-Bus, per mbus.h's
		// own MCF5307UM Table 15-1 constants.
		//
		// 0x30119E88 is read back at 0x30056580, inside FUN_30056442. That
		// function writes 0xCB to MBAR+0x290 (MBDR) at 0x300564CE -- an address
		// register write, `moveb #-53,%a0@`, which is why a search for the
		// absolute-addressed form finds nothing -- and 0xCB is the MAX1039's
		// 0x65 address with the read bit set, exactly as the 2026-08-27 note
		// says. At 0x30056592 it increments histogram bin d3 at ptr+2, and at
		// 0x300565A0 it stores ptr[0]-1 back with a `movew`. THAT is the
		// countdown FUN_30055F28 spins on, and this probe is the measurement
		// that says so rather than the disassembly that predicts it.
		{0x3001B286u, "call site of the M-Bus arming call (alternate branch)"},
		{0x30056254u, "FUN_30056254 entry -- caches 0x30132AC0 into 0x30119E88"},
		{0x30056442u, "M-Bus service FUN_30056442 entry"},
		{0x300564CEu, "moveb #0xCB into MBAR+0x290 (MBDR) -- the MAX1039 read address"},
		{0x30056580u, "M-Bus service reads the shared buffer pointer 0x30119E88"},
		{0x300565A0u, "THE DECREMENT: movew %d0,%a0@ -- ptr[0] = ptr[0] - 1"},
	};

	constexpr size_t g_probeCount = sizeof(g_probes) / sizeof(g_probes[0]);

	// Where the init ladder ends in the probe list. The ladder printout walks a
	// range of the array, so appending probes after it must not silently enrol
	// them as rungs. Named and asserted rather than left as a literal in the loop.
	constexpr size_t g_ladderBegin = 21;
	constexpr size_t g_ladderEnd   = 36;

	static_assert(g_probes[g_ladderBegin].addr == 0x3001B09Cu,
		"the init ladder no longer begins where the printout thinks it does (index 21 is FUN_3001b09c's entry)");
	static_assert(g_probes[g_ladderEnd - 1].addr == 0x3001B544u,
		"the init ladder no longer ends where the printout thinks it does");
	static_assert(g_probeCount > g_ladderEnd,
		"the probes added after the ladder are missing");
	// The first probe past the ladder is the rung's callee. The write-watch
	// reading divides by this probe's count, so the index is asserted and not
	// spelled as a literal at the point of use.
	static_assert(g_probes[g_ladderEnd].addr == 0x30055F28u,
		"probe g_ladderEnd is no longer FUN_30055f28's entry");

	// A zero from an address the counter never sees is not a reading. The counter
	// buckets only EVEN 16-bit reads and only inside the loaded image, so both
	// properties are asserted at COMPILE time against the image extent this run
	// measures at runtime and prints below. static_assert survives a release
	// build, which assert() does not.
	constexpr uint32_t g_imageLo = 0x30000400u;
	constexpr uint32_t g_imageHi = 0x3012A3D0u;

	constexpr bool probesAreEvenAndInImage()
	{
		for(size_t p = 0; p < g_probeCount; ++p)
		{
			if((g_probes[p].addr & 1u) != 0u)
				return false;
			if(g_probes[p].addr < g_imageLo || g_probes[p].addr >= g_imageHi)
				return false;
		}
		return true;
	}

	static_assert(probesAreEvenAndInImage(),
		"a probe address is odd or outside the loaded image; the counter would never see it");

	// The one address the single-line verdict names: the dispatcher's entry.
	// It is g_probes[8]; spelling it again here keeps the verdict readable.
	constexpr uint32_t g_probeTarget = 0x30012050u;

	// The three memory probes, read as single bytes at the close of the window.
	// They are state and not control flow, so a count would say nothing: what
	// decides is the value each one holds once the machine has been given its
	// chance to run.
	// The lifetime counter's own known positive.
	//
	// It USED to be 0x30055FBA, chosen because a prior run measured it as the
	// most-read address in the image. That was the wrong control and it was
	// wrong in the worst direction: 0x30055FBA is the `movew %a0@,%d0` of the
	// two-instruction busy-wait at 0x30055FBA/0x30055FBC. Its millions of hits
	// were not "the busiest code in the firmware", they were "the machine is
	// parked here". A control that is large exactly because the run is stuck
	// certifies the counter and flatters the run at the same time.
	//
	// The replacement is 0x30055F86, `movew 0x302a0da0,%a0@` -- the store that
	// puts 100 into the polled word. It is sound for three reasons that the
	// spin address had none of:
	//  * it is on the straight-line path INTO the routine, before any loop, so
	//    a non-zero count means forward progress rather than repetition;
	//  * it is corroborated by a SECOND and INDEPENDENT mechanism -- if it ran,
	//    the 16-bit word at 0x30132AC0 was set to 100, and this file now reads
	//    that word back and counts every write to it. A fetch counter and a
	//    memory value are different instruments; agreeing, they are evidence;
	//  * its expected count is small and known (once per call of FUN_30055F28),
	//    so an inflated reading is as visible as a zero one.
	// Magnitude is not abandoned: the histogram argmax EXCLUDING the two spin
	// words is reported below as a separate control.
	constexpr uint32_t g_probeLifePositive = 0x30055F86u;

	// The polled word itself, and the buffer it heads. FUN_30055F28 is handed
	// 0x30132AC0 as arg1 (pushed last at 0x3001B47A), memsets 0x202 bytes there
	// through 0x300D8CE0, writes 100 into the first word at 0x30055F86, and then
	// spins at 0x30055FBA until that word reads zero. The address is above the
	// image extent, so it is plain SDRAM and every access to it passes through
	// the Ram below -- which is what makes a write count meaningful.
	constexpr uint32_t g_spinWord       = 0x30132AC0u;
	constexpr uint32_t g_spinBufferSize = 0x202u;

	// What the firmware's OWN writes to that buffer come to, so that "somebody
	// else wrote here" is a subtraction and not an impression. The memset writes
	// g_spinBufferSize bytes; the store at 0x30055F86 writes 2 more. Per call.
	constexpr uint64_t g_spinBufferOwnWritesPerCall = g_spinBufferSize + 2u;
	constexpr uint64_t g_spinWordOwnWritesPerCall   = 2u + 2u;

	// How many writes to the polled word's high byte are recorded value-by-value.
	// A count says how often it was written; the trace says WITH WHAT, which is
	// what separates "the writer disagrees about the value" from "nobody wrote".
	constexpr size_t g_spinTraceMax = 12u;

	// Where the drain keeps the pointer to the receive descriptor it accumulates
	// into. Read at the close of the window and dereferenced, not counted: what
	// decides is the pair of values, not how often they were touched.
	constexpr uint32_t g_rxDescriptorPtr = 0x30280CF8u;

	constexpr uint32_t g_probeDspCount = 0x302AA5D4u;
	constexpr uint32_t g_probeSuspend  = 0x30115574u;
	constexpr uint32_t g_probeDepth    = 0x30142CDEu;

	// The known negative. An address inside the vector table this file writes.
	// Vectors are read as 32-bit longwords, never fetched as instruction words,
	// so the 16-bit counter must read 0 there. It is offset 4 rather than 0 so
	// that it is not the reset vector either.
	constexpr uint32_t g_probeNegative = g_vectorTableBase + 4u;

	// How many quanta the machine runs after the patch is handed over. The
	// findings name a 4000-tick deferred rebuild timer as the scheduler of the
	// routine under test, so a window shorter than that could report a routine
	// that had not been given the chance to run. This is more than ten times it.
	constexpr uint32_t g_observeQuanta = 50000u;

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size)
			: m_bytes(_size, 0u)
			, m_wordHits(_size / 2u, 0u)
		{
		}

		// The counter is a histogram and not a probe list, and that is what
		// makes the known positive a property of the run. A fixed probe list can
		// only answer about addresses this file names; a histogram over every
		// 16-bit read lets the file ask the run which address it read most, and
		// take that answer as its control. One counter per 16-bit word of SDRAM.
		struct Hottest
		{
			uint32_t absolute = 0;
			uint64_t hits     = 0;
		};

		// The count at one absolute SDRAM address. Zero outside the window and
		// zero at an odd offset, which gets no counter.
		uint64_t hitsAt(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0;

			const uint32_t offset = _absolute - g2::g_sdramBase;

			if((offset & 1u) != 0u)
				return 0;

			const size_t index = size_t(offset) >> 1;

			return index < m_wordHits.size() ? uint64_t(m_wordHits[index]) : 0u;
		}

		// The byte this RAM holds at one absolute SDRAM address. It reads the
		// same m_bytes the bus read() serves, so it observes what the machine
		// wrote and not a separate copy. Out of range reads 0, which is the
		// same value an untouched cell holds -- so a caller must know the
		// address is inside SDRAM for a 0 to mean "the machine left it 0".
		// Whether an absolute address falls inside this RAM at all. It is what
		// makes a 0 from byteAt a reading rather than an absence.
		bool covers(const uint32_t _absolute) const
		{
			return _absolute >= g2::g_sdramBase
			    && size_t(_absolute - g2::g_sdramBase) < m_bytes.size();
		}

		uint8_t byteAt(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;

			const size_t index = size_t(_absolute - g2::g_sdramBase);

			return index < m_bytes.size() ? m_bytes[index] : uint8_t(0u);
		}

		// The argmax over every counter: the address this run read more times
		// than any other. Ties go to the lowest address, so the selection is
		// deterministic across runs.
		Hottest hottest() const
		{
			return hottestInRange(g2::g_sdramBase,
				g2::g_sdramBase + uint32_t(m_wordHits.size() << 1));
		}

		// The same argmax over one half-open address range; the caller passes the
		// extent of the firmware image it just read from disk.
		//
		// The unrestricted argmax can land past the end of the loaded image, where
		// the machine reaches it as a 16-bit data read and not as an instruction
		// fetch. The probes are instruction addresses, so such a control belongs to
		// a different population and proves only that the counter runs.
		Hottest hottestInRange(const uint32_t _loAbsolute, const uint32_t _hiAbsolute) const
		{
			Hottest best;

			if(_hiAbsolute <= _loAbsolute || _loAbsolute < g2::g_sdramBase)
				return best;

			const size_t lo = (size_t(_loAbsolute - g2::g_sdramBase) + 1u) >> 1;
			const size_t hi = std::min(size_t(_hiAbsolute - g2::g_sdramBase) >> 1, m_wordHits.size());

			for(size_t index = lo; index < hi; ++index)
			{
				if(uint64_t(m_wordHits[index]) <= best.hits)
					continue;

				best.hits     = uint64_t(m_wordHits[index]);
				best.absolute = g2::g_sdramBase + uint32_t(index << 1);
			}

			return best;
		}

		// The same argmax with one half-open address range struck out. The
		// unrestricted in-image argmax lands on the busy-wait, which is the one
		// address whose size proves nothing: it is large BECAUSE the run is
		// stuck. Excluding the two words of that wait asks the histogram for the
		// busiest address that is not the parking spot.
		Hottest hottestExcluding(const uint32_t _loAbsolute, const uint32_t _hiAbsolute,
			const uint32_t _exLo, const uint32_t _exHi) const
		{
			Hottest best;

			if(_hiAbsolute <= _loAbsolute || _loAbsolute < g2::g_sdramBase)
				return best;

			const size_t lo = (size_t(_loAbsolute - g2::g_sdramBase) + 1u) >> 1;
			const size_t hi = std::min(size_t(_hiAbsolute - g2::g_sdramBase) >> 1, m_wordHits.size());

			for(size_t index = lo; index < hi; ++index)
			{
				const uint32_t absolute = g2::g_sdramBase + uint32_t(index << 1);

				if(absolute >= _exLo && absolute < _exHi)
					continue;

				if(uint64_t(m_wordHits[index]) <= best.hits)
					continue;

				best.hits     = uint64_t(m_wordHits[index]);
				best.absolute = absolute;
			}

			return best;
		}

		// Zeroes every counter, so that a window's counts are the window's and
		// not the boot's.
		void resetProbes()
		{
			std::fill(m_wordHits.begin(), m_wordHits.end(), 0u);
			m_wordFetches  = 0;
			m_oddWordReads = 0;
		}

		// 16-bit reads at an odd offset, which get no histogram counter. Reported
		// rather than dropped: if this were ever large, the histogram would be
		// missing reads it should be seeing.
		uint64_t oddWordReads() const { return m_oddWordReads; }

		// Every 16-bit read in the window, whatever its address. It is what
		// separates "the probed address was not fetched" from "the counter was
		// never reached at all".
		uint64_t wordFetches() const { return m_wordFetches; }

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			// The count is taken on 16-bit reads and on nothing else, because
			// that is the width the core fetches an instruction word at.
			if(_size == 16)
			{
				++m_wordFetches;

				// An odd offset gets no counter. Folding it into the even bucket
				// below it would be worse than dropping it: the histogram picks a
				// maximum, so two addresses sharing one bucket would inflate that
				// bucket and could hand the argmax to an address that was never
				// read that many times. Instruction words are even-aligned, so
				// this drops no fetch, and the count is reported so that claim is
				// checkable rather than assumed.
				if((_offset & 1u) != 0u)
				{
					++m_oddWordReads;
				}
				else
				{
					const size_t index = size_t(_offset) >> 1;
					if(index < m_wordHits.size())
					{
						++m_wordHits[index];
					}
				}

				// The lifetime counters, which resetProbes does NOT clear. The
				// window opens one frame after the hand-over, so a routine that
				// ran inside that frame -- or at boot -- is invisible to the
				// windowed counts above. These four say whether the address was
				// EVER fetched by this run, and separate "it did not run in the
				// window" from "it has never run".
				const uint32_t absolute = g2::g_sdramBase + _offset;

				for(size_t p = 0; p < g_probeCount; ++p)
				{
					if(absolute == g_probes[p].addr)
					{
						++m_life[p];
						break;
					}
				}

				if(absolute == g_probeLifePositive) ++m_lifePositive;
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

				// A content write is one that is not the display clear, which
				// writes 0x20 and only 0x20. Counting 0x20 as content reports a
				// blank screen as a booted machine.
				const int shift = int(8u * (count - 1u - i));
				const uint8_t byte = uint8_t((_value >> shift) & 0xffu);

				if(m_watchLength != 0 && index >= m_watchBase && index < m_watchBase + m_watchLength
					&& byte != 0x20u && byte != 0x00u)
					++m_contentWrites;

				// Write counts for the three memory probes. A byte that reads 0
				// and was never written is indistinguishable from one the
				// machine deliberately set to 0, and the difference is the
				// whole finding: "the latch never fired" versus "nothing ever
				// touched the latch". These counters tell the two apart.
				const uint32_t absolute = g2::g_sdramBase + uint32_t(index);

				if(absolute == g_probeDspCount)     ++m_writesDspCount;
				else if(absolute == g_probeSuspend) ++m_writesSuspend;
				else if(absolute == g_probeDepth)   ++m_writesDepth;

				// The write watch on the polled word and on the buffer it heads.
				// Counted per BYTE, because the poll is a `movew` and a byte
				// write to the low half would leave it non-zero: "written" and
				// "cleared" are not the same event and this separates them.
				if(absolute >= g_spinWord && absolute < g_spinWord + g_spinBufferSize)
				{
					++m_writesSpinBuffer;

					if(absolute == g_spinWord)
					{
						++m_writesSpinHi;
						if(m_spinTrace < g_spinTraceMax)
						{
							m_spinTraceSize[m_spinTrace]  = uint8_t(_size);
							m_spinTraceValue[m_spinTrace] = byte;
							++m_spinTrace;
						}
					}
					else if(absolute == g_spinWord + 1u)
					{
						++m_writesSpinLo;
					}
				}

				m_bytes[index] = byte;
			}
		}

		uint64_t life(const size_t _probe) const
		{
			return _probe < g_probeCount ? m_life[_probe] : 0u;
		}

		uint64_t lifePositive() const { return m_lifePositive; }

		uint64_t writesDspCount() const { return m_writesDspCount; }
		uint64_t writesSpinBuffer() const { return m_writesSpinBuffer; }
		uint64_t writesSpinHi() const     { return m_writesSpinHi; }
		uint64_t writesSpinLo() const     { return m_writesSpinLo; }
		size_t   spinTrace() const        { return m_spinTrace; }
		uint8_t  spinTraceSize(const size_t _i) const  { return _i < m_spinTrace ? m_spinTraceSize[_i] : 0u; }
		uint8_t  spinTraceValue(const size_t _i) const { return _i < m_spinTrace ? m_spinTraceValue[_i] : 0u; }
		uint64_t writesSuspend() const  { return m_writesSuspend; }
		uint64_t writesDepth() const    { return m_writesDepth; }

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

		void watchCells(const uint32_t _offset, const uint32_t _length)
		{
			m_watchBase   = _offset;
			m_watchLength = _length;
		}

		uint64_t contentWrites() const { return m_contentWrites; }

	private:
		std::vector<uint8_t>  m_bytes;
		std::vector<uint32_t> m_wordHits;
		uint64_t             m_oddWordReads  = 0;
		uint64_t             m_wordFetches   = 0;
		uint64_t             m_life[g_probeCount] = {};
		uint64_t             m_lifePositive  = 0;
		uint64_t             m_writesDspCount = 0;
		uint64_t             m_writesSuspend  = 0;
		uint64_t             m_writesDepth    = 0;
		uint64_t             m_writesSpinBuffer = 0;
		uint64_t             m_writesSpinHi     = 0;
		uint64_t             m_writesSpinLo     = 0;
		size_t               m_spinTrace        = 0;
		uint8_t              m_spinTraceSize[g_spinTraceMax]  = {};
		uint8_t              m_spinTraceValue[g_spinTraceMax] = {};
		uint32_t             m_watchBase     = 0;
		uint32_t             m_watchLength   = 0;
		uint64_t             m_contentWrites = 0;
	};

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

		config.memory.cs0   = {g_cs0Base,       g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,   g_cs1Size};
		config.memory.cs2   = {g_cs2Base,       g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base,   g_cs3Size};
		config.memory.cs4   = {g_cs4Base,       g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,   g_cs5Size};
		config.memory.mbar  = {g_mbarBase,      g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		return config;
	}

	// ------------------------------------------------------ the impulse pattern
	//
	// t1_egress's two values: they differ from
	// each other so a chain that carried slot 0 into both slots fails rather
	// than passes, and neither is a power of two.
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	constexpr unsigned g_overrunQuanta = 1024u;

	/* ------------------------------- the arrival instrument's known positive
	 *
	 * Without it, `arrival=-1` is printed by an instrument nothing has ever
	 * shown a frame to, so a chain that carried nothing and an arrival path
	 * that could not report anything produce the same figure.
	 *
	 * The control places a sentinel at the tail position's transmit source and
	 * reads it back out of the codec sink, through the same `pull` and the same
	 * comparator the walk uses. Its sentinel is neither impulse word, so it
	 * cannot be mistaken for the measurement it qualifies, and bit 23 is clear
	 * so fromEsaiFrame's sign extension is the identity on it. */
	constexpr uint32_t g_sinkControlWord     = 0x2B6D51u;
	constexpr int32_t  g_sinkControlExpected = int32_t(g_sinkControlWord);

	static_assert((g_sinkControlWord & 0x800000u) == 0u,
		"the sentinel's sign bit must be clear, or fromEsaiFrame's sign extension moves it");
	static_assert(g_sinkControlExpected != g_impulseLeft && g_sinkControlExpected != g_impulseRight,
		"the control's sentinel must not be either impulse word");

	constexpr uint32_t g_esaiTransmitters  = 6u;
	constexpr unsigned g_sinkControlQuanta = 64u;
	constexpr dsp56k::TWord g_dmaTxChannel = 4u;

	/* The tail is found and not typed. The chain adapter's position and the
	 * hardware port are not the same number: dspSet.cpp binds
	 * audioTxCallback(position) to peripherals(portOfPosition[position]), and
	 * portOfPosition comes from the nine-entry table the firmware builds at
	 * 0x30116970. Entry i holds the CS1 address of the port at chain position
	 * i, and A3..A10 are eight active-low one-cold selects, so the port number
	 * is the index of the single line pulled down. */
	unsigned portOfChainPosition(g2::Board& _board, const unsigned _wanted, const unsigned _count)
	{
		constexpr uint32_t g_portTableBase = 0x30116970u;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t entry =
				g2::Board::onRead(&_board, g_portTableBase + position * 4u, 4, &status);

			const uint8_t selects = uint8_t((entry >> 3) & 0xffu);
			const uint8_t low     = uint8_t(~selects);

			if(low == 0u || (low & uint8_t(low - 1u)) != 0u)
				continue;

			unsigned port = 0;
			for(uint8_t bit = low; bit > 1u; bit >>= 1)
				++port;

			if(position == _wanted && port < _count)
				return port;
		}

		return _count;
	}

	// A one-object `.pch2` whose single object is small enough for the endpoint
	// the model gives it. It carries no Clavia byte -- every byte of it is this
	// process's own.
	std::vector<uint8_t> buildProbeContainer()
	{
		std::vector<uint8_t> file;

		const char* const ascii = "Version=Nord Modular G2 File Format 1\n";
		for(const char* p = ascii; *p != 0; ++p)
			file.push_back(static_cast<uint8_t>(*p));
		file.push_back(0);

		const size_t binaryHeader = file.size();

		file.push_back(0x17);
		file.push_back(0x00);

		file.push_back(g_probeObjectType);
		file.push_back(uint8_t((g_probeObjectLength >> 8) & 0xffu));
		file.push_back(uint8_t(g_probeObjectLength & 0xffu));

		for(size_t i = 0; i < g_probeObjectLength; ++i)
			file.push_back(uint8_t(31u + i * 7u + 1u));

		file.push_back(0);
		file.push_back(0);

		const uint16_t crc = g2::crc16File(file.data(), file.size(), binaryHeader);
		g2::crc16Store(file.data() + file.size() - 2, crc);

		return file;
	}

	struct RunResult
	{
		bool     patchOffered   = false;
		size_t   patchBytes     = 0;
		unsigned patchObjects   = 0;
		g2::Pch2LoadResult loadResult = g2::Pch2LoadResult::Loaded;
		bool     loadReturned   = false;

		bool     booted         = false;
		bool     programsLanded = false;
		bool     halted         = false;
		bool     faulted        = false;
		uint32_t bootQuanta     = 0;
		unsigned dspCount       = 0;
		unsigned hopFrames      = 0;
		unsigned lookaheadFrames = 0;

		uint32_t windowPc       = 0;   // where the core sat when the window opened
		uint64_t windowPcHits   = 0;   // and how often the window read that address
		uint64_t windowFetches  = 0;
		uint64_t oddWordReads   = 0;

		// The known positive of this arm: the argmax of this arm's own histogram.
		// Each arm selects its own, because each arm is a separate run of the
		// machine and one arm's figure says nothing about the other.
		uint32_t knownPositiveAddr = 0;
		uint64_t knownPositiveHits = 0;

		// The population-matched known positive: the same argmax restricted to
		// the bytes the firmware image supplied. The probes are instruction
		// addresses inside that image, so this is the control that shares their
		// population.
		uint32_t imageBase           = 0;
		uint32_t imageEnd            = 0;
		uint32_t codeKnownPositiveAddr = 0;
		uint64_t codeKnownPositiveHits = 0;

		// One address supplied by the caller, measured on this arm. It is how the
		// two arms are compared at a common address: each arm's argmax is a
		// different address, so their counts are not comparable to each other.
		uint32_t crossAddr = 0;
		uint64_t crossHits = 0;

		// The CS3 readings, in the order they are taken.
		// The Board's own account of what the device did with the bytes, read
		// off the Board that produced it. It is per-Board and needs no
		// subtraction: a file-scope diagnostic would pool this arm with the
		// control arm and leave every reader to undo the pooling by hand.
		g2::Board::UsbTransportStats usb;

		uint8_t  peekAfterHandover = 0;  // one quantum after pch2Load returned
		uint8_t  peekAfterWindow   = 0;  // g_observeQuanta later
		uint8_t  peekProbeObject   = 0;  // after a small object goes the same way
		bool     probeLoaded       = false;

		uint64_t hitsKnownPositive = 0;
		uint64_t hitsKnownNegative = 0;
		uint64_t hitsTarget        = 0;
		uint64_t hits[g_probeCount] = {};

		// The three memory probes, and whether each address is inside the RAM
		// this run gave the machine. Without that flag a 0 cannot be told from
		// an address the model never covered.
		uint64_t life[g_probeCount] = {};

		// The receive descriptor the drain accumulates into, dereferenced through
		// the pointer the firmware keeps at 0x30280CF8. +2 is the length the drain
		// parsed out of the message's first two bytes; +6 is how many bytes it has
		// actually accumulated. The drain raises 0x25 only when they are equal, so
		// when they are not, these two numbers say by how much and in which
		// direction it fell short.
		uint32_t rxDescriptor = 0;
		uint32_t rxLength     = 0;
		uint32_t rxFill       = 0;
		bool     rxInRam      = false;
		uint64_t lifePositive = 0;

		uint64_t writesDspCount = 0;
		uint64_t writesSuspend  = 0;
		uint64_t writesDepth    = 0;

		// The write watch on the polled word.
		bool     spinInRam         = false;
		uint64_t writesSpinBuffer  = 0;
		uint64_t writesSpinHi      = 0;
		uint64_t writesSpinLo      = 0;
		uint32_t spinWordValue     = 0;
		size_t   spinTraceCount    = 0;
		uint8_t  spinTraceSize[g_spinTraceMax]  = {};
		uint8_t  spinTraceValue[g_spinTraceMax] = {};

		// The argmax with the two words of the busy-wait struck out.
		uint32_t offSpinPositiveAddr = 0;
		uint64_t offSpinPositiveHits = 0;

		// How far the firmware drove the M-Bus module the MAX1039 hangs off.
		g2::MBus::Traffic mbus;

		uint8_t byteDspCount = 0;
		uint8_t byteSuspend  = 0;
		uint8_t byteDepth    = 0;
		bool    bytesInRam   = false;

		size_t   primedPulled   = 0;
		unsigned walkQuanta     = 0;
		int      arrival        = -1;
		bool     arrivalExact   = false;
		uint64_t nonZeroFrames  = 0;
		int32_t  firstNonZeroL  = 0;
		int32_t  firstNonZeroR  = 0;

		// The arrival instrument's known positive, run after the walk on the
		// same machine so it cannot move the measurement it qualifies.
		unsigned sinkControlPort    = 0;
		bool     sinkControlPortFound = false;
		unsigned sinkControlQuanta  = 0;
		int      sinkControlArrival = -1;
		bool     sinkControlExact   = false;
		int32_t  sinkControlL       = 0;
		int32_t  sinkControlR       = 0;
	};

	// Boots one machine, optionally hands it `_patch`, opens the observation
	// window and then walks the codec. Returns false only when the machine could
	// not be placed at all; a machine that ran and moved nothing returns true
	// with a result that says so, because "the machine is silent" is a
	// measurement and must reach the assertions rather than a bail-out.
	// `_crossAddress` is 0 on the first arm and the first arm's known positive
	// on the second, so the two arms can be compared at one common address.
	bool runOnce(const std::string& _directory, const std::vector<uint8_t>& _patch,
		const bool _deliver, const uint32_t _crossAddress, RunResult& _r)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig());
		Ram ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit the configured SDRAM window" << std::endl;
			return false;
		}

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

		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::SerialExecutor          executor;
		g2::Status                  schedulerStatus{};
		const g2::Scheduler::Config config;

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		_r.dspCount        = board.dspSet().dspCount();
		_r.hopFrames       = config.hopFrames;
		_r.lookaheadFrames = config.lookaheadFrames;

		// ---------------------------------------------------------- the boot
		uint32_t settle = 0;

		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			_r.bootQuanta = i + 1;

			scheduler->runFrames(1);

			if(board.mcuHalted())
				break;

			if(ram.contentWrites() == 0)
				continue;

			if(++settle < g_bannerSettleQuanta)
				continue;

			_r.booted = true;

			unsigned landed = 0;
			for(unsigned d = 0; d < _r.dspCount; ++d)
			{
				const bool* const flag = board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}

			if(landed == _r.dspCount)
			{
				_r.programsLanded = true;
				break;
			}
		}

		// ------------------------------------------------- the patch hand-over
		//
		// The client is attached to the Board's own hub and to nothing else, so
		// what leaves it is drained by Board::pumpTransport at the next quantum
		// boundary and handed to the device with isp1181_rx. There is no second
		// path: this is the same `pch2Load` the plugin calls.
		{
			// The two inbox sizes bound the device-to-plugin direction only; the
			// bound on what may be originated is the hub's and the hub reports
			// it. 4096 clears the largest object in the corpus, measured at
			// 2492 bytes.
			g2::InternalClient client(board.transport(), 4096, 4);

			if(_deliver)
			{
				_r.patchOffered = true;
				_r.patchBytes   = _patch.size();
				_r.loadResult   = g2::pch2Load(_patch.data(), _patch.size(), client);
				_r.loadReturned = true;
			}

			// The pump, with no MCU cycle after it, and that ordering is the
			// measurement. Board::pumpTransport drains what pch2Load put in the
			// hub and hands each frame to the device with isp1181_rx; it is
			// public because tickSofIfDue calls it, and calling it directly
			// delivers the frame without letting the core run.
			//
			// Running one quantum first cannot tell two worlds apart: the pump
			// and the firmware's service both happen inside that quantum, so
			// "the packet arrived and the firmware drained it" and "the packet
			// never arrived" both leave the buffer empty and both read 0x00.
			// This reading is the arrival; the reading after the window is the
			// drain.
			board.pumpTransport();
			_r.peekAfterHandover = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// The window below expects a machine that has serviced the packet.
			scheduler->runFrames(1);

			// ------------------------------------------------ the window opens
			//
			// The address the core is sitting at at this instant. Recorded and
			// printed, but not the known positive: the two arms open their
			// windows in different places.
			_r.windowPc = board.mcuReg(g_regPc);

			ram.resetProbes();

			for(uint32_t i = 0; i < g_observeQuanta; ++i)
			{
				scheduler->runFrames(1);
			}

			const Ram::Hottest hottest = ram.hottest();

			_r.knownPositiveAddr = hottest.absolute;
			_r.knownPositiveHits = hottest.hits;

			_r.imageBase = g_entryPc;
			_r.imageEnd  = g_entryPc + uint32_t(code.size());

			const Ram::Hottest inImage = ram.hottestInRange(_r.imageBase, _r.imageEnd);

			_r.codeKnownPositiveAddr = inImage.absolute;
			_r.codeKnownPositiveHits = inImage.hits;

			_r.crossAddr = _crossAddress;
			_r.crossHits = _crossAddress != 0 ? ram.hitsAt(_crossAddress) : 0u;

			_r.windowFetches     = ram.wordFetches();
			_r.oddWordReads      = ram.oddWordReads();
			_r.windowPcHits      = ram.hitsAt(_r.windowPc);
			_r.hitsKnownPositive = hottest.hits;
			_r.hitsKnownNegative = ram.hitsAt(g_probeNegative);
			_r.hitsTarget        = ram.hitsAt(g_probeTarget);

			for(size_t p = 0; p < g_probeCount; ++p)
				_r.hits[p] = ram.hitsAt(g_probes[p].addr);

			_r.bytesInRam   = ram.covers(g_probeDspCount)
			               && ram.covers(g_probeSuspend)
			               && ram.covers(g_probeDepth);
			_r.byteDspCount = ram.byteAt(g_probeDspCount);
			_r.byteSuspend  = ram.byteAt(g_probeSuspend);
			_r.byteDepth    = ram.byteAt(g_probeDepth);

			{
				const auto long32 = [&ram](const uint32_t _a) -> uint32_t
				{
					return (uint32_t(ram.byteAt(_a))     << 24)
					     | (uint32_t(ram.byteAt(_a + 1)) << 16)
					     | (uint32_t(ram.byteAt(_a + 2)) <<  8)
					     |  uint32_t(ram.byteAt(_a + 3));
				};

				_r.rxInRam = ram.covers(g_rxDescriptorPtr) && ram.covers(g_rxDescriptorPtr + 3);

				if(_r.rxInRam)
				{
					_r.rxDescriptor = long32(g_rxDescriptorPtr);

					if(ram.covers(_r.rxDescriptor) && ram.covers(_r.rxDescriptor + 9))
					{
						_r.rxLength = long32(_r.rxDescriptor + 2);
						_r.rxFill   = long32(_r.rxDescriptor + 6);
					}
					else
					{
						_r.rxInRam = false;
					}
				}
			}

			for(size_t p = 0; p < g_probeCount; ++p)
				_r.life[p] = ram.life(p);

			_r.lifePositive = ram.lifePositive();

			_r.writesDspCount = ram.writesDspCount();
			_r.writesSuspend  = ram.writesSuspend();
			_r.writesDepth    = ram.writesDepth();

			_r.spinInRam        = ram.covers(g_spinWord)
			                   && ram.covers(g_spinWord + g_spinBufferSize - 1u);
			_r.writesSpinBuffer = ram.writesSpinBuffer();
			_r.writesSpinHi     = ram.writesSpinHi();
			_r.writesSpinLo     = ram.writesSpinLo();
			_r.spinWordValue    = _r.spinInRam
			                    ? (uint32_t(ram.byteAt(g_spinWord)) << 8) | ram.byteAt(g_spinWord + 1u)
			                    : 0u;
			_r.spinTraceCount   = ram.spinTrace();

			for(size_t i = 0; i < _r.spinTraceCount && i < g_spinTraceMax; ++i)
			{
				_r.spinTraceSize[i]  = ram.spinTraceSize(i);
				_r.spinTraceValue[i] = ram.spinTraceValue(i);
			}

			{
				const Ram::Hottest offSpin = ram.hottestExcluding(
					_r.imageBase, _r.imageEnd, 0x30055FBAu, 0x30055FBEu);

				_r.offSpinPositiveAddr = offSpin.absolute;
				_r.offSpinPositiveHits = offSpin.hits;
			}

			_r.mbus = board.mbus().traffic();

			// The second reading. If the first was non-zero and this one is
			// 0x00, the firmware took the packet out during the window; if both
			// carry the same byte, it never did.
			_r.peekAfterWindow = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// ------------------------------------------- the wire's known positive
			//
			// A one-object container this process built, small enough for the
			// buffer the model gives the endpoint, delivered through the same
			// client, the same hub, the same pump and read back through the same
			// two bus writes and one bus read. It is what makes a 0x00 above a
			// measurement of the patch rather than of a dead wire on a booted
			// machine.
			{
				const std::vector<uint8_t> probeFile = buildProbeContainer();

				_r.probeLoaded =
					g2::pch2Load(probeFile.data(), probeFile.size(), client) == g2::Pch2LoadResult::Loaded;

				board.pumpTransport();

				_r.peekProbeObject = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

				scheduler->runFrames(1);
			}
		}

		_r.halted  = board.mcuHalted();
		_r.faulted = board.faulted();

		// ------------------------------------------------- the play transition
		scheduler->beginPlayPhase();

		{
			std::vector<g2::Frame> primed(_r.lookaheadFrames);
			_r.primedPulled = scheduler->pull(primed.data(), primed.size());
		}

		// ------------------------------------------------------------ the walk
		const unsigned expected = (_r.dspCount > 0 ? _r.dspCount - 1u : 0u) * _r.hopFrames;
		const unsigned walk     = expected + g_overrunQuanta;

		g2::Frame impulse{};
		impulse.slot[0] = g_impulseLeft;
		impulse.slot[1] = g_impulseRight;

		const g2::Frame silence{};

		for(unsigned q = 0; q < walk; ++q)
		{
			const g2::Frame& in = (q == 0) ? impulse : silence;

			(void) scheduler->push(&in, 1);
			scheduler->runFrames(1);

			g2::Frame out{};
			(void) scheduler->pull(&out, 1);

			if(out.slot[0] != 0 || out.slot[1] != 0)
			{
				++_r.nonZeroFrames;

				if(_r.arrival < 0)
				{
					_r.arrival       = int(q);
					_r.arrivalExact  = out.slot[0] == g_impulseLeft && out.slot[1] == g_impulseRight;
					_r.firstNonZeroL = out.slot[0];
					_r.firstNonZeroR = out.slot[1];
				}
			}
		}

		_r.walkQuanta = walk;

		// ---------------------------- the arrival instrument's known positive
		//
		// The links it traverses: the tail DSP's X memory, its transmit DMA,
		// the ESAI transmit register file, ESAI frame assembly, the installed
		// WriteTxCallback (which is ChainAdapter::audioTxCallback(N-1)),
		// fromEsaiFrame, mailbox N, ChainAdapter::advanceAll,
		// extractCodecSink, CodecSink::push, Scheduler::pull, and the walk's
		// own two predicates.
		//
		// The links it does not: no DSP core executes any part of it, and
		// positions 0..N-2, every receive callback, the mailbox hop chain and
		// injectCodecSource are all upstream of the tail. It says the sink can
		// report a frame the tail transmitted; it says nothing about whether
		// anything reaches the tail.
		{
			const unsigned tailPort = portOfChainPosition(board, _r.dspCount - 1u, _r.dspCount);

			_r.sinkControlPortFound = tailPort < _r.dspCount;
			_r.sinkControlPort      = _r.sinkControlPortFound ? tailPort : 0u;

			if(_r.sinkControlPortFound)
			{
				dsp56k::Peripherals56311& p = board.dspSet().peripherals(_r.sinkControlPort);
				dsp56k::Esai&             tailEsai = p.getEsai();

				for(unsigned q = 0; q < g_sinkControlQuanta && _r.sinkControlArrival < 0; ++q)
				{
					++_r.sinkControlQuanta;

					const dsp56k::TWord enabled = tailEsai.hasEnabledTransmitters();

					for(uint32_t reg = 0; reg < g_esaiTransmitters; ++reg)
					{
						if(enabled & (1u << reg))
							tailEsai.writeTX(reg, g_sinkControlWord);
					}

					// And the buffer the transmit DMA refills that register
					// from. writeSlotToFrame copies the register file into the
					// slot and then triggers the transmit DMA, which is
					// serviced synchronously and overwrites the register before
					// the next slot is assembled, so a register-only injection
					// reaches one slot and the codec sink reads two. The window
					// is read off the DMA's own source register and the ESAI's
					// own transmit word count, never typed.
					{
						const dsp56k::TWord source     = p.getDMA().getDSR(g_dmaTxChannel);
						const dsp56k::TWord frameWords = tailEsai.getTxWordCount() + 1u;
						const dsp56k::TWord base       = source - (source % frameWords);

						dsp56k::Memory& tailMemory = board.dspSet().dsp(_r.sinkControlPort).memory();

						for(dsp56k::TWord i = 0; i < frameWords * 2u; ++i)
							tailMemory.set(dsp56k::MemArea_X, base + i, g_sinkControlWord);
					}

					(void) scheduler->push(&silence, 1);
					scheduler->runFrames(1);

					g2::Frame out{};

					if(scheduler->pull(&out, 1) == 0)
						continue;

					if(out.slot[0] == 0 && out.slot[1] == 0)
						continue;

					_r.sinkControlArrival = int(q);
					_r.sinkControlL       = out.slot[0];
					_r.sinkControlR       = out.slot[1];
					_r.sinkControlExact   = out.slot[0] == g_sinkControlExpected
						&& out.slot[1] == g_sinkControlExpected;
				}
			}
		}

		// Last, so it covers every quantum this arm ran. The Board is still
		// alive here; the figures are its own and belong to this arm alone.
		_r.usb = board.usbTransport();

		return true;
	}

	void report(const char* const _label, const RunResult& _r)
	{
		std::cout << _label << ": patchOffered=" << (_r.patchOffered ? 1 : 0)
		          << " patchBytes=" << _r.patchBytes
		          << " loadResult="
		          << (_r.loadReturned ? g2::pch2LoadResultName(_r.loadResult) : "(not offered)")
		          << std::endl;
		std::cout << _label << ": bootQuanta=" << _r.bootQuanta
		          << " booted=" << (_r.booted ? 1 : 0)
		          << " programsLanded=" << (_r.programsLanded ? 1 : 0)
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0)
		          << " dspCount=" << _r.dspCount << std::endl;
		std::cout << _label << ": windowQuanta=" << g_observeQuanta
		          << " windowWordFetches=" << _r.windowFetches
		          << " oddWordReads=" << _r.oddWordReads << std::endl;

		std::cout << _label << ": windowOpenPc=" << hex32(_r.windowPc)
		          << " readsAtWindowOpenPc=" << _r.windowPcHits
		          << "  (this was the OLD known positive)" << std::endl;
		std::cout << _label << ": KNOWN POSITIVE, any address (most-read of this run) "
		          << hex32(_r.knownPositiveAddr) << " = " << _r.knownPositiveHits
		          << (_r.knownPositiveAddr >= _r.imageEnd || _r.knownPositiveAddr < _r.imageBase
		              ? "  (OUTSIDE the firmware image: a 16-bit DATA read, not an instruction fetch)"
		              : "  (inside the firmware image)")
		          << std::endl;
		std::cout << _label << ": KNOWN POSITIVE, in-image (image " << hex32(_r.imageBase)
		          << ".." << hex32(_r.imageEnd) << ") " << hex32(_r.codeKnownPositiveAddr)
		          << " = " << _r.codeKnownPositiveHits
		          << "  <- the probes' own population" << std::endl;
		std::cout << _label << ": probe " << hex32(g_probeNegative)
		          << " (known negative) = " << _r.hitsKnownNegative << std::endl;

		if(_r.crossAddr != 0)
			std::cout << _label << ": at the CONTROL arm's known positive "
			          << hex32(_r.crossAddr) << " this arm read " << _r.crossHits
			          << std::endl;
		{
			bool allInMeasuredImage = true;
			for(size_t p = 0; p < g_probeCount; ++p)
				if(g_probes[p].addr < _r.imageBase || g_probes[p].addr >= _r.imageEnd)
					allInMeasuredImage = false;
			std::cout << _label << ": PROBE VALIDITY " << g_probeCount
			          << " probes, all even-aligned (compile-time), all inside the "
			          << "image THIS RUN loaded (" << hex32(_r.imageBase) << ".."
			          << hex32(_r.imageEnd) << ") = "
			          << (allInMeasuredImage ? "YES" : "NO -- SOME ZEROS BELOW ARE ABSENCES, NOT READINGS")
			          << std::endl;
		}
		for(size_t p = 0; p < g_probeCount; ++p)
		{
			std::cout << _label << ": probe " << hex32(g_probes[p].addr)
			          << "  " << g_probes[p].what
			          << "  window = " << _r.hits[p] << std::endl;
		}
		std::cout << _label << ": byte " << hex32(g_probeDspCount)
		          << " (dspCount) = " << unsigned(_r.byteDspCount)
		          << " | " << hex32(g_probeSuspend)
		          << " (suspend) = " << unsigned(_r.byteSuspend)
		          << " | " << hex32(g_probeDepth)
		          << " (depth) = " << unsigned(_r.byteDepth)
		          << (_r.bytesInRam ? "" : "  (AT LEAST ONE ADDRESS IS OUTSIDE THIS RAM: these are absences, not readings)")
		          << std::endl;
		for(size_t p = 0; p < g_probeCount; ++p)
		{
			std::cout << _label << ": LIFETIME (boot included, never reset) "
			          << hex32(g_probes[p].addr)
			          << "  " << g_probes[p].what
			          << "  = " << _r.life[p] << std::endl;
		}

		std::cout << _label << ": SAME-COUNTER KNOWN POSITIVE "
		          << hex32(g_probeLifePositive) << " = " << _r.lifePositive
		          << "  (the store of 100 into *0x30132AC0; NOT the busy-wait)" << std::endl;
		std::cout << _label << ": ARGMAX EXCLUDING THE BUSY-WAIT [30055FBA..30055FBE) "
		          << hex32(_r.offSpinPositiveAddr) << " = " << _r.offSpinPositiveHits
		          << "  (magnitude control from a population that is not the parking spot)"
		          << std::endl;

		// ---- the write watch on the polled word -----------------------------
		//
		// What the numbers mean, stated before they are printed so the reading is
		// not reconstructed afterwards. The firmware's OWN writes to the buffer
		// are known exactly: 0x202 bytes from the memset plus 2 from the store of
		// 100, per call of FUN_30055F28. Anything above that came from somewhere
		// else. Anything at or below it means nothing in this system ever touched
		// the word the machine is waiting on.
		{
			const uint64_t calls = _r.life[g_ladderEnd];   // FUN_30055f28 entry
			const uint64_t own   = calls * g_spinBufferOwnWritesPerCall;
			const uint64_t ownHi = calls * (g_spinWordOwnWritesPerCall / 2u);

			std::cout << _label << ": SPIN WORD " << hex32(g_spinWord)
			          << " final 16-bit value = " << _r.spinWordValue
			          << (_r.spinInRam ? "" : "  (OUTSIDE THIS RAM: an absence, not a reading)")
			          << std::endl;
			std::cout << _label << ": SPIN WRITE WATCH bufferBytes=" << _r.writesSpinBuffer
			          << " highByte=" << _r.writesSpinHi
			          << " lowByte=" << _r.writesSpinLo
			          << " | FUN_30055f28 entered " << calls << " time(s)"
			          << ", so the firmware's own writes account for "
			          << own << " buffer bytes and " << ownHi
			          << " high-byte writes" << std::endl;
			std::cout << _label << ": SPIN READING = "
			          << (!_r.spinInRam
			                  ? "UNMEASURED -- the address is outside the RAM this run gave the machine"
			              : _r.writesSpinBuffer <= own
			                  ? "NOTHING OUTSIDE THE FIRMWARE EVER WROTE THE BUFFER"
			                  : "SOMETHING OTHER THAN THE FIRMWARE WROTE INSIDE THE BUFFER")
			          << std::endl;

			for(size_t i = 0; i < _r.spinTraceCount && i < g_spinTraceMax; ++i)
				std::cout << _label << ": spin word high-byte write #" << i
				          << " size=" << unsigned(_r.spinTraceSize[i])
				          << " byte=" << unsigned(_r.spinTraceValue[i]) << std::endl;
		}

		// ---- the M-Bus, because a note named the MAX1039 as the clearing agent -
		std::cout << _label << ": MBUS TRAFFIC registerReads=" << _r.mbus.registerReads
		          << " registerWrites=" << _r.mbus.registerWrites
		          << " starts=" << _r.mbus.starts
		          << " addressPhases=" << _r.mbus.addressPhases
		          << " acknowledged=" << _r.mbus.acknowledged
		          << " bytesWritten=" << _r.mbus.bytesWritten
		          << " bytesRead=" << _r.mbus.bytesRead
		          << "  -- the MAX1039 is attached to this module by Board's own"
		             " constructor, so zero here is 'the firmware never asked',"
		             " not 'no device answered'"
		          << std::endl;

		// The reading, derived here so the run states it rather than leaving it
		// to be reconstructed. Two halves, and the pair that separates them is the
		// raiser's write of 0x25 against the dispatcher's 0x25 arm: if the code is
		// written and the arm never fetched, the table sent it elsewhere; if the
		// code is never written, the event was never raised and the question moves
		// up to the drain.
		{
			const uint64_t epIrq     = _r.life[0];
			const uint64_t drain     = _r.life[1];
			const uint64_t epRead    = _r.life[2];
			const uint64_t complete  = _r.life[4];
			const uint64_t raiseCode = _r.life[7];
			const uint64_t dispatch  = _r.life[8];
			const uint64_t indexed   = _r.life[9];
			const uint64_t arm25     = _r.life[10];

			const char* consumer =
				(drain == 0 && epIrq == 0)
					? "DRAIN NEVER RAN -- nothing in the firmware read the endpoint buffer"
				: (epRead == 0)
					? "DRAIN ENTERED BUT NEVER READ THE ENDPOINT"
				: (complete == 0)
					? "DRAIN READ THE ENDPOINT BUT NO MESSAGE EVER COMPLETED (fill never reached the parsed length)"
				: (raiseCode == 0)
					? "MESSAGE COMPLETED BUT 0x25 WAS NEVER WRITTEN"
					: "0x25 WAS RAISED";

			const char* dispatchReading =
				(dispatch == 0)
					? "DISPATCHER NEVER RAN -- no event reached FUN_30012050"
				: (indexed == 0)
					? "DISPATCHER RAN BUT EVERY CODE FELL OUT OF RANGE 1..69"
				: (arm25 == 0)
					? "DISPATCHER RAN AND TOOK THE INDEXED JUMP, BUT NEVER THE 0x25 ARM -- the events it routed were other codes"
					: "THE 0x25 ARM WAS TAKEN";

			const uint64_t loopCall = _r.life[13];
			const uint64_t loopEnter= _r.life[14];
			const uint64_t loopIter = _r.life[15];
			const uint64_t deq      = _r.life[16];
			const uint64_t deqQ25   = _r.life[17];
			const uint64_t nonEmpty = _r.life[18];
			const uint64_t idle     = _r.life[19];
			const uint64_t queuesMade = _r.life[20];

			const char* loopReading =
				(loopEnter == 0)
					? "EVENT LOOP NEVER FETCHED -- the scheduler never reached FUN_30004674"
				: (loopIter == 0)
					? "EVENT LOOP ENTERED BUT NEVER REACHED ITS HEAD"
				: (deq == 0)
					? "LOOP RUNS BUT THE DEQUEUE IS NEVER CALLED -- something before it diverts"
				: (nonEmpty == 0)
					? "LOOP RUNS AND DEQUEUES, BUT NO QUEUE EVER RETURNED AN ITEM -- all four always empty"
					: "A QUEUE RETURNED AN ITEM AND THE DISPATCH BRANCH WAS TAKEN";

			std::cout << _label << ": READING consumer = " << consumer << std::endl;
			std::cout << _label << ": READING dispatch = " << dispatchReading << std::endl;
			std::cout << _label << ": READING loop     = " << loopReading << std::endl;
			{
				// The last rung of the init ladder that was fetched. Probe 21 is
				// the init entry and probes 22.. are the rungs in address order,
				// so the highest index with a non-zero count names how far the
				// init got before it stopped.
				size_t last = 0;
				for(size_t p = g_ladderBegin; p < g_ladderEnd; ++p)
					if(_r.life[p] != 0)
						last = p;
				std::cout << _label << ": INIT LADDER last rung fetched = "
				          << (last == 0 ? "NONE -- FUN_3001b09c itself never ran"
				                        : g_probes[last].what)
				          << " (" << hex32(g_probes[last ? last : g_ladderBegin].addr) << ")" << std::endl;
				for(size_t p = g_ladderBegin; p < g_ladderEnd; ++p)
					std::cout << _label << ": rung " << hex32(g_probes[p].addr)
					          << " = " << _r.life[p]
					          << "  " << g_probes[p].what << std::endl;
			}
			std::cout << _label << ": LOOP DETAIL callSite=" << loopCall
			          << " enter=" << loopEnter
			          << " iterations=" << loopIter
			          << " dequeueCalls=" << deq
			          << " dequeueForQ26F8=" << deqQ25
			          << " nonEmpty=" << nonEmpty
			          << " idle=" << idle
			          << " queuesConstructed=" << queuesMade << std::endl;
		}
		std::cout << _label << ": RX DESCRIPTOR [" << hex32(g_rxDescriptorPtr)
		          << "] = " << hex32(_r.rxDescriptor)
		          << "  parsed length(+2) = " << _r.rxLength
		          << "  accumulated fill(+6) = " << _r.rxFill
		          << (_r.rxInRam
		              ? (_r.rxLength == _r.rxFill
		                 ? "  (equal -- the drain would have raised 0x25)"
		                 : "  (UNEQUAL -- this is why 0x25 was never raised)")
		              : "  (OUTSIDE THIS RAM: an absence, not a reading)")
		          << std::endl;
		std::cout << _label << ": WRITES to the byte probes (boot included) "
		          << hex32(g_probeDspCount) << " = " << _r.writesDspCount
		          << " | " << hex32(g_probeSuspend) << " = " << _r.writesSuspend
		          << " | " << hex32(g_probeDepth) << " = " << _r.writesDepth << std::endl;
		std::cout << _label << ": cs3 peek endpoint " << g2::BoardConfig{}.usbProtocolEndpoint
		          << " -- afterHandover=0x" << std::hex << unsigned(_r.peekAfterHandover)
		          << " afterWindow=0x" << unsigned(_r.peekAfterWindow)
		          << " afterProbeObject=0x" << unsigned(_r.peekProbeObject)
		          << std::dec << " (probe object type " << hex32(g_probeObjectType)
		          << ", probeLoaded=" << (_r.probeLoaded ? 1 : 0) << ")" << std::endl;
		std::cout << _label << ": usb pumps=" << _r.usb.pumps
		          << " drained=" << _r.usb.drained
		          << " offered=" << _r.usb.offered
		          << " accepted=" << _r.usb.accepted
		          << " refused=" << _r.usb.refused
		          << " completed=" << _r.usb.completed
		          << " stallReports=" << _r.usb.stallReports
		          << " held=" << (_r.usb.held ? 1 : 0)
		          << " heldAttempts=" << _r.usb.heldAttempts
		          << " heldOffset=" << _r.usb.heldOffset
		          << "/" << _r.usb.heldSize
		          << std::endl;
		std::cout << _label << ": primedPulled=" << _r.primedPulled
		          << " walkQuanta=" << _r.walkQuanta
		          << " arrival=" << _r.arrival
		          << " arrivalExact=" << (_r.arrivalExact ? 1 : 0)
		          << " nonZeroFrames=" << _r.nonZeroFrames
		          << " firstNonZero=" << _r.firstNonZeroL << "/" << _r.firstNonZeroR
		          << std::endl;
		std::cout << _label << ": sinkControl tailPosition="
		          << (_r.dspCount > 0 ? _r.dspCount - 1u : 0u)
		          << " tailPort=" << (_r.sinkControlPortFound ? int(_r.sinkControlPort) : -1)
		          << " controlQuanta=" << _r.sinkControlQuanta
		          << " sinkControlArrival=" << _r.sinkControlArrival
		          << " sinkControlExact=" << (_r.sinkControlExact ? 1 : 0)
		          << " sinkControlValue=" << _r.sinkControlL << "/" << _r.sinkControlR
		          << std::endl;
	}
}

int main()
{
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

		const std::string patchPath = directory + "/" + G2_PATCH_RELATIVE_PATH;
		const std::vector<uint8_t> patch = readFile(patchPath);

		if(patch.empty())
		{
			std::cout << "FAIL the patch is empty or unreadable at " << patchPath << std::endl;
			return false;
		}

		// The packet size is read from BoardConfig and not written here. The
		// Board the arms below construct takes it from the same default, so a
		// change to that default moves the expectation and the machine
		// together instead of turning this file red.
		const size_t packetSize = g2::BoardConfig{}.usbMaxPacketBytes;

		unsigned objectCount    = 0;
		size_t   largestObject  = 0;
		uint8_t  firstType      = 0;
		unsigned packetsNeeded  = 0;
		unsigned exactMultiples = 0;
		measureObjects(patch, objectCount, largestObject, firstType,
			packetSize, packetsNeeded, exactMultiples);

		std::cout << "patch: " << patchPath << " (" << patch.size() << " bytes, "
		          << objectCount << " objects, largest framed object "
		          << largestObject << " bytes, first object type "
		          << hex32(firstType) << ")" << std::endl;

		std::cout << "patch: at a " << packetSize << "-byte max packet size the "
		          << objectCount << " objects take " << packetsNeeded
		          << " packets, and " << exactMultiples
		          << " of them have a length that is an exact multiple of it"
		          << std::endl;

		// The largest object against the part's own ceiling, so that the split
		// cannot be argued away as an artefact of one configuration.
		// ISP1362 Rev. 06 Table 16 (p.52) gives a non-isochronous endpoint
		// exactly four legal buffer sizes -- 8, 16, 32 and 64 bytes, with
		// `0100` to `1111` reserved -- and Table 109 (p.105) states the same
		// bound as "interrupt/bulk: N <= 64 bytes". So no DcEndpointConfiguration
		// byte exists that would make this object deliverable in one packet,
		// and the only question the split answers is whether this stack cuts
		// it up or drops it.
		check(largestObject > packetSize,
			std::string("this container really does carry an object no legal bulk"
			            " endpoint could take whole: largest framed object ")
			+ std::to_string(largestObject) + " bytes against a "
			+ std::to_string(packetSize) + "-byte maximum packet");

		// ---------------------------------------------------------- the control
		//
		// The unpatched run is first and it runs the identical code path. It is
		// what makes any probe count in the patched run a statement about the
		// patch rather than about the firmware's ordinary idle.
		RunResult control;
		if(!runOnce(directory, patch, false, 0u, control))
			return false;
		report("control", control);

		// -------------------------------------------------------- the measurement
		//
		// The control's known positive is handed to the patched arm so the two
		// can be read at one common address. Each arm still selects its own.
		RunResult patched;
		if(!runOnce(directory, patch, true, control.knownPositiveAddr, patched))
			return false;
		report("patched", patched);

		// The instrument, asserted before anything is read off it.
		check(control.windowFetches > 0,
			"the fetch counter saw instruction words at all during the control window");
		check(control.hitsKnownNegative == 0,
			"KNOWN NEGATIVE: an address inside the vector table is never fetched as an instruction word");
		check(patched.hitsKnownNegative == 0,
			"KNOWN NEGATIVE, patched run: the vector-table address is still never fetched");

		// ------------------------------------------------- the sensitivity gate
		//
		// A floor and not a presence test, asserted per arm. A `> 0` gate passes
		// on a known positive of 1, and every zero on such an arm is then read
		// against a counter that fired once.
		//
		// The floor is a property of this file and the address is not: the file
		// may say how large a known positive has to be, not where that count is
		// to be found. This value fires on an instrument that has gone blind and
		// not on one that has merely moved.
		constexpr uint64_t g_sensitivityFloor = 1000u;

		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			const std::string label = run.first;

			check(run.second.knownPositiveHits >= g_sensitivityFloor,
				label + ": KNOWN POSITIVE, the most-read address of this run ("
				+ hex32(run.second.knownPositiveAddr) + "), is read at least "
				+ std::to_string(g_sensitivityFloor)
				+ " times, so a zero on this arm is read against a real count; observed "
				+ std::to_string(run.second.knownPositiveHits));

			// The one the probes are actually weighed against. The unrestricted
			// argmax can be satisfied by a hot data read; this one cannot,
			// because it is confined to the bytes the image supplied.
			check(run.second.codeKnownPositiveHits >= g_sensitivityFloor,
				label + ": KNOWN POSITIVE, in-image, the most-read address inside the loaded"
				" firmware image (" + hex32(run.second.codeKnownPositiveAddr)
				+ "), is read at least " + std::to_string(g_sensitivityFloor)
				+ " times, so a zero at an instruction address on this arm is read against a"
				" count from the SAME population; observed "
				+ std::to_string(run.second.codeKnownPositiveHits));
		}

		// The preconditions of the measurement, asserted before the measurement,
		// so that a silent machine is reported as the machine's failure and not
		// as the patch's.
		// ---------------- the arrival instrument's known positive, both runs
		//
		// Without a control, a chain that carried nothing and an arrival path
		// that could not report anything both print `arrival=-1`, and nothing
		// here tells them apart. Both runs are asserted because both print an
		// arrival figure, and a control that held on one run would say nothing
		// about the other.
		//
		// Every field is pinned. The port is the one the firmware's table puts
		// at chain position N-1 and is not the position number -- on this
		// machine it is 0 against a position of 7, and a control that assumes
		// otherwise drives chain position 1 and reports a dead path on a
		// healthy machine. The arrival is quantum 0 because the tail writes
		// mailbox N in the same quantum the egress phase reads it. Both slots
		// carry the sentinel, so neither line of extractCodecSink is passing a
		// value the other one supplied.
		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			const std::string label = run.first;

			check(run.second.sinkControlPortFound,
				label + ": the firmware's port table names a port at chain position "
				+ std::to_string(run.second.dspCount - 1u));
			check(run.second.sinkControlArrival == 0,
				label + ": the sink control arrived on the first control quantum; observed "
				+ std::to_string(run.second.sinkControlArrival));
			check(run.second.sinkControlExact,
				label + ": the sink control arrived unchanged in BOTH codec slots; observed "
				+ std::to_string(run.second.sinkControlL) + "/"
				+ std::to_string(run.second.sinkControlR) + " against "
				+ std::to_string(g_sinkControlExpected));
		}

		check(control.programsLanded, "control: every DSP position took its program");
		check(patched.programsLanded, "patched: every DSP position took its program");
		check(!patched.halted,  "patched: the core is not halted when the window closes");
		check(!patched.faulted, "patched: the board reports no fault when the window closes");

		// -------------------------------------------------- the load is accepted
		//
		// A real `.pch2` is accepted by the Board's own transport hub. Every
		// other measurement in this file is downstream of it: a load that was
		// refused originated some frames and not others, and a probe count taken
		// after a partial hand-over is a count about neither machine.
		check(patched.loadResult == g2::Pch2LoadResult::Loaded,
			std::string("a real 18-object `.pch2` loads through the BOARD'S OWN hub into the running"
			            " machine; pch2Load answered ") +
			g2::pch2LoadResultName(patched.loadResult));

		// ------------------------------------------- the wire, on a booted machine
		//
		// The known positive for every 0x00 reported above. Same board, same
		// client, same hub, same pump, same three bus calls -- only the object
		// is different, and the difference is its size.
		check(patched.probeLoaded, "the one-object probe container loads through the same hub");
		check(control.probeLoaded, "the one-object probe container loads on the control run too");

		// The known positive is taken on the control run and only there, and
		// that is a property of the instrument rather than a weakening of it.
		// The peek reads the head of the endpoint's OUT FIFO, and on the control
		// run the FIFO is empty when the probe object arrives, so the head is
		// the probe object -- with no firmware service in the story at all. The
		// patched run's own reading is reported in the verdict below rather than
		// asserted here, because on that run the head depends on whether the
		// firmware has serviced the patch packet yet, and that is the thing this
		// file is measuring rather than a precondition it may assume.
		check(control.peekProbeObject == g_probeObjectType,
			std::string("KNOWN POSITIVE, on a BOOTED machine: a small object delivered through the"
			            " same client, hub, pump and bus calls is readable at the CS3 data port as"
			            " its own type byte ") + hex32(g_probeObjectType) + "; read " +
			hex32(control.peekProbeObject));

		// And the control that makes that known positive mean something: on the
		// run where no patch was offered, the same reading before the probe
		// object is the benign 0x00.
		check(control.peekAfterHandover == 0x00u,
			"CONTROL: with no patch offered, the endpoint buffer is empty at the same instant");

		// The property this file exists to hold. A byte of a real `.pch2` is in
		// the device register file of a machine that has really booted. The
		// expected value is computed from the file this run read and is not
		// written here as a literal, so the case cannot pass against a device
		// that answers a fixed byte that happens to match.
		check(patched.peekAfterHandover == firstType,
			std::string("a byte of a REAL `.pch2` is readable at the CS3 data port of a BOOTED"
			            " machine as the patch's own first object type ") + hex32(firstType) +
			"; read " + hex32(patched.peekAfterHandover));

		// ---------------------------------------- the transport loses nothing
		//
		// The no-loss invariant, asserted and not narrated. Every frame this
		// Board took out of the hub sits in the device, is still held for
		// another offer, or is counted undeliverable; nowhere else. A pump that
		// handed each drained frame to `isp1181_rx` and discarded the answer
		// loses frames with every visible signal reading healthy.
		//
		// It is asserted on both arms. A control arm that offers one frame and
		// a patched arm that offers tens of thousands are the same invariant,
		// and an arithmetic slip that held only for the busy arm would be a
		// real defect.
		for(const RunResult* const arm : { &control, &patched })
		{
			const g2::Board::UsbTransportStats& u = arm->usb;

			check(u.offered == u.accepted + u.refused,
				std::string("every offer to the device is either accepted or refused: ")
				+ std::to_string(u.offered) + " == " + std::to_string(u.accepted)
				+ " + " + std::to_string(u.refused));

			/* The invariant reads `completed` and not `accepted`, and that is a
			 * unit repair. `accepted` counts packets, because pumpTransport
			 * splits a frame into max-packet-size pieces, while `drained`
			 * counts frames; comparing the two directly compares two units.
			 * `completed` is incremented once, when a frame's last packet is
			 * taken, so it is the frame-shaped figure this equality needs.
			 * `undeliverable` is the third destination a drained frame can
			 * reach: it left the hub and was never offered. board.h states the
			 * same thing at the declaration. */
			check(u.drained == u.completed + u.undeliverable + (u.held ? 1u : 0u),
				std::string("NOTHING DRAINED IS LOST: ") + std::to_string(u.drained)
				+ " frames left the hub, " + std::to_string(u.completed)
				+ " crossed to the device whole, " + std::to_string(u.undeliverable)
				+ " were too large to offer and " + (u.held ? "1 is" : "0 are")
				+ " still held for another offer");

			// Separate from the invariant on purpose. The invariant stays true
			// whatever this reads, and this says the hub's size guarantee held:
			// a frame too large for the hold buffer should never reach the
			// drain at all.
			check(u.undeliverable == 0u,
				std::string("no frame left the hub too large to be offered to the device: ")
				+ std::to_string(u.undeliverable));
		}

		// ------------------------------------ how much of the patch arrived
		//
		// Asserted with its unit named. One unit here is one protocol frame the
		// hub handed the Board -- one `.pch2` object, plus the one-object probe
		// container the arm also loads -- and not one packet, one object of the
		// file, or one byte. A frame the device will not take blocks the drain
		// behind it for the rest of the run.
		//
		// It is a fraction and it is asserted as one. A count on its own would
		// go green on a run that pushed fewer frames: the denominator is what
		// the arm really offered, read from the same struct as the numerator.
		{
			const g2::Board::UsbTransportStats& u = patched.usb;

			check(u.drained > 0,
				std::string("frames really left the hub on the patched arm, so the"
				            " fraction below is read against a real denominator; ")
				+ std::to_string(u.drained) + " frames");

			check(u.completed == u.drained && !u.held,
				std::string("EVERY frame the hub handed this Board reached the device"
				            " WHOLE: ") + std::to_string(u.completed) + " of "
				+ std::to_string(u.drained) + " frames completed, "
				+ std::to_string(u.accepted) + " packets accepted and "
				+ std::to_string(u.refused) + " NAKed and retried, "
				+ (u.held ? "1 frame is still held" : "nothing is still held"));

			std::cout << "measurement: " << u.completed << " of " << u.drained
			          << " protocol frames crossed to the firmware whole, in "
			          << u.accepted << " packets; " << u.refused
			          << " packets were NAKed and re-offered, and "
			          << u.stallReports << " frames outlived the NAK retry ceiling"
			          << std::endl;
		}

		// -------------------------------------------------------- the verdict lines
		//
		// Reported and not asserted, deliberately. Each names a state of the
		// machine as it is now, and an assertion on it would go red on the day
		// the machine improves.
		std::cout << "verdict: the firmware "
		          << (patched.peekAfterWindow == patched.peekAfterHandover
		              ? "NEVER TOOK the packet out of the endpoint buffer"
		              : "TOOK the packet out of the endpoint buffer")
		          << " across " << g_observeQuanta << " quanta" << std::endl;

		// The two arms are compared at a common address and not at their own.
		// Each arm's known positive is a different address, so comparing the two
		// arms' known-positive counts compares two different things and would
		// report "DIFFERENT" for a machine that did the same work.
		std::cout << "verdict: the MCU instruction-fetch stream is "
		          << (patched.windowFetches == control.windowFetches &&
		              patched.crossHits == control.knownPositiveHits
		              ? "IDENTICAL with and without the patch, so the patch changed nothing the core did"
		              : "DIFFERENT with and without the patch")
		          << std::endl;

		std::cout << "verdict: after a SECOND object was delivered, the head of the endpoint buffer is "
		          << hex32(patched.peekProbeObject) << " on the patched run and "
		          << hex32(control.peekProbeObject) << " on the control run"
		          << std::endl;

		std::cout << "verdict: routine " << hex32(g_probeTarget) << " "
		          << (patched.hitsTarget > 0 ? "FIRED" : "DID NOT FIRE")
		          << " on the patched run and "
		          << (control.hitsTarget > 0 ? "FIRED" : "DID NOT FIRE")
		          << " on the control run" << std::endl;

		// The scale every zero above is read against, per arm and never pooled:
		// quoting one arm's known positive beside the other arm's zero measures
		// nothing.
		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			std::cout << "verdict: on the " << run.first << " arm a zero at "
			          << hex32(g_probeTarget)
			          << " is weighed against an IN-IMAGE known positive of "
			          << run.second.codeKnownPositiveHits << " at "
			          << hex32(run.second.codeKnownPositiveAddr)
			          << " (and an any-address known positive of "
			          << run.second.knownPositiveHits << " at "
			          << hex32(run.second.knownPositiveAddr) << "), out of "
			          << run.second.windowFetches << " word reads in the window" << std::endl;
		}

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
