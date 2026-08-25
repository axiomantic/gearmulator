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
// answers its own register block, whose bound mbus.h carries. The interrupt
// controller answers its three register groups, whose bounds
// interruptController.h carries.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mcf5307.h>

#include "dspSet.h"
#include "flash.h"
#include "hdi08Adapter.h"
#include "interruptController.h"
#include "latches.h"
#include "max1039.h"
#include "mbus.h"
#include "memoryMap.h"
#include "panel.h"
#include "sim.h"
#include "status.h"
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
		 * widens at the call site.
		 *
		 * THE RETURN MAY EXCEED `wantCycles`, by up to the cost of one
		 * instruction, because mcf5307_exec finishes the instruction it
		 * started. That overrun is not a defect to absorb here: it is what
		 * g2::runQuantum's cycle debt exists to carry, and clamping it in this
		 * method would make the debt identically zero. */
		uint32_t runMcu(uint32_t wantCycles) noexcept;

		/* TRUE when the MCF5307 core stopped because an instruction TRAPPED --
		 * a bus error, an illegal instruction word, an illegal effective address
		 * for the opcode, an illegal operand size or a divide by zero.
		 *
		 * Fault and halt are different flags and this method reports the fault.
		 * mcf5307.h is the authority: a valid
		 * opcode with no implemented semantics halts WITHOUT faulting, and a
		 * faulted core is always also halted. mcuHalted() below is the wider
		 * condition. */
		bool faulted() const noexcept;

		/* A Board whose core pointer is nil answers a defined value rather than
		 * dereferencing it, in the shape runMcu uses for that case.
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
		 * reset. It is the strictly wider condition of the two the core reports:
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
		 * stateLoad's RETURN TYPE IS RECONCILED, AND THE DEVIATION THAT STOOD
		 * HERE IS QUOTED RATHER THAN DELETED. It read: "the plan's Check line
		 * for this task lists stateLoad among the six methods without pinning
		 * its return type, and g2::Status is owned by task SCH-18 ... so
		 * stateLoad returns void here ... The reconciliation happens there,
		 * once SCH-18 has created status.h." status.h exists, and SCH-21 step 4
		 * -- which absorbed SCH-24 -- is the task that owns the correction.
		 * stateLoad now returns g2::Status, which is design section 13.10.5's
		 * POST-correction declaration.
		 *
		 * WHAT IT REPORTS. Status::Ok, or Status::BadStateImage for an image
		 * whose version word is not the one this build writes. The version word
		 * was WRITTEN by stateSave and READ BY NOTHING before this pass, which
		 * made it a guard that could not fire: the only thing a version word is
		 * for is refusing, and a void return had nowhere to refuse to. Design
		 * section 13.10 rule 2 forbids an exception and a release build removes
		 * an assertion, so the return value is the whole channel.
		 *
		 * THE GUARD IS BEFORE THE FIRST WRITE, so a refused load changes
		 * nothing. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* THE RESET. Task SCH-21 step 3, design section 13.10.5.
		 *
		 * WHAT IT COVERS: the MCF5307 core, through the same mcf5307_reset the
		 * resetMcu above drives; this class's own snapshot state -- the fault
		 * bit and the last frame index; and the DSP set, through
		 * DspSet::reset.
		 *
		 * WHAT IT DOES NOT COVER, STATED HERE RATHER THAN LEFT TO BE FOUND.
		 * Design section 13.10.5 says a reset "zeroes every emulated memory",
		 * AND THE BUS TARGETS ATTACHED TO THIS BOARD ARE NOT ZEROED BY THIS
		 * CALL: the flash images, the SDRAM window, the latches, the panel
		 * surface, the MBAR block and the USB device all keep what they held.
		 * The reason is structural and is not a decision taken here: BusTarget
		 * declares read and write and NOTHING ELSE, MemoryMap hands out a
		 * BusTarget* and offers no walk over the attached set, and no unit in
		 * this tree carries a reset of its own. Covering them needs a reset on
		 * the BusTarget interface and one implementation for each unit, which
		 * is an edit across seven owners rather than inside this file. A
		 * CALLER THAT NEEDS A CLEARED SDRAM MUST STILL RECONSTRUCT THE BOARD.
		 *
		 * NO EXCEPTION. Design section 13.10 rule 2. */
		void reset() noexcept;

		/* THE BUS, AS THE MEMORY MAP SEES IT. onRead and onWrite forward here,
		 * so this is the routing itself; a caller may drive it without running
		 * a program.
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
		/* The one interrupt controller of the whole machine. Every source on
		 * this board arbitrates through it -- both timers and UART0 -- because
		 * two controllers would each arbitrate over half the sources and
		 * neither would see the winner. */
		InterruptController& interrupts() { return m_interrupts; }

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
			MbarRouter(Sim& _sim, Uart0& _uart0, MBus& _mbus, InterruptController& _interrupts)
				: m_sim(_sim), m_uart0(_uart0), m_mbus(_mbus), m_interrupts(_interrupts) {}

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

			/* True when the offset is one of the three register groups the
			 * interrupt controller answers -- IRQPAR, AVR and the internal
			 * control block. Every bound comes from interruptController.h, so
			 * this file states no register address of its own.
			 *
			 * The controller is not a BusTarget, which is why it is not in
			 * select() below. Every one of its registers is an 8-bit register,
			 * so read and write dispatch to it directly and the BusTarget arm
			 * below is left for the units that have one. */
			static bool isInterruptOwned(uint32_t _offset);

			BusTarget& select(uint32_t _offset);

			Sim&                 m_sim;
			Uart0&               m_uart0;
			MBus&                m_mbus;
			InterruptController& m_interrupts;
		};

		/* The ISP1181 answers CS3. The decode subtracts the window base and
		 * hands the offset down, and the device expects exactly such a
		 * window-relative address, so the offset is forwarded unaltered and
		 * every address in the window is accepted. Splitting the offsets into
		 * the command and data ports the part multiplexes onto A0/A4 is the
		 * full model's business and not this adapter's.
		 *
		 * It holds a reference to the handle rather than a copy of it, because
		 * the handle does not exist until the constructor body calls
		 * isp1181_create -- after this adapter is constructed -- and a copy
		 * taken here would stay nil for the adapter's whole life. */
		class Isp1181Window final : public BusTarget
		{
		public:
			explicit Isp1181Window(isp1181_ctx*& _usb)
				: m_usb(_usb) {}

			uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		private:
			isp1181_ctx*& m_usb;
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
		 * measured reason they are reachable. The acknowledge callback stays
		 * private: only the core drives it, and its body says why it clears
		 * nothing. */
		static void     onInterruptAck(void* user, int level, uint8_t vector);

		/* The controller's present callback hands the whole current state to
		 * the sink and does nothing else: no arbitration, no pending bit and no
		 * priority decision. The `autovector` argument is forwarded and not
		 * decided -- the controller has already read the AVEC bit of the
		 * winning source's ICR, and a present function that chose either form
		 * here would silently override the bit the firmware programmed.
		 *
		 * It is a no-op while the core handle is null. The controller exists
		 * before `mcf5307_create` returns, and `Uart0`'s constructor programs
		 * its vector into the controller, which presents; that presentation
		 * has no core to reach. */
		static void     onInterruptPresent(void* user, int level, uint8_t vector,
		                                   int autovector);

		/* DECLARATION ORDER IS INITIALISATION ORDER AND IT IS LOAD-BEARING
		 * HERE. m_memory is declared before the adapters because they bind
		 * references to it, and the units are declared before the adapters
		 * that forward to them.
		 *
		 * m_mcu and m_interrupts are first, and that position is the reason
		 * onInterruptPresent can read them at all. Constructing m_uart0 asks
		 * the controller to record its vector, and the controller presents on
		 * every change -- so the present callback runs while the members below
		 * it are still raw storage. m_mcu is the member it reads, so it is
		 * initialised before any unit that can present exists. */
		mcf5307_ctx*        m_mcu;
		InterruptController m_interrupts;

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

		/* Declared BEFORE m_usb, so it is constructed while that handle is
		 * still null. It binds a reference to the handle rather than copying
		 * it, which is what makes the order harmless: the assignment in the
		 * constructor body is what every later read sees. Storing the handle
		 * by value here would capture the null instead. */
		Isp1181Window m_usbCs3;

		/* The ISP1181 USB device this Board owns. Design sections 5.2 and 9.4
		 * put it on the Board, and tickSofIfDue is what advances it. The Board
		 * creates it in the constructor and destroys it in the destructor, so
		 * its lifetime is exactly the Board's; task BRD-22 owns both. */
		isp1181_ctx* m_usb;

		uint64_t     m_lastFrameIndex = 0;
		bool         m_faulted        = false;

		/* Last, and that position is destruction order and not construction
		 * order. The set borrows nothing at construction -- the bridges are
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

