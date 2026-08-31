// An isolated probe of the firmware's own `sprintf`, driven directly against
// the MCF5307 core with NO boot, NO peripherals and NO Board.
//
// Cell-level write tracing of the booted machine showed
// that OS banner line 1 -- composed by
// `sprintf(buf, "Version %d.%02d Exp", 1, 62)` -- reaches the display with
// fifteen of its sixteen characters byte-perfect and correctly positioned, and
// with the final character wrong: 0x09 where 'p' is 0x70. Banner line 0 is a
// plain literal copy with no formatting and is 16/16 correct. The only
// difference between the two paths is the formatter, so the suspicion is a
// CPU-core defect in executing the formatter rather than a display defect.
//
// A boot cannot separate those two. This test removes everything but the
// formatter: a flat RAM, the firmware image loaded at its load address, the
// four arguments pushed exactly as the firmware's own call site pushes them,
// and the program counter set to `sprintf`'s first instruction. If the buffer
// comes back wrong here, the fault is in the core executing the formatter and
// the display is exonerated. If it comes back right here, the fault is not in
// sprintf, which is an equally decisive result.
//
// Tier T1 and gated: the test needs `CODE_30000400.bin`, a Clavia artifact, so
// it resolves through ArtifactResolver and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// The measured facts this test is built on:
//   - The image loads at 0x30000400. File offset = address - 0x30000400.
//   - `sprintf` begins at 0x300D90C6 with `4e56 ffa8` = LINK.W A6,#-0x58.
//     Case 0 below re-reads those four bytes out of the loaded image, so a
//     wrong entry point fails loudly instead of executing whatever is there.
//   - The format string "Version %d.%02d Exp" lives at 0x300E9FEE. Case 0
//     re-reads it too.
//   - The call site at 0x3001B884 pushes, in order, PEA 0x3E (62), PEA 0x01
//     (1), PEA 0x300E9FEE (the format), then MOVE.L D2,-(A7) with D2 = A6-18
//     (the output buffer), then JSR. So on entry the stack is
//         (A7+ 0) return address
//         (A7+ 4) char*       buffer
//         (A7+ 8) const char* format
//         (A7+12) int         first  argument
//         (A7+16) int         second argument
//     which is the ordinary C ABI: arguments pushed right to left.
//
// The return address pushed under the arguments is a sentinel inside RAM
// holding one HALT word, so the RTS at the end of
// `sprintf` lands on it and the core stops of its own accord. A sentinel in
// unmapped space would also stop the core, but through a bus fault, and a
// fault is exactly the signal this test needs to keep meaning "something went
// wrong" -- so the sentinel is mapped and the fault channel is left clean.
// Every case asserts that the core reached the sentinel; a run that exhausts
// its cycle budget is reported as such and never silently compared.

#include "../artifactResolver.h"
#include "gatedFixture.h"

