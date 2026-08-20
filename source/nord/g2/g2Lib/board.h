// The Board is the MCU substrate the Scheduler runs on, and it is the one
// object that every CS0 to CS5 device and the TransportHub eventually hang
// off.
//
// The surface the Scheduler uses is exactly six methods, and the Scheduler
// never touches anything else on the Board:
//
//     runMcu(uint32_t) -> uint32_t    one quantum of the MCU context
//     faulted()                       TRUE when an instruction TRAPPED
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
// The bases of CS1, CS3, CS5 and the SDRAM live as constants in memoryMap.h.
// CS0's, CS2's and CS4's bases are recorded by no authority, and no authority
// records a size for any window, so every base and every size is CONFIGURATION
// and BoardConfig below is how a caller supplies it. This file invents no
// address and ships no default layout. A default-constructed Board answers
// NOWHERE, which is the honest answer for a board nobody has configured.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mcf5307.h>

#include "dspSet.h"
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

		/* The populated-port set of the HDI08 array. The expanded machine is the
		 * default; it reaches no address unless the caller also gives CS1 a
		 * window. */
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

		/* Builds the units from `_config`, attaches each to the region it
		 * answers, and points the MCF5307 core's bus callbacks at the decode.
		 * Every base and every size comes from `_config`; this class chooses
		 * none of them. */
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

		/* TRUE when the MCF5307 core stopped because an instruction TRAPPED --
		 * a bus error, an illegal instruction word, an illegal effective address
		 * for the opcode, an illegal operand size or a divide by zero.
		 *
		 * FAULT AND HALT ARE DIFFERENT FLAGS AND THIS METHOD REPORTS THE FAULT,
		 * which is what its name says. mcf5307.h is the authority: a valid
		 * opcode with no implemented semantics halts WITHOUT faulting, and a
		 * faulted core is always also halted. mcuHalted() below is the wider
		 * condition. */
		bool faulted() const noexcept;

		/* THE HANDLE TO THE CORE THIS BOARD ALREADY OWNS. Each of these
		 * forwards to the matching mcf5307_ call and does nothing else, which is
		 * what runMcu above already does against mcf5307_exec. A Board whose
		 * core pointer is nil answers a defined value rather than dereferencing
		 * it, in the shape runMcu uses for that case.
		 *
		 * NO CREATE AND NO DESTROY IS PUBLISHED. The Board's lifetime already
		 * owns both, and a second pair would be a second core -- which is the
		 * shape this handle exists to make unnecessary.
		 *
		 * mcuReg and setMcuReg take the register file's own index: 0 to 7 are
		 * d0 to d7, 8 to 15 are a0 to a7, 16 is the status register and 17 is
		 * the program counter. mcf5307.h owns that mapping and this class
		 * restates none of it. setMcuReg answers FALSE for an out-of-range index
		 * and for a nil core, which is what the C call already answers. */
		void     resetMcu(uint32_t initialSp, uint32_t initialPc) noexcept;
		uint32_t mcuReg(int index) const noexcept;
		bool     setMcuReg(int index, uint32_t value) noexcept;

		/* TRUE when the core will run no further instruction until the next
		 * reset. It is the strictly WIDER condition of the two the core reports:
		 * every faulted core is halted and a halted core need not be faulted, so
		 * this is the one a caller asking "may I run more" must ask, and
		 * faulted() is the one a caller asking "did this instruction trap" must
		 * ask. */
		bool mcuHalted() const noexcept;

		/* The 1 kHz USB start-of-frame tick of design section 9.4. The Board
		 * owns the test: the Scheduler calls this on every frame,
		 * unconditionally, passing the authoritative virtual frame index, and
		 * the Board tests frameIndex % 96 == 0 itself. THE REAL TICK IS TASK
		 * BRD-22's (its Check is t0_sof_tick) and task BRD-22 owns board.cpp's
		 * body of this method. On each due frame it advances m_usb by exactly
		 * one SOF frame. */
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

		/* The bus as the memory map sees it. onRead and onWrite forward here, so
		 * this is the routing itself; a caller may drive it without running a
		 * program.
		 *
		 * `_size` here is a width in BITS -- 8, 16 or 32 -- which is the
		 * MemoryMap's unit and not the core's. The two callbacks below take the
		 * core's unit and convert; this pair is below that conversion. */
		uint32_t busRead(uint32_t _address, int _size, mcf5307_bus_status& _status);
		void     busWrite(uint32_t _address, int _size, uint32_t _value,
		                  mcf5307_bus_status& _status);

		/* The installed callbacks, public on purpose: these are the exact
		 * function pointers handed to mcf5307_create, so they are the path the
		 * CORE takes, and a test that drives busRead instead cannot see whether
		 * the forwarding happens at all.
		 *
		 * They are not a second route into the Board. Each one forwards to
		 * busRead or busWrite and converts the one argument whose unit differs
		 * between the two sides, and does nothing else.
		 *
		 * `size` here is a count of BYTES -- 1, 2 or 4 -- because that is what
		 * mcf5307.h hands an mcf5307_read_fn and an mcf5307_write_fn, and these
		 * two ARE that pair. busRead and busWrite above take bits, and the
		 * conversion between the two units happens here and nowhere else. A
		 * caller that drives these directly must therefore supply 1, 2 or 4; any
		 * other value is refused as MCF5307_BUS_SIZE_ILLEGAL, 8, 16 and 32
		 * included, because those are legal widths in the OTHER unit. */
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
		DspSet&       dspSet()  { return m_dspSet; }

	private:
		/* One Flash object answers two windows, so it cannot be a BusTarget
		 * itself: a BusTarget is attached to a single region and receives a
		 * window-relative offset, while Flash addresses its two images
		 * ABSOLUTELY and tells them apart by address. This adapter is the join.
		 *
		 * It holds a reference to the MemoryMap rather than its own copy of the
		 * base. The decode has already subtracted the window base to make the
		 * offset and this adapter adds it back; two separate copies of that
		 * number would keep agreeing with each other after either one moved. */
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

		/* The MBAR window is shared and the decode attaches one target per
		 * region. This router is that one target, and it forwards the
		 * MBAR-relative offset the decode produced without altering it, because
		 * every unit behind it already expects an MBAR-relative offset.
		 *
		 * The SIM answers MBAR+$1D0 because the firmware reads it as a model
		 * strap; UART0 owns every other UART offset. sim.cpp's DIVERGENCE note
		 * is the authority for that split.
		 *
		 * The M-Bus arm's range is disjoint from both UART blocks, so the order
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
		// register table carries it as UIPCR.
		static constexpr uint32_t g_simUartStrapOffset = 0x1D0u;

		/* Attach every unit to the region it answers. It is called from the
		 * constructor and takes nothing: an ABSENT window (size zero) decodes
		 * to no region at all, so attaching unconditionally is what makes a
		 * default-constructed Board answer nowhere without a second code path
		 * that could disagree with this one. */
		void attachUnits();

		/* onRead and onWrite are declared in the public section above, with the
		 * reason they are reachable. The acknowledge callback stays private:
		 * nothing drives it yet. */
		static void     onInterruptAck(void* user, int level, uint8_t vector);

		/* Declaration order is initialisation order and it is load-bearing here.
		 * m_memory is declared before the adapters because they bind references
		 * to it, and the units are declared before the adapters that forward to
		 * them. */
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

		/* The ISP1181 USB device this Board owns; tickSofIfDue is what advances
		 * it. The Board creates it in the constructor and destroys it in the
		 * destructor, so its lifetime is exactly the Board's. */
		isp1181_ctx* m_usb;

		uint64_t     m_lastFrameIndex = 0;
		bool         m_faulted        = false;

		/* LAST, AND THAT POSITION IS DESTRUCTION ORDER AND NOT CONSTRUCTION
		 * ORDER. The set borrows nothing at construction -- the bridges are
		 * attached from the constructor BODY, after every member exists -- but
		 * `~Hdi08Bridge` uninstalls through the host port it was handed, so a
		 * set destroyed after m_hdi08 would dereference a dead port once per
		 * slot. Members are destroyed in reverse declaration order, so any
		 * position before m_hdi08 is a use-after-free at teardown. */
		DspSet       m_dspSet;
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

