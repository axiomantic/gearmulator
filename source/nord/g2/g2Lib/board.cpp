// Task BRD-21. The board. Tier T0: this file carries no firmware artifact of
// any kind.
//
// Plan section 13.4, BRD-21. Design sections 6.4, 13.10 and 26.
//
// WHAT THIS FILE IS. board.cpp owns the Board's lifetime and its six-method
// surface. The Check of BRD-21 is t0_board_surface: concreteness (final,
// non-polymorphic, non-copyable, non-movable), the six methods the Scheduler
// uses, and the single construction log line that names the MCU core clock
// and its owner. The bodies below are the T0 minimum that makes
// every method real and callable; the board-track integration that follows
// (the CS0 to CS5 devices, the TransportHub, the real memory routing) widens
// the constructor and the bodies, and BRD-22 owns the real tickSofIfDue.
//
// THE BUS CALLBACKS ROUTE, AND THAT IS TASK INT-1's WRITE UNDER PLAN SECTION
// 24.6 ROW W3-115. The paragraph that used to stand here said these callbacks
// "accept every access and report MCF5307_BUS_OK" and deferred the routing to
// a board-track integration it did not name. That is the M3 blocker W3-96
// records: a core running against such a board fetches zero for ever. The
// paragraph is REWRITTEN rather than left standing beside code that no longer
// does what it describes.
//
// WHAT THE ROUTING IS. onRead and onWrite forward to busRead and busWrite,
// which hand the access to the BRD-1 MemoryMap. The decode turns the address
// into a region and a window-relative offset and calls the BusTarget attached
// there. The seven units W3-115 names are attached like this:
//
//     CS0    the flash boot image, through FlashWindow
//     CS1    the HDI08 array
//     CS2    the flash main image, through FlashWindow
//     CS4    the panel
//     CS5    the latches
//     MBAR   the SIM, UART0 and the M-Bus, through MbarRouter
//
// CS3 AND THE SDRAM GET NO TARGET, AND THAT IS DELIBERATE RATHER THAN
// UNFINISHED. CS3 is the ISP1181 and the SDRAM is main memory; neither is one
// of the seven units this task composes, and INT-1's own title is "boot the
// firmware with a STUBBED CS3". A region with no target answers exactly as a
// region with no window does -- see the unmapped note below.
//
// WHAT AN ADDRESS IN NO WINDOW DOES, AND WHY THIS FILE DOES NOT DECIDE IT.
// memoryMap.cpp already fixes the answer: an access that decodes to no region,
// or to a region with no target, reports MCF5307_BUS_UNMAPPED and writes one
// log line, and design section 5.2.1 rule 2 is what ties the report and the
// trace together. This task ROUTES to that decision and does not re-take it.
// The blanket MCF5307_BUS_OK the old callbacks returned is the one answer that
// is definitely wrong, because it makes an unmapped access indistinguishable
// from a device that legitimately answered zero.
//
// THE STATE METHODS AND THE MISSING NIM BLOCKS, STATED RATHER THAN HIDDEN.
// Design section 13.10 says stateSize/stateSave/stateLoad "embed the Nim
// mcf5307_state_* and isp1181_state_* blocks of section 5.2". The pinned core
// commit exports none of those C symbols. This task therefore serialises the
// Board's own determinism-relevant state and documents the deviation rather
// than stubbing a call to a symbol that would not link -- the same
// documented-deviation route BRD-23 took for mcf5307_exec earlier in this
// track. The Nim blocks are appended here the day a cpu task exports them.
//
// THE MCU CORE CLOCK LINE. The Board logs one line at construction that names
// G2_MCU_CORE_CLOCK_HZ, states that the value is DERIVED -- the schematic's
// CLKIN label times the PLL multiplier, agreeing with the MCF5407CAI162 speed
// grade -- and not scope-measured, and names spike criterion (j) as its owner.
// Measurement register row 7 (plan section 4.1) still owns the value: the
// derivation narrows it, a scope on CLKIN closes it. The line is emitted to
// standard output so the surface test can capture and count it. It lives in
// g2Lib/board.cpp because the board track owns this file; SCH-3 keeps the
// tree-wide grep, which is a property of the tree and not of the Board.

#include "board.h"

#include <cstring>
#include <iostream>

#include "g2/timebase.h"