#include <mcf5307.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

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

	// ------------------------------------------------------------ the address map
	//
	// One flat RAM, big enough for the image, the scratch strings, the buffer,
	// the stack and the sentinel. Nothing else answers, so any access the
	// formatter makes outside this window is reported as a bus fault rather
	// than being invented -- which is the whole reason for not reusing the
	// Board here.
	constexpr uint32_t g_ramBase = 0x30000000u;
	constexpr uint32_t g_ramSize = 0x01000000u;   // 16 MB

	constexpr uint32_t g_loadAddress = 0x30000400u;

	constexpr uint32_t g_sprintfEntry = 0x300D90C6u;
	constexpr uint32_t g_imageFormat  = 0x300E9FEEu;

	// The sentinel the RTS returns to, and the one word stored there. 0x4AC8
	// is HALT on this part. Whether the core executes it as HALT or stops on it
	// as an opcode with no implemented semantics, it stops -- and the test
	// asserts on the program counter, so either route is proof the formatter
	// returned to exactly this address.
	constexpr uint32_t g_sentinel     = 0x30FF0000u;
	constexpr uint16_t g_haltOpcode   = 0x4AC8u;

	constexpr uint32_t g_bufferAddr   = 0x30FE0000u;
	constexpr uint32_t g_bufferSize   = 64u;
	constexpr uint32_t g_scratchAddr  = 0x30FE1000u;   // caller-supplied formats
	constexpr uint32_t g_stackTop     = 0x30FD0000u;

	// The byte the buffer is filled with before every run. It is neither NUL,
	// nor a printable character, nor 0x09 -- the value the booted machine
	// actually delivered -- so a case cannot pass by the buffer never being
	// written, and a stray 0x09 cannot be confused with the fill.
	constexpr uint8_t g_fill = 0xAAu;

	// The register indices mcf5307.h publishes: 8..15 are a0..a7, 17 is the PC.
	constexpr int g_regA7 = 15;
	constexpr int g_regPc = 17;

	// A budget far larger than any plausible formatter. `sprintf` on this part
	// is a few thousand instructions at worst; a million cycles is three orders
	// of magnitude of headroom, and a run that hits it is reported and never
	// compared.
	constexpr uint32_t g_cycleBudget       = 4000000u;
	constexpr uint32_t g_cyclesPerIteration = 4096u;

	// ------------------------------------------------------------ the flat memory

	struct Ram
	{
		std::vector<uint8_t> bytes = std::vector<uint8_t>(g_ramSize, 0x00u);

		uint32_t faults = 0;
		uint32_t lastFaultAddress = 0;

		bool contains(const uint32_t _address, const int _size) const
		{
			if(_address < g_ramBase)
				return false;
			const uint64_t offset = static_cast<uint64_t>(_address) - g_ramBase;
			return offset + static_cast<uint64_t>(_size) <= g_ramSize;
		}

		uint8_t peek8(const uint32_t _address) const
		{
			return bytes[_address - g_ramBase];
		}

		void poke8(const uint32_t _address, const uint8_t _value)
		{
			bytes[_address - g_ramBase] = _value;
		}

		void poke32(const uint32_t _address, const uint32_t _value)
		{
			poke8(_address + 0, static_cast<uint8_t>((_value >> 24) & 0xffu));
			poke8(_address + 1, static_cast<uint8_t>((_value >> 16) & 0xffu));
			poke8(_address + 2, static_cast<uint8_t>((_value >>  8) & 0xffu));
			poke8(_address + 3, static_cast<uint8_t>( _value        & 0xffu));
		}

		void poke16(const uint32_t _address, const uint16_t _value)
		{
			poke8(_address + 0, static_cast<uint8_t>((_value >> 8) & 0xffu));
			poke8(_address + 1, static_cast<uint8_t>( _value       & 0xffu));
		}
	};

	// The two callbacks handed to mcf5307_create. Big endian, because the part
	// is.
	uint32_t onRead(void* _user, const uint32_t _address, const int _size, mcf5307_bus_status* _status)
	{
		Ram& ram = *static_cast<Ram*>(_user);

		if(!ram.contains(_address, _size))
		{
			++ram.faults;
			ram.lastFaultAddress = _address;
			*_status = MCF5307_BUS_UNMAPPED;
			return 0;
		}

		uint32_t value = 0;
		for(int i = 0; i < _size; ++i)
			value = (value << 8) | ram.peek8(_address + static_cast<uint32_t>(i));
		return value;
	}

	void onWrite(void* _user, const uint32_t _address, const int _size, const uint32_t _value,
		mcf5307_bus_status* _status)
	{
		Ram& ram = *static_cast<Ram*>(_user);

		if(!ram.contains(_address, _size))
		{
			++ram.faults;
			ram.lastFaultAddress = _address;
			*_status = MCF5307_BUS_UNMAPPED;
			return;
		}

		for(int i = 0; i < _size; ++i)
		{
			const int shift = (_size - 1 - i) * 8;
			ram.poke8(_address + static_cast<uint32_t>(i),
				static_cast<uint8_t>((_value >> shift) & 0xffu));
		}
	}

	// ------------------------------------------------------------ presentation

	std::string toHex(const std::vector<uint8_t>& _bytes)
	{
		std::stringstream ss;
		ss << std::hex << std::setfill('0');
		for(size_t i = 0; i < _bytes.size(); ++i)
		{
			if(i)
				ss << ' ';
			ss << std::setw(2) << static_cast<uint32_t>(_bytes[i]);
		}
		return ss.str();
	}

	std::string toEscaped(const std::vector<uint8_t>& _bytes)
	{
		std::stringstream ss;
		ss << std::hex << std::setfill('0');
		for(const uint8_t b : _bytes)
		{
			if(b == 0x00u)
				ss << "\\0";
			else if(b >= 0x20u && b < 0x7Fu)
				ss << static_cast<char>(b);
			else
				ss << "\\x" << std::setw(2) << static_cast<uint32_t>(b);
		}
		return ss.str();
	}

	std::vector<uint8_t> expectedBytes(const std::string& _text)
	{
		std::vector<uint8_t> out(_text.begin(), _text.end());
		out.push_back(0x00u);
		return out;
	}

	// ------------------------------------------------------------ the run

	struct RunResult
	{
		bool reachedSentinel = false;
		bool budgetExhausted = false;
		bool faulted = false;
		uint32_t busFaults = 0;
		uint32_t lastFaultAddress = 0;
		uint32_t pc = 0;
		uint32_t cycles = 0;
		std::vector<uint8_t> buffer;
	};

	std::vector<uint8_t> g_image;

	// A caller-supplied format string, poked into scratch RAM by every run.
	// The strings are re-poked per run because runSprintf rebuilds the Ram.
	struct Scratch
	{
		std::string text;
		uint32_t address = 0;
	};

	std::vector<Scratch> g_scratch;

	uint32_t addScratch(const std::string& _text)
	{
		const uint32_t address = g_scratchAddr + static_cast<uint32_t>(g_scratch.size()) * 0x40u;
		g_scratch.push_back({_text, address});
		return address;
	}

	void pokeScratch(Ram& _ram)
	{
		for(const Scratch& s : g_scratch)
		{
			for(size_t i = 0; i < s.text.size(); ++i)
				_ram.poke8(s.address + static_cast<uint32_t>(i), static_cast<uint8_t>(s.text[i]));
			_ram.poke8(s.address + static_cast<uint32_t>(s.text.size()), 0x00u);
		}
	}

	// Rebuilds the whole machine for every run. A formatter that left state
	// behind would otherwise make case N depend on case N-1, and this test's
	// job is to isolate.
	bool runSprintf(const uint32_t _formatAddress, const uint32_t _argA, const uint32_t _argB,
		const size_t _readBack, RunResult& _result)
	{
		Ram ram;

		std::memcpy(ram.bytes.data() + (g_loadAddress - g_ramBase), g_image.data(), g_image.size());

		ram.poke16(g_sentinel, g_haltOpcode);

		pokeScratch(ram);

		for(uint32_t i = 0; i < g_bufferSize; ++i)
			ram.poke8(g_bufferAddr + i, g_fill);

		// The frame the call site builds, laid out under the return address.
		const uint32_t sp = g_stackTop - 20u;
		ram.poke32(sp +  0, g_sentinel);
		ram.poke32(sp +  4, g_bufferAddr);
		ram.poke32(sp +  8, _formatAddress);
		ram.poke32(sp + 12, _argA);
		ram.poke32(sp + 16, _argB);

		mcf5307_runtime_init();

		mcf5307_ctx* mcu = mcf5307_create(&ram, &onRead, &onWrite, nullptr);
		if(!mcu)
		{
			std::cout << "FAIL mcf5307_create returned no context" << std::endl;
			return false;
		}

		// reset takes the initial stack pointer and the initial program
		// counter, which is the only way to place the PC across this
		// interface. A7 is then re-stated through set_reg so that the frame
		// above is what the callee sees, whatever reset chose to do with the
		// value it was handed.
		mcf5307_reset(mcu, sp, g_sprintfEntry);
		mcf5307_set_reg(mcu, g_regA7, sp);

		uint32_t spent = 0;
		while(spent < g_cycleBudget)
		{
			spent += mcf5307_exec(mcu, g_cyclesPerIteration);

			if(mcf5307_halted(mcu))
				break;

			if(mcf5307_get_reg(mcu, g_regPc) == g_sentinel)
				break;
		}

		_result.cycles = spent;
		_result.pc = mcf5307_get_reg(mcu, g_regPc);
		_result.faulted = mcf5307_faulted(mcu) != 0;
		// Either PC is proof the formatter returned here: this core fetches the
		// sentinel word, advances the program counter past it and
		// then stops, so the PC settles at sentinel + 2 and the faulted flag is
		// set by the sentinel itself. A stop anywhere else is not a return.
		_result.reachedSentinel = _result.pc == g_sentinel || _result.pc == g_sentinel + 2u;
		_result.budgetExhausted = spent >= g_cycleBudget && !_result.reachedSentinel;
		_result.busFaults = ram.faults;
		_result.lastFaultAddress = ram.lastFaultAddress;

		_result.buffer.clear();
		for(size_t i = 0; i < _readBack; ++i)
			_result.buffer.push_back(ram.peek8(g_bufferAddr + static_cast<uint32_t>(i)));

		mcf5307_destroy(mcu);
		return true;
	}

	// One case: run, print both sides in both renderings, then assert.
	void probe(const std::string& _label, const uint32_t _formatAddress,
		const uint32_t _argA, const uint32_t _argB, const std::string& _expectedText)
	{
		const std::vector<uint8_t> expected = expectedBytes(_expectedText);

		RunResult r;
		if(!runSprintf(_formatAddress, _argA, _argB, expected.size(), r))
		{
			++g_failures;
			++g_cases;
			return;
		}

		std::cout << "---- " << _label << std::endl;
		std::cout << "     format at 0x" << std::hex << _formatAddress << std::dec
			<< ", args " << _argA << ", " << _argB << std::endl;
		std::cout << "     expected hex: " << toHex(expected) << std::endl;
		std::cout << "     actual   hex: " << toHex(r.buffer) << std::endl;
		std::cout << "     expected str: \"" << toEscaped(expected) << "\"" << std::endl;
		std::cout << "     actual   str: \"" << toEscaped(r.buffer) << "\"" << std::endl;
		std::cout << "     pc 0x" << std::hex << r.pc << std::dec
			<< ", cycles " << r.cycles
			<< ", faulted " << (r.faulted ? "yes" : "no")
			<< ", bus faults " << r.busFaults;
		if(r.busFaults)
			std::cout << " (last at 0x" << std::hex << r.lastFaultAddress << std::dec << ")";
		std::cout << std::endl;

		// The first divergence, named, so the report says which byte and not
		// only that the blocks differ.
		size_t firstDiff = expected.size();
		for(size_t i = 0; i < expected.size() && i < r.buffer.size(); ++i)
		{
			if(expected[i] != r.buffer[i])
			{
				firstDiff = i;
				break;
			}
		}
		if(firstDiff < expected.size())
		{
			std::cout << "     FIRST DIVERGENCE at index " << firstDiff
				<< ": expected 0x" << std::hex << std::setw(2) << std::setfill('0')
				<< static_cast<uint32_t>(expected[firstDiff])
				<< ", got 0x" << std::setw(2) << static_cast<uint32_t>(r.buffer[firstDiff])
				<< std::dec << std::setfill(' ') << std::endl;
		}

		check(!r.budgetExhausted, _label + ": the run ended before the cycle budget was exhausted");
		check(r.reachedSentinel, _label + ": the formatter RETURNED to the sentinel");

		// The faulted flag is not asserted on. The sentinel word is what stops
		// the core, and stopping on it sets that
		// flag, so the flag is 1 on every successful run and can discriminate
		// nothing. The two signals that do discriminate are asserted instead:
		// the program counter, which says the formatter returned to exactly the
		// address it was given, and the bus-fault count, which is zero only
		// when every access the formatter made landed inside the flat RAM. The
		// control case below plants a failure and proves both fire.
		check(r.busFaults == 0,
			_label + ": every access the formatter made landed inside the flat RAM");
		checkEqual(toHex(r.buffer), toHex(expected), _label + ": the buffer matches byte for byte");
	}

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

		g_image = readFile(directory + "/CODE_30000400.bin");

		check(!g_image.empty(), "CODE_30000400.bin is readable");
		if(g_image.empty())
			return false;

		check(g_loadAddress - g_ramBase + g_image.size() < g_ramSize,
			"the image fits under the buffer, the scratch, the stack and the sentinel");

		// ---------------- case 0: the two addresses this test depends on
		//
		// A wrong entry point would execute whatever happens to live there and
		// produce a mysterious result. These two checks make that failure
		// explicit, and they read the loaded image rather than restating a
		// number this file already carries.
		{
			const size_t entryOffset = g_sprintfEntry - g_loadAddress;
			const std::vector<uint8_t> prologue(g_image.begin() + static_cast<long>(entryOffset),
			                                    g_image.begin() + static_cast<long>(entryOffset) + 4);
			checkEqual(toHex(prologue), std::string("4e 56 ff a8"),
				"0x300D90C6 begins LINK.W A6,#-0x58, so the entry point is sprintf");

			const size_t formatOffset = g_imageFormat - g_loadAddress;
			std::string fmt;
			for(size_t i = formatOffset; i < g_image.size() && g_image[i]; ++i)
				fmt.push_back(static_cast<char>(g_image[i]));
			checkEqual(fmt, std::string("Version %d.%02d Exp"),
				"0x300E9FEE holds the banner format string");
		}

		// ---------------- case 1: the defect, reproduced or not
		//
		// The firmware's own format, the firmware's own arguments. This is the
		// call the booted machine makes, with the boot removed.
		probe("the banner call, image format", g_imageFormat, 1, 62, "Version 1.62 Exp");

		// ---------------- narrowing
		//
		// Every case below asks one question, and each differs from case 1 in
		// exactly one respect, so a divergence names its own cause.

		// Does the address of the format matter, or its content? Same string,
		// caller-supplied, in RAM the image does not occupy.
		const uint32_t sameFormat = addScratch("Version %d.%02d Exp");

		// Is it the conversions at all? A literal of the same length with no
		// conversion in it. If this one is correct and case 1 is not, the fault
		// is in a conversion; if both are wrong, it is in the copy or the
		// terminator.
		const uint32_t literal16 = addScratch("Version 1.62 Exp");

		// Does the fault depend on output length? One character shorter, one
		// longer, both literals.
		const uint32_t literal15 = addScratch("Version 1.62 Ex");
		const uint32_t literal17 = addScratch("Version 1.62 Expo");

		// The conversions on their own, shortest first.
		const uint32_t justD    = addScratch("%d");
		const uint32_t just02D  = addScratch("%02d");
		const uint32_t twoD     = addScratch("%d.%02d");

		// A conversion not at the end: if the final character alone is wrong,
		// a trailing literal after the last conversion is where it shows.
		const uint32_t dThenTail = addScratch("%d.%02dZ");

		probe("the same format, caller-supplied in RAM", sameFormat, 1, 62, "Version 1.62 Exp");
		probe("a 16-character literal, no conversion",   literal16,  1, 62, "Version 1.62 Exp");
		probe("a 15-character literal, no conversion",   literal15,  1, 62, "Version 1.62 Ex");
		probe("a 17-character literal, no conversion",   literal17,  1, 62, "Version 1.62 Expo");
		probe("%d alone",                                justD,      62, 0, "62");
		probe("%02d alone",                              just02D,    62, 0, "62");
		probe("%d.%02d, conversion last",                twoD,       1, 62, "1.62");
		probe("%d.%02d then one literal character",      dThenTail,  1, 62, "1.62Z");

		// ---------------- the control: a planted failure, traced to the verdict
		//
		// Every case above passes. A harness in which nothing can fail would
		// print exactly that, so one run is deliberately given a format pointer
		// in unmapped space. The two signals the cases assert on must both
		// react, and the buffer must not come back as the banner. Without this
		// the whole file is a green run over inputs that all pass, which proves
		// only that the path is quiet.
		{
			RunResult control;
			const bool ran = runSprintf(0x40000000u, 1, 62, 17, control);
			check(ran, "the control run was constructed");

			std::cout << "---- CONTROL: format pointer in unmapped space" << std::endl;
			std::cout << "     actual   hex: " << toHex(control.buffer) << std::endl;
			std::cout << "     pc 0x" << std::hex << control.pc << std::dec
				<< ", bus faults " << control.busFaults
				<< " (last at 0x" << std::hex << control.lastFaultAddress << std::dec << ")"
				<< std::endl;

			check(control.busFaults > 0,
				"CONTROL: the bus-fault count reacts to an access outside the flat RAM");
			check(toHex(control.buffer) != toHex(expectedBytes("Version 1.62 Exp")),
				"CONTROL: the buffer does NOT come back as the banner, so a byte "
				"comparison can fail");
		}

		return g_failures == 0;
	});

	if(g_failures)
		std::cout << "t1_sprintf_isolated: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
	else
		std::cout << "t1_sprintf_isolated: " << g_cases << " of " << g_cases
			<< " cases passed" << std::endl;

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
