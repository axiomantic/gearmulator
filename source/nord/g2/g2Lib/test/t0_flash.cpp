// Task BRD-7. Tier T0: this test runs with NMG2_ARTIFACTS unset.
//
// Plan section 9.2, BRD-7. Design section 7.4.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail. Every case below reports through a counter and
// the process exit status.
//
// The properties this test holds the model to:
//   1. CS0 and CS2 are reachable for read at their configured bases, and the
//      reads are big-endian (the ColdFire byte order).
//   2. Longword 0 of the CS0 image is the test's synthetic stack pointer and
//      longword 4 is the test's synthetic program counter. The reset vector
//      is whatever the loaded image carries.
//   3. Reads of three different widths from the same address return the right
//      bytes in the right order. A byte at the high address must be the high
//      byte of a 32-bit read, and the high byte of a 16-bit read.
//   4. Writes are rejected. The flash is read-only: the underlying bytes are
//      not changed, and a log line names the address and the access width.

#include "../flash.h"
#include "baseLib/logging.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

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

	// The CS0 and CS2 bases and sizes come from the test fixture: AGENTS.md
	// section 2.2 records them as unrecorded, and section 1.3 rule 1 forbids
	// writing them into a header.
	constexpr uint32_t kFixtureCs0Base = 0x30000000;
	constexpr uint32_t kFixtureCs0Size = 0x00010000;
	constexpr uint32_t kFixtureCs2Base = 0x20000000;
	constexpr uint32_t kFixtureCs2Size = 0x00010000;

	// The reset-vector longwords. They are the SAME numbers the boot loader
	// places in the real boot ROM but they are owned by this test and they
	// have no Clavia byte behind them. They are read directly against the
	// model, rather than through mcf5307_reset, so the test stays T0 and runs
	// with NMG2_ARTIFACTS unset.
	constexpr uint32_t syntheticSp = 0x00080000;
	constexpr uint32_t syntheticPc = 0x30000100;

	// The rejection messages are emitted through
	// baseLib::logging::logToConsole, whose LogFunc defaults to fputs on
	// stdout (or OutputDebugString on Windows). Neither a std::cout.rdbuf
	// redirect nor a C-stdout redirect can observe that. setLogFunc() is the
	// mechanism baseLib provides for redirecting the messages, so the test
	// installs a capture LogFunc and compares the captured text.
	std::string g_capturedLog;

	void captureLog(const std::string& _s)
	{
		g_capturedLog += _s;
		g_capturedLog += '\n';
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
	// a synthetic program counter, in the shape BRD-6 already uses.

	std::vector<uint8_t> cs0(kFixtureCs0Size, 0xAA);
	cs0[0] = static_cast<uint8_t>((syntheticSp >> 24) & 0xff);
	cs0[1] = static_cast<uint8_t>((syntheticSp >> 16) & 0xff);
	cs0[2] = static_cast<uint8_t>((syntheticSp >>  8) & 0xff);
	cs0[3] = static_cast<uint8_t>( syntheticSp        & 0xff);
	cs0[4] = static_cast<uint8_t>((syntheticPc >> 24) & 0xff);
	cs0[5] = static_cast<uint8_t>((syntheticPc >> 16) & 0xff);
	cs0[6] = static_cast<uint8_t>((syntheticPc >>  8) & 0xff);
	cs0[7] = static_cast<uint8_t>( syntheticPc        & 0xff);
	flash.loadCs0(cs0);

	// ---------------- CS2 image: 0x55 fill, 0x12/0x34/0x56/0x78 at offset 0x10
	//
	// The three reads below exercise the byte order at the same address.

	std::vector<uint8_t> cs2(kFixtureCs2Size, 0x55);
	cs2[0x10] = 0x12;
	cs2[0x11] = 0x34;
	cs2[0x12] = 0x56;
	cs2[0x13] = 0x78;
	flash.loadCs2(cs2);

	// ---------------- reads

	checkEqual(flash.read32(kFixtureCs0Base    ), syntheticSp,
		"longword 0 of the CS0 image reads back as the synthetic stack pointer");
	checkEqual(flash.read32(kFixtureCs0Base + 4), syntheticPc,
		"longword 4 of the CS0 image reads back as the synthetic program counter");

	checkEqual(uint32_t(flash.read8 (kFixtureCs2Base + 0x10)), uint32_t(0x12u),
		"an 8-bit read at the CS2 offset returns the byte at that address");
	checkEqual(uint32_t(flash.read16(kFixtureCs2Base + 0x10)), uint32_t(0x1234u),
		"a 16-bit read is big-endian: the low address is the high byte");
	checkEqual(flash.read32(kFixtureCs2Base + 0x10), uint32_t(0x12345678u),
		"a 32-bit read is big-endian: the low address is the most significant byte");

	// ---------------- writes are rejected; the bytes do not change, and the
	// rejection is REPORTED with the address and the width
	//
	// Byte invariance alone cannot separate a rejection from a write that was
	// never routed to the model at all, so each write is wrapped in a capture
	// of baseLib::logging and the captured text is compared in full. The whole
	// message is compared rather than a fragment of it, so the access width is
	// pinned too: the write8 message carries no width word and the write32
	// message carries "32-bit".

	g_capturedLog.clear();
	flash.write8(kFixtureCs0Base, 0xff);
	checkEqual(flash.read32(kFixtureCs0Base), syntheticSp,
		"an 8-bit write leaves the CS0 bytes unchanged");
	checkEqual(g_capturedLog, std::string("Rejected write to read-only Flash at 0x30000000\n"),
		"the 8-bit write is reported as rejected, naming the address");

	g_capturedLog.clear();
	flash.write32(kFixtureCs2Base + 0x10, 0u);
	checkEqual(flash.read32(kFixtureCs2Base + 0x10), uint32_t(0x12345678u),
		"a 32-bit write leaves the CS2 bytes unchanged");
	checkEqual(g_capturedLog, std::string("Rejected 32-bit write to read-only Flash at 0x20000010\n"),
		"the 32-bit write is reported as rejected, naming the width and the address");

	if(g_failures)
	{
		std::cout << "t0_flash: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_flash: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
