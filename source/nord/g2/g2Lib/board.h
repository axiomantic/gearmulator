// Task BRD-21. The board header. Tier T0: this file carries no firmware
// artifact of any kind.
//
// Plan section 13.4, BRD-21. Design sections 6.4, 13.10 and 26.
// Logbook: AGENTS.md section 2.2 (the two clock domains and the MCU clock).
//
// WHAT THIS FILE IS. The Board is the MCU substrate the Scheduler runs on,
// and it is the one object that every CS0 to CS5 device and the TransportHub
// eventually hang off. THIS TASK ESTABLISHES THE SURFACE of the class and
// nothing more: task BRD-21 is the `The Board class` keystone of the board
// track (plan section 7.4), and its Check names t0_board_surface. The
// constructor's parameters are design section 6.4's business -- the flash
// image, the chip-select map, the panel configuration -- and the design
// deliberately declares no type for them here (design section 13.10). This
// task supplies a default constructor that owns the MCF5307 core context, so
// that the surface test can construct and exercise the class; later tasks
// widen the constructor as the CS0 to CS5 devices and the TransportHub land.
//
// THE SURFACE THE SCHEDULER USES IS EXACTLY SIX METHODS, and the Scheduler
// never touches anything else on the Board:
//
//     runMcu(uint32_t) -> uint32_t    one quantum of the MCU context
//     faulted()                       TRUE when the MCF5307 core has halted
//     tickSofIfDue(uint64_t)          the 1 kHz USB start-of-frame tick
//     stateSize / stateSave / stateLoad
//
// The Board is CONCRETE and that is a requirement, not an accident. It is
// declared `final` and the static_asserts below make "no virtual method,
// nothing derives from it, and it is neither copyable nor movable" a
// COMPILE-TIME property rather than a convention: the Scheduler's job array
// and the ChainAdapter's callbacks point into it, so it must never move, be
// copied, or participate in dynamic dispatch. The five static_asserts name
// their mechanism so that a test cannot pass by asserting nothing.
//
// THE MCU CLOCK PLACEHOLDER LOG. The Board logs one line at construction that
// names G2_MCU_CORE_CLOCK_HZ, states that the value 45,000,000 is a
// PLACEHOLDER, and names spike criterion (j) as its owner. Measurement
// register row 7 (plan section 4.1) owns all of this: G2_MCU_CORE_CLOCK_HZ is
// the lowest in-spec catalog speed grade until SPK-9 reports, and no golden
// reference and no capture may be recorded until then. t0_board_surface
// asserts the line is emitted exactly once per construction.

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
// THE MBAR WINDOW IS SHARED BY TWO UNITS AND THE MemoryMap ATTACHES ONE TARGET
// PER REGION, so a small router sits between them. The split is not a choice
// made here: sim.cpp's DIVERGENCE note states it. The SIM answers MBAR+0x1D0
// because the firmware reads it as a model strap and BRD-2's check requires it,
// and BRD-4 "owns every other UART offset".

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mcf5307.h>

#include "flash.h"
#include "hdi08Adapter.h"
#include "latches.h"
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
	};

	// The Board. Concrete, final, neither copyable nor movable. Constructed by
	// the Device subclass before the Scheduler and destroyed after it; the
	// Scheduler borrows the Board and never destroys it.
	class Board final
	{
	public:
		/* The constructor's parameters are design section 6.4's business (the
		 * flash image, the chip-select map, the panel configuration) and none
		 * of them is part of the surface the Scheduler uses, so the design
		 * declares no type for them and this task supplies a default
		 * constructor. It creates the MCF5307 core context, initialises the
		 * Nim runtime once, and logs the G2_MCU_CORE_CLOCK_HZ placeholder line
		 * exactly once. */
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
		 * exactly what mcf5307_exec returns. It forwards DIRECTLY to
		 * mcf5307_exec, which already takes a cycle budget. uint32_t, not
		 * int64_t: it returns exactly what the core returned, and the Scheduler
		 * widens at the call site. */
		uint32_t runMcu(uint32_t wantCycles) noexcept;

		/* TRUE when the MCF5307 core has halted, and that is the only condition
		 * it reports. One bit is all there is. T0 note: the C ABI does not yet
		 * export a halt getter at the pinned core commit, so this task holds the
		 * fault bit on the Board; the wiring that sets it from the core is a
		 * later cpu/board task. */
		bool faulted() const noexcept;

		/* The 1 kHz USB start-of-frame tick of design section 9.4. The Board
		 * owns the test: the Scheduler calls this on every frame,
		 * unconditionally, passing the authoritative virtual frame index, and
		 * the Board tests frameIndex % 96 == 0 itself. THE REAL TICK IS TASK
		 * BRD-22's (its Check is t0_sof_tick) and task BRD-22 owns board.cpp's
		 * body of this method. On each due frame it advances m_usb by exactly
		 * one SOF frame. */
		void tickSofIfDue(uint64_t frameIndex) noexcept;

		/* The MCU context's determinism-relevant state, embedded in the
		 * Scheduler snapshot. T0 note: the design says this embeds the Nim
		 * mcf5307_state_* and isp1181_state_* blocks of design section 5.2,
		 * but the pinned core commit exports none of them yet. This task
		 * serialises the Board's own state and documents the deviation (see
		 * the cpp); the Nim blocks are appended when a cpu task exports them.
		 *
		 * stateLoad DEVIATION, STATED RATHER THAN HIDDEN: the plan's Check line
		 * for this task lists stateLoad among the six methods without pinning
		 * its return type, and g2::Status is owned by task SCH-18 (Files:
		 * g2Lib/status.h), which is NOT in BRD-21's Depends: line. BRD-21 must
		 * not define g2::Status (that would be implementing SCH-18's owned
		 * file), so stateLoad returns void here, matching the design's
		 * PRE-correction declaration. Design section 13.10.5's "stateLoad
		 * returns g2::Status" is the POST-correction shape that task SCH-24
		 * owns, and SCH-24's own Check text says "the previous declaration
		 * returned void". The reconciliation happens there, once SCH-18 has
		 * created status.h. */
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

		/* THE MBAR WINDOW CARRIES TWO UNITS AND THE DECODE ATTACHES ONE TARGET
		 * PER REGION. This router is that one target, and it forwards the
		 * MBAR-relative offset the decode produced without altering it, because
		 * both units already expect an MBAR-relative offset.
		 *
		 * THE SPLIT IS sim.cpp's AND NOT THIS FILE'S. Its DIVERGENCE note says
		 * the SIM answers MBAR+$1D0 because the firmware reads it as a model
		 * strap and BRD-2's check requires it, and that BRD-4 owns every other
		 * UART offset. That sentence is the whole rule below. */
		class MbarRouter final : public BusTarget
		{
		public:
			MbarRouter(Sim& _sim, Uart0& _uart0) : m_sim(_sim), m_uart0(_uart0) {}

			uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		private:
			// TRUE when the offset belongs to UART0's model rather than the
			// SIM's. The one strap offset the SIM answers is excluded here and
			// nowhere else, so the rule has a single site.
			static bool isUartOwned(uint32_t _offset);

			BusTarget& select(uint32_t _offset);

			Sim&   m_sim;
			Uart0& m_uart0;
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
	// These assertions name their mechanism and Board is declared final,
	// which is what makes "nothing derives from it" a property at all.
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

