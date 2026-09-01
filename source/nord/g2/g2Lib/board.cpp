// board.cpp owns the Board's lifetime and its six-method surface.
//
// mcf5307_create installs read/write/ack callbacks. These accept every access
// and report MCF5307_BUS_OK, which is what a board that models no fault does;
// the routing to the CS0 to CS5 devices comes later.
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
// The routing. onRead and onWrite forward to busRead and busWrite, which hand
// the access to the MemoryMap. The decode turns the address into a region and a
// window-relative offset and calls the BusTarget attached there:
//
//     CS0    the flash boot image, through FlashWindow
//     CS1    the HDI08 array
//     CS2    the flash main image, through FlashWindow
//     CS3    the ISP1181 USB device, through Isp1181Window
//     CS4    the panel
//     CS5    the latches
//     MBAR   the SIM, UART0, the M-Bus and the interrupt controller,
//            through MbarRouter
//
// The SDRAM gets no target on purpose. Main memory is not one of the units
// this Board composes; the harness supplies it (see main.cpp). A region with
// no target answers exactly as a
// region with no window does -- see the unmapped note below.
//
// memoryMap.cpp already fixes what an address in no window does: an access that
// decodes to no region, or to a region with no target, reports
// MCF5307_BUS_UNMAPPED and writes one log line. This file routes to that
// decision and does not re-take it. A blanket MCF5307_BUS_OK is the one answer
// that is definitely wrong, because it makes an unmapped access
// indistinguishable from a device that legitimately answered zero.
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
// The MCU core clock line. The Board logs one line at construction that names
// G2_MCU_CORE_CLOCK_HZ, states that the value is DERIVED -- the schematic's
// CLKIN label times the PLL multiplier, agreeing with the MCF5407CAI162 speed
// grade -- and not scope-measured, and names spike criterion (j) as its owner.
// It is emitted to standard output so the surface test can capture and count it.

#include "board.h"

#include "hdi08Bridge.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "g2/timebase.h"

namespace g2
{
	namespace
	{
		/* The unit conversion of the bus `size` argument, and the only place it
		 * happens. mcf5307.h's `size` is a count of BYTES -- 1, 2 or 4 -- which
		 * is what a ColdFire SIZ[1:0] transfer size encodes; memoryMap.h's
		 * `_size` is a width in BITS -- 8, 16 or 32. The two readings disagree
		 * on EVERY access a core can make, so forwarding the argument
		 * unconverted refuses all of them: the first instruction fetch presents
		 * 2, the decode reads it as 2 bits, and the firmware executes nothing.
		 *
		 * The conversion is here and not in the MemoryMap. This function sits at
		 * the callback boundary -- the one place where the core's arguments
		 * arrive -- so the core's unit stops at the boundary and everything
		 * inside the library keeps the single unit it already had.
		 *
		 * A size the ABI cannot produce maps to zero, which the decode refuses.
		 * An unknown size answered with a legal width would make every access
		 * legal, including the 8, 16 and 32 that belong to the other unit. Zero
		 * is a width no transfer produces, so the SIZE_ILLEGAL line the decode
		 * logs cannot be mistaken for a real access. A multiplication by eight
		 * would overflow on a large argument; a total switch cannot. */
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

		// The USB Start-of-Frame rate. One SOF frame is 1 ms, which makes the
		// ISP1181 the owner of the millisecond.
		constexpr uint64_t g_sofFrameRateHz = 1000u;

		// The quanta in one SOF frame, DERIVED from the frame rate rather than
		// written out, so the relation moves with the one symbol that fixes it.
		constexpr uint64_t g_quantaPerSofFrame =
			G2_FRAME_RATE_HZ / g_sofFrameRateHz;

		// The division must be exact, or the divisor below drifts against the
		// millisecond it is supposed to model.
		static_assert(G2_FRAME_RATE_HZ % g_sofFrameRateHz == 0u,
		              "The frame rate must be a whole number of SOF frames");
		static_assert(g_quantaPerSofFrame != 0u,
		              "A zero divisor would make every frame due");

	}

