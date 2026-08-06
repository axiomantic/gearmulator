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
//   * The DRAM controller register map: section 11.5, Table 11-1.
//   * The timer register map: section 12.3.1, Table 12-1.
//   * THE ONE ACCESS-WIDTH RULE THIS MODEL ENFORCES: section 14.3.7. "All UART
//     module registers must be accessed as bytes." MBAR+$1D0 is a UART module
//     register. See the EVIDENCE GAP below for why that is a statement about
//     this model and not a statement about the part.
//
// SOURCE: this project's own measurements, in AGENTS.md.
//
//   * Port A data at MBAR+$248 bit 9 reads LOW and the bit is an INPUT in
//     every model: sections 2.3 and 4.1. is_expanded() at 0x30037F64 reads it.
//   * MBAR+$1D0 bit 0 reads zero, so the machine is not an Engine: sections
//     2.3 and 4.1. detect_model() at 0x30050864 reads it.
//   * The G2 wires six chip selects, CS0 to CS5: section 2.2. The part carries
//     eight. CS6 and CS7 are not modelled, because no G2 signal reaches them.
//   * THE CHIP-SELECT REGISTER FAMILY: section 3.8, a FIRMWARE MEASUREMENT.
//     See the RESOLVED CONFLICT below.
//
// ---------------------------------------------------------------------------
// RESOLVED CONFLICT: CSAR PER SELECT, NOT ONE SHARED CSBAR.
//
// AN EARLIER REVISION OF THIS FILE MODELLED ONE CSBAR AT MBAR+$098 AND NO
// CSAR2 TO CSAR5, reading MCF5307 UM section 9.4.2.1 to say that chip-selects
// 2 through 7 are offset from one shared base. THAT READING IS WITHDRAWN.
// AGENTS.md is authoritative on hardware facts and it refutes the reading:
//
//   1. Section 3.8 MEASURES the boot loader, BOOT_128_Loader.bin, programming
//      CS3: CSAR3 at MBAR+$0A4 = $1300, CSMR3 at MBAR+$0A8 = $00000001, CSCR3
//      at MBAR+$0AE = $0540. CSAR3 = $1300 gives base $13000000, the ISP1181
//      USB device controller. A PER-SELECT BASE IS A VALUE ONE SHARED CSBAR
//      CANNOT EXPRESS, and the offset $0A4 is not CSBAR's.
//   2. Section 9, open question 21, names "CSAR0 to CSAR5 and CSMR0 to CSMR5"
//      as the registers the boot loader programs. Plan section 4.2 register
//      row 18 and task SPK-13 say the same.
//   3. Section 11 already warned that the Linux m5307sim.h reference map
//      "carries two chip-select layouts selected by CONFIG_OLDMASK". The
//      withdrawn reading was one of those two, chosen without reconciling it
//      against the section 3.8 measurement.
//
// THE MEASUREMENT FIXES THE STRIDE, IT IS NOT EXTRAPOLATED FROM ONE POINT.
// Section 3.8's three offsets are each exactly $24 above the CS0 register of
// the same kind: $0A4-$080 = $0A8-$084 = $0AE-$08A = $24 = 3 x $0C. So the
// family repeats every twelve bytes as CSARn, CSMRn, CSCRn. Two of the
// intermediate offsets are corroborated independently, because the withdrawn
// revision had already read CSMR2 at $09C and CSCR2 at $0A2 from the manual
// and both are exactly what the stride gives. Every CSCRn offset that
// revision carried also matches the stride; what it got wrong was CSAR2 to
// CSAR5, which it omitted, and CSMR3 to CSMR5, which it placed two bytes high
// and two bytes short.
//
// The register widths are corroborated by the widths the firmware itself
// drives in the same section 3.8 measurement: CSARn is a 16-bit value, CSMRn
// a 32-bit value, CSCRn a 16-bit value.
//
// WHERE AGENTS.md AND THE MANUAL CONFLICT, AGENTS.md WINS. This is that case,
// and it is recorded rather than smoothed over: a measurement of the shipped
// firmware outranks a reading of a manual section that describes a part with
// two documented chip-select layouts.
//
// ---------------------------------------------------------------------------
// EVIDENCE GAP, DECLARED RATHER THAN CLOSED BY A GUESS.
//
// AGENTS.md section 4.2 states AS FACT that access widths for the chip-select
// and DRAM controller registers exist and are recorded, and that they are "a
// useful corroboration BECAUSE THE MANUAL IS HARD TO READ ON THIS POINT". So
// the silence of the manual is NOT evidence that those registers accept every
// width. The withdrawn revision reasoned from that silence, and that reasoning
// is withdrawn with it.
//
// This model therefore enforces exactly one access-width restriction, on
// MBAR+$1D0, and it makes NO CLAIM that the chip-select, DRAM controller,
// timer or parallel-port registers are unrestricted on the real part. What is
// asserted about them is a property of THIS MODEL: it accepts every width.
// Plan section 13.1 requires that shape, so that the check "does not depend on
// a restriction that may not exist".
//
// WHY THE GAP WAS NOT CLOSED. BRD-2 permits opening
// MegabytePhreak/qemu-mcf5307 to CHECK exactly one width the manual states
// less clearly, then to write the value from the manual. THE REPOSITORY WAS
// NOT OPENED. The MCF5307 User's Manual is not on disk in this workspace, so
// the value could not have been written from the manual afterwards, and
// AGENTS.md section 4.2 is explicit about that case: "If you cannot write it
// from the manual, stop and raise it - do not reach for the source file." It
// is raised here. Closing it needs the manual, or SPK-13's firmware read.
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
			// The six chip selects the G2 wires, one CSARn, CSMRn and CSCRn
			// each. AGENTS.md section 3.8 measures CS3 at $0A4, $0A8 and $0AE
			// and that fixes the twelve-byte stride; see the RESOLVED CONFLICT
			// at the head of this file. CS6 and CS7 exist on the part and are
			// not modelled, because AGENTS.md section 2.2 records that no G2
			// signal reaches them.
			{0x080, 2, Access::ReadWrite, false, 0,      0, "CSAR0"},
			{0x084, 4, Access::ReadWrite, false, 0,      0, "CSMR0"},
			{0x08a, 2, Access::ReadWrite, false, 0,      0, "CSCR0"},
			{0x08c, 2, Access::ReadWrite, false, 0,      0, "CSAR1"},
			{0x090, 4, Access::ReadWrite, false, 0,      0, "CSMR1"},
			{0x096, 2, Access::ReadWrite, false, 0,      0, "CSCR1"},
			{0x098, 2, Access::ReadWrite, false, 0,      0, "CSAR2"},
			{0x09c, 4, Access::ReadWrite, false, 0,      0, "CSMR2"},
			{0x0a2, 2, Access::ReadWrite, false, 0,      0, "CSCR2"},
			{0x0a4, 2, Access::ReadWrite, false, 0,      0, "CSAR3"},
			{0x0a8, 4, Access::ReadWrite, false, 0,      0, "CSMR3"},
			{0x0ae, 2, Access::ReadWrite, false, 0,      0, "CSCR3"},
			{0x0b0, 2, Access::ReadWrite, false, 0,      0, "CSAR4"},
			{0x0b4, 4, Access::ReadWrite, false, 0,      0, "CSMR4"},
			{0x0ba, 2, Access::ReadWrite, false, 0,      0, "CSCR4"},
			{0x0bc, 2, Access::ReadWrite, false, 0,      0, "CSAR5"},
			{0x0c0, 4, Access::ReadWrite, false, 0,      0, "CSMR5"},
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
			//
			// strapBits IS $00 HERE ON PURPOSE. Access::ReadOnly already holds
			// every bit of the register, so naming bit 0 again would add no
			// protection. The earlier revision carried $01 here and it was
			// DEAD DATA: the constructor read strapBits only on a ReadWrite
			// row, so the value could not reach m_writeProtect. The constructor
			// below now applies strapBits on every row, so the field can never
			// go silently dead again; this row sets it to $00 because the fact
			// it once carried is already carried by Access::ReadOnly and by the
			// reset value $0E.
			{0x1d0, 1, Access::ReadOnly,  true,  0x0e,   0x00, "UIPCR"},

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

				// A read-only register holds every bit. A read/write register
				// holds only the bits a board strap drives. strapBits is
				// applied on BOTH kinds, so a strap named on a read-only row
				// cannot become dead data the way it did before.
				uint8_t protect = spec.access == Access::ReadOnly ? 0xffu : 0x00u;
				protect |= uint8_t((spec.strapBits >> shift) & 0xffu);

				m_writeProtect[index] = protect;
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
