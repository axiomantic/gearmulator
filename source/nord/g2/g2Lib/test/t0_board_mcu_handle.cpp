// The Board's own MCF5307 core is reachable from outside the class: it can be
// reset, stepped, read and asked whether it stopped. Tier T0: this test needs
// no firmware artifact of any kind.
//
// This test builds no mcf5307_ctx of its own, and that is the point of it. A
// check that created a core against Board::onRead and Board::onWrite would pass
// against a Board whose own core is still unreachable, because the core it
// asserted about would be the one it built. Every assertion below is about the
// core the Board constructed.
//
// Why the program and its operand are placed directly into the unit. A
// placement that went through the bus callbacks would depend on the very path
// the second assertion is about, so the setup pokes bytes into the unit and the
// assertion reads back through Board::onRead.
//
// Why there is a second phase. A predicate asserted only in its FALSE state
// cannot be told apart from a body that answers a constant, so the same Board is
// reset a second time at a word the core refuses and both predicates are
// asserted in their TRUE state. The refused word is the core's rather than this
// file's invention.
//
// What this does not establish. It runs a program this file wrote, so a Board
// that starts a synthetic program correctly is no evidence that the firmware
// boots. The program-counter assertion is weaker than it looks: phase one's loop
// already leaves when the counter reaches the code end, so on a passing run that
// assertion largely restates the exit condition rather than witnessing it a
// second time. What it discriminates is the other two ways the loop can leave --
// the bound exhausted, and a halt.
//
// Nothing here uses assert(). The default build defines NDEBUG, so an assert()
// would be removed and a report built on one could never fire.

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

	/* Byte-addressed memory, big-endian, which is what the 68000 family is. It
	 * carries no access record: the assertions here are about the core's state
	 * and about what the bus reads back, not about what arrived at the unit. */
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const uint32_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			uint32_t value = 0u;
			for(int i = 0; i < count; ++i)
				value = (value << 8) | uint32_t(m_bytes[_offset + uint32_t(i)]);

			_status = MCF5307_BUS_OK;
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
		           mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			for(int i = 0; i < count; ++i)
			{
				const int shift = 8 * (count - 1 - i);
				m_bytes[_offset + uint32_t(i)] = uint8_t((_value >> shift) & 0xffu);
			}

			_status = MCF5307_BUS_OK;
		}

		void pokeWord(const uint32_t _offset, const uint16_t _value)
		{
			m_bytes[_offset]      = uint8_t(_value >> 8);
			m_bytes[_offset + 1u] = uint8_t(_value & 0xffu);
		}

		void pokeLong(const uint32_t _offset, const uint32_t _value)
		{
			for(uint32_t i = 0; i < 4u; ++i)
				m_bytes[_offset + i] = uint8_t((_value >> (24u - 8u * i)) & 0xffu);
		}

	private:
		// The unit's own reading of the width, kept deliberately narrow. A width
		// it does not recognise is refused rather than guessed at.
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
	};

	// One window, and the code, the stack and the operands all live in it. The
	// base is the constant memoryMap.h ships; the size and every offset below are
	// this test's own configuration, because no authority records them.
	constexpr uint32_t g_windowBase = g2::g_sdramBase;
	constexpr uint32_t g_windowSize = 0x1000u;
	constexpr uint32_t g_stackTop   = g_windowBase + 0x800u;

	constexpr uint32_t g_codeOffset = 0x000u;
	constexpr uint32_t g_srcOffset  = 0x100u;
	constexpr uint32_t g_dstOffset  = 0x110u;
	constexpr uint32_t g_trapOffset = 0x200u;

	constexpr uint32_t g_codeBase = g_windowBase + g_codeOffset;
	constexpr uint32_t g_srcAddr  = g_windowBase + g_srcOffset;
	constexpr uint32_t g_dstAddr  = g_windowBase + g_dstOffset;
	constexpr uint32_t g_trapBase = g_windowBase + g_trapOffset;

	/* THE PROGRAM. Hand-encoded from the MOVE format: bits 15-14 zero, bits
	 * 13-12 the size (10 long), bits 11-9 the destination register, bits 8-6 the
	 * destination mode, bits 5-3 the source mode, bits 2-0 the source register.
	 * Mode 000 is Dn and mode 010 is (An).
	 *
	 *     2010   move.l (a0),d0     a longword read  at the source address
	 *     2280   move.l d0,(a1)     a longword write at the destination address
	 *
	 * The address registers are seeded through Board::setMcuReg rather than by
	 * instructions, so that a decode this core does not implement cannot be
	 * mistaken for an unreachable core. */
	constexpr uint16_t g_program[] = {0x2010u, 0x2280u};
	constexpr uint32_t g_codeEnd   = g_codeBase + uint32_t(sizeof(g_program));

	/* NOP, which the core implements and which neither halts nor faults. The
	 * span between the program and the operands is filled with it so that a core
	 * running past the code end under the step bound stays in a defined state.
	 * Without that fill, a mutation that only breaks the register read would also
	 * halt the core on undefined memory, and the halt assertion would go red for
	 * a reason that has nothing to do with what was mutated. */
	constexpr uint16_t g_nop = 0x4E71u;

	// The operand. Distinct in every byte, so a transfer of the wrong width
	// lands on a value no other width produces.
	constexpr uint32_t g_operand = 0xC0DEDA7Au;

	// The refused word, from the core's own line-A tests, where a line-A word
	// reaches no operation and traps with the fault flag and the halt flag set
	// and the counter left past the opcode.
	constexpr uint16_t g_lineA    = 0xA001u;
	constexpr uint32_t g_trapEnd  = g_trapBase + 2u;

	// The step bound is what makes a core that makes no progress terminate
	// rather than hang the suite; the assertions after the loop are what make it
	// fail. It is small enough that a core running NOPs from the code end cannot
	// reach the operands.
	constexpr int g_stepBound = 64;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = {g_windowBase, g_windowSize};
		return config;
	}

	uint32_t boardReadLong(g2::Board& _board, const uint32_t _address, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, 4, &_status);
	}

	/* The loop leaves on three conditions and the assertions afterwards tell
	 * them apart. Phase two uses the same predicate and the same bound, and on a
	 * correct core it leaves through the halt rather than through the counter. */
	void stepUntil(g2::Board& _board, const uint32_t _stopPc)
	{
		int steps = 0;
		while(steps < g_stepBound && _board.mcuReg(17) != _stopPc && !_board.mcuHalted())
		{
			(void)_board.runMcu(1);
			++steps;
		}
	}
}

