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

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mcf5307.h>

namespace g2
{
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
		 * body of this method. This task declares the method so the Scheduler
		 * surface is complete; the body here records the frame index only. */
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

	private:
		/* The installed bus callbacks of the MCF5307 core. The real routing to
		 * the CS0 to CS5 devices is the board-track integration that follows
		 * this surface task; for the T0 surface these accept every access and
		 * report success, which is the shape design section 5.2.1 requires for
		 * a board that models no fault. */
		static uint32_t onRead(void* user, uint32_t addr, int size,
		                       mcf5307_bus_status* status);
		static void     onWrite(void* user, uint32_t addr, int size,
		                        uint32_t value, mcf5307_bus_status* status);
		static void     onInterruptAck(void* user, int level, uint8_t vector);

		mcf5307_ctx* m_mcu;
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