	uint32_t Board::FlashWindow::absolute(const uint32_t _offset) const
	{
		/* The base comes from the decode that produced the offset: the MemoryMap
		 * subtracted window(_region).base to make _offset and this adds the SAME
		 * expression back. A second copy of the base in this object would stay
		 * self-consistent with the first after either one moved. */
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

		/* The Flash model is read-only and its own write entry points log the
		 * rejection. They report no status, so the bus CYCLE completes and the
		 * device ignores it, which is what a board that models no fault does. */
		switch(_size)
		{
		case 8:  m_flash.write8 (address, uint8_t (_value & 0xffu));   return;
		case 16: m_flash.write16(address, uint16_t(_value & 0xffffu)); return;
		case 32: m_flash.write32(address, _value);                     return;
		default: break;
		}

		_status = MCF5307_BUS_SIZE_ILLEGAL;
	}

	uint32_t Board::Isp1181Window::read(const uint32_t _offset, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		/* The device answers every cycle at every offset, so neither arm can
		 * produce anything but BUS_OK. The 16 and 32-bit cycles are answered
		 * rather than refused: the driver reads the part word-sized in the
		 * register file idiom, and a refusal here would turn each such cycle
		 * into a logged bus failure indistinguishable from an unmapped one. The
		 * byte answer is replicated across the access width, big-endian. */
		const uint8_t value = isp1181_read(m_usb, _offset);

		switch(_size)
		{
		case 8:  return value;
		case 16: return uint32_t(value) * 0x0101u;
		case 32: return uint32_t(value) * 0x01010101u;
		default: break;
		}

		_status = MCF5307_BUS_SIZE_ILLEGAL;
		return 0;
	}

	void Board::Isp1181Window::write(const uint32_t _offset, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		/* The low byte only: the model routes one register per address, so
		 * there is no wider state to compose a multi-byte store into. */
		switch(_size)
		{
		case 8:
		case 16:
		case 32: isp1181_write(m_usb, _offset, uint8_t(_value & 0xffu)); return;
		default: break;
		}

		_status = MCF5307_BUS_SIZE_ILLEGAL;
	}

	bool Board::MbarRouter::isUartOwned(const uint32_t _offset)
	{
		// The one offset the SIM answers inside the UART block, encoded here and
		// nowhere else.
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

	bool Board::MbarRouter::isInterruptOwned(const uint32_t _offset)
	{
		if(_offset == InterruptController::gIrqparOffset)
			return true;
		if(_offset == InterruptController::gAvrOffset)
			return true;
		return _offset >= InterruptController::gIcrBase
			&& _offset < InterruptController::gIcrBase + InterruptController::gIcrCount;
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
		/* IRQPAR, AVR and the internal control block are byte registers, so a
		 * wider access is refused here rather than split. */
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}
			return m_interrupts.readRegister(_offset);
		}

