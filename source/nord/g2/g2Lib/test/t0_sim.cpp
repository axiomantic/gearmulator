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
// AGENTS.md is authoritative on hardware facts. It refutes the shared-CSBAR
// reading an earlier revision of this test carried. sim.cpp's RESOLVED
// CONFLICT block holds the full record. Case group 8 drives those three
// measured offsets directly.
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
// nothing. THAT REASONING IS WITHDRAWN. AGENTS.md section 4.2 states as fact
// that access widths for the chip-select and DRAM controller registers exist
// and are recorded, and that they are a useful corroboration "because the
// manual is hard to read on this point". Silence in a document that is hard to
// read is not evidence of absence. The gap is declared in sim.cpp and stays
// open until the manual or SPK-13's firmware read closes it.

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
	// 11.5 Table 11-1 and section 12.3.1 Table 12-1.
	//
	// EACH CHIP SELECT CARRIES ITS OWN CSARn. Section 3.8 measures the boot
	// loader writing CSAR3 at $0A4, CSMR3 at $0A8 and CSCR3 at $0AE, and each
	// of the three sits exactly $24 above the CS0 register of the same kind,
	// so the family repeats every twelve bytes. CSAR3 = $1300 is a PER-SELECT
	// base, $13000000, which one shared CSBAR could not express. The earlier
	// revision of this table modelled a single CSBAR at $098 and no CSAR2 to
	// CSAR5, and it was written from the same manual reading as the model it
	// was meant to check, so it could not catch the fault.
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
	// about them proves nothing about them. What is pinned here is that the
	// model rejects a width at exactly one offset and nowhere else, which is
	// the shape plan section 13.1 requires.
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
	// Case group 6. THE TWO CLASSES COVER EVERY MODELLED REGISTER.
	//
	// The count below is what stops a register from being dropped out of both
	// groups above and going unasserted.
	{
		size_t restricted = 0;
		size_t unrestricted = 0;

		for(const RegisterFact& r : g_registers)
		{
			if(r.byteAccessOnly)
				++restricted;
			else
				++unrestricted;
		}

		checkEqual(restricted + unrestricted, g_registerCount,
			"every modelled register is in exactly one width class");
		checkEqual(restricted, size_t(1),
			"one register carries a width restriction the manual states");
		checkEqual(unrestricted, size_t(g_registerCount - 1),
			"this model enforces no width restriction on any other register, which is a fact about the model and not about the part");
		checkEqual(g_registerCount, size_t(36),
			"the model covers 18 chip-select, 5 DRAM controller, 10 timer, 1 strap and 2 parallel-port registers");

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
			"the six chip selects the G2 wires carry three registers each, so there is a CSARn for every one of them");
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
