// Task BRD-2. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.1, BRD-2. Design sections 6.4, 8.2.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1, 4.2.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail.
//
// THE REGISTER TABLE BELOW IS WRITTEN OUT AGAIN, ON PURPOSE. It is read from
// the MCF5307 User's Manual and not from sim.cpp. A test that imported the
// implementation's own table would assert that the table equals itself.
//
// THE ACCESS IS DRIVEN THROUGH THE BRD-1 DECODE. The SIM sits in the MBAR
// window of a MemoryMap and every access below goes through
// memoryMapRead/memoryMapWrite, so the offset the SIM sees is produced by the
// decode and not by this test.
//
// THE CHIP-SELECT FAMILY BELOW IS AGENTS.md SECTION 3.8, NOT THE MANUAL.
// Section 3.8 is a FIRMWARE MEASUREMENT of BOOT_128_Loader.bin programming
// CS3 - CSAR3 $0A4 = $1300, CSMR3 $0A8 = $00000001, CSCR3 $0AE = $0540 - and
// AGENTS.md is authoritative on hardware facts. Case group 8 drives those
// three measured offsets directly.
//
// THE SHARED-CSBAR READING AN EARLIER REVISION CARRIED WAS A CORRECT READING
// OF THE 1998 MANUAL. It was checked against the manual on 2026-08-06 and
// section 9.4.2.1 says what that revision quoted. The 1998 manual documents an
// earlier silicon mask; the G2 carries a later one. sim.cpp's MASK-REVISION
// CONFLICT block holds the full record and lists every modelled value the 1998
// manual contradicts. Do not read the rows below as a manual disagreement that
// somebody already settled in the model's favour. They are a mask difference,
// and the operator owns whether the model keeps them.
//
// THE TWO WIDTH CLASSES. Plan section 13.1 requires that every modelled
// register is in exactly one of two classes, so that the check does not depend
// on a restriction that may not exist:
//
//   * RESTRICTED. A width other than the one stated must be REJECTED and must
//     write one log line. MCF5307 UM section 14.3.7 states one such rule for
//     the registers this task models: "All UART module registers must be
//     accessed as bytes." MBAR+$1D0 is a UART module register.
//   * UNRESTRICTED. Every width must be ACCEPTED.
//
// WHAT THE UNRESTRICTED CLASS DOES AND DOES NOT CLAIM. It claims that THIS
// MODEL accepts every width at those offsets. IT DOES NOT CLAIM THAT THE REAL
// PART DOES. An earlier revision of this file asserted "every other modelled
// register carries no width restriction" and rested that on the manual saying
// nothing. THAT REASONING STAYS WITHDRAWN, and the manual now confirms why it
// had to be. The whole manual was searched on 2026-08-06. It states exactly
// one access-width rule, section 14.3.7, and that rule is about the UART
// block. Chapters 9, 11 and 12 state none. The WIDTH columns of Table 9-5,
// Table B-1 and Table 11-1 give the width of each REGISTER, not the widths a
// bus access may use. So the manual cannot close this either way: it does not
// license a restriction and it does not license a claim of permissiveness.
// sim.cpp records the finding. SPK-13's firmware read is now the only route
// to a positive answer.

#include "memoryMap.h"
#include "sim.h"

#include <mcf5307.h>

