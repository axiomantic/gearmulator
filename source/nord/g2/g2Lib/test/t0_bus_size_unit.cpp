// The unit of the `size` argument the MCF5307 core hands to a board, proved by
// running the core against the board. Tier T0: this test needs no firmware
// artifact of any kind.
//
// Design sections 5.2.1, 6.4.
//
// WHAT DEFECT THIS TEST EXISTS TO CATCH, AND WHY IT COST A MILESTONE. The core
// passes `size` as A COUNT OF BYTES -- 1, 2 or 4 -- and mcf5307.h states it
// twice, once per callback typedef. g2Lib's MemoryMap takes a WIDTH IN BITS --
// 8, 16 or 32 -- and memoryMap.h states that. The two readings disagree on
// EVERY access a core can make, so a board that forwards the argument
// unconverted refuses all of them: the very first instruction fetch presents 2,
// the decode reads 2 as a width in bits, rejects it, and the firmware executes
// zero instructions. That is exactly what happened -- "memoryMap: SIZE_ILLEGAL
// read of 2 bits at 0x30000400", the first fetch of the boot image.
//
// WHY IT SURVIVED. Nothing had ever let the CORE drive the board. Every board
// test up to that point supplied the width by hand, and every one of them
// supplied it in the MemoryMap's unit, so no test ever presented the core's
// unit to the callbacks. A mismatch that is invisible to every caller except
// the one caller that matters is not caught by adding more callers of the same
// kind; it is caught by making the real one drive.
//
// SO THIS TEST DRIVES THE REAL ONE. A real mcf5307 core, created against a real
// g2::Board through Board::onRead and Board::onWrite -- the exact function
// pointers the Board itself installs -- executes a real instruction of each of
// the three widths, and the assertions are about what ARRIVED AT THE UNIT and
// what LANDED IN THE REGISTERS. A test that only checked that 1 maps to 8 would
// be checking the conversion's arithmetic against itself; it would pass against
// a board whose conversion is right and whose forwarding is broken, and it
// would say nothing about the fetch that actually stopped the milestone.
//
// THE NEGATIVE HALF IS NOT OPTIONAL. A conversion that answered "8 bits" to
// anything it did not recognise would make every case below pass while turning
// the callbacks into a funnel that accepts every width. So the sizes the ABI
// CANNOT produce are presented too, and each must be refused. 8, 16 and 32 are
// among them and they are the interesting ones: they are the MemoryMap's own
// legal widths, they are what the pre-conversion callbacks silently ACCEPTED,
// and after the conversion they are byte counts no ColdFire transfer size
// encodes. The set that reaches a unit is asserted to be exactly {1, 2, 4},
// from both directions -- every member accepted, every non-member refused --
// which is the same shape the core's own t_bus_size_unit holds on its side.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG, so an assert() would be removed and a report
// built on one could never fire.

