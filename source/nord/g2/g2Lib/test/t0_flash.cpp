// The read-only flash model. Tier T0: this test runs with NMG2_ARTIFACTS
// unset.
//
// The properties this test holds the model to:
//   1. CS0 and CS2 are reachable for read at their configured bases, and the
//      reads are big-endian (the ColdFire byte order).
//   2. Longword 0 of the CS0 image is the test\'s synthetic stack pointer and
//      longword 4 is the test\'s synthetic program counter. The reset vector
//      is whatever the loaded image carries.
//   3. Reads of three different widths from the same address return the right
//      bytes in the right order. A byte at the high address must be the high
//      byte of a 32-bit read, and the high byte of a 16-bit read.
//   4. Writes are rejected. The flash is read-only: the underlying bytes are
//      not changed, and a log line names the address and the access width.
//   5. An image larger than the configured size is refused. The load returns
//      false, reports the refusal, and leaves the contents alone.

#include "../flash.h"
#include "baseLib/logging.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	// The CS0 and CS2 bases and sizes come from the test fixture: no authority
	// records them, so no shipped header carries a number for them.
	constexpr uint32_t kFixtureCs0Base = 0x30000000;
	constexpr uint32_t kFixtureCs0Size = 0x00010000;
	constexpr uint32_t kFixtureCs2Base = 0x20000000;
	constexpr uint32_t kFixtureCs2Size = 0x00010000;

	// The reset-vector longwords, owned by this test.
	constexpr uint32_t syntheticSp = 0x00080000;
	constexpr uint32_t syntheticPc = 0x30000100;

	// The rejection messages are emitted through
	// baseLib::logging::logToConsole, whose LogFunc defaults to fputs on
	// stdout (or OutputDebugString on Windows). Neither a std::cout.rdbuf
	// redirect nor a C-stdout redirect can observe that. SetLogFunc() is the
	// mechanism baseLib provides for redirecting the messages, so the test
	// installs a capture LogFunc and asserts on the captured text. If a
	// future refactor drops or rewrites the rejection log, the capture
	// function is never called with the expected text and the assertions
	// below fail.
	std::string g_capturedLog;

	void captureLog(const std::string& _s)
	{
		g_capturedLog += _s;
		g_capturedLog += '\n';
	}

	bool contains(const std::string& _haystack, const std::string& _needle)
	{
		return _haystack.find(_needle) != std::string::npos;
	}

	// The default build is Release and defines NDEBUG, which removes assert()
	// from the translation unit entirely. A case written with assert() is then
	// not a check at all: the test runs the calls, evaluates nothing, and
	// exits 0. Every case below therefore reports through this counter, which
	// the exit status reads, so the test fails in every build type.
	int failures = 0;

	void check(const bool _condition, const char* const _what)
	{
		if(_condition)
			return;

		std::cout << "FAIL " << _what << std::endl;
		++failures;
	}

	void checkEqual(const uint32_t _observed, const uint32_t _expected, const char* const _what)
	{
		if(_observed == _expected)
			return;

		std::cout << "FAIL " << _what << ": observed 0x" << std::hex << _observed
			<< ", expected 0x" << _expected << std::dec << std::endl;
		++failures;
	}
}

