// The Board is the MCU substrate the Scheduler runs on, and it is the one
// object that every CS0 to CS5 device and the TransportHub eventually hang
// off.
//
// The surface the Scheduler uses is exactly six methods, and the Scheduler
// never touches anything else on the Board:
//
//     runMcu(uint32_t) -> uint32_t    one quantum of the MCU context
//     faulted()                       true when the MCF5307 core has halted
//     tickSofIfDue(uint64_t)          the 1 kHz USB start-of-frame tick
//     stateSize / stateSave / stateLoad
//
// The Board is concrete, and that is a requirement, not an accident. The
// Scheduler's job array and the ChainAdapter's callbacks point into it, so it
// must never move, be copied, or participate in dynamic dispatch; `final` plus
// the static_asserts below make that a compile-time property rather than a
// convention.
//
// The Board logs one line at construction naming G2_MCU_CORE_CLOCK_HZ and
// stating that the value 45,000,000 is a placeholder -- the lowest in-spec
// catalog speed grade. No golden reference and no capture may be recorded
// until the real value is measured.

// THE COMPOSITION, ADDED BY TASK INT-1 UNDER PLAN SECTION 24.6 ROW W3-115.
// The bus callbacks below used to accept every access and return zero, and the
// comment that described that shape is rewritten rather than left standing: the
// board now routes every access through the BRD-1 MemoryMap to the seven units
// W3-115 names. INT-1's scope in this file is the composition and nothing else.
//
// WHERE EVERY WINDOW COMES FROM. FOUR bases are recorded by AGENTS.md section
// 2.2 and live as constants in memoryMap.h: CS1, CS3, CS5 and the SDRAM. CS0's,
// CS2's and CS4's bases are recorded by NO AUTHORITY, and NO AUTHORITY RECORDS
// A SIZE FOR ANY WINDOW. Plan section 1.3 rule 1 therefore makes every base and
// every size CONFIGURATION, and BoardConfig below is how a caller supplies it.
// THIS FILE INVENTS NO ADDRESS AND SHIPS NO DEFAULT LAYOUT, for the reason
// memoryMap.h already gives: three of the eight bases have nothing to take a
// default from. A default-constructed Board therefore answers NOWHERE, which is
// the honest answer for a board nobody has configured.
//
// THE MBAR WINDOW IS SHARED AND THE MemoryMap ATTACHES ONE TARGET PER REGION,
// so a small router sits between the units that answer it. The SIM-and-UART
// split is not a choice made here: sim.cpp's DIVERGENCE note states it. The SIM
// answers MBAR+0x1D0 because the firmware reads it as a model strap and BRD-2's
// check requires it, and BRD-4 "owns every other UART offset". The M-Bus module
// answers its own register block, whose bound mbus.h carries.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mcf5307.h>

#include "flash.h"
#include "hdi08Adapter.h"
#include "latches.h"
#include "max1039.h"
#include "mbus.h"
#include "memoryMap.h"
#include "panel.h"
#include "sim.h"
#include "uart0.h"

namespace g2
{
	/* The whole board layout a caller supplies. It carries no default address
	 * of its own: `memory` starts with every window absent (size zero), which
	 * answers at no address at all, and a caller fills in the windows the
	 * machine it is modelling actually has. */
	struct BoardConfig
	{
		MemoryMapConfig memory;

		/* The populated-port set of the HDI08 array. The expanded machine is
		 * the default because AGENTS.md section 4.1 targets it; it reaches no
		 * address unless the caller also gives CS1 a window. */
		Hdi08Decode hdi08{g_hdi08ExpandedPorts};

		/* The one two-wire slave the machine carries. Its potentials start at
		 * zero, because the only figure anyone has for this board is a schematic
		 * annotation and a shipped default would make it look measured. A caller
		 * that wants conversions supplies them. */
		Max1039Config adc;
	};

	// The Board. Concrete, final, neither copyable nor movable. Constructed by
	// the Device subclass before the Scheduler and destroyed after it; the
	// Scheduler borrows the Board and never destroys it.
	class Board final
	{
	public:
		/* Creates the MCF5307 core context, initialises the Nim runtime once,
		 * and logs the G2_MCU_CORE_CLOCK_HZ placeholder line exactly once. */
		Board();

		/* The composed board of task INT-1. It builds the seven units from
		 * `_config`, attaches each to the region it answers, and points the
		 * MCF5307 core's bus callbacks at the decode. Every base and every
		 * size comes from `_config`; this class chooses none of them. */
		explicit Board(const BoardConfig& _config);

		~Board();

		Board(const Board&)            = delete;
		Board& operator=(const Board&) = delete;
		Board(Board&&)                 = delete;
		Board& operator=(Board&&)      = delete;

		/* One quantum of the MCU context. Returns the emulated cycles spent --
		 * exactly what mcf5307_exec returns. It forwards directly to
		 * mcf5307_exec, which already takes a cycle budget. uint32_t, not
		 * int64_t: it returns exactly what the core returned, and the Scheduler
		 * widens at the call site. */
		uint32_t runMcu(uint32_t wantCycles) noexcept;

