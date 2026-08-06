// Task BRD-2. The SIM registers.
//
// Plan section 13.1, BRD-2. Design sections 6.4, 8.2.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1, 4.2.
//
// ---------------------------------------------------------------------------
// PROVENANCE. Plan section 13.1 requires a record of what this task took and
// where each fact was read. This is that record.
//
// NO FILE OF MegabytePhreak/qemu-mcf5307 WAS OPENED FOR THIS TASK, and no
// value below was checked against it. AGENTS.md section 4.2 forbids taking
// expression from that repository and the plan withdrew the harvest it once
// instructed. Everything here comes from the manual.
//
// SOURCE: MCF5307 User's Manual, which AGENTS.md section 11 names.
//
//   * The register offsets, widths, reset values and read/write access of
//     every register modelled here: Appendix B, Table B-1, "MCF5307 User
//     Programming Model".
//   * The chip-select register map, and the rule that a write to a reserved
//     address or a read-only register has no effect and that a read of a
//     write-only register returns zero: section 9.4.1, Table 9-5, footnotes 1
//     and 3.
//   * CS2 to CS7 share one base register: section 9.4.2.1. "CSAR0 and CSAR1
//     determine the base addresses from which chip-selects 0 and 1 will be
//     offset, respectively. CSBAR determines the base address from which
//     chip-selects 2 through 7 will be offset." The part carries no CSAR2 to
//     CSAR5, so the base of CS2 to CS5 is CSBAR.
//   * The DRAM controller register map: section 11.5, Table 11-1.
//   * The timer register map: section 12.3.1, Table 12-1.
//   * THE ONE ACCESS-WIDTH RULE: section 14.3.7. "All UART module registers
//     must be accessed as bytes." MBAR+$1D0 is a UART module register, and it
//     is the only register modelled here that carries a width restriction.
//     No other section of the manual states an access-width rule for the
//     chip-select, DRAM controller, timer or parallel-port registers, and the
//     ACCESS column of Table 9-5 is read/write and not width. So this model
//     accepts every width everywhere else, and inventing a restriction there
//     would be inventing a fact.
//
// SOURCE: this project's own measurements, in AGENTS.md.
//
//   * Port A data at MBAR+$248 bit 9 reads LOW and the bit is an INPUT in
//     every model: sections 2.3 and 4.1. is_expanded() at 0x30037F64 reads it.
//   * MBAR+$1D0 bit 0 reads zero, so the machine is not an Engine: sections
//     2.3 and 4.1. detect_model() at 0x30050864 reads it.
//   * The G2 wires six chip selects, CS0 to CS5: section 2.2. The part carries
//     eight. CS6 and CS7 are not modelled, because no G2 signal reaches them.
//
// DIVERGENCE, STATED RATHER THAN HIDDEN. MBAR+$1D0 belongs to the UART block,
// and task BRD-4 owns UART0. This model answers that one offset because the
// firmware reads it as a MODEL STRAP and BRD-2's check requires it. BRD-4 owns
// every other UART offset.
// ---------------------------------------------------------------------------

#include "sim.h"

#include <cstddef>

namespace g2
{
	namespace
	{
		enum class Access { ReadWrite, ReadOnly };

		struct RegisterSpec
		{
			uint32_t offset;
			uint32_t widthBytes;
			Access access;
			bool byteAccessOnly;
			uint32_t resetValue;
			uint32_t strapBits;     // bits a board strap holds, which a write cannot change
			const char* name;
		};