int main()
{
	g2::Flash flash(kFixtureCs0Base, kFixtureCs0Size, kFixtureCs2Base, kFixtureCs2Size);

	// Every rejection message below goes through baseLib::logging, so capture
	// it from the first write onward.
	baseLib::logging::setLogFunc(&captureLog);

	// ---------------- CS0 image: reset vector at offsets 0 and 4, 0xAA fill
	//
	// The test sets longword 0 to a synthetic stack pointer and longword 4 to
	// a synthetic program counter.

	std::vector<uint8_t> cs0(kFixtureCs0Size, 0xAA);
	cs0[0] = static_cast<uint8_t>((syntheticSp >> 24) & 0xff);
	cs0[1] = static_cast<uint8_t>((syntheticSp >> 16) & 0xff);
	cs0[2] = static_cast<uint8_t>((syntheticSp >>  8) & 0xff);
	cs0[3] = static_cast<uint8_t>( syntheticSp        & 0xff);
	cs0[4] = static_cast<uint8_t>((syntheticPc >> 24) & 0xff);
	cs0[5] = static_cast<uint8_t>((syntheticPc >> 16) & 0xff);
	cs0[6] = static_cast<uint8_t>((syntheticPc >>  8) & 0xff);
	cs0[7] = static_cast<uint8_t>( syntheticPc        & 0xff);
	check(flash.loadCs0(cs0), "a CS0 image of exactly the configured size loads");

	// ---------------- CS2 image: 0x55 fill, 0x12/0x34/0x56/0x78 at offset 0x10
	//
	// The three reads below exercise the byte order at the same address.

	std::vector<uint8_t> cs2(kFixtureCs2Size, 0x55);
	cs2[0x10] = 0x12;
	cs2[0x11] = 0x34;
	cs2[0x12] = 0x56;
	cs2[0x13] = 0x78;
	check(flash.loadCs2(cs2), "a CS2 image of exactly the configured size loads");

	// ---------------- reads

	checkEqual(flash.read32(kFixtureCs0Base    ), syntheticSp, "longword 0 of CS0 is the stack pointer");
	checkEqual(flash.read32(kFixtureCs0Base + 4), syntheticPc, "longword 4 of CS0 is the program counter");

	checkEqual(flash.read8 (kFixtureCs2Base + 0x10), 0x12u,       "read8 returns the byte at the address");
	checkEqual(flash.read16(kFixtureCs2Base + 0x10), 0x1234u,     "read16 is big-endian");
	checkEqual(flash.read32(kFixtureCs2Base + 0x10), 0x12345678u, "read32 is big-endian");

	// ---------------- an oversize image is refused, and refused without
	// touching the contents
	//
	// The refusal is a return value rather than an exception, so the case has
	// to read the return. A load that silently truncated would return true
	// here and would leave a different byte under the boot vector, so both
	// halves are checked.
	{
		const std::vector<uint8_t> oversize(kFixtureCs0Size + 1, 0x5A);

		g_capturedLog.clear();

		check(!flash.loadCs0(oversize), "a CS0 image larger than CS0 is refused");
		check(contains(g_capturedLog, "Refused CS0 image"), "the CS0 refusal is reported");
		checkEqual(flash.read32(kFixtureCs0Base), syntheticSp, "a refused CS0 load leaves the contents alone");

		const std::vector<uint8_t> oversize2(kFixtureCs2Size + 1, 0x5A);

		g_capturedLog.clear();

		check(!flash.loadCs2(oversize2), "a CS2 image larger than CS2 is refused");
		check(contains(g_capturedLog, "Refused CS2 image"), "the CS2 refusal is reported");
		checkEqual(flash.read32(kFixtureCs2Base + 0x10), 0x12345678u, "a refused CS2 load leaves the contents alone");
	}

	// ---------------- writes are rejected; the bytes do not change, and the
	// rejection is REPORTED with the address and the width
	//
	// The byte invariance above is necessary but not sufficient: a refactor
	// that silently dropped the rejection log would keep every assertion
	// green. Each write is therefore wrapped in a capture of
	// baseLib::logging, and the captured text must name both 'Rejected' and
	// the exact address of the write. The full message prefix also locks the
	// access width -- the write8 message carries no width word, the write32
	// message carries "32-bit" -- so a width routed to the wrong message
	// fails here as well.
	g_capturedLog.clear();
	flash.write8(kFixtureCs0Base, 0xff);
	checkEqual(flash.read32(kFixtureCs0Base), syntheticSp, "write8 does not change the bytes");
	check(contains(g_capturedLog, "Rejected write to read-only Flash at 0x30000000"),
		"write8 is reported as rejected, with its address");

	g_capturedLog.clear();
	flash.write32(kFixtureCs2Base + 0x10, 0u);
	checkEqual(flash.read32(kFixtureCs2Base + 0x10), 0x12345678u, "write32 does not change the bytes");
	check(contains(g_capturedLog, "Rejected 32-bit write to read-only Flash at 0x20000010"),
		"write32 is reported as rejected, with its width and address");

	if(failures != 0)
	{
		std::cout << "t0_flash: " << failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_flash passed" << std::endl;
	return 0;
}