		/* True when the MCF5307 core has halted, and that is the only condition
		 * it reports. The C ABI exports no halt getter at the pinned core
		 * commit, so the fault bit is held on the Board. */
		bool faulted() const noexcept;

		/* The 1 kHz USB start-of-frame tick. The Board owns the test: the
		 * Scheduler calls this on every frame, unconditionally, passing the
		 * authoritative virtual frame index, and the Board tests
		 * frameIndex % 96 == 0 itself. The body here records the frame index
		 * only. */
		void tickSofIfDue(uint64_t frameIndex) noexcept;

		/* The MCU context's determinism-relevant state, embedded in the
		 * Scheduler snapshot. This serialises the Board's own state only: the
		 * pinned core commit exports no Nim mcf5307_state_* or isp1181_state_*
		 * block, and those are appended once it does.
		 *
		 * stateLoad deliberately returns void rather than g2::Status: that type
		 * does not exist yet. It is reconciled once status.h exists. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		void   stateLoad(const void* src) noexcept;

		/* THE BUS, AS THE MEMORY MAP SEES IT. onRead and onWrite forward here,
		 * so this is the routing itself; a caller may drive it without running
		 * a program.
		 *
		 * `_size` HERE IS A WIDTH IN BITS -- 8, 16 or 32 -- which is the
		 * MemoryMap's unit and NOT the core's. The two callbacks below take the
		 * core's unit and convert; this pair is below that conversion. */
		uint32_t busRead(uint32_t _address, int _size, mcf5307_bus_status& _status);
		void     busWrite(uint32_t _address, int _size, uint32_t _value,
		                  mcf5307_bus_status& _status);

		/* THE INSTALLED CALLBACKS, PUBLIC ON PURPOSE, AND THE REASON IS A
		 * DEFECT THAT WAS MEASURED RATHER THAN IMAGINED. These are the exact
		 * function pointers handed to mcf5307_create, so they are the path the
		 * CORE takes. An earlier revision of this composition kept them private
		 * and the check drove busRead instead; restoring the old
		 * "return 0u with a bus-OK status" body into onRead then left the check
		 * fully GREEN, because nothing exercised the forwarding. The check now
		 * drives THESE, and they are reachable for exactly that reason.
		 *
		 * THEY ARE NOT A SECOND ROUTE INTO THE BOARD. Each one forwards to
		 * busRead or busWrite and converts the one argument whose unit differs
		 * between the two sides, and does nothing else, so there is one routing
		 * path and this is its entry point rather than a parallel copy.
		 *
		 * `size` HERE IS A COUNT OF BYTES -- 1, 2 or 4 -- because that is what
		 * mcf5307.h hands an mcf5307_read_fn and an mcf5307_write_fn, and these
		 * two ARE that pair. It is NOT a width in bits. busRead and busWrite
		 * above take bits, and the conversion between the two units happens
		 * here and nowhere else. A CALLER THAT DRIVES THESE DIRECTLY -- which
		 * is what every test of the routing should do, for the reason stated
		 * above -- MUST THEREFORE SUPPLY 1, 2 OR 4. Any other value is refused
		 * as MCF5307_BUS_SIZE_ILLEGAL, 8, 16 and 32 included: they are legal
		 * widths in the OTHER unit and were silently accepted here before the
		 * conversion existed, which is precisely the defect. */
		static uint32_t onRead(void* user, uint32_t addr, int size,
		                       mcf5307_bus_status* status);
		static void     onWrite(void* user, uint32_t addr, int size,
		                        uint32_t value, mcf5307_bus_status* status);

		/* The units, so a caller can load the flash images, install the HDI08
		 * callbacks the DSP side needs, feed UART0 and read each unit's own
		 * log. The Board owns every one of them and hands out references
		 * rather than copies: none of these types is copyable in a meaningful
		 * sense and the Scheduler's callbacks point into them. */
		Flash&        flash()   { return m_flash; }
		Panel&        panel()   { return m_panel; }
		Latches&      latches() { return m_latches; }
		Hdi08Adapter& hdi08()   { return m_hdi08; }
		Sim&          sim()     { return m_sim; }
		Uart0&        uart0()   { return m_uart0; }
		MBus&         mbus()    { return m_mbus; }
		Max1039&      adc()     { return m_adc; }
		MemoryMap&    memory()  { return m_memory; }

	private:
		/* ONE FLASH OBJECT ANSWERS TWO WINDOWS, so it cannot be a BusTarget
		 * itself: a BusTarget is attached to a single region and receives a
		 * window-relative offset, while Flash addresses its two images
		 * ABSOLUTELY and tells them apart by address. This adapter is the join.
		 *
		 * IT TAKES THE BASE FROM THE MemoryMap RATHER THAN KEEPING ITS OWN
		 * COPY, and that is the whole reason it holds a reference instead of a
		 * uint32_t. The decode has already subtracted the window base to make
		 * the offset, and this adapter adds it back; if the two used separate
		 * copies of that number, a mutation of either one would leave the pair
		 * agreeing with itself and the test green. There is exactly one base
		 * here, and it is the one the decode used. */
		class FlashWindow final : public BusTarget
		{
		public:
			FlashWindow(Flash& _flash, const MemoryMap& _map, const Region _region)
				: m_flash(_flash), m_map(_map), m_region(_region) {}

			uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		private:
			uint32_t absolute(uint32_t _offset) const;

			Flash&           m_flash;
			const MemoryMap& m_map;
			Region           m_region;
		};

		/* THE MBAR WINDOW IS SHARED AND THE DECODE ATTACHES ONE TARGET PER
		 * REGION. This router is that one target, and it forwards the
		 * MBAR-relative offset the decode produced without altering it, because
		 * every unit behind it already expects an MBAR-relative offset.
		 *
		 * THE SIM-AND-UART SPLIT IS sim.cpp's AND NOT THIS FILE'S. Its
		 * DIVERGENCE note says the SIM answers MBAR+$1D0 because the firmware
		 * reads it as a model strap and BRD-2's check requires it, and that
		 * BRD-4 owns every other UART offset. That sentence is the whole of the
		 * second rule below.
		 *
		 * THE M-BUS ARM'S RANGE IS DISJOINT FROM BOTH UART BLOCKS, so the order
		 * the branches are written in is a reading convenience rather than a
		 * rule. */
		class MbarRouter final : public BusTarget
		{
		public:
			MbarRouter(Sim& _sim, Uart0& _uart0, MBus& _mbus)
				: m_sim(_sim), m_uart0(_uart0), m_mbus(_mbus) {}

			uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		private:
			// TRUE when the offset belongs to UART0's model rather than the
			// SIM's. The one strap offset the SIM answers is excluded here and
			// nowhere else, so the rule has a single site.
			static bool isUartOwned(uint32_t _offset);

			// TRUE when the offset belongs to the M-Bus module. The bound comes
			// from mbus.h, so this file states no register address of its own.
			static bool isMbusOwned(uint32_t _offset);

			BusTarget& select(uint32_t _offset);

			Sim&   m_sim;
			Uart0& m_uart0;
			MBus&  m_mbus;
		};

		// The strap offset the SIM answers inside the UART block. sim.cpp's
		// register table carries it as UIPCR and its DIVERGENCE note is the
		// authority for the SIM answering it.
		static constexpr uint32_t g_simUartStrapOffset = 0x1D0u;

		/* Attach every unit to the region it answers. It is called from the
		 * constructor and takes nothing: an ABSENT window (size zero) decodes
		 * to no region at all, so attaching unconditionally is what makes a
		 * default-constructed Board answer nowhere without a second code path
		 * that could disagree with this one. */
		void attachUnits();

		/* onRead and onWrite are declared in the public section above, with the
		 * measured reason they are reachable. The acknowledge callback stays
		 * private: nothing drives it yet and the interrupt presentation is task
		 * BRD-3's. */
		static void     onInterruptAck(void* user, int level, uint8_t vector);

		/* DECLARATION ORDER IS INITIALISATION ORDER AND IT IS LOAD-BEARING
		 * HERE. m_memory is declared before the adapters because they bind
		 * references to it, and the units are declared before the adapters
		 * that forward to them. */
		MemoryMap    m_memory;

		Flash        m_flash;
		Panel        m_panel;
		Latches      m_latches;
		Hdi08Adapter m_hdi08;
		Sim          m_sim;
		Uart0        m_uart0;

		/* The slave is declared BEFORE the controller that points at it, for
		 * the same reason the units are declared before the adapters: the
		 * controller binds its address at construction. */
		Max1039      m_adc;
		MBus         m_mbus;

		FlashWindow  m_flashCs0;
		FlashWindow  m_flashCs2;
		MbarRouter   m_mbar;

		mcf5307_ctx* m_mcu;

		/* The ISP1181 USB device this Board owns. Design sections 5.2 and 9.4
		 * put it on the Board, and tickSofIfDue is what advances it. The Board
		 * creates it in the constructor and destroys it in the destructor, so
		 * its lifetime is exactly the Board's; task BRD-22 owns both. */
		isp1181_ctx* m_usb;

		uint64_t     m_lastFrameIndex = 0;
		bool         m_faulted        = false;
	};

	// Concreteness as a compile-time property, so that "nothing derives from
	// it, no virtual, neither copyable nor movable" cannot be silently lost.
	static_assert(!std::is_polymorphic_v<Board>,
	              "Board must be concrete: no virtual method and no vtable");
	static_assert(!std::is_copy_constructible_v<Board>,
	              "Board must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<Board>,
	              "Board must not be copy assignable");
	static_assert(!std::is_move_constructible_v<Board>,
	              "Board must not be move constructible");
	static_assert(!std::is_move_assignable_v<Board>,
	              "Board must not be move assignable");
}