namespace g2
{
	namespace
	{
		/* THE UNIT CONVERSION OF THE BUS `size` ARGUMENT, AND THE ONLY PLACE IT
		 * HAPPENS.
		 *
		 * THE TWO SIDES MEASURE THE SAME QUANTITY IN DIFFERENT UNITS, and both
		 * say so in their own header. mcf5307.h states it twice, once per
		 * callback typedef: `size` IS A COUNT OF BYTES -- 1, 2 or 4 -- which is
		 * what a ColdFire SIZ[1:0] transfer size encodes. memoryMap.h states
		 * that its own `_size` is a WIDTH IN BITS -- 8, 16 or 32. The two
		 * readings disagree on EVERY access a core can make, so forwarding the
		 * argument unconverted refuses all of them: the first instruction fetch
		 * presents 2, the decode reads it as 2 bits, and the firmware executes
		 * nothing.
		 *
		 * THE CONVERSION IS HERE AND NOT IN THE MemoryMap. This function sits at
		 * the callback boundary -- the one place where the core's arguments
		 * arrive -- so the core's unit stops at the boundary and everything
		 * inside the library keeps the single unit it already had. Unifying the
		 * library on bytes is the tidier end state and it is DELIBERATELY NOT
		 * TAKEN HERE: it would rewrite every board test that supplies a width by
		 * hand, and that churn is a separate decision.
		 *
		 * A SIZE THE ABI CANNOT PRODUCE MAPS TO ZERO, WHICH THE DECODE REFUSES.
		 * That is the half without which this function would be a funnel: an
		 * unknown size answered with a legal width would make every access
		 * legal, including the 8, 16 and 32 that the unconverted callbacks used
		 * to accept silently. Zero is a width no transfer produces, so the
		 * SIZE_ILLEGAL line the decode logs cannot be mistaken for a real
		 * access. A multiplication by eight would be shorter and would overflow
		 * on a large argument; a total switch cannot. */
		int busWidthBits(const int _sizeInBytes)
		{
			switch(_sizeInBytes)
			{
			case 1:  return 8;
			case 2:  return 16;
			case 4:  return 32;
			default: return 0;
			}
		}

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

	uint32_t Board::FlashWindow::absolute(const uint32_t _offset) const
	{
		/* THE BASE COMES FROM THE DECODE THAT PRODUCED THE OFFSET. The
		 * MemoryMap subtracted window(_region).base to make _offset, and this
		 * adds the SAME expression back. Keeping a second copy of the base in
		 * this object would let a mutation of one copy leave the pair
		 * self-consistent and every test green. */
		return m_map.window(m_region).base + _offset;
	}

	uint32_t Board::FlashWindow::read(const uint32_t _offset, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		const uint32_t address = absolute(_offset);

		switch(_size)
		{
		case 8:  return m_flash.read8(address);
		case 16: return m_flash.read16(address);
		case 32: return m_flash.read32(address);
		default: break;
		}

		_status = MCF5307_BUS_SIZE_ILLEGAL;
		return 0;
	}

	void Board::FlashWindow::write(const uint32_t _offset, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		const uint32_t address = absolute(_offset);

		/* THE FLASH MODEL IS READ-ONLY AND ITS OWN WRITE ENTRY POINTS LOG THE
		 * REJECTION. They report no status, so the bus CYCLE completes and the
		 * device ignores it, which is what a board that models no fault does
		 * (design section 5.2.1). Reporting a fault here would be this file
		 * inventing a policy BRD-7 did not state. */
		switch(_size)
		{
		case 8:  m_flash.write8 (address, uint8_t (_value & 0xffu));   return;
		case 16: m_flash.write16(address, uint16_t(_value & 0xffffu)); return;
		case 32: m_flash.write32(address, _value);                     return;
		default: break;
		}

		_status = MCF5307_BUS_SIZE_ILLEGAL;
	}

	bool Board::MbarRouter::isUartOwned(const uint32_t _offset)
	{
		// The one offset the SIM answers inside the UART block. sim.cpp's
		// DIVERGENCE note is the authority and this is the only site that
		// encodes it.
		if(_offset == g_simUartStrapOffset)
			return false;

		if(_offset >= Uart0::gUart0Base && _offset < Uart0::gUart0Base + Uart0::gUartModuleSize)
			return true;

		return _offset >= Uart0::gUart1Base && _offset < Uart0::gUart1Base + Uart0::gUartModuleSize;
	}

