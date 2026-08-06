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
// THE TWO WIDTH CLASSES. Plan section 13.1 requires that every modelled
// register is in exactly one of two classes, so that the check does not depend
// on a restriction that may not exist:
//
//   * RESTRICTED. The manual states an access-width rule. MCF5307 UM section
//     14.3.7 states one, and one only, for the registers this task models:
//     "All UART module registers must be accessed as bytes." MBAR+$1D0 is a
//     UART module register. A width other than 8 must be REJECTED and must
//     write one log line.
//   * UNRESTRICTED. The manual states a register WIDTH for every chip-select,
//     DRAM controller, timer and parallel-port register, and it states no
//     access-width rule for any of them. The ACCESS column of UM Table 9-5 is
//     read/write/read-only/write-only and is not a width. So every width must
//     be ACCEPTED, and a model that rejected one would be inventing a
//     restriction the manual does not carry.

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

	// MCF5307 User's Manual, Appendix B Table B-1, section 9.4.1 Table 9-5,
	// section 11.5 Table 11-1 and section 12.3.1 Table 12-1.
	//
	// CS2 to CS5 share CSBAR at MBAR+$098. UM section 9.4.2.1: "CSAR0 and
	// CSAR1 determine the base addresses from which chip-selects 0 and 1 will
	// be offset ... CSBAR determines the base address from which chip-selects
	// 2 through 7 will be offset." The part carries no CSAR2 to CSAR5, so the
	// three-register family of the six chip selects the G2 uses is CSAR0,
	// CSAR1 and CSBAR for the bases, CSMR0 to CSMR5 and CSCR0 to CSCR5.
	const RegisterFact g_registers[] =
	{
		{0x080, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR0"},
		{0x084, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR0"},
		{0x08a, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR0"},
		{0x08c, 16, Kind::ReadWrite, false, 0, false, 0, "CSAR1"},
		{0x090, 32, Kind::ReadWrite, false, 0, false, 0, "CSMR1"},
		{0x096, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR1"},
		{0x098,  8, Kind::ReadWrite, false, 0, false, 0, "CSBAR"},
		{0x09c, 16, Kind::ReadWrite, false, 0, false, 0, "CSMR2"},
		{0x0a2, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR2"},
		{0x0aa, 16, Kind::ReadWrite, false, 0, false, 0, "CSMR3"},
		{0x0ae, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR3"},
		{0x0b6, 16, Kind::ReadWrite, false, 0, false, 0, "CSMR4"},
		{0x0ba, 16, Kind::ReadWrite, false, 0, false, 0, "CSCR4"},
		{0x0c2, 16, Kind::ReadWrite, false, 0, false, 0, "CSMR5"},
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
		{0x1d0,  8, Kind::ReadOnly,  true, 0x0e, true, 0x01, "UIPCR"},

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
	// The manual states no access-width rule for any chip-select, DRAM
	// controller, timer or parallel-port register. A model that rejected a
	// width here would carry a restriction the manual does not.
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
			"every other modelled register carries no width restriction");
		checkEqual(g_registerCount, size_t(33),
			"the model covers 15 chip-select, 5 DRAM controller, 10 timer, 1 strap and 2 parallel-port registers");
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
