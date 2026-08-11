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
// instructed.
//
// ---------------------------------------------------------------------------
// THE MANUAL IS NOW READ. A PREVIOUS REVISION OF THIS BLOCK CONTRADICTED
// ITSELF.
//
// That revision said "Everything here comes from the manual". Ninety lines
// later it said the manual was not on disk, and it kept one evidence gap open
// for that reason. The two statements cannot both be true. Whoever wrote the
// citations either had the manual or did not.
//
// The manual is now on disk and every citation below is verified against it,
// one by one. The document is:
//
//   MCF5307 ColdFire Integrated Microprocessor User's Manual, Motorola, 1998.
//   456 pages. 27,240,768 bytes.
//   sha256 86cbcc8c9caa933fe10275a975a78d914df86771df9f0bc22d03de8b1aff91fa
//
// It is the part manual, not the errata and not the MCF5206.
//
// ---------------------------------------------------------------------------
// SOURCE: MCF5307 User's Manual. Each citation below was opened and read.
//
//   * The register offsets, widths, reset values and read/write access of the
//     timer, parallel-port and UART registers modelled here: Appendix B,
//     Table B-1, "MCF5307 User Programming Model". VERIFIED. Table B-1 ends at
//     MBAR+$3D4, which is why sim.h models one kilobyte.
//   * The rule that a write to a reserved address has no effect, and the rule
//     that a read of a write-only register returns zeros and a write to a
//     read-only register has no effect: section 9.4.1, Table 9-5, footnotes 1
//     and 3. VERIFIED. Both footnotes say what this file uses them for.
//   * The DRAM controller register map: Table 11-1, "DRAM Controller Memory
//     Map". VERIFIED: DCR $100, DACR0 $108, DMR0 $10C, DACR1 $110, DMR1 $114.
//     The table sits in section 11.5.1, "Asynchronous Memory Map", inside
//     section 11.5. The previous citation said only "section 11.5", which is
//     correct but not exact. It is made exact here.
//   * The timer register map: section 12.3.1, Table 12-1, "Programming Model
//     for Timers". VERIFIED: TMR $140/$180, TRR $144/$184, TCR $148/$188,
//     TCN $14C/$18C, TER $151/$191. Table 12-1 gives offsets only. The widths,
//     reset values and the read-only access of TCR come from Table B-1.
//   * THE ONE ACCESS-WIDTH RULE THIS MODEL ENFORCES: section 14.3.7, "Bus
//     Operation". "All UART module registers must be accessed as bytes."
//     VERIFIED, and the section number is correct. MBAR+$1D0 is UIPCR1, a UART
//     module register.
//   * MBAR+$1D0 is UIPCR1 on read and UACR1 on write. Both are 8 bits.
//     UIPCR1 reset is $0F and its access is read-only. Table B-1. VERIFIED.
//
// SOURCE: this project's own measurements, in AGENTS.md.
//
//   * Port A data at MBAR+$248 bit 9 reads LOW and the bit is an INPUT in
//     every model: sections 2.3 and 4.1. is_expanded() at 0x30037F64 reads it.
//     The manual gives PADAT reset $0000 with no strap; the strap is ours.
//   * MBAR+$1D0 bit 0 reads zero, so the machine is not an Engine: sections
//     2.3 and 4.1. detect_model() at 0x30050864 reads it. This is why the
//     reset value modelled here is $0E and not the manual's $0F.
//   * The G2 wires six chip selects, CS0 to CS5: section 2.2. The part carries
//     eight. CS6 and CS7 are not modelled, because no G2 signal reaches them.
//   * THE CHIP-SELECT REGISTER FAMILY: section 3.8, a FIRMWARE MEASUREMENT.
//     See the MASK-REVISION CONFLICT below.
//
// ---------------------------------------------------------------------------
// THE SECTION 9.4.2.1 CITATION WAS ACCURATE. IT WAS NOT FABRICATED.
//
// This entry corrects the record, and it corrects it against the previous
// revision of this same block.
//
// A revision before that one modelled ONE CSBAR at MBAR+$098 and no CSAR2 to
// CSAR5. It cited MCF5307 UM section 9.4.2.1 and quoted it. The revision that
// replaced it called that reading "withdrawn" and implied the citation was
// unsound. IT WAS NOT. The manual was read on 2026-08-06 and section 9.4.2.1
// says exactly what was quoted:
//
//   Section 9.4.2.1 is titled "CHIP-SELECT ADDRESS REGISTER (CSAR0, CSAR1 AND
//   CSBAR)". It states that CSBAR determines the base address from which
//   chip-selects 2 through 7 are offset.
//
// Eight separate places in the manual agree: sections 9.3 and 9.3.1, Table
// 9-5, section 9.4.2.1 twice, section 9.4.2.2, the section 9.5 code example
// ("CSBAR EQU MBARx+$098"), and the index. Table 9-5 was also read from the
// rendered page, not only from the text layer, so this is not an OCR artefact.
//
// SO THE CONFLICT IS NOT A BAD CITATION. IT IS TWO SILICON MASK REVISIONS.
// The 1998 manual documents the older chip-select layout. The G2 carries the
// newer one. The evidence:
//
//   1. In the 1998 layout, MBAR+$0A4, $0B0, $0BC, $0C8 and $0D4 are marked
//      "Reserved", 32 bits. Those five holes are EXACTLY where CSAR3 to CSAR7
//      sit in the newer layout. The newer layout fills the older one's gaps.
//   2. AGENTS.md section 3.8 MEASURES the boot loader, BOOT_128_Loader.bin,
//      writing CSAR3 at MBAR+$0A4 = $1300, CSMR3 at MBAR+$0A8 = $00000001 and
//      CSCR3 at MBAR+$0AE = $0540. A write to $0A4 is a write to a reserved
//      hole under the 1998 layout, and footnote 1 says it would have no
//      effect. The firmware would then never program CS3 at all.
//   3. The USB driver reaches the ISP1181 at 0x13000000 and 0x13000010. Under
//      the newer layout CSAR3 = $1300 gives base $13000000 directly. Under the
//      1998 layout CS3 is CSBAR<<24 with A[23:21] = 011, which puts it at
//      0x13600000. The driver's own addresses agree with the newer layout and
//      disagree with the 1998 one.
//   4. AGENTS.md section 11 already warned that the Linux m5307sim.h map
//      "carries two chip-select layouts selected by CONFIG_OLDMASK". This is
//      that split, seen from the manual side. CONFIG_OLDMASK is the 1998 one.
//
// THE REVIEWER'S MCF5206 HYPOTHESIS IS REFUTED. The shared-CSBAR text is not
// borrowed from an MCF5206 document. It is in the MCF5307 manual, under the
// MCF5307 name, describing MCF5307 silicon of an earlier mask.
//
// THE MEASUREMENT FIXES THE STRIDE, IT IS NOT EXTRAPOLATED FROM ONE POINT.
// Section 3.8's three offsets are each exactly $24 above the CS0 register of
// the same kind: $0A4-$080 = $0A8-$084 = $0AE-$08A = $24 = 3 x $0C. So the
// family repeats every twelve bytes as CSARn, CSMRn, CSCRn. The manual's own
// section 9.5 code example corroborates three of the intermediate offsets:
// it gives CSMR3 at $0A8, CSMR4 at $0B4 and CSMR5 at $0C0, which are the
// stride's values and NOT the values its own Table 9-5 gives. The 1998 manual
// is internally inconsistent on this point, and its code example agrees with
// the model.
//
// The register widths are corroborated by the widths the firmware itself
// drives in the same section 3.8 measurement: CSARn is a 16-bit value, CSMRn
// a 32-bit value, CSCRn a 16-bit value.
//
// WHERE AGENTS.md AND THE MANUAL CONFLICT, AGENTS.md WINS, and here it wins
// on merit and not only on rank: the firmware measurement and the driver's
// own addresses both point at the newer layout.
//
// ---------------------------------------------------------------------------
// MODELLED VALUES THE 1998 MANUAL CONTRADICTS. REPORTED, NOT CHANGED.
//
// The chip-select rows below do not match the 1998 manual, for the mask
// reason above. The differences are listed so that no later reader has to
// rediscover them:
//
//   $098  modelled CSAR2, 16 bits. 1998 manual: CSBAR, 8 bits, shared by
//         CS2 to CS7.
//   $09C  modelled CSMR2, 32 bits. 1998 manual: CSMR2, 16 bits.
//   $0A4  modelled CSAR3, 16 bits. 1998 manual: reserved, 32 bits.
//   $0A8  modelled CSMR3, 32 bits. 1998 manual: CSMR3 at $0AA, 16 bits.
//         The manual's own section 9.5 example gives $0A8.
//   $0B0  modelled CSAR4, 16 bits. 1998 manual: reserved, 32 bits.
//   $0B4  modelled CSMR4, 32 bits. 1998 manual: CSMR4 at $0B6, 16 bits.
//         The manual's own section 9.5 example gives $0B4.
//   $0BC  modelled CSAR5, 16 bits. 1998 manual: reserved, 32 bits.
//   $0C0  modelled CSMR5, 32 bits. 1998 manual: CSMR5 at $0C2, 16 bits.
//         The manual's own section 9.5 example gives $0C0.
//
// NOTHING IN THE TABLE BELOW WAS CHANGED TO MATCH THE MANUAL. AGENTS.md
// section 1.3 rule 5 requires that a modelled offset, width or reset value
// that the manual contradicts is REPORTED and not silently corrected. The
// operator owns that decision. Every non-chip-select row was checked and
// every one of them agrees with the manual.
//
// ---------------------------------------------------------------------------
// ACCESS WIDTHS: WHAT THE MANUAL ACTUALLY GIVES.
//
// The gap the previous revision left open is now closed, and the answer is
// that the manual states NO access-width rule for the chip-select or DRAM
// controller registers.
//
// The whole manual was searched. It carries exactly ONE statement of this
// kind, in section 14.3.7, and it is about the UART block. No section of
// chapter 9, chapter 11 or chapter 12 states a permitted access size. What
// Table 9-5, Table B-1 and Table 11-1 give is the WIDTH OF EACH REGISTER, not
// the widths a bus access may use. Table 11-1 gives its DRAM rows as BYTE0 to
// BYTE3 columns, which shows where each register sits inside its longword and
// still says nothing about permitted access size. THIS IS WHY AGENTS.md
// section 4.2 calls the manual "hard to read on this point". The reading is
// hard because the answer is not there.
//
// So this model keeps exactly one access-width restriction, on MBAR+$1D0, and
// it still makes NO CLAIM that the chip-select, DRAM controller, timer or
// parallel-port registers are unrestricted on the real part. What is asserted
// about them stays a property of THIS MODEL: it accepts every width. Plan
// section 13.1 requires that shape, so that the check "does not depend on a
// restriction that may not exist". The manual does not let anyone tighten it,
// and it does not let anyone claim the part is permissive either.
//
// DIVERGENCE, STATED RATHER THAN HIDDEN. MBAR+$1D0 belongs to the UART block,
// and task BRD-4 owns UART0. This model answers that one offset because the
// firmware reads it as a MODEL STRAP and BRD-2's check requires it. BRD-4 owns
// every other UART offset.
// ---------------------------------------------------------------------------