#include <cstdint>
#include <iostream>
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

	// This fixture chooses the MBAR base. AGENTS.md section 4.2 records that
	// the boot loader programs MBAR through MOVEC, so the value is a firmware
	// choice and not a property of the part.
	constexpr uint32_t g_mbarBase = 0x10000000u;
	constexpr uint32_t g_mbarSize = 0x00000400u;

	// MBAR+$1D0 is UIPCR on a read and UACR on a write, so from a reader's
	// side it is ReadOnly: a write reaches UACR and never changes what a read
	// returns. That is why no WriteOnly kind is needed here.
	enum class Kind { ReadWrite, ReadOnly };

	struct RegisterFact
	{
		uint32_t offset;
		int widthBits;      // UM Table B-1, the register's own width
		Kind kind;          // UM Table 9-5 footnote 3
		bool byteAccessOnly;// UM section 14.3.7, the UART module
		uint32_t resetValue;
		bool resetIsKnown;  // "uninitialized" in UM Table B-1 for many
		uint32_t readOnlyBits; // bits a strap holds and a write cannot change
		const char* name;
	};

	// The chip-select family: AGENTS.md section 3.8. Everything else: MCF5307
	// User's Manual, Appendix B Table B-1, section 9.4.1 Table 9-5, section
	// 11.5.1 Table 11-1 and section 12.3.1 Table 12-1. Every one of those
	// citations was verified against the manual on 2026-08-06.
	//
	// EACH CHIP SELECT CARRIES ITS OWN CSARn ON THE MASK THE G2 CARRIES.
	// Section 3.8 measures the boot loader writing CSAR3 at $0A4, CSMR3 at $0A8
	// and CSCR3 at $0AE, and each of the three sits exactly $24 above the CS0
	// register of the same kind, so the family repeats every twelve bytes.
	// CSAR3 = $1300 is a PER-SELECT base, $13000000, and the USB driver reaches
	// the ISP1181 at exactly that address.
	//
	// THE 1998 MANUAL DOCUMENTS A DIFFERENT LAYOUT, one shared 8-bit CSBAR at
	// $098 and no CSAR2 to CSAR7. That is a real MCF5307 layout on an earlier
	// mask, not an error. The rows below therefore DISAGREE WITH THE MANUAL at
	// $098, $09C, $0A4, $0A8, $0B0, $0B4, $0BC and $0C0. sim.cpp lists each
	// difference. The disagreement is reported and is not settled here.
	const RegisterFact g_registers[] =
	{
		{0x080, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR0"},
		{0x084, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR0"},
		{0x08a, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR0"},
		{0x08c, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR1"},
		{0x090, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR1"},
		{0x096, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR1"},
		{0x098, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR2"},
		{0x09c, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR2"},
		{0x0a2, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR2"},
		{0x0a4, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR3"},
		{0x0a8, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR3"},
		{0x0ae, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR3"},
		{0x0b0, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR4"},
		{0x0b4, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR4"},
		{0x0ba, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR4"},
		{0x0bc, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR5"},
		{0x0c0, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR5"},
		{0x0c6, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR5"},

		{0x100, 16, Kind::ReadWrite, false, 0, false, 0, "DCR"},
		{0x108, 32, Kind::ReadWrite, false, 0, false, 0, "DACR0"},
		{0x10c, 32, Kind::ReadWrite, false, 0, false, 0, "DMR0"},
		{0x110, 32, Kind::ReadWrite, false, 0, false, 0, "DACR1"},
		{0x114, 32, Kind::ReadWrite, false, 0, false, 0, "DMR1"},

		{0x140, 16, Kind::ReadWrite, false, 0x0000, true, 0, "TMR1"},
		{0x144, 16, Kind::ReadWrite, false, 0xffff, true, 0, "TRR1"},
		{0x148, 16, Kind::ReadOnly,  false, 0x0000, true, 0, "TCR1"},
		{0x14c, 16, Kind::ReadWrite, false, 0x0000, true, 0, "TCN1"},
		{0x151,  8, Kind::ReadWrite, false, 0x00,   true, 0, "TER1"},
		{0x180, 16, Kind::ReadWrite, false, 0x0000, true, 0, "TMR2"},
		{0x184, 16, Kind::ReadWrite, false, 0xffff, true, 0, "TRR2"},
		{0x188, 16, Kind::ReadOnly,  false, 0x0000, true, 0, "TCR2"},
		{0x18c, 16, Kind::ReadWrite, false, 0x0000, true, 0, "TCN2"},
		{0x191,  8, Kind::ReadWrite, false, 0x00,   true, 0, "TER2"},

		// UM Table B-1 gives UIPCR reset $0F. AGENTS.md section 4.1 fixes
		// bit 0 at zero for the machine this project presents, and the bit is
		// a board strap, so a write cannot change it. Bits 3:1 stay at the
		// manual's reset value.
		//
		// readOnlyBits IS 0 HERE ON PURPOSE, and it matches sim.cpp's row.
		// Kind::ReadOnly already holds every bit, so a strap mask on this row
		// changes nothing that any case group reads. The earlier revision set
		// it to $01 and the value was DEAD DATA in both files. Case group 3
		// carries the strap fact as behaviour instead, which is where it
		// belongs.
		{0x1d0,  8, Kind::ReadOnly,  true, 0x0e, true, 0x00, "UIPCR"},

		{0x244, 16, Kind::ReadWrite, false, 0x0000, true, 0x0000, "PADDR"},
		// AGENTS.md sections 2.3 and 4.1: Port A bit 9 is an INPUT in every
		// model and it reads LOW on an expanded machine.
		{0x248, 16, Kind::ReadWrite, false, 0x0000, true, 0x0200, "PADAT"},
	};

	constexpr size_t g_registerCount = sizeof(g_registers) / sizeof(g_registers[0]);

	uint32_t widthMask(const int _widthBits)
	{
		return _widthBits == 32 ? 0xffffffffu : ((1u << _widthBits) - 1u);
	}

	class Bus
	{
	public:
		Bus()
		{
			g2::MemoryMapConfig config;
			config.mbar = {g_mbarBase, g_mbarSize};
			m_map = new g2::MemoryMap(config);
			m_map->attach(g2::Region::Mbar, &m_sim);
		}

		~Bus() { delete m_map; }

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
		{
			_status = MCF5307_BUS_OK;
			return g2::memoryMapRead(m_map, g_mbarBase + _offset, _size, &_status);
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
		{
			_status = MCF5307_BUS_OK;
			g2::memoryMapWrite(m_map, g_mbarBase + _offset, _size, _value, &_status);
		}

		g2::Sim& sim() { return m_sim; }

		std::string logLine(const size_t _index) const
		{
			if(_index >= m_sim.log().size())
				return "<no log line " + std::to_string(_index) + ">";
			return m_sim.log()[_index];
		}

	private:
		g2::Sim m_sim;
		g2::MemoryMap* m_map = nullptr;
	};

	// -----------------------------------------------------------------------
	// THE EXPECTATION SIDE AND THE OBSERVATION SIDE ARE DRAWN FROM DIFFERENT
	// PLACES ON PURPOSE.
	//
	// tableCovers and tableWritableMask read THIS FILE'S table, which is the
	// manual and AGENTS.md written out by hand. modelAnswersAt and
	// modelWritableMask read THE MODEL, through its own behaviour and through
	// nothing else. Case groups 11 to 14 hold one against the other.
	//
	// This is the shape case group 6 did not have. Group 6 compared this
	// file's table with itself, so it could only fail when this file was
	// edited, and a register added to sim.cpp alone went unobserved. The
	// groups below fail when EITHER side moves without the other.

	// True when the offset falls inside a register this file's table names.
	// Written out again rather than calling into sim.cpp, so that the two
	// sides stay independent.
	bool tableCovers(const uint32_t _offset)
	{
		for(const RegisterFact& r : g_registers)
		{
			const uint32_t bytes = uint32_t(r.widthBits) / 8u;
			if(_offset >= r.offset && (_offset - r.offset) < bytes)
				return true;
		}
		return false;
	}

	// The bits this file's table says a write may change at this offset. A
	// byte no register covers is reserved and yields zero: UM Table 9-5
	// footnote 1. A read-only register yields zero. A read/write register
	// yields every bit no board strap holds.
	uint8_t tableWritableMask(const uint32_t _offset)
	{
		for(const RegisterFact& r : g_registers)
		{
			const uint32_t bytes = uint32_t(r.widthBits) / 8u;

			if(_offset < r.offset || (_offset - r.offset) >= bytes)
				continue;

			if(r.kind == Kind::ReadOnly)
				return 0x00u;

			const int shift = int(8 * (bytes - 1 - (_offset - r.offset)));
			return uint8_t(~uint8_t((r.readOnlyBits >> shift) & 0xffu));
		}
		return 0x00u;
	}

	// Whether THE MODEL carries a register at this offset, asked of the model
	// and not of any table. sim.h states the contract this reads: the SIM
	// writes one log line for "every access to an offset the manual assigns to
	// no register this model carries", and writes none for an offset it does
	// carry. So an empty log after one access IS the model's own answer to
	// "do you have a register here".
	bool modelAnswersAt(Bus& _bus, const uint32_t _offset)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		_bus.sim().clearLog();
		_bus.read(_offset, 8, status);
		return _bus.sim().log().empty();
	}

	// The bits THE MODEL actually lets a write change at this offset, asked of
	// the model by writing both polarities and reading each back. This
	// recovers the model's write-protect mask from behaviour alone.
	uint8_t modelWritableMask(Bus& _bus, const uint32_t _offset)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;

		_bus.write(_offset, 8, 0x00u, status);
		const uint8_t low = uint8_t(_bus.read(_offset, 8, status));

		_bus.write(_offset, 8, 0xffu, status);
		const uint8_t high = uint8_t(_bus.read(_offset, 8, status));

		// A bit that reads zero after a write of zeros and one after a write
		// of ones is a bit the write reached.
		return uint8_t(high & ~low);
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. THE RESET VALUES THE MANUAL STATES.
	//
	// UM Table B-1 marks the chip-select and DRAM registers "uninitialized",
	// so no reset value is asserted for those. Every register the table gives
	// a value for is asserted here.
	{
		Bus bus;

		for(const RegisterFact& r : g_registers)
		{
			if(!r.resetIsKnown)
				continue;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t value = bus.read(r.offset, r.widthBits, status);

			checkEqual(status, MCF5307_BUS_OK,
				std::string("the reset read of ") + r.name + " completes");
			checkEqual(value, r.resetValue,
				std::string(r.name) + " reads its reset value " + hex32(r.resetValue));
		}
	}

	// -----------------------------------------------------------------------
	// Case group 2. EVERY MODELLED REGISTER IS DRIVEN AT ITS OWN WIDTH AND THE
	// VALUE IT READS BACK IS ASSERTED.
	//
	// A read/write register returns what was written, less any bit a strap
	// holds. A read-only register ignores the write and keeps its reset value.
	{
		Bus bus;

		for(const RegisterFact& r : g_registers)
		{
			const uint32_t pattern = 0x5aa55aa5u & widthMask(r.widthBits);

			mcf5307_bus_status status = MCF5307_BUS_OK;
			bus.write(r.offset, r.widthBits, pattern, status);

			checkEqual(status, MCF5307_BUS_OK,
				std::string("a write to ") + r.name + " at its own width completes");

			const uint32_t readBack = bus.read(r.offset, r.widthBits, status);

			checkEqual(status, MCF5307_BUS_OK,
				std::string("a read of ") + r.name + " at its own width completes");

			const uint32_t expected = r.kind == Kind::ReadWrite
				? ((pattern & ~r.readOnlyBits) | (r.resetValue & r.readOnlyBits))
				: r.resetValue;

			checkEqual(readBack, expected,
				std::string(r.name) + " reads back " + hex32(expected) + " after a write of " + hex32(pattern));
		}
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE TWO STRAPS THE FIRMWARE READS.
	//
	// AGENTS.md section 4.1 is authoritative on both. is_expanded() at
	// 0x30037F64 reads MBAR+0x248 bit 9 and takes LOW to mean expanded, which
	// presents eight DSPs. detect_model() at 0x30050864 reads MBAR+0x1D0 bit 0
	// and takes SET to mean an Engine, which has no panel board.
	//
	// Each is asserted AFTER a write of all ones, because a strap is an input
	// and a model that let the firmware write over it would present a
	// different machine one instruction later.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		checkEqual((bus.read(0x248, 16, status) >> 9) & 1u, uint32_t(0),
			"Port A data at MBAR+0x248 reads bit 9 LOW, so the machine is expanded");

		bus.write(0x248, 16, 0xffffu, status);

		checkEqual((bus.read(0x248, 16, status) >> 9) & 1u, uint32_t(0),
			"Port A bit 9 stays LOW after a write of all ones, because it is an input");

		checkEqual(bus.read(0x1d0, 8, status) & 1u, uint32_t(0),
			"MBAR+0x1D0 bit 0 reads zero, so the machine is not an Engine");

		bus.write(0x1d0, 8, 0xffu, status);

		checkEqual(bus.read(0x1d0, 8, status) & 1u, uint32_t(0),
			"MBAR+0x1D0 bit 0 stays zero after a write of all ones");
	}

	// -----------------------------------------------------------------------
	// Case group 4. THE UNRESTRICTED CLASS ACCEPTS EVERY WIDTH.
	//
	// This asserts a property of THIS MODEL and not of the part. See the note
	// at the head of this file: AGENTS.md section 4.2 records that chip-select
	// and DRAM controller access widths exist, so the manual saying nothing
	// about them proves nothing about them. The manual was searched on
	// 2026-08-06 and it does say nothing about them: section 14.3.7 is its only
	// access-width rule. What is pinned here is that the model rejects a width
	// at exactly one offset and nowhere else, which is the shape plan section
	// 13.1 requires.
	{
		Bus bus;
		const int widths[] = {8, 16, 32};

		for(const RegisterFact& r : g_registers)
		{
			if(r.byteAccessOnly)
				continue;

			for(const int width : widths)
			{
				mcf5307_bus_status readStatus = MCF5307_BUS_OK;
				bus.read(r.offset, width, readStatus);
				checkEqual(readStatus, MCF5307_BUS_OK,
					std::string("a ") + std::to_string(width) + "-bit read of " + r.name + " is accepted");

				mcf5307_bus_status writeStatus = MCF5307_BUS_OK;
				bus.write(r.offset, width, 0u, writeStatus);
				checkEqual(writeStatus, MCF5307_BUS_OK,
					std::string("a ") + std::to_string(width) + "-bit write to " + r.name + " is accepted");
			}
		}
	}

	// -----------------------------------------------------------------------
	// Case group 5. THE RESTRICTED CLASS REJECTS EVERY FORBIDDEN WIDTH AND
	// WRITES ONE LOG LINE NAMING THE OFFSET, THE WIDTH AND THE DIRECTION.
	//
	// UM section 14.3.7: "All UART module registers must be accessed as
	// bytes." MBAR+$1D0 is one. A model that accepted every width at every
	// offset fails this group, which is what makes the width data known to be
	// live rather than decorative.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		bus.read(0x1d0, 16, status);
		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "a 16-bit read of UIPCR is rejected");
		checkEqual(bus.sim().log().size(), size_t(1), "a rejected 16-bit read writes exactly one log line");
		checkEqual(bus.logLine(0),
			std::string("sim: SIZE_ILLEGAL read of 16 bits at offset 0x000001d0"),
			"the log line names the offset, the width and the direction");

		bus.sim().clearLog();
		bus.read(0x1d0, 32, status);
		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "a 32-bit read of UIPCR is rejected");
		checkEqual(bus.logLine(0),
			std::string("sim: SIZE_ILLEGAL read of 32 bits at offset 0x000001d0"),
			"the log line of the 32-bit read names its own width");

		bus.sim().clearLog();
		bus.write(0x1d0, 16, 0u, status);
		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "a 16-bit write to UACR is rejected");
		checkEqual(bus.logLine(0),
			std::string("sim: SIZE_ILLEGAL write of 16 bits at offset 0x000001d0"),
			"the log line of the write names the write direction");

		bus.sim().clearLog();
		bus.read(0x1d0, 8, status);
		checkEqual(status, MCF5307_BUS_OK, "an 8-bit read of UIPCR is accepted");
		checkEqual(bus.sim().log().size(), size_t(0), "an accepted access writes no log line");
	}

	// -----------------------------------------------------------------------
	// Case group 6. A HYGIENE GUARD ON THE SKIP LOGIC OF GROUPS 4 AND 5. IT
	// SAYS NOTHING ABOUT THE MODEL.
	//
	// WHAT THIS GROUP IS AND IS NOT. It constructs no Bus, it names no g2::
	// symbol and it reads only this file's own table. It therefore CANNOT
	// observe sim.cpp, and every assertion below is worded as a fact about
	// THIS FILE. An earlier revision worded them as facts about the model -
	// "the model covers 18 chip-select ... registers" - which was misleading:
	// a register added to sim.cpp alone left every one of them green.
	//
	// The job it does do is real and worth keeping. Groups 4 and 5 divide the
	// table by byteAccessOnly: group 4 SKIPS every restricted row and group 5
	// drives the one restricted offset by hand. If the table ever carried two
	// restricted rows, group 4 would silently stop driving one register and
	// group 5 would never notice. The counts below are what stops that.
	//
	// THE MODEL-SIDE CLAIM IS CARRIED BY CASE GROUPS 11 TO 14, which read the
	// model's own answers and hold them against this table.
	{
		size_t restricted = 0;
		size_t unrestricted = 0;
		uint32_t restrictedOffset = 0xffffffffu;

		for(const RegisterFact& r : g_registers)
		{
			if(r.byteAccessOnly)
			{
				++restricted;
				restrictedOffset = r.offset;
			}
			else
				++unrestricted;
		}

		checkEqual(restricted + unrestricted, g_registerCount,
			"every row of this file's table is in exactly one width class, so group 4 and group 5 between them reach every row");
		checkEqual(restricted, size_t(1),
			"exactly one row of this file's table is restricted, so case group 4 skips exactly one row");
		checkEqual(restrictedOffset, uint32_t(0x1d0),
			"the row case group 4 skips is the row case group 5 drives by hand, so no row is skipped by both");
		checkEqual(unrestricted, size_t(g_registerCount - 1),
			"case group 4 drives every other row of this file's table at all three widths");
		checkEqual(g_registerCount, size_t(36),
			"this file's table carries 36 rows: 18 chip-select, 5 DRAM controller, 10 timer, 1 strap and 2 parallel-port");

		// The chip-select family is counted on its own, because it is the part
		// of the table AGENTS.md section 3.8 governs and the part the earlier
		// revision got wrong. Six selects, three registers each.
		size_t chipSelect = 0;

		for(const RegisterFact& r : g_registers)
		{
			const std::string name = r.name;
			if(name.rfind("CS", 0) == 0)
				++chipSelect;
		}

		checkEqual(chipSelect, size_t(18),
			"this file's table names 18 chip-select registers, three for each of the six selects the G2 wires");
	}

	// -----------------------------------------------------------------------
	// Case group 7. AN OFFSET THE MANUAL ASSIGNS TO NO REGISTER IS NOT
	// SILENTLY ACCEPTED.
	//
	// UM Table 9-5 footnote 1: a write to a reserved address has no effect.
	// The access completes, and the board writes one log line so that a
	// firmware access nobody modelled leaves a trace. BRD-5 owns the full
	// anomaly log; this is the trace the SIM itself writes.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t value = bus.read(0x0f0, 32, status);

		checkEqual(status, MCF5307_BUS_OK, "a reserved offset completes rather than faulting");
		checkEqual(value, uint32_t(0), "a reserved offset reads zero");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED read of 32 bits at offset 0x000000f0"),
			"a reserved offset writes one log line naming the offset, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 8. THE BOOT LOADER'S OWN CS3 PROGRAMMING, FROM AGENTS.md 3.8.
	//
	// Section 3.8 measures BOOT_128_Loader.bin writing CSAR3 at MBAR+$0A4 =
	// $1300, CSMR3 at MBAR+$0A8 = $00000001 and CSCR3 at MBAR+$0AE = $0540,
	// and it reads CSAR3 = $1300 as the base $13000000 where the ISP1181 USB
	// device controller sits.
	//
	// EVERY CASE HERE ASSERTS THE LOG AS WELL AS THE READ-BACK, because a
	// read-back on its own is not evidence that a write was modelled. Under
	// the withdrawn CSBAR layout the 32-bit write of $00000001 to $0A8 READ
	// BACK CORRECTLY - the low half landed in a two-byte register that layout
	// placed at $0AA - while the model logged the same write UNMODELLED. The
	// read-back said yes, the log said no, and the log was right.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		struct Measured
		{
			uint32_t offset;
			int widthBits;
			uint32_t value;
			const char* name;
		};

		const Measured measured[] =
		{
			{0x0a4, 16, 0x00001300u, "CSAR3"},
			{0x0a8, 32, 0x00000001u, "CSMR3"},
			{0x0ae, 16, 0x00000540u, "CSCR3"},
		};

		for(const Measured& m : measured)
		{
			bus.sim().clearLog();
			bus.write(m.offset, m.widthBits, m.value, status);

			checkEqual(status, MCF5307_BUS_OK,
				std::string("the boot loader's write of ") + m.name + " at " + hex32(m.offset) + " completes");
			checkEqual(bus.sim().log().size(), size_t(0),
				std::string("the boot loader's write of ") + m.name + " is MODELLED, so the SIM logs nothing");

			const uint32_t readBack = bus.read(m.offset, m.widthBits, status);

			checkEqual(bus.sim().log().size(), size_t(0),
				std::string("the read of ") + m.name + " is MODELLED, so the SIM logs nothing");
			checkEqual(readBack, m.value,
				std::string(m.name) + " reads back the measured " + hex32(m.value));
		}

		bus.sim().clearLog();

		// The value CSAR3 carries, read the way AGENTS.md 3.8 reads it.
		checkEqual(bus.read(0x0a4, 16, status) << 16, uint32_t(0x13000000u),
			"CSAR3 gives CS3 the base 0x13000000, which AGENTS.md 3.8 names as the ISP1181");

		// The three are separate storage. Neither neighbour of the CS3 block
		// may see any part of the CS3 programming.
		checkEqual(bus.read(0x0a2, 16, status), uint32_t(0),
			"CSCR2, below CSAR3, is untouched by the CS3 programming");
		checkEqual(bus.read(0x0b0, 16, status), uint32_t(0),
			"CSAR4, above CSCR3, is untouched by the CS3 programming");
	}

	// -----------------------------------------------------------------------
	// Case group 9. THE SIX CSARn ARE SIX SEPARATE REGISTERS.
	//
	// AGENTS.md open question 21 and plan section 4.2 register row 18 both
	// name CSAR0 to CSAR5. One shared base cannot answer for six selects, so
	// each CSARn holds its own value. A table that collapsed two of them onto
	// one offset passes every group above and fails here.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t csar[] = {0x080, 0x08c, 0x098, 0x0a4, 0x0b0, 0x0bc};
		const size_t count = sizeof(csar) / sizeof(csar[0]);

		for(size_t i = 0; i < count; ++i)
			bus.write(csar[i], 16, uint32_t(0x1000u + (i << 8)), status);

		checkEqual(bus.sim().log().size(), size_t(0),
			"all six CSARn writes are MODELLED, so the SIM logs nothing");

		for(size_t i = 0; i < count; ++i)
			checkEqual(bus.read(csar[i], 16, status), uint32_t(0x1000u + (i << 8)),
				"CSAR" + std::to_string(i) + " holds its own base and no other CSARn's");
	}

	// -----------------------------------------------------------------------
	// Case group 10. THE POSITIVE CONTROL FOR EVERY "THE LOG IS EMPTY" ABOVE.
	//
	// Groups 8 and 9 read an EMPTY log as proof that a write was modelled.
	// That reading is sound only while the log can still record something. A
	// control drawn from the chip-select block would share the assumption it
	// guards and would prove nothing, so this one is drawn from OUTSIDE that
	// block: MBAR+$300 lies past every register BRD-2 models, in the part of
	// UM Table B-1 that belongs to blocks this task does not own. Its being
	// unmodelled follows from nothing in the chip-select family.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		checkEqual(bus.sim().log().size(), size_t(0), "the log starts empty");

		bus.write(0x300, 16, 0x1234u, status);

		checkEqual(bus.sim().log().size(), size_t(1),
			"an offset outside every block this task models writes one log line, so an empty log is a live observation");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED write of 16 bits at offset 0x00000300"),
			"the control's log line names the offset, the width and the direction");
	}

	// -----------------------------------------------------------------------
	// Case group 11. WHAT THE MODEL ANSWERS, ASKED OF THE MODEL.
	//
	// EVERY GROUP ABOVE READS THE MODEL ONLY AT OFFSETS THIS FILE ALREADY
	// NAMES, so the model was free to grow registers this file knows nothing
	// about. Adding CSAR6, CSMR6 and CSCR6 to sim.cpp - which AGENTS.md
	// section 2.2 forbids, because no G2 signal reaches CS6 or CS7, and which
	// sim.cpp itself cites as its reason for omitting them - left every case
	// above green. That is the original defect's exact shape: the model and
	// the test disagreed and nothing observed the disagreement.
	//
	// This group closes it by sweeping the WHOLE MBAR window and asking the
	// model, at every one of its own g_simSpaceSize offsets, whether it
	// carries a register there. The answer comes out of the model's log, which
	// sim.h defines as carrying one line for every access to an offset the
	// model does not carry. The expectation comes from this file's table. The
	// two sides move independently, so the check fails when EITHER moves.
	//
	// IT CANNOT BE SATISFIED BY EDITING THIS FILE'S TABLE. Adding a row here
	// to match a register added to sim.cpp makes this group pass again, and
	// then case group 6's literal count of 36 rows fails instead. A register
	// can only enter the model with both files, and this comment, changed
	// together.
	{
		Bus bus;

		size_t modelledBytes = 0;
		size_t expectedBytes = 0;
		uint32_t firstDisagreement = g2::g_simSpaceSize;

		for(uint32_t offset = 0; offset < g2::g_simSpaceSize; ++offset)
		{
			const bool answered = modelAnswersAt(bus, offset);
			const bool named = tableCovers(offset);

			if(answered)
				++modelledBytes;
			if(named)
				++expectedBytes;

			if(answered != named && firstDisagreement == g2::g_simSpaceSize)
				firstDisagreement = offset;
		}

		checkEqual(firstDisagreement, g2::g_simSpaceSize,
			"the model answers at exactly the offsets this file's table names and at no other offset in the whole MBAR window");
		checkEqual(modelledBytes, expectedBytes,
			"the model carries as many register bytes as this file's table names");
		checkEqual(modelledBytes, size_t(89),
			"the model carries 89 register bytes: 48 chip-select, 18 DRAM controller, 18 timer, 1 strap and 4 parallel-port");

		// CS6 and CS7 by name. AGENTS.md section 2.2 records that the G2 wires
		// six chip selects and that no G2 signal reaches the other two. The
		// twelve-byte stride the CS3 measurement fixes puts a seventh and an
		// eighth family at exactly these six offsets, and the 1998 manual
		// marks $0C8 and $0D4 Reserved. The model must answer at none of them.
		const uint32_t forbidden[] = {0x0c8, 0x0cc, 0x0d2, 0x0d4, 0x0d8, 0x0de};

		for(const uint32_t offset : forbidden)
			check(!modelAnswersAt(bus, offset),
				"the model carries no register at " + hex32(offset) + ", where a CS6 or CS7 family would sit, because AGENTS.md section 2.2 records that no G2 signal reaches them");
	}

	// -----------------------------------------------------------------------
	// Case group 12. EVERY REGISTER'S UPPER BOUNDARY.
	//
	// No group above asserted where a register STOPS. Changing the model's
	// lookup so that a register claims one byte past its end left every case
	// green, which meant the reserved-address contract was unverified in both
	// directions: nothing said a reserved offset stays reserved, and nothing
	// said a register does not reach into one.
	//
	// The byte directly above each register is asserted here, for every row
	// this file's table names whose upper neighbour is not another register.
	// Rows whose neighbour IS another register are covered by case group 11's
	// sweep instead, which holds every offset in the window.
	{
		Bus bus;

		for(const RegisterFact& r : g_registers)
		{
			const uint32_t after = r.offset + uint32_t(r.widthBits) / 8u;

			if(after >= g2::g_simSpaceSize || tableCovers(after))
				continue;

			check(!modelAnswersAt(bus, after),
				std::string(r.name) + " stops at its stated width, so the model carries no register at " + hex32(after));
		}
	}

	// -----------------------------------------------------------------------
	// Case group 13. A WRITE TO A RESERVED ADDRESS HAS NO EFFECT.
	//
	// UM Table 9-5 footnote 1. sim.cpp cites it, sim.h cites it and case group
	// 7 cites it, and until now NO CASE DROVE IT: group 7 only READ a reserved
	// offset and group 10 only WROTE one and looked at the log. Neither wrote
	// a reserved offset and read it back, so the protection could be removed
	// with every case still green.
	//
	// WHAT THE FAILURE WOULD COST. Without the protection the model stores a
	// firmware write into reserved MBAR space and hands it back on a later
	// read. A misdirected write then reads back exactly like a modelled one,
	// which is the one thing BRD-5's anomaly log exists to make visible.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		// An offset inside the window that no register covers.
		bus.write(0x0f0, 32, 0xffffffffu, status);
		checkEqual(status, MCF5307_BUS_OK, "a write to the reserved offset 0x0f0 completes rather than faulting");
		checkEqual(bus.read(0x0f0, 32, status), uint32_t(0),
			"the reserved offset 0x0f0 still reads zero after a write of all ones, because a write to a reserved address has no effect");

		// The control group 10 already uses, now read back as well as logged.
		bus.write(0x300, 16, 0x1234u, status);
		checkEqual(bus.read(0x300, 16, status), uint32_t(0),
			"the reserved offset 0x300 still reads zero after a write of 0x1234");

		// A hole BETWEEN two modelled registers. CSMR0 ends at 0x087 and CSCR0
		// begins at 0x08a, so 0x088 and 0x089 belong to no register.
		bus.write(0x088, 16, 0xbeefu, status);
		checkEqual(bus.read(0x088, 16, status), uint32_t(0),
			"the hole at 0x088, between CSMR0 and CSCR0, still reads zero after a write");
		checkEqual(bus.read(0x084, 32, status), uint32_t(0),
			"CSMR0, below the hole, is untouched by the write to the hole");
		checkEqual(bus.read(0x08a, 16, status), uint32_t(0),
			"CSCR0, above the hole, is untouched by the write to the hole");

		// THE MISDIRECTED WRITE, IN THE SHAPE THE FIRMWARE WOULD MAKE IT. A
		// 32-bit write at 0x086 starts inside CSMR0 and runs off its top into
		// the reserved hole. The two bytes inside the register take the value
		// and the two bytes in the hole must not.
		bus.sim().clearLog();
		bus.write(0x086, 32, 0xdeadbeefu, status);

		checkEqual(status, MCF5307_BUS_OK, "a 32-bit write starting inside CSMR0 and running into the reserved hole completes");
		checkEqual(bus.sim().log().size(), size_t(0),
			"the write is not logged UNMODELLED, because the offset it starts at is a register the model carries");
		checkEqual(bus.read(0x084, 32, status), uint32_t(0x0000deadu),
			"CSMR0 takes the two bytes of the write that landed inside it");
		checkEqual(bus.read(0x088, 16, status), uint32_t(0),
			"the reserved hole takes none of the write that ran off the top of CSMR0");
	}

	// -----------------------------------------------------------------------
	// Case group 14. THE MODEL'S WHOLE WRITE-PROTECT MASK, RECOVERED FROM
	// BEHAVIOUR.
	//
	// Group 13 drives the protection at four offsets. This drives it at all
	// g_simSpaceSize of them, by writing both polarities to every byte and
	// reading each back, which recovers the model's mask without reading a
	// line of sim.cpp. It pins three separate facts at once: every reserved
	// byte is fully protected, every read-only register is fully protected,
	// and every bit a board strap holds is protected while its neighbours in
	// the same register are not.
	//
	// ON THE strapBits FIELD, AND WHAT THIS GROUP CANNOT DO. sim.cpp's
	// constructor once read strapBits only on a ReadWrite row. The form that
	// replaced it applies strapBits on every row. THOSE TWO FORMS ARE THE SAME
	// FUNCTION: Access::ReadOnly already sets every bit of the mask, so
	// 0xff | strap is 0xff for every strap, and the ReadWrite arm is strap in
	// both. No test can be red on that change, because it changes no
	// behaviour. What CAN go wrong, and what this group does catch, is the
	// mask being NARROWED - on either arm - so that a strap bit becomes
	// writable. sim.cpp carries a static_assert for the same invariant at
	// compile time. See sim.cpp for the full record.
	{
		Bus bus;

		size_t writableBytes = 0;
		uint32_t firstDisagreement = g2::g_simSpaceSize;

		for(uint32_t offset = 0; offset < g2::g_simSpaceSize; ++offset)
		{
			const uint8_t observed = modelWritableMask(bus, offset);
			const uint8_t named = tableWritableMask(offset);

			if(observed == 0xffu)
				++writableBytes;

			if(observed != named && firstDisagreement == g2::g_simSpaceSize)
				firstDisagreement = offset;
		}

		checkEqual(firstDisagreement, g2::g_simSpaceSize,
			"the model lets a write change exactly the bits this file's table says it may, at every offset in the MBAR window");
		checkEqual(writableBytes, size_t(83),
			"83 of the model's 89 register bytes are fully writable; the other six are the four bytes of the two read-only timer counters, the one strap byte at 0x1d0 and the one Port A byte that carries bit 9");

		// The three protected places by name, so that a reader does not have
		// to reconstruct them from the sweep.
		checkEqual(uint32_t(modelWritableMask(bus, 0x248)), uint32_t(0xfd),
			"Port A bit 9 is the one bit of MBAR+0x248 a write cannot change, because AGENTS.md sections 2.3 and 4.1 record it as an input strap");
		checkEqual(uint32_t(modelWritableMask(bus, 0x249)), uint32_t(0xff),
			"the low byte of Port A carries no strap, so a write reaches every bit of it");
		checkEqual(uint32_t(modelWritableMask(bus, 0x1d0)), uint32_t(0x00),
			"a write reaches no bit of MBAR+0x1d0, because a write there goes to UACR and a read comes from UIPCR");
		checkEqual(uint32_t(modelWritableMask(bus, 0x148)), uint32_t(0x00),
			"a write reaches no bit of TCR1, which UM Table B-1 gives as read-only");
		checkEqual(uint32_t(modelWritableMask(bus, 0x0f0)), uint32_t(0x00),
			"a write reaches no bit of the reserved offset 0x0f0");
	}

	// -----------------------------------------------------------------------
	// Case group 15. THE SIM'S OWN WIDTH GUARD.
	//
	// sim.cpp rejects any width that is not 8, 16 or 32, and NO CASE ABOVE
	// DROVE IT, because every access above goes through the decode and the
	// decode carries the same guard and answers first. The two guards are
	// separate code in separate files and each needs its own case.
	//
	// THIS GROUP DRIVES THE SIM AT ITS BusTarget INTERFACE, deliberately and
	// as the only group in this file that does. The SIM is a BusTarget and its
	// width guard is part of what it presents to any decode, not only to this
	// one. Driving it through the decode could never reach the branch.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;
		const int illegal[] = {0, 4, 12, 24, 64};

		for(const int width : illegal)
		{
			bus.sim().clearLog();
			status = MCF5307_BUS_OK;
			const uint32_t value = bus.sim().read(0x080, width, status);

			checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL,
				std::string("the SIM rejects a ") + std::to_string(width) + "-bit read");
			checkEqual(value, uint32_t(0),
				std::string("a rejected ") + std::to_string(width) + "-bit read returns zero");
			checkEqual(bus.sim().log().size(), size_t(1),
				std::string("a rejected ") + std::to_string(width) + "-bit read writes exactly one log line");
			checkEqual(bus.logLine(0),
				std::string("sim: SIZE_ILLEGAL read of ") + std::to_string(width) + " bits at offset 0x00000080",
				std::string("the log line of the ") + std::to_string(width) + "-bit read names its width, its offset and its direction");

			bus.sim().clearLog();
			status = MCF5307_BUS_OK;
			bus.sim().write(0x080, width, 0xffffffffu, status);

			checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL,
				std::string("the SIM rejects a ") + std::to_string(width) + "-bit write");
			checkEqual(bus.logLine(0),
				std::string("sim: SIZE_ILLEGAL write of ") + std::to_string(width) + " bits at offset 0x00000080",
				std::string("the log line of the ") + std::to_string(width) + "-bit write names the write direction");
		}

		// A rejected write changes nothing.
		status = MCF5307_BUS_OK;
		checkEqual(bus.sim().read(0x080, 16, status), uint32_t(0),
			"CSAR0 is unchanged by every rejected write above, so a rejected access reaches no storage");

		// THE TWO GUARDS ARE SEPARATE. The same widths driven through the
		// decode are rejected by the decode, and the SIM never sees them.
		bus.sim().clearLog();
		status = MCF5307_BUS_OK;
		bus.read(0x080, 24, status);

		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "the decode rejects a 24-bit read before the SIM sees it");
		checkEqual(bus.sim().log().size(), size_t(0),
			"the SIM writes no log line for a width the decode already rejected, so the two width guards are separate layers");
	}

	// -----------------------------------------------------------------------
	// Case group 16. AN ACCEPTED WIDE ACCESS IS BYTE-ADDRESSED, AND WHAT IT
	// REACHES IS PINNED.
	//
	// Case group 4 drives every unrestricted register at all three widths and
	// asserts ONLY that the access is accepted. It never asserts that
	// acceptance is non-destructive. A 32-bit access at CSCR0, which is two
	// bytes at 0x08a, runs into CSAR1 at 0x08c, and nothing said whether that
	// was intended.
	//
	// IT IS INTENDED, AND IT IS RECORDED HERE RATHER THAN LEFT OPEN. The model
	// treats an access as a run of bytes from the offset it is given, exactly
	// as the part's bus does, and applies the write protection byte by byte.
	// So a wide access at CSCR0 DOES reach CSAR1, and the neighbour it reaches
	// is named below. A later change that made a wide access stop at the
	// register boundary would fail here, which is the point: the behaviour is
	// pinned either way and no longer rests on nobody having looked.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		bus.sim().clearLog();
		bus.write(0x08a, 32, 0x11223344u, status);

		checkEqual(status, MCF5307_BUS_OK, "a 32-bit write at CSCR0 is accepted");
		checkEqual(bus.sim().log().size(), size_t(0),
			"the 32-bit write at CSCR0 is not logged UNMODELLED, because CSCR0 is a register the model carries");
		checkEqual(bus.read(0x08a, 16, status), uint32_t(0x1122u),
			"CSCR0 takes the top half of a 32-bit write made at its own offset");
		checkEqual(bus.read(0x08c, 16, status), uint32_t(0x3344u),
			"CSAR1 takes the bottom half, because a wide access is a run of bytes and does not stop at the register boundary");

		// The register BELOW CSCR0 is not reached, so the spill runs upward
		// only and a wide access never writes behind itself.
		checkEqual(bus.read(0x084, 32, status), uint32_t(0),
			"CSMR0, below CSCR0, takes nothing from the 32-bit write");
	}

	if(g_failures)
	{
		std::cout << "t0_sim: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_sim: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