	bool Board::MbarRouter::isMbusOwned(const uint32_t _offset)
	{
		return _offset >= MBus::g_base && _offset < MBus::g_base + MBus::g_size;
	}

	BusTarget& Board::MbarRouter::select(const uint32_t _offset)
	{
		if(isMbusOwned(_offset))
			return m_mbus;
		if(isUartOwned(_offset))
			return m_uart0;
		return m_sim;
	}

	uint32_t Board::MbarRouter::read(const uint32_t _offset, const int _size,
		mcf5307_bus_status& _status)
	{
		// The offset is already MBAR-relative and BOTH units expect it that
		// way, so nothing is adjusted here.
		return select(_offset).read(_offset, _size, _status);
	}

	void Board::MbarRouter::write(const uint32_t _offset, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		select(_offset).write(_offset, _size, _value, _status);
	}

	uint32_t Board::busRead(const uint32_t _address, const int _size,
		mcf5307_bus_status& _status)
	{
		return m_memory.read(_address, _size, _status);
	}

	void Board::busWrite(const uint32_t _address, const int _size, const uint32_t _value,
		mcf5307_bus_status& _status)
	{
		m_memory.write(_address, _size, _value, _status);
	}

	void Board::attachUnits()
	{
		m_memory.attach(Region::Cs0,  &m_flashCs0);
		m_memory.attach(Region::Cs1,  &m_hdi08);
		m_memory.attach(Region::Cs2,  &m_flashCs2);
		m_memory.attach(Region::Cs4,  &m_panel);
		m_memory.attach(Region::Cs5,  &m_latches);
		m_memory.attach(Region::Mbar, &m_mbar);

		// Region::Cs3 and Region::Sdram are left with no target on purpose;
		// see the file header.
	}

	uint32_t Board::onRead(void* const user, const uint32_t addr, const int size,
	                       mcf5307_bus_status* const status)
	{
		mcf5307_bus_status local = MCF5307_BUS_OK;
		const uint32_t value = static_cast<Board*>(user)->busRead(addr, busWidthBits(size), local);
		if(status)
			*status = local;
		return value;
	}

	void Board::onWrite(void* const user, const uint32_t addr, const int size,
	                    const uint32_t value, mcf5307_bus_status* const status)
	{
		mcf5307_bus_status local = MCF5307_BUS_OK;
		static_cast<Board*>(user)->busWrite(addr, busWidthBits(size), value, local);
		if(status)
			*status = local;
	}

	void Board::onInterruptAck(void*, const int, const uint8_t)
	{
		// The T0 surface keeps the interrupt source set empty, so there is
		// nothing to clear here.
	}

	/* The unconfigured Board. It DELEGATES rather than repeating the body, so
	 * there is one construction path and the core-clock line below cannot be
	 * emitted twice or differ between the two forms. A default BoardConfig
	 * leaves every window absent, so this Board answers at no address -- which
	 * is what BRD-21's surface task always had, stated honestly instead of as
	 * a blanket bus-OK. */
	Board::Board() : Board(BoardConfig{})
	{
	}

	Board::Board(const BoardConfig& _config)
		: m_memory(_config.memory)
		, m_flash(_config.memory.cs0.base, _config.memory.cs0.size,
		          _config.memory.cs2.base, _config.memory.cs2.size)
		, m_panel(_config.memory.cs4.size)
		, m_latches(_config.memory.cs5.size)
		, m_hdi08(_config.hdi08)
		, m_adc(_config.adc)
		, m_mbus(&m_adc)
		, m_flashCs0(m_flash, m_memory, Region::Cs0)
		, m_flashCs2(m_flash, m_memory, Region::Cs2)
		, m_mbar(m_sim, m_uart0, m_mbus)
		, m_mcu(nullptr)
		, m_usb(nullptr)
	{
		// Every unit is attached before the core exists, so no callback can
		// reach a half-built decode.
		attachUnits();

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

		// The single MCU core-clock line. Naming the macro AND the numeric
		// value AND how the value was arrived at AND criterion (j) is what the
		// Check requires and what t0_board_surface asserts. The value is
		// streamed from the macro and never written here as a literal, so the
		// line cannot drift from timebase.h.
		std::cout << "board: G2_MCU_CORE_CLOCK_HZ = "
		          << G2_MCU_CORE_CLOCK_HZ
		          << " is derived from the schematic CLKIN label and the PLL"
		             " multiplier, and is not scope-measured;"
		             " owner spike criterion (j)"
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