#include "sim.h"

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
		// 11.5.1 Table 11-1, section 12.3.1 Table 12-1.
		constexpr RegisterSpec g_registers[] =
		{
			// The six chip selects the G2 wires, one CSARn, CSMRn and CSCRn
			// each. AGENTS.md section 3.8 measures CS3 at $0A4, $0A8 and $0AE
			// and that fixes the twelve-byte stride. THESE ROWS DO NOT MATCH
			// THE 1998 MANUAL, which documents an earlier mask with one shared
			// CSBAR; see the MASK-REVISION CONFLICT at the head of this file
			// for the evidence and for the list of differences. CS6 and CS7
			// exist on the part and are not modelled, because AGENTS.md
			// section 2.2 records that no G2 signal reaches them.
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
			// DEAD DATA.
			//
			// THE REASON IT WAS DEAD IS NOT THE REASON A PREVIOUS REVISION OF
			// THIS COMMENT GAVE. That revision said the constructor read
			// strapBits only on a ReadWrite row and that applying it on every
			// row meant the field "can never go silently dead again". The
			// second half is false. A strap on a read-only row is dead in BOTH
			// forms, because a read-only row is fully protected either way, and
			// the two constructor forms compute the same mask for every input
			// they can be given. See the static_assert block below for the
			// full record. What keeps the field honest is the invariant those
			// static_asserts hold, not the shape of the constructor.
			{0x1d0, 1, Access::ReadOnly,  true,  0x0e,   0x00, "UIPCR"},

			// The parallel port. Port A bit 9 is an input strap.
			{0x244, 2, Access::ReadWrite, false, 0x0000, 0x0000, "PADDR"},
			{0x248, 2, Access::ReadWrite, false, 0x0000, 0x0200, "PADAT"},
		};

		// The bits a write cannot reach, for one byte of one register. A
		// read-only register holds every bit. A read/write register holds only
		// the bits a board strap drives.
		//
		// IT IS constexpr SO THAT THE static_assert BLOCK BELOW CAN GUARD IT.
		// A static_assert is not removed by NDEBUG, so this invariant is
		// checked in the Release build the project ships, and it is checked
		// when the file is compiled rather than by a case somebody has to
		// remember to run.
		constexpr uint8_t protectByte(const Access _access, const uint32_t _strapBits,
			const uint32_t _widthBytes, const uint32_t _byte)
		{
			const int shift = int(8 * (_widthBytes - 1 - _byte));
			const uint8_t base = _access == Access::ReadOnly ? 0xffu : 0x00u;
			return uint8_t(base | uint8_t((_strapBits >> shift) & 0xffu));
		}

		// THE INVARIANT: whatever the access kind, every bit a board strap
		// holds is a bit a write cannot reach. Narrowing either arm breaks the
		// build here instead of going quiet.
		//
		// WHY THIS GUARD AND NOT A CASE IN t0_sim.cpp. An earlier revision
		// wrote that applying strapBits on every row meant the field "can
		// never go silently dead again", and a reviewer found that reverting
		// to the earlier form left all 390 cases green. The reviewer was
		// right about the coverage and the earlier revision was wrong about
		// the reason. THE TWO FORMS ARE THE SAME FUNCTION. Access::ReadOnly
		// already sets every bit of the mask, so `0xff | strap` is `0xff` for
		// every strap, and the read/write arm is `strap` in both forms. All
		// 512 possible inputs were compared and none of them differ. The
		// change made the field REACHABLE, not COVERED, and no runtime case
		// can be red on it because it changes no behaviour at all.
		//
		// So the honest guard is a guard on the invariant the field exists
		// for, and it lives here where it can see the arms. t0_sim.cpp case
		// group 14 recovers the whole mask from behaviour and carries the
		// runtime half of the same claim.
		static_assert((protectByte(Access::ReadWrite, 0x0200u, 2, 0) & 0x02u) == 0x02u,
			"a strap named on a read/write row must reach the write-protect mask");
		static_assert((protectByte(Access::ReadOnly, 0x0200u, 2, 0) & 0x02u) == 0x02u,
			"a strap named on a read-only row must reach the write-protect mask");
		static_assert(protectByte(Access::ReadWrite, 0x0200u, 2, 0) == 0x02u,
			"a read/write row protects the bits a strap holds and no other bit");
		static_assert(protectByte(Access::ReadWrite, 0x0200u, 2, 1) == 0x00u,
			"a byte of a read/write row that carries no strap bit stays fully writable");
		static_assert(protectByte(Access::ReadOnly, 0x0000u, 1, 0) == 0xffu,
			"a read-only row protects every bit");

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

				// The mask comes from protectByte, which the static_assert
				// block above guards. strapBits is applied on BOTH access
				// kinds. That makes the field reachable on every row; it does
				// NOT make the read-only arm observable, because a read-only
				// row is already fully protected. See the note beside the
				// static_asserts.
				m_writeProtect[index] = protectByte(spec.access, spec.strapBits, spec.widthBytes, byte);
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