#include "board.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

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

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << hex32(_expected)
		          << ">, got <" << hex32(_actual) << ">" << std::endl;
		++g_failures;
	}

	// -----------------------------------------------------------------------
	// THE RECORDING UNIT.
	//
	// A BusTarget receives the width in the MemoryMap's unit, which this task
	// does not change: a unit still sees 8, 16 or 32. Recording that number is
	// how this test observes what came out of the conversion, and recording the
	// OFFSET and the VALUE beside it is what makes the record a statement about
	// the access rather than about the width alone.
	struct Access
	{
		bool     isWrite  = false;
		uint32_t offset   = 0;
		int      sizeBits = 0;
		uint32_t value    = 0;

		bool operator==(const Access& _other) const
		{
			return isWrite  == _other.isWrite
			    && offset   == _other.offset
			    && sizeBits == _other.sizeBits
			    && value    == _other.value;
		}
	};

	std::string toString(const Access& _access)
	{
		return std::string(_access.isWrite ? "write " : "read  ")
		     + "offset " + hex32(_access.offset)
		     + " of " + std::to_string(_access.sizeBits)
		     + " bits, value " + hex32(_access.value);
	}

	/* Byte-addressed memory that logs every access it answers. Big-endian, which
	 * is what the 68000 family is and what flash.cpp already assembles. */
	class RecordingMemory final : public g2::BusTarget
	{
	public:
		explicit RecordingMemory(const uint32_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				m_accesses.push_back(Access{false, _offset, _size, 0u});
				return 0u;
			}

			uint32_t value = 0u;
			for(int i = 0; i < count; ++i)
				value = (value << 8) | uint32_t(m_bytes[_offset + uint32_t(i)]);

			_status = MCF5307_BUS_OK;
			m_accesses.push_back(Access{false, _offset, _size, value});
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
		           mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				m_accesses.push_back(Access{true, _offset, _size, _value});
				return;
			}

			for(int i = 0; i < count; ++i)
			{
				const int shift = 8 * (count - 1 - i);
				m_bytes[_offset + uint32_t(i)] = uint8_t((_value >> shift) & 0xffu);
			}

			_status = MCF5307_BUS_OK;
			m_accesses.push_back(Access{true, _offset, _size, _value});
		}

		void poke(const uint32_t _offset, const uint8_t _value) { m_bytes[_offset] = _value; }
		uint8_t peek(const uint32_t _offset) const              { return m_bytes[_offset]; }

		const std::vector<Access>& accesses() const { return m_accesses; }

	private:
		// The unit's OWN reading of the width, kept deliberately narrow: 8, 16
		// and 32 and nothing else. A width this unit does not recognise is
		// refused rather than guessed at, so a conversion that invented a width
		// cannot be absorbed here and reported as a success.
		static int byteCount(const int _sizeBits)
		{
			switch(_sizeBits)
			{
			case 8:  return 1;
			case 16: return 2;
			case 32: return 4;
			default: return 0;
			}
		}

		std::vector<uint8_t> m_bytes;
		std::vector<Access>  m_accesses;
	};

	// -----------------------------------------------------------------------
	// THE FIXTURE.
	//
	// TWO WINDOWS AND NO MORE. The code and the stack live in the SDRAM window
	// and the data the program touches lives in the CS3 window, so the CS3
	// record holds THE PROGRAM'S OPERAND ACCESSES AND NOTHING ELSE -- no
	// instruction fetch, no stack traffic. That is what makes an exact-sequence
	// assertion possible instead of a "contains" one.
	//
	// Both bases are the constants memoryMap.h ships from AGENTS.md section 2.2.
	// Every other window is left absent, so the Board's own units answer
	// nowhere; this test asserts nothing about them.
	constexpr uint32_t g_codeBase  = g2::g_sdramBase;
	constexpr uint32_t g_codeSize  = 0x1000u;
	constexpr uint32_t g_stackTop  = g_codeBase + 0x800u;
	constexpr uint32_t g_dataBase  = g2::g_cs3Base;
	constexpr uint32_t g_dataSize  = 0x10u;

	// An address inside no configured window, used for the negative cases: the
	// sizes the ABI can produce must get PAST the width check and fail on the
	// decode instead, and the sizes it cannot must fail on the width check.
	constexpr uint32_t g_unmapped  = 0x20000000u;

	/* THE PROGRAM. Six instructions, one word each, hand-encoded from the MOVE
	 * format: bits 15-14 zero, bits 13-12 the size (01 byte, 11 word, 10 long),
	 * bits 11-9 the destination register, bits 8-6 the destination mode, bits
	 * 5-3 the source mode, bits 2-0 the source register. Mode 000 is Dn and
	 * mode 010 is (An).
	 *
	 *     1010   move.b (a0),d0     a byte  read  at the data window + 0
	 *     3211   move.w (a1),d1     a word  read  at the data window + 4
	 *     2412   move.l (a2),d2     a long  read  at the data window + 8
	 *     1083   move.b d3,(a0)     a byte  write at the data window + 0
	 *     3284   move.w d4,(a1)     a word  write at the data window + 4
	 *     2485   move.l d5,(a2)     a long  write at the data window + 8
	 *
	 * The address registers are loaded through mcf5307_set_reg rather than by
	 * instructions, so that a decode this core does not implement cannot be
	 * mistaken for the bus defect under test. */
	constexpr uint16_t g_program[] =
	{
		0x1010u, 0x3211u, 0x2412u, 0x1083u, 0x3284u, 0x2485u,
	};
	constexpr uint32_t g_codeEnd = g_codeBase + uint32_t(sizeof(g_program));

	// The data the program reads. Distinct in every byte, so a read of the
	// wrong width lands on a value no other width produces.
	constexpr uint8_t g_dataByte  = 0x11u;
	constexpr uint8_t g_dataWord0 = 0x22u;
	constexpr uint8_t g_dataWord1 = 0x33u;
	constexpr uint8_t g_dataLong0 = 0x44u;
	constexpr uint8_t g_dataLong1 = 0x55u;
	constexpr uint8_t g_dataLong2 = 0x66u;
	constexpr uint8_t g_dataLong3 = 0x77u;

	// What the program stores back. Also distinct per width.
	constexpr uint32_t g_storeByte = 0x000000AAu;
	constexpr uint32_t g_storeWord = 0x0000BBBBu;
	constexpr uint32_t g_storeLong = 0xCCCCCCCCu;

	// The three destination registers start all-ones. A sized move into a data
	// register replaces the low bytes and KEEPS the rest, so the preserved high
	// bytes are themselves evidence of the width: a byte move that had been
	// widened to a longword would wipe them.
	constexpr uint32_t g_regFill = 0xFFFFFFFFu;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = {g_codeBase, g_codeSize};
		config.memory.cs3   = {g_dataBase, g_dataSize};
		return config;
	}

	// Every access below goes through Board::onRead and Board::onWrite, never
	// through Board::busRead. The callbacks are the path the CORE takes, and an
	// earlier board test that drove busRead instead passed 65 cases while the
	// callbacks were broken.
	uint32_t boardRead(g2::Board& _board, const uint32_t _address, const int _size,
	                   mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, _size, &_status);
	}

	void boardWrite(g2::Board& _board, const uint32_t _address, const int _size,
	                const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		g2::Board::onWrite(&_board, _address, _size, _value, &_status);
	}

	// A size the ABI cannot produce must be refused by the WIDTH check, before
	// the decode is consulted. Asserting SIZE_ILLEGAL rather than "not OK" is
	// what separates that from the decode's own UNMAPPED answer -- a conversion
	// that mapped an unknown size onto a legal width would report UNMAPPED here
	// and a weaker assertion would call that a pass.
	void checkRefused(g2::Board& _board, const int _size, const std::string& _what)
	{
		mcf5307_bus_status readStatus = MCF5307_BUS_OK;
		(void)boardRead(_board, g_unmapped, _size, readStatus);
		checkEqual(uint32_t(readStatus), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
		           "a read of " + _what + " is refused as an illegal size");

		mcf5307_bus_status writeStatus = MCF5307_BUS_OK;
		boardWrite(_board, g_unmapped, _size, 0u, writeStatus);
		checkEqual(uint32_t(writeStatus), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
		           "a write of " + _what + " is refused as an illegal size");
	}

	// The other direction of the set equality: a size the ABI DOES produce must
	// pass the width check, which is observable as the decode's UNMAPPED answer
	// at an address in no window. A conversion that dropped one of the three
	// would report SIZE_ILLEGAL here instead.
	void checkAccepted(g2::Board& _board, const int _size, const std::string& _what)
	{
		mcf5307_bus_status readStatus = MCF5307_BUS_OK;
		(void)boardRead(_board, g_unmapped, _size, readStatus);
		checkEqual(uint32_t(readStatus), uint32_t(MCF5307_BUS_UNMAPPED),
		           "a read of " + _what + " passes the width check and reaches the decode");

		mcf5307_bus_status writeStatus = MCF5307_BUS_OK;
		boardWrite(_board, g_unmapped, _size, 0u, writeStatus);
		checkEqual(uint32_t(writeStatus), uint32_t(MCF5307_BUS_UNMAPPED),
		           "a write of " + _what + " passes the width check and reaches the decode");
	}

	void checkSequence(const std::vector<Access>& _actual, const std::vector<Access>& _expected,
	                   const std::string& _what)
	{
		++g_cases;

		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}

		std::cout << "FAIL " << _what << std::endl;
		std::cout << "     expected " << _expected.size() << " accesses:" << std::endl;
		for(const Access& access : _expected)
			std::cout << "       " << toString(access) << std::endl;
		std::cout << "     got " << _actual.size() << " accesses:" << std::endl;
		for(const Access& access : _actual)
			std::cout << "       " << toString(access) << std::endl;
		++g_failures;
	}
}