		// The offset is already MBAR-relative and BOTH units expect it that
		// way, so nothing is adjusted here.
		return select(_offset).read(_offset, _size, _status);
	}

	void Board::MbarRouter::write(const uint32_t _offset, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		if(isInterruptOwned(_offset))
		{
			if(_size != 8)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}
			m_interrupts.writeRegister(_offset, uint8_t(_value & 0xffu));
			return;
		}

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
		m_memory.attach(Region::Cs3,  &m_usbCs3);
		m_memory.attach(Region::Cs4,  &m_panel);
		m_memory.attach(Region::Cs5,  &m_latches);
		m_memory.attach(Region::Mbar, &m_mbar);

		// Region::Sdram is left with no target on purpose; see the file
		// header.
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

	void Board::onInterruptPresent(void* const user, const int level, const uint8_t vector,
	                               const int autovector)
	{
		Board* const board = static_cast<Board*>(user);

		/* The guard is on the handle and not on the reset. Presenting a level 1
		 * to 6 interrupt immediately after mcf5307_reset is a defined case, so
		 * a presentation before the first reset is fine; a presentation before
		 * the core exists has nowhere to go. */
		if(!board->m_mcu)
			return;

		/* The whole current state, unconditionally, on every recomputation.
		 * mcf5307_set_irq is idempotent, which is what licenses the board to
		 * present on a clear exactly as it does on an assert. */
		mcf5307_set_irq(board->m_mcu, level, vector, autovector);
	}

	void Board::onInterruptAck(void*, const int, const uint8_t)
	{
		/* Nothing is cleared here. An acknowledge clears an edge-triggered
		 * source on the board's own side, and every source this board carries
		 * is level-triggered: the timer's TER[REF] drops when the firmware
		 * writes it and UART0's condition drops when the firmware empties the
		 * receiver. */
	}

	/* The unconfigured Board. It DELEGATES rather than repeating the body, so
	 * there is one construction path and the core-clock line below cannot be
	 * emitted twice or differ between the two forms. A default BoardConfig
	 * leaves every window absent, so this Board answers at no address. */
	Board::Board() : Board(BoardConfig{})
	{
	}

	namespace
	{
		/* The two sizes TransportHub cannot derive for itself, supplied here
		 * because the Board is what constructs the hub.
		 *
		 * g_transportMaxFrameBytes is derived and not measured. The wire
		 * framing is [1-byte type][2-byte length][payload], so the ceiling is
		 * 1 + 2 + 65,535. It is deliberately the loosest bound available.
		 *
		 * g_transportQueueDepth is chosen, an ordinary tunable. A `.pch2` load
		 * is originated whole: g2::pch2Load validates the container and then
		 * hands the hub one frame per object with no quantum boundary in
		 * between, so the whole patch must fit the queue at once. A depth below
		 * the object count does not refuse the load cleanly: pass 2 stops at
		 * the first frame the hub declines, and the frames already accepted are
		 * delivered at the next boundary, so the device is left holding a
		 * prefix. A patch in the artifact corpus carries 18 objects, so a depth
		 * of 16 refuses every real patch with PCH2-SEND-REFUSED. */
		constexpr size_t g_transportMaxFrameBytes = 1u + 2u + 65535u;
		constexpr size_t g_transportQueueDepth    = 32u;
	}

	Board::Board(const BoardConfig& _config)
		: m_mcu(nullptr)
		, m_interrupts(this, &Board::onInterruptPresent)
		, m_memory(_config.memory)
		, m_flash(_config.memory.cs0.base, _config.memory.cs0.size,
		          _config.memory.cs2.base, _config.memory.cs2.size)
		, m_panel(_config.memory.cs4.size)
		, m_latches(_config.memory.cs5.size)
		, m_hdi08(_config.hdi08)
		, m_uart0(&m_interrupts)
		, m_adc(_config.adc)
		, m_mbus(&m_adc)
		, m_flashCs0(m_flash, m_memory, Region::Cs0)
		, m_flashCs2(m_flash, m_memory, Region::Cs2)
		, m_mbar(m_sim, m_uart0, m_mbus, m_interrupts)
		, m_usbCs3(m_usb)
		, m_usb(nullptr)
		, m_usbProtocolEndpoint(_config.usbProtocolEndpoint)
		, m_transport(g_transportMaxFrameBytes, g_transportQueueDepth)
		, m_drained(TransportHub::kMaxEndpoints * g_transportQueueDepth)
		, m_heldBytes(g_transportMaxFrameBytes)
	{
		// Every unit is attached before the core exists, so no callback can
		// reach a half-built decode.
		attachUnits();

		/* Both timer modules assert on the board's one controller. Uart0 is
		 * handed it by the initialiser list above; this call is the timers'
		 * half of the same wire, and Sim::setInterruptController forwards it to
		 * both modules. */
		m_sim.setInterruptController(&m_interrupts);

		/* The DSP side of the host ports, attached from the constructor BODY
		 * rather than from the initialiser list: the call takes both members by
		 * reference and each one has to be fully built first. */
		attachHdi08Bridges(m_hdi08, m_dspSet);

		// The Nim runtime must be initialised before any mcf5307_ call. It is
		// idempotent behind a latch, so the second Board in a process is safe.
		mcf5307_runtime_init();
		m_mcu = mcf5307_create(this, &Board::onRead, &Board::onWrite,
		                       &Board::onInterruptAck);

		/* The Board's own ISP1181, created here so its lifetime is exactly the
		 * Board's and destroyed in the reverse order below.
		 *
		 * Both callbacks are this Board's own. A packet the device hands to tx
		 * reaches every attachment through the hub above, and the device's
		 * service request reaches the machine's one interrupt controller.
		 *
		 * Both G2 images install the USB handler at level 3, autovectored,
		 * vector 27 at VBR+0x6C, by two independent encodings that agree; both
		 * leave IRQPAR unwritten; and both enable the device's own interrupts
		 * unconditionally during USB bring-up (DcInterruptEnable = 0x00001F07,
		 * then Mode bit 3). So the wired callback is a path the firmware will
		 * exercise.
		 *
		 * `src/isp1181/isp1181.nim` stores the tx callback at construction and
		 * calls it from nowhere: no line of that model invokes `m.tx`. So the
		 * wire is installed and correct and the device cannot yet drive it.
		 *
		 * That model does call the stored irq callback, from `updateIrq`,
		 * whenever the masked interrupt register changes state. What no
		 * implemented command does is set a bit of that register:
		 * `raiseInterrupt` is the only writer and nothing calls it, because
		 * that model has no event-to-bit assignment yet. */
		m_usb = isp1181_create(this, &Board::onUsbIrq, &Board::onUsbTx);

		/* The handle is moved off the Stub deliberately and here. The Stub is
		 * the create-time default: present in the CS3 window and inert -- every
		 * read answers 0x00, every packet is discarded, neither callback is
		 * ever called. A Board that left the default in place would call
		 * isp1181_rx once per drained frame into a device that throws every one
		 * away.
		 *
		 * The return is checked and not discarded. The call is refused for a
		 * nil handle and for a backend value neither macro names, and a refusal
		 * moves nothing, so an unchecked call would leave a Stub in place and
		 * look identical to a successful one. The failure is reported and the
		 * Board continues: a Board without a usable USB device is still a Board
		 * that boots. */
		if(m_usb != nullptr &&
		   isp1181_set_backend(m_usb, MCF5307_ISP1181_BACKEND_FULL_MODEL) != 1)
		{
			std::cout << "board: isp1181_set_backend(FULL_MODEL) was REFUSED;"
			             " the USB device stays inert and no patch byte can"
			             " reach the firmware"
			          << std::endl;
		}

		// The value is streamed from the macro and never written here as a
		// literal, so the line cannot drift from timebase.h.
		std::cout << "board: G2_MCU_CORE_CLOCK_HZ = "
		          << G2_MCU_CORE_CLOCK_HZ
		          << " is derived from the schematic CLKIN label and the PLL"
		             " multiplier, and is not scope-measured;"
		             " owner spike criterion (j)"
		          << std::endl;
	}

	Board::UsbTransportStats Board::usbTransport() const noexcept
	{
		UsbTransportStats s = m_usbStats;
		s.held         = m_heldValid;
		s.heldAttempts = m_heldAttempts;
		return s;
	}

	void Board::pumpTransport() noexcept
	{
		++m_usbStats.pumps;

		/* ONE FRAME PER QUANTUM, AND THE DEVICE'S SHAPE IS WHY. The endpoint
		 * the firmware configures carries a SINGLE buffer, and ISP1362 Rev. 06
		 * section 12.1.2 step 3 (p.49) says what the second packet of a
		 * quantum meets: "If the endpoint is enabled, the SIE checks the
		 * contents of the ESR. If the endpoint is empty, the data from USB is
		 * stored in the buffer memory during the data phase else a NAK
		 * handshake is sent." Section 15.2.5 (p.115) says how long that
		 * lasts: "Any subsequent packets are refused by returning a NAK
		 * condition, until the buffer is unlocked". So offering a second
		 * frame before the firmware has cleared the first CANNOT succeed --
		 * and the run that motivated this code measured exactly that, 17
		 * refusals in one quantum, every one of them reporting a buffer that
		 * held 1 of 1.
		 *
		 * ONE DRAIN, AT THE BOUNDARY, STILL. transportHub.h makes the count of
		 * drains the quantum count -- "drainToDevice runs exactly once for
		 * each quantum" -- so a second drain in one quantum would advance the
		 * hub's frame index past the machine's and stamp two frames that
		 * crossed together with different indices. The drain therefore ALWAYS
		 * happens; what changes with back-pressure is the `max` it is given,
		 * and a max of 0 leaves every queued frame where it is. */
		/* A BOARD WITH NO DEVICE NOW TAKES NOTHING OUT OF THE HUB EITHER, AND
		 * THAT IS THE SAME RULE AND NOT A SECOND ONE. The old body drained the
		 * whole queue and then returned without offering any of it, so a null
		 * handle discarded every frame in silence -- the identical defect at a
		 * different address. A max of 0 leaves them queued, and the producer
		 * learns through TransportHub::toDevice's false when the queue fills.
		 * m_usbStats.pumps still counts these quanta, so a Board that is
		 * pumping and delivering nothing is visible rather than inferred. */
		const size_t max = (m_usb == nullptr || m_heldValid) ? 0u : 1u;

		const size_t drained = m_transport.drainToDevice(m_drained.data(), max);

		m_usbStats.drained += drained;

		if(m_usb == nullptr)
			return;

		if(drained != 0)
		{
			/* THE BYTES ARE COPIED AND NOTHING HERE READS ONE. Design section
			 * 15.3: "the emulator does not implement the protocol by hand,
			 * the firmware implements it; the emulator only carries the
			 * bytes." A Board that inspected a frame here would be a second
			 * implementation of the protocol. The copy is a LIFETIME
			 * requirement and not an inspection: the hub's pointer dies at the
			 * next drain and this frame may outlive several. */
			const ProtocolFrame& frame = m_drained[0].frame;

			/* A FRAME TOO LARGE TO HOLD IS REPORTED, NEVER TRUNCATED. The hub
			 * makes this unreachable -- TransportHub::toDevice refuses a frame
			 * larger than maxFrameBytes and this buffer is sized to the same
			 * figure from the same constant -- so the branch exists to make
			 * the ONE way that guarantee could ever break audible rather than
			 * to handle a case that occurs. A silent clamp here would hand the
			 * firmware a short frame that looked delivered. */
			if(frame.size > m_heldBytes.size())
			{
				std::fprintf(stderr,
					"board: a %zu-byte frame left the hub and does not fit the"
					" %zu-byte hold buffer; it CANNOT be delivered and is being"
					" reported rather than truncated or silently dropped.\n",
					frame.size, m_heldBytes.size());
				++m_usbStats.stallReports;

				/* THE FRAME IS COUNTED WHERE IT WENT, so the no-loss invariant
				 * stays total. This is the one path on which a drained frame
				 * reaches neither the device nor the hold, and leaving it
				 * uncounted would put `drained` one ahead of
				 * `accepted + held` with nothing naming the difference. It is
				 * not subtracted from `drained` either: the frame did leave
				 * the hub, and a counter that said otherwise would hide the
				 * loss in the same silence this function was rewritten to
				 * remove. */
				++m_usbStats.undeliverable;
				return;
			}

			m_heldSize = frame.size;

			if(m_heldSize != 0 && frame.data != nullptr)
				std::memcpy(m_heldBytes.data(), frame.data, m_heldSize);

			m_heldValid    = true;
			m_heldAttempts = 0;
		}

		if(!m_heldValid)
			return;

		++m_usbStats.offered;
		++m_heldAttempts;

		/* THE RETURN IS READ. `isp1181_rx` answers 1 when an OUT buffer holds
		 * the packet and 0 for the NAK, and the header marks it
		 * MCF5307_MUST_USE so this call cannot go back to discarding it
		 * without a compiler diagnostic. Discarding it is the defect this
		 * function was rewritten to remove. */
		if(isp1181_rx(m_usb, m_usbProtocolEndpoint, m_heldBytes.data(), m_heldSize) == 1)
		{
			++m_usbStats.accepted;
			m_heldValid    = false;
			m_heldAttempts = 0;
			return;
		}

		++m_usbStats.refused;

		/* THE FRAME IS NOT DROPPED, AND THE STALL IS NOT SILENT.
		 *
		 * WHY IT IS NEVER DROPPED. Back-pressure already has an end-to-end
		 * path: a held frame stops the drain, the hub's queue fills, and
		 * TransportHub::toDevice answers false to the producer -- a refusal
		 * the producer already has to handle. Nothing in that chain has to
		 * throw a frame away, so nothing does.
		 *
		 * WHY IT STILL SHOUTS, AND WHERE THE NUMBER COMES FROM. A frame the
		 * firmware never takes would otherwise stall this Board in perfect
		 * silence, which is the same failure shape in a new place. The
		 * threshold is the LARGEST NAK retry window the ISP1362 can be
		 * programmed with: Table 108 (p.104) gives HcATLPTDDoneThresholdTimeOut
		 * the field PTDDoneTimeOut[7:0], the "Maximum allowable time in ms for
		 * the host controller to retry a transaction with NAK returned", so
		 * its ceiling is 0xFF ms. A frame still refused after longer than any
		 * time-out that register can express is, on the datasheet's own scale,
		 * no longer flow control.
		 *
		 * THE SCOPE OF THAT CITATION IS NOT STRETCHED. Section 14.9.9 is the
		 * ISP1362's HOST controller, not the peripheral the G2 carries, and
		 * the host retrying a NAKed transaction here is the PC's stack rather
		 * than that register. It is quoted for the one thing it settles and
		 * the only thing this code needs: what a NAK obliges a host to do, and
		 * on what scale the obligation is bounded. It is a time, not a count
		 * of attempts, which is why this constant is derived from a
		 * millisecond figure and the SOF frame rather than chosen as a number
		 * of tries.
		 *
		 * IT REPEATS. A stall that outlives one window is reported again at
		 * every further window, so a reader who joins late still sees it. */
		constexpr uint64_t g_nakRetryCeilingMs = 255u;

		const uint64_t stallQuanta = g_nakRetryCeilingMs * g_quantaPerSofFrame;

		if(m_heldAttempts % stallQuanta == 0u)
		{
			++m_usbStats.stallReports;

			/* stderr THROUGH fprintf AND NOT std::cout. This function is
			 * noexcept and runs at a quantum boundary; a stream insertion that
			 * threw here would call std::terminate. */
			/* IT REPORTS WHAT THIS SIDE KNOWS AND NAMES WHO KNOWS THE REST.
			 * `isp1181_rx` answers one bit, and mcf5307.h lists seven distinct
			 * conditions behind it -- a full buffer and an oversized packet
			 * among them. The Board cannot tell them apart and must not guess:
			 * an earlier draft of this line asserted the firmware was not
			 * clearing the buffer, and the device's own log then reported the
			 * buffer EMPTY and the packet too LONG for it. The cause lives in
			 * that log, one line per refusal, and the line below sends the
			 * reader there rather than inventing an answer. */
			std::fprintf(stderr,
				"board: the USB device has refused the same %zu-byte frame on"
				" endpoint %d for %llu consecutive quanta, which is longer than"
				" the %llu ms ceiling of the ISP1362's own NAK retry time-out."
				" The frame is STILL HELD and is being re-offered; nothing has"
				" been discarded. WHY the device refuses it is in the device's"
				" own log, read through isp1181_log_written,"
				" isp1181_log_retained and isp1181_log_line.\n",
				m_heldSize, m_usbProtocolEndpoint,
				static_cast<unsigned long long>(m_heldAttempts),
				static_cast<unsigned long long>(g_nakRetryCeilingMs));
		}
	}

	void Board::onUsbTx(void* const user, const int, const uint8_t* const data,
		const size_t len)
	{
		Board* const board = static_cast<Board*>(user);

		if(board == nullptr)
			return;

		/* Straight to the hub, which delivers to every attachment in attachment
		 * order. The buffer is the device's and is borrowed for the duration of
		 * this call; TransportHub::fromDevice hands each endpoint the same
		 * borrow, so an endpoint that keeps it copies it. Nothing is copied
		 * here. */
		board->m_transport.fromDevice(ProtocolFrame{ data, len });
	}

	void Board::onUsbIrq(void* const user, const int asserted)
	{
		Board* const board = static_cast<Board*>(user);

		if(board == nullptr)
			return;

		/* The pin is named and the level is not. IRQ3 is what the firmware
		 * operates: BOOT:0x31FE writes its USB handler to VBR+108 -- vector
		 * 27, which is the ColdFire autovector formula 24+level at level 3 --
		 * sets AVR to 0x08 and clears IMR bit 3, and CODE reaches the same
		 * three effects through install_autovector(3, 0x30053C38) at that
		 * helper's only call site in the image. Neither image ever writes
		 * IRQPAR, so IRQ3 keeps its reset level.
		 *
		 * The level is read out of the firmware. That the pin is IRQ3 is
		 * inferred from it: only IRQ3 can present at level 3, and no image
		 * names a pin. The schematic cannot settle it -- U24 carries exactly
		 * one extracted pin and the string IRQ occurs nowhere in it.
		 *
		 * The controller derives everything else. externalLevel applies UM
		 * Table 8-4 to IRQPAR and the autovector bit comes from AVR, both of
		 * which the firmware programs through the MBAR window at run time. A
		 * level written down here would stay 3 after a firmware that moved
		 * IRQPAR[1] had made it 6. */
		board->m_interrupts.setExternalPending(ExternalPin::Irq3, asserted != 0);
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

		const uint32_t cycles = mcf5307_exec(m_mcu, wantCycles);

		/* The fault bit is taken from the core rather than decided here. It is
		 * read back on the same call that advanced the core, so the answer
		 * faulted() gives cannot drift from the machine it describes. */
		m_faulted = mcf5307_faulted(m_mcu) != 0;

		/* The timers are advanced from the cycles this call actually ran, not
		 * from the budget it was asked for. That is what makes a timer tick a
		 * function of executed cycles and keeps it deterministic under the
		 * scheduler's quantum. */
		m_sim.advanceTimers(cycles);

		return cycles;
	}

	bool Board::faulted() const noexcept
	{
		return m_faulted;
	}

	void Board::resetMcu(const uint32_t initialSp, const uint32_t initialPc) noexcept
	{
		if(!m_mcu)
			return;

		mcf5307_reset(m_mcu, initialSp, initialPc);

		/* The reset clears the core's own fault, so a bit left standing here
		 * would report a machine that no longer exists. */
		m_faulted = false;
	}

	uint32_t Board::mcuReg(const int index) const noexcept
	{
		if(!m_mcu)
			return 0u;
		return mcf5307_get_reg(m_mcu, index);
	}

	bool Board::setMcuReg(const int index, const uint32_t value) noexcept
	{
		if(!m_mcu)
			return false;
		return mcf5307_set_reg(m_mcu, index, value) != 0;
	}

	bool Board::mcuHalted() const noexcept
	{
		if(!m_mcu)
			return false;
		return mcf5307_halted(m_mcu) != 0;
	}

	void Board::tickSofIfDue(const uint64_t frameIndex) noexcept
	{
		/* The Board owns the test and the Scheduler never makes it. The
		 * Scheduler calls this on every frame, unconditionally, and passes the
		 * authoritative virtual frame index; the 96:1 relation is a property of
		 * the USB device model rather than of the scheduler, so it is tested
		 * here. Design section 9.4.
		 *
		 * The transport is pumped first, and this method is where it happens
		 * because scheduler.cpp calls this on every frame, unconditionally,
		 * immediately before runMcu: it is already the per-quantum boundary a
		 * hub drain must sit on. The SOF test below must not gate it -- a drain
		 * that ran once every 96 quanta would make the hub's frame index count
		 * SOF frames rather than quanta.
		 *
		 * The order is deliberate. The frames cross into the device before the
		 * MCU runs the quantum that may read them, so a frame that entered at
		 * quantum N is visible to the firmware in quantum N and not in N+1. */
		pumpTransport();

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

	/* The reset reuses resetMcu rather than calling mcf5307_reset itself, so
	 * the core's reset and the clearing of this class's fault bit stay one
	 * decision in one place; a second call site here could drift from that one.
	 *
	 * The vectors are zero because there is no other defensible value. A reset
	 * that is not handed an initial stack pointer and an initial program
	 * counter has no source for either: the firmware's reset vector lives in a
	 * flash image this call is explicitly not reading, and a plausible-looking
	 * address invented here would be a machine configuration wearing the
	 * costume of a measurement. A caller that knows the vectors calls resetMcu
	 * with them.
	 *
	 * board.h states in full what this call does not zero, and why. */
	void Board::reset() noexcept
	{
		resetMcu(0u, 0u);

		m_lastFrameIndex = 0;

		m_dspSet.reset();
	}

	/* The comparison against the version word is what turns it from decoration
	 * into a guard, and it is before the first write, so a refused load leaves
	 * this object exactly as it was -- the same rule ChainAdapter::stateLoad's
	 * geometry header and DspSet::stateLoad's bridge test already follow. */
	Status Board::stateLoad(const void* const src) noexcept
	{
		BoardState s;
		std::memcpy(&s, src, sizeof s);

		if(s.version != g_boardStateVersion)
			return Status::BadStateImage;

		m_lastFrameIndex = s.lastFrameIndex;
		m_faulted        = s.faulted != 0u;

		return Status::Ok;
	}
}
