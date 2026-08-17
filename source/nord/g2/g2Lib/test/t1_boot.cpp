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
// WHY IT DRIVES Board::onRead AND Board::onWrite RATHER THAN Board::busRead.
// board.h records the measurement: an earlier revision of the composition kept
// the callbacks private, the check drove busRead instead, and restoring the old
// "return 0u with a bus-OK status" body into onRead left that check fully
// GREEN. onRead and onWrite are the exact function pointers the Board hands to
// mcf5307_create, so a core driven through them takes the path the real core
// takes. Nothing in this file reaches the routing by any other door.
//
// WHY IT CREATES ITS OWN mcf5307_ctx. The Board owns a core but publishes no
// handle to it, so nothing outside the Board can call mcf5307_reset (which is
// the only way to set the program counter), mcf5307_halted, mcf5307_faulted or
// mcf5307_get_reg. Adding an accessor would be a write to `g2Lib/board.h`,
// which is NOT on this task's Files: line. The core this file creates is
// pointed at the SAME Board through the SAME two callbacks, so it exercises the
// composition and not a copy of it.
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
#include "../memoryMap.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
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

	// The two expected lines. Plan section 6.6.1 is their one home.
	//
	// LINE 0 CARRIES A TRAILING SPACE AND IT IS LOAD-BEARING. The stored string
	// `Nord Modular G2` is FIFTEEN characters; the sixteenth is produced by the
	// display helper padding a NUL source byte to 0x20 at 0x30057060, without
	// advancing the source pointer. A comparison that trims trailing whitespace
	// is comparing a different value. Both literals are written out in full
	// rather than composed, so nothing here can agree with a wrong firmware by
	// deriving its expectation from the same place the firmware got it.
	const std::string g_expectedLine0 = "Nord Modular G2 ";
	const std::string g_expectedLine1 = "Version 1.62 Exp";

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

	// The register indices of the mcf5307 C ABI. 17 is the program counter.
	constexpr int g_regPc = 17;

	// -------------------------------------------------------------- the windows

	// MEASURED. Plan section 6.6.3: the loader writes `movel #0x10000001,%d0`
	// then `movec %d0,%mbar` at loader offset 0x1E, and the OS image contains no
	// `movec` to %mbar at all. Booting CODE directly means this harness supplies
	// it. The size is the SIM's own g_simSpaceSize, which covers UM Table B-1.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// MEASURED. Plan section 6.6.9, from the loader's CSAR2 = $1200,
	// CSMR2 = $007F0001 at loader offsets 0x70 and 0x7c: the window is
	// 0x12000000..0x127FFFFF, and the OS never reprograms it.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

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

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
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
				m_bytes[index] = uint8_t((_value >> shift) & 0xffu);
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
		std::vector<uint8_t> m_bytes;
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
		// CS3 is left ABSENT. INT-1's own title is "boot the firmware with a
		// STUBBED CS3", and an absent window is the honest stub: an access to it
		// is reported unmapped and logged, rather than answered with a zero that
		// a caller cannot tell from a device.
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
			out.push_back(char(byte & 0xffu));
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

	// The number of HDI08 ports that completed the HF0/HF2 handshake. HF0 is the
	// host's own flag in ICR and HF2 is the DSP's answer in ISR, so a port has
	// completed the handshake only when BOTH are set: HF0 alone is the firmware
	// talking to itself.
	int handshakePortCount(g2::Board& _board)
	{
		int completed = 0;

		for(int port = 0; port < g2::g_hdi08PortCount; ++port)
		{
			auto& hdi08 = _board.hdi08().port(port);

			// icr() and isr() are the model's own register readers. The
			// BusTarget-facing read8 override is deliberately not used here:
			// this is an inspection and it must not look like a bus cycle.
			const uint8_t icr = hdi08.icr();
			const uint8_t isr = hdi08.isr();

			if((icr & mc68k::Hdi08::Hf0) && (isr & mc68k::Hdi08::Hf2))
				++completed;
		}

		return completed;
	}

	// The iteration bound INT-1's Check names. It is the firmware's own retry
	// count for the handshake and it bounds this harness's polling too, so a
	// machine that never converges stops rather than hanging the suite.
	constexpr uint32_t g_handshakeIterations = 0xFDE8u;

	// The cycle budget of one polling iteration. Small enough that the banner is
	// noticed close to the instruction that wrote it, large enough that the
	// whole boot fits in g_handshakeIterations iterations.
	constexpr uint32_t g_cyclesPerIteration = 4096u;

	// What one boot produced. Everything the assertions below read comes from
	// here, so the run happens once and no assertion can re-run the machine and
	// quietly get a second answer.
	struct BootResult
	{
		bool     imageLoaded    = false;
		bool     readPathProven = false;
		bool     bannerAppeared = false;
		std::string line0;
		std::string line1;
		int      handshakePorts = 0;
		uint32_t pcAtBanner     = 0;
		uint32_t pcAfterBanner  = 0;
		uint32_t pcLater        = 0;
		bool     halted         = false;
		bool     faulted        = false;
		uint32_t iterations     = 0;
		std::vector<std::string> busLog;
	};

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

		// THE CORE IS POINTED AT Board::onRead AND Board::onWrite, which are the
		// exact pointers the Board hands to its own mcf5307_create. See the file
		// header for why this file creates a core rather than borrowing one.
		mcf5307_ctx* mcu = mcf5307_create(&board, &g2::Board::onRead, &g2::Board::onWrite, nullptr);

		if(!mcu)
		{
			std::cout << "FAIL mcf5307_create returned no context" << std::endl;
			return false;
		}

		mcf5307_reset(mcu, g_entrySp, g_entryPc);

		// PHASE 1 -- run until the banner appears, or until the bound.
		for(uint32_t i = 0; i < g_handshakeIterations; ++i)
		{
			_result.iterations = i + 1;

			mcf5307_exec(mcu, g_cyclesPerIteration);

			if(mcf5307_halted(mcu))
				break;

			const std::string line0 = readDisplayLine(board, 0, 0);

			// The banner has appeared once display 0 line 0 holds anything
			// printable. A buffer that starts as zeroes cannot satisfy this by
			// already being right, which is the point: the RAM this harness
			// supplies is zero-filled at construction and the expected text
			// contains no NUL.
			if(std::any_of(line0.begin(), line0.end(), [](const char c) { return uint8_t(c) >= 0x20; }))
			{
				_result.bannerAppeared = true;
				_result.pcAtBanner = mcf5307_get_reg(mcu, g_regPc);
				break;
			}
		}

		_result.line0 = readDisplayLine(board, 0, 0);
		_result.line1 = readDisplayLine(board, 0, 1);
		_result.pcAfterBanner = mcf5307_get_reg(mcu, g_regPc);

		// PHASE 2 -- the answer to plan section 24.6 row W3-129. A green read of
		// correct cells does NOT by itself show the firmware ran on, so the
		// machine is run further and its program counter is sampled again. Plan
		// section 6.6.5's blocking claim is REFUTED by row W3-144 -- the spin at
		// 0x30056E52 services its own work at 0x30056E7E and terminates without a
		// timer -- so a machine that reached the banner is expected to leave it,
		// and a machine that did not is expected to sit still. The two are told
		// apart below.
		for(uint32_t i = 0; i < 64u && !mcf5307_halted(mcu); ++i)
			mcf5307_exec(mcu, g_cyclesPerIteration);

		_result.pcLater = mcf5307_get_reg(mcu, g_regPc);
		_result.halted  = mcf5307_halted(mcu) != 0;
		_result.faulted = mcf5307_faulted(mcu) != 0;
		_result.handshakePorts = handshakePortCount(board);
		_result.busLog = board.memory().log();

		mcf5307_destroy(mcu);
		return true;
	}

	bool insideBanner(const uint32_t _pc)
	{
		return _pc >= g_bannerFunction && _pc < g_bannerFunctionEnd;
	}

	void report(const BootResult& _r)
	{
		std::cout << "boot: iterations=" << _r.iterations
		          << " readPathProven=" << (_r.readPathProven ? 1 : 0)
		          << " bannerAppeared=" << (_r.bannerAppeared ? 1 : 0)
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0) << std::endl;
		std::cout << "boot: pcAtBanner=0x" << std::hex << _r.pcAtBanner
		          << " pcAfterBanner=0x" << _r.pcAfterBanner
		          << " pcLater=0x" << _r.pcLater << std::dec << std::endl;
		std::cout << "boot: line0=" << escapedLine(_r.line0) << std::endl;
		std::cout << "boot: line1=" << escapedLine(_r.line1) << std::endl;
		std::cout << "boot: handshakePorts=" << _r.handshakePorts << std::endl;

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

		// CLAUSE 2, the handshake count, compared AS A NUMBER.
		check(result.handshakePorts == int(g2::g_hdi08PortCount),
		      "HDI08 ports completing the HF0/HF2 handshake within " +
		      std::to_string(g_handshakeIterations) + " iterations equals " +
		      std::to_string(g2::g_hdi08PortCount) + "; counted " +
		      std::to_string(result.handshakePorts));

		// ------------------------------ the answer to plan section 24.6 row W3-129
		//
		// W3-129 records that correct cells can be read off a machine that is
		// stuck, so a green comparison alone does not show the firmware ran on
		// past the banner. Row W3-144 then refuted the blocking premise: the
		// spin at 0x30056E52 calls the flush at 0x3005687C from INSIDE its own
		// body at 0x30056E7E, so it terminates with no timer, no scheduler and
		// no interrupt. THIS TEST THEREFORE ASSERTS PROGRESS RATHER THAN
		// ACCEPTING THE LIMITATION, and the three assertions below are what
		// distinguish "reached the banner and continued" from "halted with the
		// cells already correct":
		//
		//   1. the core is not halted, so it did not stop at either hard halt;
		//   2. the program counter is at neither halt address, which names the
		//      two failure modes plan sections 6.6.8 and 6.6.3 record;
		//   3. the program counter has LEFT the banner function, so execution
		//      returned to 0x3001B43E or beyond rather than sitting in the spin.
		//
		// Assertion 3 is the one that cannot be satisfied by a machine that
		// stopped with a correct buffer.

		// EACH OF THE FOUR REQUIRES THE BANNER TO HAVE APPEARED, AND THAT
		// CONJUNCTION IS THE POINT RATHER THAN BELT AND BRACES. Written as bare
		// predicates over the program counter, all three of the last three pass
		// on a machine that never executed one useful instruction: a core that
		// faulted at its entry point sits at neither halt address and is
		// trivially outside the banner function, so it collects three green
		// ticks for having done nothing. That is this project's signature defect
		// -- an assertion satisfied by a value that was already there -- and it
		// was OBSERVED on this very test before the conjunction was added.
		check(result.bannerAppeared,
		      "the firmware composed a banner into display 0 at all");

		check(result.bannerAppeared && !result.halted,
		      "the banner appeared AND the core is still running rather than halted");

		check(result.bannerAppeared && result.pcLater != g_haltFlashGate,
		      "the banner appeared AND the program counter is not at the "
		      "flash-gate halt 0x3001BB4C");

		check(result.bannerAppeared && result.pcLater != g_haltModelByte,
		      "the banner appeared AND the program counter is not at the "
		      "model-byte halt 0x3001B86C");

		check(result.bannerAppeared && !insideBanner(result.pcLater),
		      "the banner appeared AND the program counter has left the banner "
		      "function 0x3001B7FC, so the firmware ran on past the banner "
		      "rather than stopping inside it");

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	// A run that executed no gated test reports NOT VERIFIED and must not be
	// read as a pass, but it must not fail the suite either: an artifact-less
	// machine is a legitimate configuration. A run that DID execute and failed
	// is a failure.
	return counters.failed > 0 ? 1 : 0;
}
