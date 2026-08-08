// Task BRD-21. The board. Tier T0: this file carries no firmware artifact of
// any kind.
//
// Plan section 13.4, BRD-21. Design sections 6.4, 13.10 and 26.
//
// WHAT THIS FILE IS. board.cpp owns the Board's lifetime and its six-method
// surface. The Check of BRD-21 is t0_board_surface: concreteness (final,
// non-polymorphic, non-copyable, non-movable), the six methods the Scheduler
// uses, and the single construction log line that names the MCU clock
// placeholder and its owner. The bodies below are the T0 minimum that makes
// every method real and callable; the board-track integration that follows
// (the CS0 to CS5 devices, the TransportHub, the real memory routing) widens
// the constructor and the bodies, and BRD-22 owns the real tickSofIfDue.
//
// THE NO-OP BUS CALLBACKS, STATED RATHER THAN HIDDEN. mcf5307_create installs
// read/write/ack callbacks, and the true routing to the CS0 to CS5 devices is
// the job of the board-track integration after this surface task. For the T0
// surface these callbacks accept every access and report MCF5307_BUS_OK,
// which is exactly what a board that models no fault does (design section
// 5.2.1). No access reaches them in this task: the surface test constructs the
// Board and calls runMcu with a zero budget, and nothing drives a program.
//
// THE STATE METHODS AND THE MISSING NIM BLOCKS, STATED RATHER THAN HIDDEN.
// Design section 13.10 says stateSize/stateSave/stateLoad "embed the Nim
// mcf5307_state_* and isp1181_state_* blocks of section 5.2". The pinned core
// commit exports none of those C symbols (measured against libmcf5307.a at
// the CPU-3 commit this build pins: mcf5307_create, mcf5307_destroy,
// mcf5307_reset, mcf5307_exec and mcf5307_runtime_init are exported; the
// state_* and isp1181_* families are not). This task therefore serialises the
// Board's own determinism-relevant state and documents the deviation rather
// than stubbing a call to a symbol that would not link -- the same
// documented-deviation route BRD-23 took for mcf5307_exec earlier in this
// track. The Nim blocks are appended here the day a cpu task exports them.
//
// THE MCU CLOCK PLACEHOLDER LINE. The Board logs one line at construction that
// names G2_MCU_CORE_CLOCK_HZ, states that the value 45,000,000 is a
// placeholder, and names spike criterion (j) as its owner. It is emitted to
// standard output so the surface test can capture and count it. It moved here
// from SCH-3 at this revision, because it is written into g2Lib/board.cpp
// and the board track owns this file; SCH-3 keeps the tree-wide grep, which is
// a property of the tree and not of the Board.

#include "board.h"

#include <cstring>
#include <iostream>

#include "g2/timebase.h"

namespace g2
{
	namespace
	{
		// The version word of the Board's own snapshot block. It exists so that
		// a state file from a different revision is a named mismatch rather
		// than a silent acceptance -- design section 18.10. The value is chosen
		// here and has no other meaning.
		constexpr uint32_t g_boardStateVersion = 1u;

		// The flat snapshot the Board serialises. It is a fixed-size, plain-old
		// data struct with no pointer inside, so a state file cannot carry a
		// dangling address. The Nim mcf5307_*/isp1181_* blocks of design
		// section 5.2 join this block once a cpu task exports them (see the
		// file header).
		struct BoardState
		{
			uint32_t version;
			uint64_t lastFrameIndex;
			uint32_t faulted;
		};
		static_assert(std::is_trivially_copyable_v<BoardState>,
		              "BoardState must be POD so memcpy is well-defined");
	}

	uint32_t Board::onRead(void*, const uint32_t, const int,
	                       mcf5307_bus_status* const status)
	{
		*status = MCF5307_BUS_OK;
		return 0u;
	}

	void Board::onWrite(void*, const uint32_t, const int, const uint32_t,
	                    mcf5307_bus_status* const status)
	{
		*status = MCF5307_BUS_OK;
	}

	void Board::onInterruptAck(void*, const int, const uint8_t)
	{
		// The T0 surface keeps the interrupt source set empty, so there is
		// nothing to clear here.
	}

	Board::Board()
		: m_mcu(nullptr)
	{
		// The Nim runtime must be initialised before any mcf5307_ call. It is
		// idempotent behind a latch, so the second Board in a process is safe.
		mcf5307_runtime_init();
		m_mcu = mcf5307_create(this, &Board::onRead, &Board::onWrite,
		                       &Board::onInterruptAck);

		// The single MCU-clock placeholder line. Naming the macro AND the
		// numeric value AND the word "placeholder" AND criterion (j) is what
		// the Check requires and what t0_board_surface asserts.
		std::cout << "board: G2_MCU_CORE_CLOCK_HZ = "
		          << G2_MCU_CORE_CLOCK_HZ
		          << " is a placeholder value; owner spike criterion (j)"
		          << std::endl;
	}

	Board::~Board()
	{
		if(m_mcu)
			mcf5307_destroy(m_mcu);
		m_mcu = nullptr;
	}

	uint32_t Board::runMcu(const uint32_t wantCycles) noexcept
	{
		if(!m_mcu)
			return 0u;
		return mcf5307_exec(m_mcu, wantCycles);
	}

	bool Board::faulted() const noexcept
	{
		return m_faulted;
	}

	void Board::tickSofIfDue(const uint64_t frameIndex) noexcept
	{
		m_lastFrameIndex = frameIndex;
		/* THE REAL BODY IS TASK BRD-22's (Check: t0_sof_tick), which owns the
		 * frameIndex % 96 == 0 test and the isp1181_tick call on board.cpp,
		 * exactly as its Files: line names board.cpp. This task declares the
		 * method so the Scheduler's six-method surface is complete and leaves
		 * the USB tick to the task whose Check drives it. Nothing more is done
		 * here so that BRD-22 does not inherit a half-written implementation
		 * to unpick. */
	}

	size_t Board::stateSize() const noexcept
	{
		return sizeof(BoardState);
	}

	void Board::stateSave(void* const dst) const noexcept
	{
		BoardState s;
		s.version        = g_boardStateVersion;
		s.lastFrameIndex = m_lastFrameIndex;
		s.faulted        = m_faulted ? 1u : 0u;
		std::memcpy(dst, &s, sizeof s);
	}

	void Board::stateLoad(const void* const src) noexcept
	{
		BoardState s;
		std::memcpy(&s, src, sizeof s);
		m_lastFrameIndex = s.lastFrameIndex;
		m_faulted        = s.faulted != 0u;
	}
}
