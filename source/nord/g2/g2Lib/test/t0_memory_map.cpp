// Task BRD-1. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.1, BRD-1. Design sections 5.2.1, 6.4, 17 row 7.24.
// Logbook: AGENTS.md section 2.2.
//
// WHAT THIS TEST IS FOR. It drives the board's address decode and the two bus
// callbacks the MCF5307 core installs, mcf5307_read_fn and mcf5307_write_fn.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail. Every case below reports through a counter and
// the process exit status.
//
// THE THREE UNRECORDED BASES ARE SUPPLIED BY THIS FIXTURE. AGENTS.md section
// 2.2 records CS1 at 0x11000000, CS3 at 0x13000000 and the CS5 latch at
// 0x15000000, and it records NO address for CS0, CS2 or CS4. Open question 21
// carries all three and SPK-13 reads them from CSAR0 to CSAR5. So this test
// drives TWO DISTINCT BASES for each of the three and asserts the decode
// follows the configuration, which is what makes each parameter known to be
// live rather than decorative. No shipped header carries a number for them.
//
// THE WINDOW SIZES ARE CONFIGURATION FOR THE SAME REASON. No authority
// records the size of any chip-select window, so every size below is a value
// this test chose.

#include "memoryMap.h"

#include <mcf5307.h>

