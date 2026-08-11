// The SIM registers. Tier T0: this test needs no firmware artifact.
//
// The register table below is written out again on purpose. It is read from the
// MCF5307 User's Manual and not from sim.cpp: a test that imported the
// implementation's own table would assert that the table equals itself.
//
// The SIM sits in the MBAR window of a MemoryMap and every access below goes
// through memoryMapRead/memoryMapWrite, so the offset the SIM sees is produced
// by the decode and not by this test.
//
// The chip-select family below is a firmware measurement of
// BOOT_128_Loader.bin programming CS3 -- CSAR3 $0A4 = $1300, CSMR3 $0A8 =
// $00000001, CSCR3 $0AE = $0540 -- and not the manual. The 1998 manual
// documents an earlier silicon mask; the G2 carries a later one, and sim.cpp
// lists every modelled value the 1998 manual contradicts.
//
// Every modelled register is in exactly one of two width classes, so that the
// check does not depend on a restriction that may not exist:
//
//   * Restricted. A width other than the one stated must be rejected and must
//     write one log line. "All UART module registers must be accessed as
//     bytes." MBAR+$1D0 is a UART module register.
//   * Unrestricted. Every width must be accepted.
//
// The unrestricted class claims that this MODEL accepts every width at those
// offsets. It does not claim the real part does. The manual states exactly one
// access-width rule, and that rule is about the UART block; the width columns
// of its register tables give the width of each register, not the widths a bus
// access may use.

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

	// This fixture chooses the MBAR base. The boot loader programs MBAR through
	// MOVEC, so the value is a firmware choice and not a property of the part.
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

	// The chip-select family is a firmware measurement; everything else comes
	// from the MCF5307 User's Manual.
	//
	// Each chip select carries its own CSARn on the mask the G2 carries. The
	// boot loader writes CSAR3 at $0A4, CSMR3 at $0A8 and CSCR3 at $0AE, and
	// each sits exactly $24 above the CS0 register of the same kind, so the
	// family repeats every twelve bytes. CSAR3 = $1300 is a per-select base,
	// $13000000, and the USB driver reaches the ISP1181 at exactly that
	// address.
	//
	// The 1998 manual documents a different layout, one shared 8-bit CSBAR at
	// $098 and no CSAR2 to CSAR7. That is a real MCF5307 layout on an earlier
	// mask, not an error. The rows below therefore disagree with the manual at
	// $098, $09C, $0A4, $0A8, $0B0, $0B4, $0BC and $0C0.
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

		// The manual gives UIPCR reset $0F. Bit 0 is fixed at zero for the
		// machine this project presents, and the bit is a board strap, so a
		// write cannot change it. Bits 3:1 stay at the manual's reset value.
		//
		// readOnlyBits is 0 here on purpose, matching sim.cpp's row:
		// Kind::ReadOnly already holds every bit, so a strap mask on this row
		// changes nothing any case group reads.
		{0x1d0,  8, Kind::ReadOnly,  true, 0x0e, true, 0x00, "UIPCR"},

		{0x244, 16, Kind::ReadWrite, false, 0x0000, true, 0x0000, "PADDR"},
		// Port A bit 9 is an input in every model and reads LOW on an expanded
		// machine.
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
	// The expectation side and the observation side are drawn from different
	// places on purpose.
	//
	// tableCovers and tableWritableMask read this file's table, written out by
	// hand. ModelAnswersAt and modelWritableMask read the model, through its own
	// behaviour and through nothing else. The groups that hold one against the
	// other fail when either side moves without the other.

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

	// The bits this file's table says a write may change at this offset. A byte
	// no register covers is reserved and yields zero, because a write to a
	// reserved address has no effect. A read-only register yields zero. A
	// read/write register yields every bit no board strap holds.
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

	// Whether the MODEL carries a register at this offset, asked of the model
	// and not of any table. Sim.h states the contract this reads: the SIM
	// writes one log line for "every access to an offset the manual assigns to
	// no register this model carries", and writes none for an offset it does
	// carry. So an empty log after one access is the model's own answer to
	// "do you have a register here".
	bool modelAnswersAt(Bus& _bus, const uint32_t _offset)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		_bus.sim().clearLog();
		_bus.read(_offset, 8, status);
		return _bus.sim().log().empty();
	}

	// The bits the MODEL actually lets a write change at this offset, asked of
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
	// Case group 1. The reset values the manual states.
	//
	// The manual marks the chip-select and DRAM registers "uninitialized", so
	// no reset value is asserted for those.
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
	// Case group 2. Every modelled register is driven at its own width and the
	// value it reads back is asserted.
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
	// Case group 3. The two straps the firmware reads.
	//
	// is_expanded() at 0x30037F64 reads MBAR+0x248 bit 9 and takes LOW to mean
	// expanded, which presents eight DSPs. Detect_model() at 0x30050864 reads
	// MBAR+0x1D0 bit 0 and takes SET to mean an Engine, which has no panel
	// board.
	//
	// Each is asserted after a write of all ones, because a strap is an input
	// and a model that let the firmware write over it would present a different
	// machine one instruction later.
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
	// Case group 4. The unrestricted class accepts every width.
	//
	// This asserts a property of this model and not of the part. What is pinned
	// is that the model rejects a width at exactly one offset and nowhere
	// else.
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
	// Case group 5. The restricted class rejects every forbidden width and
	// writes one log line naming the offset, the width and the direction.
	//
	// "All UART module registers must be accessed as bytes." MBAR+$1D0 is one.
	// A model that accepted every width at every offset fails this group.
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
	// Case group 6. A hygiene guard on the skip logic of groups 4 and 5. It
	// says nothing about the model: it constructs no Bus, names no g2:: symbol
	// and reads only this file's own table.
	//
	// Groups 4 and 5 divide the table by byteAccessOnly: group 4 skips every
	// restricted row and group 5 drives the one restricted offset by hand. If
	// the table ever carried two restricted rows, group 4 would silently stop
	// driving one register and group 5 would never notice. The counts below are
	// what stops that.
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

		// The chip-select family is counted on its own: six selects, three
		// registers each.
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
	// Case group 7. An offset the manual assigns to no register is not silently
	// accepted.
	//
	// A write to a reserved address has no effect. The access completes, and
	// the board writes one log line so that a firmware access nobody modelled
	// leaves a trace.
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
	// Case group 8. The boot loader's own CS3 programming.
	//
	// BOOT_128_Loader.bin writes CSAR3 at MBAR+$0A4 = $1300, CSMR3 at
	// MBAR+$0A8 = $00000001 and CSCR3 at MBAR+$0AE = $0540. CSAR3 = $1300 is
	// the base $13000000 where the ISP1181 USB device controller sits.
	//
	// Every case here asserts the log as well as the read-back, because a
	// read-back on its own is not evidence that a write was modelled. Under the
	// shared-CSBAR layout the 32-bit write of $00000001 to $0A8 reads back
	// correctly -- the low half lands in a two-byte register that layout places
	// at $0AA -- while the model logs the same write UNMODELLED.
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

		// The value CSAR3 carries.
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
	// Case group 9. The six CSARn are six separate registers.
	//
	// One shared base cannot answer for six selects, so each CSARn holds its
	// own value. A table that collapsed two of them onto one offset passes
	// every group above and fails here.
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
	// Case group 10. The positive control for every "the log is empty" above.
	//
	// Groups 8 and 9 read an empty log as proof that a write was modelled. That
	// reading is sound only while the log can still record something. A control
	// drawn from the chip-select block would share the assumption it guards, so
	// this one is drawn from outside it: MBAR+$300 lies past every register
	// this file models, and its being unmodelled follows from nothing in the
	// chip-select family.
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
	// Case group 11. What the model answers, asked of the model.
	//
	// Every group above reads the model only at offsets this file already
	// names, so the model is free to grow registers this file knows nothing
	// about: adding CSAR6, CSMR6 and CSCR6 to sim.cpp leaves every case above
	// green.
	//
	// This group closes it by sweeping the whole MBAR window and asking the
	// model, at every one of its own g_simSpaceSize offsets, whether it carries
	// a register there. The answer comes out of the model's log, which carries
	// one line for every access to an offset the model does not carry. The
	// expectation comes from this file's table, so the check fails when either
	// side moves.
	//
	// It cannot be satisfied by editing this file's table alone: adding a row
	// here makes this group pass again and case group 6's row count fail
	// instead.
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

		// CS6 and CS7 by name. The G2 wires six chip selects and no G2 signal
		// reaches the other two. The twelve-byte stride puts a seventh and an
		// eighth family at exactly these six offsets, and the 1998 manual marks
		// $0C8 and $0D4 Reserved. The model must answer at none of them.
		const uint32_t forbidden[] = {0x0c8, 0x0cc, 0x0d2, 0x0d4, 0x0d8, 0x0de};

		for(const uint32_t offset : forbidden)
			check(!modelAnswersAt(bus, offset),
				"the model carries no register at " + hex32(offset) + ", where a CS6 or CS7 family would sit, because AGENTS.md section 2.2 records that no G2 signal reaches them");
	}

	// -----------------------------------------------------------------------
	// Case group 12. Every register's upper boundary.
	//
	// Without it, changing the model's lookup so that a register claims one
	// byte past its end leaves every case green: nothing says a reserved offset
	// stays reserved, and nothing says a register does not reach into one.
	//
	// The byte directly above each register is asserted here, for every row
	// whose upper neighbour is not another register. Rows whose neighbour is
	// another register are covered by case group 11's sweep instead.
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
	// Case group 13. A write to a reserved address has no effect.
	//
	// Group 7 only reads a reserved offset and group 10 only writes one and
	// looks at the log. Neither writes a reserved offset and reads it back, so
	// without this group the protection could be removed with every case still
	// green.
	//
	// Without the protection the model stores a firmware write into reserved
	// MBAR space and hands it back on a later read, so a misdirected write
	// reads back exactly like a modelled one.
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

		// The misdirected write, in the shape the firmware would make it. A
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
	// Case group 14. The model's whole write-protect mask, recovered from
	// behaviour.
	//
	// Group 13 drives the protection at four offsets. This drives it at all
	// g_simSpaceSize of them, by writing both polarities to every byte and
	// reading each back, which recovers the model's mask without reading a line
	// of sim.cpp. It pins three facts at once: every reserved byte is fully
	// protected, every read-only register is fully protected, and every bit a
	// board strap holds is protected while its neighbours in the same register
	// are not.
	//
	// What this group catches is the mask being narrowed, on either arm, so
	// that a strap bit becomes writable. Sim.cpp carries a static_assert for
	// the same invariant at compile time.
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
	// Case group 15. The SIM's own width guard.
	//
	// sim.cpp rejects any width that is not 8, 16 or 32, and no case above
	// drives it, because every access above goes through the decode and the
	// decode carries the same guard and answers first. The two guards are
	// separate code in separate files and each needs its own case.
	//
	// This is the only group that drives the SIM at its BusTarget interface.
	// Driving it through the decode could never reach the branch.
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

		// The two guards are separate: the same widths driven through the
		// decode are rejected by the decode, and the SIM never sees them.
		bus.sim().clearLog();
		status = MCF5307_BUS_OK;
		bus.read(0x080, 24, status);

		checkEqual(status, MCF5307_BUS_SIZE_ILLEGAL, "the decode rejects a 24-bit read before the SIM sees it");
		checkEqual(bus.sim().log().size(), size_t(0),
			"the SIM writes no log line for a width the decode already rejected, so the two width guards are separate layers");
	}

	// -----------------------------------------------------------------------
	// Case group 16. An accepted wide access is byte-addressed, and what it
	// reaches is pinned.
	//
	// Case group 4 drives every unrestricted register at all three widths and
	// asserts only that the access is accepted, never that acceptance is
	// non-destructive. A 32-bit access at CSCR0, which is two bytes at 0x08a,
	// runs into CSAR1 at 0x08c.
	//
	// That is intended. The model treats an access as a run of bytes from the
	// offset it is given, exactly as the part's bus does, and applies the write
	// protection byte by byte. A later change that made a wide access stop at
	// the register boundary would fail here.
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

	// -----------------------------------------------------------------------
	// Case group 17. THE WINDOW BOUND. AN ACCESS THAT RUNS OFF THE TOP OF THE
	// MBAR WINDOW REACHES NOTHING BEYOND IT.
	//
	// sim.h sizes m_space and m_writeProtect at g_simSpaceSize each and
	// declares them one after the other, so THE BYTE ONE PAST THE TOP OF
	// m_space IS THE FIRST BYTE OF m_writeProtect. Sim::read and Sim::write
	// each carry a bound check for that reason, and UNTIL NOW NO CASE DROVE
	// EITHER: deleting both left every case above green.
	//
	// THE ACCESS BELOW IS NOT EXOTIC, AND IT IS NOT A BACK DOOR. It arrives
	// through the same decode every other group in this file uses.
	// MemoryMap::decode examines only the START address, so MBAR+0x3fe is
	// inside the one-kilobyte window and a 32-bit access there is handed to the
	// SIM as offset 0x3fe. The SIM then walks four bytes, 0x3fe to 0x401, and
	// the last two lie past the top of the window.
	//
	// IT IS THE COMPLEMENT OF CASE GROUP 16. Group 16 pins that a wide access
	// does not stop at a REGISTER boundary. This group pins that it does stop
	// at the WINDOW boundary. The two are different boundaries and only one of
	// them bounds the storage.
	//
	// WHAT THE FAILURE WOULD COST. Without the write bound the model stores two
	// bytes of a firmware write into m_writeProtect, which UNPROTECTS reserved
	// MBAR space that UM Table 9-5 footnote 1 requires to stay inert - so a
	// later write to a reserved offset would be kept, which is the exact defect
	// case group 13 exists to prevent. Without the read bound the model hands
	// protection bytes back as if they were register contents. Both are
	// out-of-bounds accesses on a fixed-size array, reached through the
	// ordinary bus entry point.
	//
	// THE LOG LINE IS ASSERTED ON BOTH HALVES, AND PLAN SECTION 13.1 REQUIRES
	// IT: the case "asserts the model refuses it AND writes one log line naming
	// the offset, the width and the direction". The refusal above was asserted
	// and the log line was not, so the trace half of that sentence rested on
	// nobody having looked - the same shape group 18 repaired for the byte-run
	// contract.
	//
	// THE LINE READS UNMODELLED, AND THAT IS THE WHOLE POINT OF ASSERTING IT.
	// No register covers MBAR+0x3fe, so Sim::find answers null and the access
	// is traced exactly as any other unmodelled access is. An access that
	// silently walks off the top of the window with NO trace is the failure
	// this asserts against: the operator would see the model complete an
	// out-of-bounds access and report nothing about it. The offset asserted is
	// the one the access STARTED at, which is the rule group 18 states.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		// THE READ BOUND. The two bytes past the top must contribute nothing.
		// They are the write-protect bytes of MBAR+0x000 and MBAR+0x001, which
		// no register covers and which therefore hold 0xff, so an UNBOUNDED
		// read returns 0x0000ffff here and a bounded one returns zero. The
		// discriminating value is a property of the model's own reset state and
		// not of any allocator, so this case is deterministic.
		bus.sim().clearLog();
		const uint32_t value = bus.read(0x3fe, 32, status);

		checkEqual(status, MCF5307_BUS_OK,
			"a 32-bit read that runs off the top of the MBAR window completes rather than faulting");
		checkEqual(value, uint32_t(0),
			"a 32-bit read at MBAR+0x3fe reads zero for the two bytes that lie past the top of the window, rather than the bytes the model stores after it");
		checkEqual(bus.sim().log().size(), size_t(1),
			"the 32-bit read at MBAR+0x3fe writes exactly one log line, so an access that runs off the top of the window is never silent");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED read of 32 bits at offset 0x000003fe"),
			"the log line of the read names the offset it started at, its width and its direction");
	}
	{
		// THE WRITE BOUND, observed exactly where an unbounded write would
		// land. The first byte past m_space is the write-protect byte of
		// MBAR+0x000, so an unbounded write at 0x3fe unprotects part of a
		// reserved offset. modelWritableMask recovers that protection from
		// behaviour, in the same way case group 14 does.
		//
		// THE FIRST CASE IS THE BEFORE PICTURE and it is not decoration: it
		// establishes that the mask this group watches starts fully protected,
		// so the second case's 0x00 is a value that was OBSERVED to be
		// reachable rather than one that could never have been anything else.
		Bus bus;

		checkEqual(uint32_t(modelWritableMask(bus, 0x000)), uint32_t(0x00),
			"the reserved offset 0x000 is fully write-protected before anything runs off the top of the window");
	}
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		bus.sim().clearLog();
		bus.write(0x3fe, 32, 0xdeadbeefu, status);

		checkEqual(status, MCF5307_BUS_OK,
			"a 32-bit write that runs off the top of the MBAR window completes rather than faulting");

		// ASSERTED BEFORE modelWritableMask RUNS, because that helper drives
		// four further accesses of its own at an offset no register covers and
		// each of them appends to the same log.
		checkEqual(bus.sim().log().size(), size_t(1),
			"the 32-bit write at MBAR+0x3fe writes exactly one log line, so a write that runs off the top of the window is never silent");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED write of 32 bits at offset 0x000003fe"),
			"the log line of the write names the offset it started at, its width and the write direction");

		checkEqual(uint32_t(modelWritableMask(bus, 0x000)), uint32_t(0x00),
			"the reserved offset 0x000 is STILL fully write-protected after a 32-bit write at MBAR+0x3fe, so the write reached no byte past the top of the window");
	}

	// -----------------------------------------------------------------------
	// Case group 18. THE MIRROR OF CASE GROUP 16: AN ACCESS THAT STARTS IN
	// RESERVED SPACE AND RUNS INTO A REGISTER.
	//
	// GROUP 16 PINS THE BYTE-RUN CONTRACT IN ONE DIRECTION ONLY - an access
	// that starts INSIDE a register and runs out of it - while claiming the
	// behaviour is "pinned either way". The mirror was unpinned, and two
	// separate mutations proved it: making Sim::write discard an access whose
	// START offset is unmodelled left every case above green, and so did making
	// Sim::read return zero for the same case.
	//
	// THE DIRECTION THIS PINS, AND WHY IT IS THE RIGHT ONE. The model keeps the
	// byte-run reading: the bytes that land inside a register DO reach it, and
	// the log line records the offset the access STARTED at. Three things
	// already in the model decide this, and none of them admits the other
	// reading:
	//
	//   * GROUP 16 STATES THE RULE WITHOUT A DIRECTION. "The model treats an
	//     access as a run of bytes from the offset it is given, exactly as the
	//     part's bus does, and applies the write protection byte by byte." A
	//     run of bytes has no start-offset exemption, so the reading that
	//     discards on an unmodelled start is not a narrower version of that
	//     rule - it CONTRADICTS it.
	//   * THE PROTECTION IS PER BYTE. Group 13's hole is kept inert by the
	//     write-protect mask of ITS OWN bytes, not by any decision taken at the
	//     access's start offset. Discarding on an unmodelled start would be a
	//     SECOND and redundant mechanism answering the same question, and the
	//     two would disagree on exactly this case.
	//   * sim.h DEFINES THE LOG as carrying one line "for every access to an
	//     offset the manual assigns to no register this model carries". That is
	//     the access's offset, singular - the one it was given. Logging by
	//     start offset and transferring by byte run are orthogonal facts, which
	//     is why the two halves below assert them separately.
	//
	// THE OFFSET IS DRAWN FROM THE DRAM CONTROLLER ON PURPOSE. UM Table 11-1
	// gives DCR at $100 and DACR0 at $108, so $102 to $107 belong to no
	// register, and sim.cpp records those rows as VERIFIED against the manual.
	// The chip-select block carries an OPEN MASK-REVISION QUESTION and its holes
	// could move if that question is answered against the model; this hole
	// cannot. What is pinned here therefore does not rest on the answer.
	//
	// WHAT THE FAILURE WOULD COST. It is the harm case group 13 names, arriving
	// from the other side. A 32-bit write at $106 leaves DACR0 holding half of
	// it while the log calls the access UNMODELLED - a misdirected write that
	// reads back exactly like a modelled one, which is the one thing BRD-5's
	// anomaly log exists to make visible.
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		bus.sim().clearLog();
		bus.write(0x106, 32, 0x11223344u, status);

		checkEqual(status, MCF5307_BUS_OK,
			"a 32-bit write starting in the reserved hole below DACR0 completes rather than faulting");
		checkEqual(bus.sim().log().size(), size_t(1),
			"the write writes exactly one log line, because the offset it starts at is one no register covers");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED write of 32 bits at offset 0x00000106"),
			"the log line names the offset the access STARTED at, and not the register it ran into");
		checkEqual(bus.read(0x108, 32, status), uint32_t(0x33440000u),
			"DACR0 takes the two bytes of the write that landed inside it, because a wide access is a run of bytes and does not stop where a register BEGINS");
		checkEqual(bus.read(0x106, 16, status), uint32_t(0),
			"the two reserved bytes the write started in take none of it");
		checkEqual(bus.read(0x100, 16, status), uint32_t(0),
			"DCR, below the hole, takes nothing from the write");
	}
	{
		Bus bus;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		bus.write(0x108, 32, 0xa5a5a5a5u, status);

		bus.sim().clearLog();
		const uint32_t value = bus.read(0x106, 32, status);

		checkEqual(status, MCF5307_BUS_OK,
			"a 32-bit read starting in the reserved hole below DACR0 completes rather than faulting");
		checkEqual(bus.sim().log().size(), size_t(1),
			"the read writes exactly one log line, because the offset it starts at is one no register covers");
		checkEqual(bus.logLine(0),
			std::string("sim: UNMODELLED read of 32 bits at offset 0x00000106"),
			"the log line of the read names the offset the access STARTED at");
		checkEqual(value, uint32_t(0x0000a5a5u),
			"the read returns the two reserved bytes as zero and the two bytes of DACR0 it ran into, so a read is a run of bytes in the same way a write is");
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