int main()
{
	g2::Board board(makeConfig());

	Ram ram(g_windowSize);
	board.memory().attach(g2::Region::Sdram, &ram);

	for(uint32_t i = 0; i < uint32_t(sizeof(g_program) / sizeof(g_program[0])); ++i)
		ram.pokeWord(g_codeOffset + i * 2u, g_program[i]);

	for(uint32_t offset = g_codeOffset + uint32_t(sizeof(g_program)); offset < g_srcOffset; offset += 2u)
		ram.pokeWord(offset, g_nop);

	ram.pokeLong(g_srcOffset, g_operand);

	ram.pokeWord(g_trapOffset, g_lineA);

	// ==================================================================
	// PHASE ONE -- the program that completes.
	// ==================================================================
	{
		board.resetMcu(g_stackTop, g_codeBase);

		check(board.setMcuReg(8, g_srcAddr), "a0 is seeded with the source address");
		check(board.setMcuReg(9, g_dstAddr), "a1 is seeded with the destination address");

		stepUntil(board, g_codeEnd);

		checkEqual(board.mcuReg(17), g_codeEnd,
		           "phase one, first: the Board's own core ran the program and stopped after the last instruction");

		mcf5307_bus_status status = MCF5307_BUS_OK;
		const uint32_t readBack = boardReadLong(board, g_dstAddr, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "phase one, second: the destination address answers on the Board's own bus path");
		checkEqual(readBack, g_operand,
		           "phase one, second: the longword the program stored reads back through Board::onRead");

		check(!board.mcuHalted(), "phase one, third: the core that completed the program has not halted");
		check(!board.faulted(),   "phase one, third: the core that completed the program has not faulted");
	}

	// ==================================================================
	// PHASE TWO -- the word the core refuses.
	// ==================================================================
	{
		board.resetMcu(g_stackTop, g_trapBase);

		stepUntil(board, g_trapEnd);

		check(board.faulted(),   "phase two, fourth: the refused word left the Board's own core faulted");
		check(board.mcuHalted(), "phase two, fifth: the refused word left the Board's own core halted");
	}

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;

	return g_failures == 0 ? 0 : 1;
}