#include <cstdint>
#include <iostream>
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

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	// A target that records every access it answers. It is the observable that
	// says WHICH region a decoded access reached and with WHICH width, so a
	// decode that sends an address to the wrong window fails here rather than
	// passing silently.
	class RecordingTarget final : public g2::BusTarget
	{
	public:
		struct Access
		{
			uint32_t offset = 0;
			int size = 0;
			uint32_t value = 0;
			bool isWrite = false;

			bool operator==(const Access& _other) const
			{
				return offset == _other.offset && size == _other.size
					&& value == _other.value && isWrite == _other.isWrite;
			}
		};

		std::vector<Access> accesses;
		mcf5307_bus_status answer = MCF5307_BUS_OK;
		uint32_t readValue = 0;

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			accesses.push_back(Access{_offset, _size, 0, false});
			_status = answer;
			return answer == MCF5307_BUS_OK ? readValue : 0;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status) override
		{
			accesses.push_back(Access{_offset, _size, _value, true});
			_status = answer;
		}
	};

	// Index helpers that never throw. A mutation run must report EVERY case it
	// fails, and an out-of-range .at() would end the process at the first one.
	std::string logLine(const g2::MemoryMap& _map, const size_t _index)
	{
		if(_index >= _map.log().size())
			return "<no log line " + std::to_string(_index) + ">";
		return _map.log()[_index];
	}

	std::string describe(const RecordingTarget::Access& _access)
	{
		std::ostringstream out;
		out << (_access.isWrite ? "write " : "read ") << _access.size
			<< " bits at offset " << hex32(_access.offset)
			<< " value " << hex32(_access.value);
		return out.str();
	}

	std::string describeAt(const RecordingTarget& _target, const size_t _index)
	{
		if(_index >= _target.accesses.size())
			return "<no access " + std::to_string(_index) + ">";
		return describe(_target.accesses[_index]);
	}

	// A window layout with every base and size chosen by this test. The four
	// recorded bases arrive through the named constants memoryMap.h carries;
	// every other number below belongs to this fixture alone.
	g2::MemoryMapConfig layoutA()
	{
		g2::MemoryMapConfig config;
		config.cs0 = {0x00000000u, 0x00100000u};   // unrecorded, this fixture chose it
		config.cs1 = {g2::g_cs1Base, 0x00000800u};
		config.cs2 = {0x02000000u, 0x00800000u};   // unrecorded, this fixture chose it
		config.cs3 = {g2::g_cs3Base, 0x00000100u};
		config.cs4 = {0x14000000u, 0x00010000u};   // unrecorded, this fixture chose it
		config.cs5 = {g2::g_cs5Base, 0x00000010u};
		config.mbar = {0x10000000u, 0x00000400u};
		config.sdram = {g2::g_sdramBase, 0x00400000u};
		return config;
	}

	// The SAME layout with the three unrecorded bases moved somewhere else and
	// nothing else changed. Every assertion that pairs this with layoutA() is
	// what proves the three parameters are read rather than ignored.
	g2::MemoryMapConfig layoutB()
	{
		g2::MemoryMapConfig config = layoutA();
		config.cs0.base = 0x04000000u;
		config.cs2.base = 0x06000000u;
		config.cs4.base = 0x08000000u;
		return config;
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. The two callbacks satisfy the core's own typedefs.
	//
	// The pointers below are of the core's types, so a signature that drifts
	// from mcf5307.h stops the COMPILE step. Every access in this file then
	// goes THROUGH these pointers rather than calling the functions directly,
	// so the typedef is exercised and not merely declared.
	const mcf5307_read_fn busRead = &g2::memoryMapRead;
	const mcf5307_write_fn busWrite = &g2::memoryMapWrite;

	check(busRead != nullptr, "memoryMapRead is installable as an mcf5307_read_fn");
	check(busWrite != nullptr, "memoryMapWrite is installable as an mcf5307_write_fn");

	// -----------------------------------------------------------------------
	// Case group 2. Every one of the eight windows decodes, at its first byte
	// and at its last byte, and the byte below each window decodes to nothing.
	{
		g2::MemoryMap map(layoutA());
		const g2::MemoryMapConfig config = layoutA();

		struct Expected { g2::Region region; g2::Window window; const char* name; };
		const Expected expected[] = {
			{g2::Region::Cs0,   config.cs0,   "CS0"},
			{g2::Region::Cs1,   config.cs1,   "CS1"},
			{g2::Region::Cs2,   config.cs2,   "CS2"},
			{g2::Region::Cs3,   config.cs3,   "CS3"},
			{g2::Region::Cs4,   config.cs4,   "CS4"},
			{g2::Region::Cs5,   config.cs5,   "CS5"},
			{g2::Region::Mbar,  config.mbar,  "the MBAR window"},
			{g2::Region::Sdram, config.sdram, "the SDRAM"},
		};

		for(const Expected& e : expected)
		{
			checkEqual(map.decode(e.window.base), e.region,
				std::string("the first byte of ") + e.name + " decodes to it");
			checkEqual(map.decode(e.window.base + e.window.size - 1u), e.region,
				std::string("the last byte of ") + e.name + " decodes to it");
			checkEqual(map.decode(e.window.base + e.window.size), g2::Region::None,
				std::string("the byte above ") + e.name + " decodes to nothing");
		}

		// The recorded bases are the ones AGENTS.md section 2.2 carries, and
		// they are asserted as literals here so that a header that renamed or
		// moved one is caught by this test and not by a later boot.
		checkEqual(map.decode(0x11000000u), g2::Region::Cs1, "0x11000000 is CS1");
		checkEqual(map.decode(0x13000000u), g2::Region::Cs3, "0x13000000 is CS3");
		checkEqual(map.decode(0x15000000u), g2::Region::Cs5, "0x15000000 is the CS5 latch");
		checkEqual(map.decode(0x30000000u), g2::Region::Sdram, "0x30000000 is the SDRAM");
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE THREE UNRECORDED BASES ARE LIVE.
	//
	// One layout puts CS0, CS2 and CS4 at one set of bases and the other puts
	// them somewhere else. Each base is asserted to decode in the layout that
	// carries it AND to decode to nothing in the layout that does not. A decode
	// that ignores the configuration and hardcodes a base fails one half of
	// every pair below.
	{
		g2::MemoryMap mapA(layoutA());
		g2::MemoryMap mapB(layoutB());

		checkEqual(mapA.decode(0x00000000u), g2::Region::Cs0, "layout A puts CS0 at its own base");
		checkEqual(mapB.decode(0x00000000u), g2::Region::None, "layout B answers nothing at layout A's CS0 base");
		checkEqual(mapB.decode(0x04000000u), g2::Region::Cs0, "layout B puts CS0 at its own base");
		checkEqual(mapA.decode(0x04000000u), g2::Region::None, "layout A answers nothing at layout B's CS0 base");

		checkEqual(mapA.decode(0x02000000u), g2::Region::Cs2, "layout A puts CS2 at its own base");
		checkEqual(mapB.decode(0x02000000u), g2::Region::None, "layout B answers nothing at layout A's CS2 base");
		checkEqual(mapB.decode(0x06000000u), g2::Region::Cs2, "layout B puts CS2 at its own base");
		checkEqual(mapA.decode(0x06000000u), g2::Region::None, "layout A answers nothing at layout B's CS2 base");

		checkEqual(mapA.decode(0x14000000u), g2::Region::Cs4, "layout A puts CS4 at its own base");
		checkEqual(mapB.decode(0x14000000u), g2::Region::None, "layout B answers nothing at layout A's CS4 base");
		checkEqual(mapB.decode(0x08000000u), g2::Region::Cs4, "layout B puts CS4 at its own base");
		checkEqual(mapA.decode(0x08000000u), g2::Region::None, "layout A answers nothing at layout B's CS4 base");
	}

	// -----------------------------------------------------------------------
	// Case group 4. THE SIZES ARE LIVE TOO.
	//
	// A decode that read the base and ignored the size would pass every case
	// above. Two maps that differ only in one window's size are compared at one
	// address inside the larger window and outside the smaller one.
	{
		g2::MemoryMapConfig small = layoutA();
		small.cs4.size = 0x00000010u;

		g2::MemoryMap wide(layoutA());
		g2::MemoryMap narrow(small);

		checkEqual(wide.decode(0x14000020u), g2::Region::Cs4, "a wide CS4 window answers at base+0x20");
		checkEqual(narrow.decode(0x14000020u), g2::Region::None, "a narrow CS4 window answers nothing at base+0x20");
	}

	// -----------------------------------------------------------------------
	// Case group 4b. THE TIE-BREAK IS A TESTED CLAIM AND NOT A COMMENT.
	//
	// Two chip selects cannot share one base on real hardware, so an
	// overlapping layout is a caller error and not a machine. The decode still
	// has to give ONE answer for it, memoryMap.cpp states which, and this case
	// is what holds that statement to account: the windows are examined in the
	// order of the Region enumeration and the first match wins.
	{
		g2::MemoryMapConfig overlapping = layoutA();
		overlapping.cs4 = overlapping.cs2;

		g2::MemoryMap map(overlapping);

		checkEqual(map.decode(0x02000000u), g2::Region::Cs2,
			"where two windows overlap the earlier region of the enumeration wins");
	}

	// -----------------------------------------------------------------------
	// Case group 5. An unmapped read reports MCF5307_BUS_UNMAPPED through the
	// out-parameter, returns zero, and writes ONE log line that carries the
	// address, the width and the direction.
	//
	// Design section 5.2.1 rule 2 ties the report and the trace together, so
	// the log line is asserted in full rather than by a substring.
	{
		g2::MemoryMap map(layoutA());

		mcf5307_bus_status status = MCF5307_BUS_OK;
		const uint32_t value = busRead(&map, 0x20000000u, 32, &status);

		checkEqual(status, MCF5307_BUS_UNMAPPED, "an unmapped read reports MCF5307_BUS_UNMAPPED");
		checkEqual(value, uint32_t(0), "an unmapped read returns zero");
		checkEqual(map.log().size(), size_t(1), "an unmapped read writes exactly one log line");
		checkEqual(logLine(map, 0),
			std::string("memoryMap: UNMAPPED read of 32 bits at 0x20000000"),
			"the log line of an unmapped read carries the address, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 6. An unmapped write does the same, and the width and the
	// address in the line follow the access rather than being constants.
	{
		g2::MemoryMap map(layoutA());

		mcf5307_bus_status status = MCF5307_BUS_OK;
		busWrite(&map, 0x20000004u, 8, 0x5au, &status);

		checkEqual(status, MCF5307_BUS_UNMAPPED, "an unmapped write reports MCF5307_BUS_UNMAPPED");
		checkEqual(map.log().size(), size_t(1), "an unmapped write writes exactly one log line");
		checkEqual(logLine(map, 0),
			std::string("memoryMap: UNMAPPED write of 8 bits at 0x20000004"),
			"the log line of an unmapped write carries the address, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 7. A decoded window with NO target attached answers nothing.
	// "No device answers at this address" is the status the header defines and
	// an empty socket is that case.
	{
		g2::MemoryMap map(layoutA());

		mcf5307_bus_status status = MCF5307_BUS_OK;
		busRead(&map, 0x30000000u, 32, &status);

		checkEqual(status, MCF5307_BUS_UNMAPPED, "a window with no target attached reports MCF5307_BUS_UNMAPPED");
		checkEqual(logLine(map, 0),
			std::string("memoryMap: UNMAPPED read of 32 bits at 0x30000000"),
			"the log line of an empty window carries the address, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 8. A decoded access reaches the target of its OWN window,
	// with the offset inside that window, and it leaves the status at
	// MCF5307_BUS_OK and the log empty.
	{
		g2::MemoryMap map(layoutA());
		RecordingTarget cs1;
		RecordingTarget sdram;
		map.attach(g2::Region::Cs1, &cs1);
		map.attach(g2::Region::Sdram, &sdram);

		sdram.readValue = 0xdeadbeefu;

		mcf5307_bus_status status = MCF5307_BUS_UNMAPPED;
		busWrite(&map, 0x110007f8u, 16, 0x1234u, &status);

		checkEqual(status, MCF5307_BUS_OK, "a decoded write reports MCF5307_BUS_OK");
		checkEqual(cs1.accesses.size(), size_t(1), "a decoded write reaches the target of its own window once");
		checkEqual(sdram.accesses.size(), size_t(0), "a decoded write reaches no other window's target");
		checkEqual(describeAt(cs1, 0),
			std::string("write 16 bits at offset 0x000007f8 value 0x00001234"),
			"the target sees the offset inside its own window, the width and the value");

		status = MCF5307_BUS_UNMAPPED;
		const uint32_t readBack = busRead(&map, 0x30000010u, 32, &status);

		checkEqual(status, MCF5307_BUS_OK, "a decoded read reports MCF5307_BUS_OK");
		checkEqual(readBack, uint32_t(0xdeadbeefu), "a decoded read returns what the target answered");
		checkEqual(describeAt(sdram, 0),
			std::string("read 32 bits at offset 0x00000010 value 0x00000000"),
			"the read reaches the SDRAM target at the offset inside its own window");
		checkEqual(map.log().size(), size_t(0), "an access that completes writes no log line");
	}

	// -----------------------------------------------------------------------
	// Case group 9. All three widths are carried by ONE callback pair, and the
	// 32-bit case is NATIVE.
	//
	// The ColdFire issues 32-bit bus accesses, so a board that decomposed one
	// into four byte cycles would be a model error. The assertion is the count
	// of target calls, which is one, and the width the target saw, which is 32.
	{
		g2::MemoryMap map(layoutA());
		RecordingTarget sdram;
		map.attach(g2::Region::Sdram, &sdram);

		mcf5307_bus_status status = MCF5307_BUS_OK;
		busWrite(&map, 0x30000000u, 8, 0x11u, &status);
		checkEqual(status, MCF5307_BUS_OK, "an 8-bit write is accepted");
		busWrite(&map, 0x30000002u, 16, 0x2233u, &status);
		checkEqual(status, MCF5307_BUS_OK, "a 16-bit write is accepted");
		busWrite(&map, 0x30000004u, 32, 0x44556677u, &status);
		checkEqual(status, MCF5307_BUS_OK, "a 32-bit write is accepted");

		checkEqual(sdram.accesses.size(), size_t(3),
			"three accesses of three widths make exactly three target calls, so the 32-bit case is not decomposed");
		checkEqual(describeAt(sdram, 0),
			std::string("write 8 bits at offset 0x00000000 value 0x00000011"), "the 8-bit write arrives whole");
		checkEqual(describeAt(sdram, 1),
			std::string("write 16 bits at offset 0x00000002 value 0x00002233"), "the 16-bit write arrives whole");
		checkEqual(describeAt(sdram, 2),
			std::string("write 32 bits at offset 0x00000004 value 0x44556677"), "the 32-bit write arrives whole");
	}

	// -----------------------------------------------------------------------
	// Case group 10. A width that is not 8, 16 or 32 is rejected, the target is
	// never reached, and one log line names the offending width.
	{
		g2::MemoryMap map(layoutA());
		RecordingTarget sdram;
		map.attach(g2::Region::Sdram, &sdram);

		mcf5307_bus_status status = MCF5307_BUS_OK;
		busWrite(&map, 0x30000000u, 24, 0x112233u, &status);

		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "a 24-bit write reports MCF5307_BUS_SIZE_ILLEGAL");
		checkEqual(sdram.accesses.size(), size_t(0), "a rejected width never reaches the target");
		checkEqual(logLine(map, 0),
			std::string("memoryMap: SIZE_ILLEGAL write of 24 bits at 0x30000000"),
			"the log line of a rejected width carries the address, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 11. A target that reports a fault of its own has that status
	// carried back to the core, and the board writes the trace for it.
	{
		g2::MemoryMap map(layoutA());
		RecordingTarget cs3;
		cs3.answer = MCF5307_BUS_FAULT;
		map.attach(g2::Region::Cs3, &cs3);

		mcf5307_bus_status status = MCF5307_BUS_OK;
		busRead(&map, 0x13000000u, 16, &status);

		checkEqual(status, MCF5307_BUS_FAULT, "a target fault is carried back to the core");
		checkEqual(logLine(map, 0),
			std::string("memoryMap: FAULT read of 16 bits at 0x13000000"),
			"the log line of a target fault carries the address, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 12. The decode never aborts. Every address of a sparse sweep
	// returns a status the header defines and never ends the process.
	{
		g2::MemoryMap map(layoutA());

		bool everyStatusIsDefined = true;
		for(uint64_t address = 0; address <= 0xffffffffull; address += 0x00010000ull)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			busRead(&map, uint32_t(address), 32, &status);
			if(status != MCF5307_BUS_OK && status != MCF5307_BUS_UNMAPPED
				&& status != MCF5307_BUS_SIZE_ILLEGAL && status != MCF5307_BUS_FAULT)
			{
				everyStatusIsDefined = false;
				break;
			}
		}

		check(everyStatusIsDefined,
			"a sweep of the whole 32-bit address space returns a defined status at every address and never aborts");
	}

	if(g_failures)
	{
		std::cout << "t0_memory_map: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_memory_map: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
