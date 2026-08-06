// Task BRD-1. The memory decode and the two bus callbacks.
//
// Plan section 13.1, BRD-1. Design sections 5.2.1, 6.4, 17 row 7.24.
// Logbook: AGENTS.md section 2.2.
//
// WHAT THIS FILE IS. The MCF5307 core owns no address map. It installs two
// callbacks and asks the board where each access goes. This file is the board
// side of that contract: one decode, and one pair of callbacks that carries
// all three access widths.
//
// THREE CHIP-SELECT BASES ARE NOT RECORDED BY ANY AUTHORITY, AND THIS FILE
// CARRIES NO NUMBER FOR THEM. AGENTS.md section 2.2 records CS1 at
// 0x11000000, CS3 at 0x13000000 and the CS5 latch at 0x15000000, and it
// records NO address for CS0, CS2 or CS4. AGENTS.md open question 21 carries
// all three and SPK-13 reads them from CSAR0 to CSAR5. Plan section 1.3 rule 1
// therefore applies: the base AND the size of those three windows arrive as
// configuration, and a caller supplies them.
//
// NO WINDOW SIZE IS RECORDED EITHER, so every size is configuration for the
// same reason. Only the four bases below have a source.
//
// THE DECODE REPORTS THROUGH THE OUT-PARAMETER AND NEVER ABORTS. It uses no
// language assert() as a reporting mechanism. The default build is Release and
// it defines NDEBUG, so an assert() is removed and a report built on one could
// never fire.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include <mcf5307.h>

namespace g2
{
	// The four bases AGENTS.md section 2.2 records. Nothing else in this file
	// is an address.
	constexpr uint32_t g_cs1Base = 0x11000000u;    // the HDI08 array
	constexpr uint32_t g_cs3Base = 0x13000000u;    // the ISP1181 USB device
	constexpr uint32_t g_cs5Base = 0x15000000u;    // the latches
	constexpr uint32_t g_sdramBase = 0x30000000u;  // the SDRAM

	enum class Region
	{
		None,
		Cs0,
		Cs1,
		Cs2,
		Cs3,
		Cs4,
		Cs5,
		Mbar,
		Sdram,
	};

	const char* toString(Region _region);
	std::ostream& operator<<(std::ostream& _out, Region _region);

	// One decoded window. A size of zero means the window is absent, and an
	// absent window answers at no address at all, including address zero.
	struct Window
	{
		uint32_t base = 0;
		uint32_t size = 0;
	};

	// The whole layout. A caller fills it. This library ships no default,
	// because three of the eight bases have no authority to take a default
	// from.
	struct MemoryMapConfig
	{
		Window cs0;
		Window cs1;
		Window cs2;
		Window cs3;
		Window cs4;
		Window cs5;
		Window mbar;
		Window sdram;
	};

	// What a device model presents to the decode. The offset is relative to
	// the base of the window the device sits in, so a device carries no
	// knowledge of where the firmware put it.
	class BusTarget
	{
	public:
		virtual ~BusTarget() = default;

		virtual uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) = 0;
		virtual void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) = 0;
	};

	class MemoryMap
	{
	public:
		explicit MemoryMap(const MemoryMapConfig& _config);

		// Region::None when no window answers. Windows are examined in the
		// order of the Region enumeration and the first match wins.
		Region decode(uint32_t _address) const;

		// The window of a region, so that a caller can turn an address into an
		// offset without repeating the layout.
		const Window& window(Region _region) const;

		void attach(Region _region, BusTarget* _target);
		BusTarget* target(Region _region) const;

		uint32_t read(uint32_t _address, int _size, mcf5307_bus_status& _status);
		void write(uint32_t _address, int _size, uint32_t _value, mcf5307_bus_status& _status);

		// One line for every access that did not complete. Design section
		// 5.2.1 rule 2 ties the report and the trace together, so a fault
		// cannot be reported without a trace of it.
		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		void logFailure(mcf5307_bus_status _status, bool _isWrite, int _size, uint32_t _address);

		MemoryMapConfig m_config;
		BusTarget* m_targets[9] = {};
		std::vector<std::string> m_log;
	};

	// The two callbacks the core installs. They are free functions with the
	// exact signatures of mcf5307_read_fn and mcf5307_write_fn, and `user` is
	// a MemoryMap*.
	//
	// ONE PAIR CARRIES ALL THREE WIDTHS. The `size` argument holds 8, 16 or 32,
	// so the board writes two handlers and not six, and the 32-bit case is
	// native rather than decomposed into byte cycles. The ColdFire does issue
	// 32-bit bus accesses, so a decomposition would be a model error.
	uint32_t memoryMapRead(void* _user, uint32_t _address, int _size, mcf5307_bus_status* _status);
	void memoryMapWrite(void* _user, uint32_t _address, int _size, uint32_t _value, mcf5307_bus_status* _status);
}
