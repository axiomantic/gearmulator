// board.cpp owns the Board's lifetime and its six-method surface.
//
// mcf5307_create installs read/write/ack callbacks. These accept every access
// and report MCF5307_BUS_OK, which is what a board that models no fault does;
// the routing to the CS0 to CS5 devices comes later.
//
// stateSave/stateLoad serialise the Board's own determinism-relevant state
// only. The core's mcf5307_state_* and isp1181_state_* blocks are not folded
// in here yet: doing so fixes a snapshot layout across two repositories, and
// the Board does not own an isp1181 handle to snapshot in the first place.
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
		// dangling address. The core's own state blocks are not folded in here.
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
		// The interrupt source set is empty, so there is nothing to clear
		// here.
	}

	Board::Board()
		: m_mcu(nullptr)
	{
		// The Nim runtime must be initialised before any mcf5307_ call. It is
		// idempotent behind a latch, so the second Board in a process is safe.
		mcf5307_runtime_init();
		m_mcu = mcf5307_create(this, &Board::onRead, &Board::onWrite,
		                       &Board::onInterruptAck);

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
