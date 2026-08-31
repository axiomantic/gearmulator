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
		/* Creates the MCF5307 core context, initialises the Nim runtime once,
		 * and logs the G2_MCU_CORE_CLOCK_HZ placeholder line exactly once. */
		Board();
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

	private:
		/* The installed bus callbacks of the MCF5307 core. These accept every
		 * access and report success, the shape required of a board that models
		 * no fault. */
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

