// board.cpp owns the Board's lifetime and its six-method surface.
//
// mcf5307_create installs read/write/ack callbacks. These accept every access
// and report MCF5307_BUS_OK, which is what a board that models no fault does;
// the routing to the CS0 to CS5 devices comes later.
//
// The pinned core commit exports mcf5307_create, mcf5307_destroy,
// mcf5307_reset, mcf5307_exec and mcf5307_runtime_init, and no state_* or
// isp1181_* symbol. stateSave/stateLoad therefore serialise the Board's own
// determinism-relevant state, rather than calling a symbol that would not
// link. The Nim blocks are appended here the day a cpu task exports them.
//
// The MCU-clock placeholder line is emitted to standard output so a test can
// capture and count it.

#include "board.h"

#include <cassert>
#include <cstring>
#include <iostream>

#include "g2/timebase.h"

namespace g2
{
	namespace
	{
		// The version word of the Board's own snapshot block. stateLoad has no
		// error channel -- it returns void -- so a mismatch is an assert and
		// not a reported status. A snapshot written by a different revision
		// has a different field layout, and reading it would restore
		// plausible-looking wrong values in silence.
		constexpr uint32_t g_boardStateVersion = 1u;

		// The flat snapshot the Board serialises. It is a fixed-size, plain-old
		// data struct with no pointer inside, so a state file cannot carry a
		// dangling address. The Nim mcf5307_*/isp1181_* blocks join it once a
		// cpu task exports them.
		struct BoardState
		{
			uint32_t version;
			uint64_t lastFrameIndex;
			uint32_t faulted;
		};
		static_assert(std::is_trivially_copyable_v<BoardState>,
		              "BoardState must be POD so memcpy is well-defined");

		// The USB Start-of-Frame rate. One SOF frame is 1 ms, which is what
		// makes the ISP1181 the owner of the millisecond in design section 9.4.
		constexpr uint64_t g_sofFrameRateHz = 1000u;

		// The quanta in one SOF frame. It is DERIVED from the frame rate rather
		// than written as 96, so the 96:1 relation moves with the one symbol
		// that fixes it instead of being restated here. t0_sof_tick writes 96
		// out as its own literal, so a change to either side turns that test
		// red.
		constexpr uint64_t g_quantaPerSofFrame =
			G2_FRAME_RATE_HZ / g_sofFrameRateHz;

		// The division must be exact, or the divisor below drifts against the
		// millisecond it is supposed to model. A static_assert makes that a
		// property the compiler enforces rather than one a reader keeps.
		static_assert(G2_FRAME_RATE_HZ % g_sofFrameRateHz == 0u,
		              "The frame rate must be a whole number of SOF frames");
		static_assert(g_quantaPerSofFrame != 0u,
		              "A zero divisor would make every frame due");

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
		// The interrupt source set is empty, so there is nothing to clear
		// here.
	}

	Board::Board()
		: m_mcu(nullptr)
		, m_usb(nullptr)
	{
		// The Nim runtime must be initialised before any mcf5307_ call. It is
		// idempotent behind a latch, so the second Board in a process is safe.
		mcf5307_runtime_init();
		m_mcu = mcf5307_create(this, &Board::onRead, &Board::onWrite,
		                       &Board::onInterruptAck);

		/* The Board's own ISP1181, created here so its lifetime is exactly the
		 * Board's and destroyed in the reverse order below.
		 *
		 * THE IRQ AND TX CALLBACKS ARE NULL, AND THAT IS STATED RATHER THAN
		 * HIDDEN. The device signals service requests through irq and hands
		 * outbound packets to tx, and the Board has nowhere to route either
		 * yet: the interrupt presentation is task BRD-3's and the transport is
		 * the TransportHub's, and neither is in this task's closure. Null is
		 * DEFINED behaviour and not an oversight -- the model tests each
		 * callback before it calls it -- so the device runs and its frame
		 * number advances, which is what the SOF tick needs. */
		m_usb = isp1181_create(this, nullptr, nullptr);

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
		// Reverse order of construction: the USB device was created last and
		// is released first. Each pointer is cleared as it is released, so a
		// second destruction cannot reach an already-released handle.
		if(m_usb)
			isp1181_destroy(m_usb);
		m_usb = nullptr;

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
		/* THE BOARD OWNS THE TEST AND THE SCHEDULER NEVER MAKES IT. The
		 * Scheduler calls this on every frame, unconditionally, and passes the
		 * authoritative virtual frame index; the 96:1 relation is a property of
		 * the USB device model rather than of the scheduler, so it is tested
		 * here. Design section 9.4. */
		m_lastFrameIndex = frameIndex;

		if(frameIndex % g_quantaPerSofFrame != 0u)
			return;

		isp1181_tick(m_usb, 1u);
	}

	size_t Board::stateSize() const noexcept
	{
		return sizeof(BoardState);
	}

	void Board::stateSave(void* const dst) const noexcept
	{
		// Value-initialised so the padding between the fields is zero. The
		// snapshot is memcpy'd whole, and indeterminate padding would make two
		// saves of the same Board state differ byte for byte.
		BoardState s{};
		s.version        = g_boardStateVersion;
		s.lastFrameIndex = m_lastFrameIndex;
		s.faulted        = m_faulted ? 1u : 0u;
		std::memcpy(dst, &s, sizeof s);
	}

	void Board::stateLoad(const void* const src) noexcept
	{
		BoardState s;
		std::memcpy(&s, src, sizeof s);
		assert(s.version == g_boardStateVersion
			&& "a snapshot from a different revision would be read with this "
			"revision's field layout");
		m_lastFrameIndex = s.lastFrameIndex;
		m_faulted        = s.faulted != 0u;
	}
}