		// MCF5307 UM Appendix B Table B-1, section 9.4.1 Table 9-5, section
		// 11.5 Table 11-1, section 12.3.1 Table 12-1.
		constexpr RegisterSpec g_registers[] =
		{
			// The six chip selects the G2 wires. CSAR0 and CSAR1 carry the
			// bases of CS0 and CS1; CSBAR carries the base of CS2 to CS5.
			{0x080, 2, Access::ReadWrite, false, 0,      0, "CSAR0"},
			{0x084, 4, Access::ReadWrite, false, 0,      0, "CSMR0"},
			{0x08a, 2, Access::ReadWrite, false, 0,      0, "CSCR0"},
			{0x08c, 2, Access::ReadWrite, false, 0,      0, "CSAR1"},
			{0x090, 4, Access::ReadWrite, false, 0,      0, "CSMR1"},
			{0x096, 2, Access::ReadWrite, false, 0,      0, "CSCR1"},
			{0x098, 1, Access::ReadWrite, false, 0,      0, "CSBAR"},
			{0x09c, 2, Access::ReadWrite, false, 0,      0, "CSMR2"},
			{0x0a2, 2, Access::ReadWrite, false, 0,      0, "CSCR2"},
			{0x0aa, 2, Access::ReadWrite, false, 0,      0, "CSMR3"},
			{0x0ae, 2, Access::ReadWrite, false, 0,      0, "CSCR3"},
			{0x0b6, 2, Access::ReadWrite, false, 0,      0, "CSMR4"},
			{0x0ba, 2, Access::ReadWrite, false, 0,      0, "CSCR4"},
			{0x0c2, 2, Access::ReadWrite, false, 0,      0, "CSMR5"},
			{0x0c6, 2, Access::ReadWrite, false, 0,      0, "CSCR5"},

			// The DRAM controller.
			{0x100, 2, Access::ReadWrite, false, 0,      0, "DCR"},
			{0x108, 4, Access::ReadWrite, false, 0,      0, "DACR0"},
			{0x10c, 4, Access::ReadWrite, false, 0,      0, "DMR0"},
			{0x110, 4, Access::ReadWrite, false, 0,      0, "DACR1"},
			{0x114, 4, Access::ReadWrite, false, 0,      0, "DMR1"},

			// The two timers.
			{0x140, 2, Access::ReadWrite, false, 0x0000, 0, "TMR1"},
			{0x144, 2, Access::ReadWrite, false, 0xffff, 0, "TRR1"},
			{0x148, 2, Access::ReadOnly,  false, 0x0000, 0, "TCR1"},
			{0x14c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN1"},
			{0x151, 1, Access::ReadWrite, false, 0x00,   0, "TER1"},
			{0x180, 2, Access::ReadWrite, false, 0x0000, 0, "TMR2"},
			{0x184, 2, Access::ReadWrite, false, 0xffff, 0, "TRR2"},
			{0x188, 2, Access::ReadOnly,  false, 0x0000, 0, "TCR2"},
			{0x18c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN2"},
			{0x191, 1, Access::ReadWrite, false, 0x00,   0, "TER2"},

			// The model strap. UM Table B-1 gives UIPCR reset $0F; AGENTS.md
			// section 4.1 fixes bit 0 at zero, so this machine presents $0E.
			// A read returns UIPCR and a write reaches UACR, so a write never
			// changes what a read returns.
			{0x1d0, 1, Access::ReadOnly,  true,  0x0e,   0x01, "UIPCR"},

			// The parallel port. Port A bit 9 is an input strap.
			{0x244, 2, Access::ReadWrite, false, 0x0000, 0x0000, "PADDR"},
			{0x248, 2, Access::ReadWrite, false, 0x0000, 0x0200, "PADAT"},
		};

		constexpr size_t g_registerCount = sizeof(g_registers) / sizeof(g_registers[0]);

		const RegisterSpec* find(const uint32_t _offset)
		{
			for(const RegisterSpec& spec : g_registers)
			{
				if(_offset >= spec.offset && (_offset - spec.offset) < spec.widthBytes)
					return &spec;
			}
			return nullptr;
		}

		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}

		std::string hex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for(int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}
	}

	Sim::Sim()
	{
		// Every byte starts write-protected. UM Table 9-5 footnote 1: a write
		// to a reserved address has no effect.
		for(uint8_t& protect : m_writeProtect)
			protect = 0xffu;

		for(const RegisterSpec& spec : g_registers)
		{
			// The part is big-endian, so byte 0 of a register holds its most
			// significant eight bits.
			for(uint32_t byte = 0; byte < spec.widthBytes; ++byte)
			{
				const int shift = int(8 * (spec.widthBytes - 1 - byte));
				const uint32_t index = spec.offset + byte;

				m_space[index] = uint8_t((spec.resetValue >> shift) & 0xffu);

				if(spec.access == Access::ReadWrite)
					m_writeProtect[index] = uint8_t((spec.strapBits >> shift) & 0xffu);
			}
		}
	}

	void Sim::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
	{
		m_log.push_back(std::string("sim: ") + _reason
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at offset " + hex32(_offset));
	}

	uint32_t Sim::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const RegisterSpec* const spec = find(_offset);

		if(!spec)
			logLine("UNMODELLED", false, _size, _offset);
		else if(spec->byteAccessOnly && _size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;
		uint32_t value = 0;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			value <<= 8;
			if(index < g_simSpaceSize)
				value |= m_space[index];
		}

		return value;
	}

	void Sim::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const RegisterSpec* const spec = find(_offset);

		if(!spec)
			logLine("UNMODELLED", true, _size, _offset);
		else if(spec->byteAccessOnly && _size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			if(index >= g_simSpaceSize)
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			const uint8_t incoming = uint8_t((_value >> shift) & 0xffu);
			const uint8_t protect = m_writeProtect[index];

			m_space[index] = uint8_t((m_space[index] & protect) | (incoming & ~protect));
		}
	}
}