int main()
{
	g2::Board board(makeConfig());

	RecordingMemory code(g_codeSize);
	RecordingMemory data(g_dataSize);

	board.memory().attach(g2::Region::Sdram, &code);
	board.memory().attach(g2::Region::Cs3,   &data);

	// The program and its data are placed directly into the units. This is
	// setup and not the path under test, so it deliberately does not go through
	// the bus: a placement that went through the callbacks would depend on the
	// very conversion the assertions are about.
	for(uint32_t i = 0; i < uint32_t(sizeof(g_program) / sizeof(g_program[0])); ++i)
	{
		code.poke(i * 2u,      uint8_t(g_program[i] >> 8));
		code.poke(i * 2u + 1u, uint8_t(g_program[i] & 0xffu));
	}

	data.poke(0u,  g_dataByte);
	data.poke(4u,  g_dataWord0);
	data.poke(5u,  g_dataWord1);
	data.poke(8u,  g_dataLong0);
	data.poke(9u,  g_dataLong1);
	data.poke(10u, g_dataLong2);
	data.poke(11u, g_dataLong3);

	// ==================================================================
	// PART 1 -- the real core, driving the real board.
	// ==================================================================
	{
		/* THE CORE IS POINTED AT Board::onRead AND Board::onWrite, the exact
		 * pair the Board hands to mcf5307_create for its own context. The
		 * Board's own context is private and cannot be reset or read from
		 * here, so this test creates a second one against the same callbacks
		 * and the same Board -- which is what the boot harness does, and for
		 * the same reason. */
		mcf5307_ctx* mcu = mcf5307_create(&board, &g2::Board::onRead, &g2::Board::onWrite, nullptr);
		check(mcu != nullptr, "the core is created against the board's own callbacks");

		mcf5307_reset(mcu, g_stackTop, g_codeBase);

		check(mcf5307_set_reg(mcu, 8,  g_dataBase)      == 1, "a0 is loaded with the data window base");
		check(mcf5307_set_reg(mcu, 9,  g_dataBase + 4u) == 1, "a1 is loaded with the data window base + 4");
		check(mcf5307_set_reg(mcu, 10, g_dataBase + 8u) == 1, "a2 is loaded with the data window base + 8");
		check(mcf5307_set_reg(mcu, 0,  g_regFill)       == 1, "d0 starts all-ones");
		check(mcf5307_set_reg(mcu, 1,  g_regFill)       == 1, "d1 starts all-ones");
		check(mcf5307_set_reg(mcu, 2,  g_regFill)       == 1, "d2 starts all-ones");
		check(mcf5307_set_reg(mcu, 3,  g_storeByte)     == 1, "d3 holds the byte the program stores");
		check(mcf5307_set_reg(mcu, 4,  g_storeWord)     == 1, "d4 holds the word the program stores");
		check(mcf5307_set_reg(mcu, 5,  g_storeLong)     == 1, "d5 holds the longword the program stores");

		/* Stepping, with a bound. The loop stops when the program counter
		 * reaches the instruction after the last one, so the core never runs
		 * off the end of the program into whatever a zeroed window decodes as.
		 * The bound is what makes a core that makes no progress terminate;
		 * the assertion after the loop is what makes it FAIL. */
		int steps = 0;
		while(steps < 64 && mcf5307_get_reg(mcu, 17) != g_codeEnd && mcf5307_halted(mcu) == 0)
		{
			(void)mcf5307_exec(mcu, 1);
			++steps;
		}

		/* THE PROGRESS ASSERTIONS, WRITTEN SO THAT "NOTHING RAN" FAILS THEM.
		 * A core that faults on its first instruction fetch -- which is what
		 * the unconverted board produces -- leaves the program counter at the
		 * entry point and the fault flag set, so both of these are red in that
		 * state rather than vacuously green. */
		checkEqual(mcf5307_get_reg(mcu, 17), g_codeEnd,
		           "the core executed the whole program and stopped after the last instruction");
		checkEqual(uint32_t(mcf5307_faulted(mcu)), 0u,
		           "no access in the program faulted");

		// ------------------------------------------------------------------
		// WHAT ARRIVED AT THE UNIT. This is the sequence a correct conversion
		// produces and no other conversion does: a byte transfer presents 8
		// bits, a word transfer 16 and a longword transfer 32. A conversion
		// that widened everything to 32 changes all six rows; one that dropped
		// the byte case changes the first and the fourth.
		const std::vector<Access> expected =
		{
			{false, 0u, 8,  uint32_t(g_dataByte)},
			{false, 4u, 16, (uint32_t(g_dataWord0) << 8) | uint32_t(g_dataWord1)},
			{false, 8u, 32, (uint32_t(g_dataLong0) << 24) | (uint32_t(g_dataLong1) << 16)
			              | (uint32_t(g_dataLong2) << 8)  |  uint32_t(g_dataLong3)},
			{true,  0u, 8,  g_storeByte},
			{true,  4u, 16, g_storeWord},
			{true,  8u, 32, g_storeLong},
		};

		checkSequence(data.accesses(), expected,
		              "the three transfer widths reach the unit as 8, 16 and 32 bits, in order");

		// ------------------------------------------------------------------
		// THE INSTRUCTION FETCH ITSELF. It is the access the defect stopped
		// first, and it is a WORD access: the core presents 2 bytes and the
		// unit must see 16 bits. Asserting the first recorded code access
		// pins that directly rather than by implication.
		check(!code.accesses().empty(), "the code window answered at least one access");
		if(!code.accesses().empty())
		{
			const Access expectedFetch{false, 0u, 16, uint32_t(g_program[0])};
			checkSequence({code.accesses().front()}, {expectedFetch},
			              "the first instruction fetch reaches the unit as a 16-bit read of the opcode");
		}

		// ------------------------------------------------------------------
		// WHAT LANDED IN THE REGISTERS. The access record above says what width
		// the unit was asked for; these say what width the core actually
		// CONSUMED. Both are needed: a board that reported the right width and
		// returned the wrong number of bytes would satisfy the record and fail
		// here.
		checkEqual(mcf5307_get_reg(mcu, 0),
		           (g_regFill & 0xFFFFFF00u) | uint32_t(g_dataByte),
		           "the byte read replaced exactly the low byte of d0");
		checkEqual(mcf5307_get_reg(mcu, 1),
		           (g_regFill & 0xFFFF0000u) | (uint32_t(g_dataWord0) << 8) | uint32_t(g_dataWord1),
		           "the word read replaced exactly the low word of d1");
		checkEqual(mcf5307_get_reg(mcu, 2),
		           (uint32_t(g_dataLong0) << 24) | (uint32_t(g_dataLong1) << 16)
		         | (uint32_t(g_dataLong2) << 8)  |  uint32_t(g_dataLong3),
		           "the longword read replaced the whole of d2");

		// ------------------------------------------------------------------
		// WHAT THE STORES LEFT BEHIND. A byte store that had been widened
		// would have overwritten the three bytes after it, so the untouched
		// neighbours are as load-bearing as the written bytes.
		checkEqual(data.peek(0u), g_storeByte & 0xffu,       "the byte store wrote data + 0");
		checkEqual(data.peek(1u), 0u,                        "the byte store left data + 1 alone");
		checkEqual(data.peek(4u), (g_storeWord >> 8) & 0xffu, "the word store wrote data + 4");
		checkEqual(data.peek(5u), g_storeWord & 0xffu,        "the word store wrote data + 5");
		checkEqual(data.peek(6u), 0u,                         "the word store left data + 6 alone");
		checkEqual(data.peek(8u),  (g_storeLong >> 24) & 0xffu, "the longword store wrote data + 8");
		checkEqual(data.peek(9u),  (g_storeLong >> 16) & 0xffu, "the longword store wrote data + 9");
		checkEqual(data.peek(10u), (g_storeLong >> 8)  & 0xffu, "the longword store wrote data + 10");
		checkEqual(data.peek(11u),  g_storeLong        & 0xffu, "the longword store wrote data + 11");
		checkEqual(data.peek(12u), 0u,                          "the longword store left data + 12 alone");

		mcf5307_destroy(mcu);
	}

	// ==================================================================
	// PART 2 -- the set of sizes that reaches a unit is exactly {1, 2, 4}.
	// ==================================================================
	{
		// Every member is accepted.
		checkAccepted(board, 1, "1 byte");
		checkAccepted(board, 2, "2 bytes");
		checkAccepted(board, 4, "4 bytes");

		// No non-member is. 8, 16 and 32 are here because they are the widths
		// the MemoryMap itself calls legal: a callback that forwarded its
		// argument unconverted accepted all three, and a conversion that let
		// them through would be that same defect wearing the new unit.
		checkRefused(board, 8,  "8 bytes, which is the MemoryMap's legal width in the old unit");
		checkRefused(board, 16, "16 bytes, which is the MemoryMap's legal width in the old unit");
		checkRefused(board, 32, "32 bytes, which is the MemoryMap's legal width in the old unit");

		checkRefused(board, 0,  "0 bytes");
		checkRefused(board, 3,  "3 bytes");
		checkRefused(board, 5,  "5 bytes");
		checkRefused(board, -1, "a negative size");

		// A size large enough that a conversion written as a multiplication
		// would overflow. The answer must still be a refusal and not whatever
		// the overflow produced.
		checkRefused(board, 0x40000000, "a size that would overflow a bits conversion");
	}

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;

	return g_failures == 0 ? 0 : 1;
}
