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
//     CS3    the ISP1181 USB device, through Isp1181Window
//     CS4    the panel
//     CS5    the latches
//     MBAR   the SIM, UART0, the M-Bus and the interrupt controller,
//            through MbarRouter
//
// THE SDRAM GETS NO TARGET, AND THAT IS DELIBERATE RATHER THAN UNFINISHED.
// Main memory is not one of the seven units this task composes; the harness
// supplies it (see main.cpp). A region with no target answers exactly as a
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

#include "hdi08Bridge.h"

#include <cstdio>
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

	uint32_t Board::Isp1181Window::read(const uint32_t _offset, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		/* THE STUB ANSWERS EVERY CYCLE AT EVERY OFFSET, so neither arm can
		 * produce anything but BUS_OK -- a fault status from a device that
		 * accepts everything would be this file inventing a policy no design
		 * section states, which is exactly what FlashWindow::write's comment
		 * records for the flash. The 16 and 32-bit cycles are ANSWERED rather
		 * than refused: the driver reads the part word-sized in the register
		 * file idiom, and a refusal here would turn each such cycle into a
		 * logged bus failure indistinguishable from the unmapped one this
		 * wiring removes. The byte answer is REPLICATED across the access
		 * width, big-endian, in the shape main.cpp's own Ram target models. */
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

		/* THE LOW BYTE ONLY. The stub keeps nothing it is handed and the full
		 * model routes one register per address, so there is no wider state to
		 * compose a multi-byte store into. */
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
		/* TASK BRD-34. IRQPAR, AVR and the internal control block are BYTE
		 * registers -- MCF5307 UM Table B-1 gives each of them one byte -- so
		 * a wider access is refused here rather than split, in the shape
		 * uart0.cpp already uses for its own byte-only rule. */
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

		/* THE GUARD IS ON THE HANDLE AND NOT ON THE RESET. mcf5307.h records
		 * that a board presenting a level 1 to 6 interrupt immediately after
		 * mcf5307_reset is a DEFINED case, so a presentation before the first
		 * reset is fine; a presentation before the core EXISTS has nowhere to
		 * go. */
		if(!board->m_mcu)
			return;

		/* The whole current state, unconditionally, on every recomputation.
		 * mcf5307_set_irq is IDEMPOTENT and mcf5307.h says so, which is what
		 * licenses the board to present on a CLEAR exactly as it does on an
		 * assert. */
		mcf5307_set_irq(board->m_mcu, level, vector, autovector);
	}

	void Board::onInterruptAck(void*, const int, const uint8_t)
	{
		/* NOTHING IS CLEARED HERE AND THAT IS THE CONTRACT RATHER THAN AN
		 * OMISSION. mcf5307.h states that an acknowledge clears an
		 * EDGE-TRIGGERED source on the board's own side, and every source this
		 * board carries is level-triggered: the timer's TER[REF] drops when
		 * the firmware writes it and UART0's condition drops when the
		 * firmware empties the receiver. */
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

	namespace
	{
		/* THE TWO SIZES TransportHub CANNOT DERIVE FOR ITSELF, supplied here
		 * because the Board is what constructs the hub.
		 *
		 * g_transportMaxFrameBytes IS DERIVED AND NOT MEASURED. Design section
		 * 15.3's wire framing is [1-byte type][2-byte length][payload], so the
		 * ceiling is 1 + 2 + 65,535. transportHub.h states that it is
		 * deliberately the loosest bound the design can give and that PROTO-10
		 * lowers it once the largest real patch message has been measured.
		 * That measurement has not been taken here and the loose bound stands.
		 *
		 * g_transportQueueDepth IS CHOSEN. An ordinary tunable, not a gated
		 * constant, exactly as transportHub.h records.
		 *
		 * IT WAS 16 AND THAT WAS TOO SMALL FOR A REAL PATCH. A `.pch2` load is
		 * ORIGINATED WHOLE: g2::pch2Load validates the container and then hands
		 * the hub one frame per object with no quantum boundary in between, so
		 * the whole patch must fit the queue at once or the load is refused and
		 * NOTHING is delivered. Every one of the 73 files in the artifact
		 * corpus carries EXACTLY 18 objects -- measured over the corpus, not
		 * recalled -- so a depth of 16 refused every real patch with
		 * PCH2-SEND-REFUSED. t1_patch_running is what fails when this number is
		 * below the object count of the largest patch. */
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

		/* TASK BRD-34. BOTH TIMER MODULES ASSERT ON THE BOARD'S ONE
		 * CONTROLLER. Uart0 already holds it -- the initialiser list above
		 * hands it in, which is what replaces the nullptr default that left
		 * its interrupt path dead on the assembled machine. THIS CALL IS THE
		 * TIMERS' HALF OF THE SAME WIRE, and Sim::setInterruptController
		 * forwards it to both modules. */
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
		 * BOTH CALLBACKS ARE THIS BOARD'S OWN. The comment that stood here
		 * said both were null because the Board had nowhere to route either.
		 * Neither half of that is true any more. A packet the device hands to
		 * tx reaches every attachment through the hub above, and the device's
		 * service request reaches the machine's one interrupt controller.
		 *
		 * THE AUTHORITY THAT WAS MISSING IS THE FIRMWARE, AND IT HAS NOW BEEN
		 * READ. The sentence that stood here -- that routing the IRQ needed a
		 * level and a vector "that no authority in this project records for
		 * CS3" -- is superseded, not merely softened. Both G2 images install
		 * the USB handler at level 3, autovectored, vector 27 at VBR+0x6C, by
		 * two independent encodings that agree; both leave IRQPAR unwritten;
		 * and both enable the device's own interrupts unconditionally during
		 * USB bring-up (DcInterruptEnable = 0x00001F07, then Mode bit 3). So
		 * the wired callback is a path the firmware WILL exercise, and the
		 * null that stood here was a functional gap rather than a no-op.
		 * onUsbIrq carries the derivation and the READ-versus-INFERRED split.
		 *
		 * WHAT THE TX CALLBACK CAN AND CANNOT DO IN THIS TREE, MEASURED RATHER
		 * THAN ASSUMED. `src/isp1181/isp1181.nim` STORES the callback at
		 * construction and CALLS IT FROM NOWHERE: no line of that model
		 * invokes `m.tx`. So the wire is installed and correct and the device
		 * cannot yet drive it. Installing it anyway is what makes the
		 * direction exist at all, and t0_board_transport drives the pointer
		 * directly for exactly that reason.
		 *
		 * THE SAME IS MEASURED FOR THE IRQ CALLBACK AND IT COMES OUT ONE STEP
		 * FURTHER ALONG. `src/isp1181/isp1181.nim` DOES call the stored irq
		 * callback, from `updateIrq`, whenever the masked interrupt register
		 * changes state. What no implemented command does is SET a bit of that
		 * register: `raiseInterrupt` is the only writer and its own head block
		 * records that nothing calls it, because that model has no
		 * event-to-bit assignment yet. So the wire installed here is correct
		 * and complete on this side, and the device cannot yet drive it.
		 * t0_board_interrupts case group 5 drives the pointer directly for the
		 * same reason t0_board_transport does. */
		m_usb = isp1181_create(this, &Board::onUsbIrq, &Board::onUsbTx);

		/* THE HANDLE IS MOVED OFF THE STUB, DELIBERATELY AND HERE. `mcf5307`'s
		 * header states it plainly: the Stub is the create-time default, it is
		 * a device that is present in the CS3 window and inert -- every read
		 * answers 0x00, every packet is discarded, neither callback is ever
		 * called -- and "select it deliberately; nothing selects it for you".
		 * A Board that left the default in place would call isp1181_rx once
		 * per drained frame into a device that throws every one away, which is
		 * the state t0_board_transport's case 3 records as unobservable.
		 *
		 * THE RETURN IS CHECKED AND NOT DISCARDED. The call is refused for a
		 * nil handle and for a backend value neither macro names, and a
		 * refusal MOVES NOTHING -- so an unchecked call would leave a Stub in
		 * place and look identical to a successful one. That is the silent
		 * failure this project refuses to build. The failure is reported on
		 * the same stream the clock line below uses and the Board continues:
		 * a Board without a usable USB device is still a Board that boots, and
		 * t0_usb_ingress_byte is what fails when the wire is dead. */
		if(m_usb != nullptr &&
		   isp1181_set_backend(m_usb, MCF5307_ISP1181_BACKEND_FULL_MODEL) != 1)
		{
			std::cout << "board: isp1181_set_backend(FULL_MODEL) was REFUSED;"
			             " the USB device stays inert and no patch byte can"
			             " reach the firmware"
			          << std::endl;
		}

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

		/* STRAIGHT TO THE HUB, WHICH DELIVERS TO EVERY ATTACHMENT IN
		 * ATTACHMENT ORDER. The buffer is the DEVICE'S and is borrowed for the
		 * duration of this call; TransportHub::fromDevice hands each endpoint
		 * the same borrow, and design section 13.10.6 gives that argument the
		 * shortest of the three lifetimes, so an endpoint that keeps it copies
		 * it. Nothing is copied here. */
		board->m_transport.fromDevice(ProtocolFrame{ data, len });
	}

	void Board::onUsbIrq(void* const user, const int asserted)
	{
		Board* const board = static_cast<Board*>(user);

		if(board == nullptr)
			return;

		/* THE PIN IS NAMED AND THE LEVEL IS NOT. IRQ3 is what the firmware
		 * operates: BOOT:0x31FE writes its USB handler to VBR+108 -- vector
		 * 27, which is the ColdFire autovector formula 24+level at level 3 --
		 * sets AVR to 0x08 and clears IMR bit 3, and CODE reaches the same
		 * three effects through install_autovector(3, 0x30053C38) at that
		 * helper's ONLY call site in the image. Neither image ever writes
		 * IRQPAR, so IRQ3 keeps its reset level.
		 *
		 * READ VERSUS INFERRED, AND THE DIFFERENCE IS NOT SMOOTHED OVER. The
		 * LEVEL is read out of the firmware. That the PIN is IRQ3 is INFERRED
		 * from it: only IRQ3 can present at level 3, and no image names a pin.
		 * The schematic cannot settle it -- U24 carries exactly one extracted
		 * pin and the string IRQ occurs nowhere in it.
		 *
		 * THE CONTROLLER DERIVES EVERYTHING ELSE. externalLevel applies UM
		 * Table 8-4 to IRQPAR and the autovector bit comes from AVR, both of
		 * which the firmware programs through the MBAR window at run time. A
		 * level written down here would be a constant standing where a
		 * derivation belongs, and it would stay 3 after a firmware that moved
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

		/* THE FAULT BIT IS TAKEN FROM THE CORE RATHER THAN DECIDED HERE. It is
		 * read back on the same call that advanced the core, so the answer
		 * faulted() gives cannot drift from the machine it describes. */
		m_faulted = mcf5307_faulted(m_mcu) != 0;

		/* TASK BRD-33. THE TIMERS ARE ADVANCED FROM THE CYCLES THIS CALL
		 * ACTUALLY RAN, and not from the budget it was asked for and not from
		 * any clock outside the machine. That is what makes a timer tick a
		 * function of executed cycles and keeps it deterministic under the
		 * scheduler's quantum. No new callback, no wall clock and no thread.
		 *
		 * `cycles` MAY BE GREATER THAN `wantCycles`, by up to the cost of one
		 * instruction. mcf5307.h is the contract: the core tests its budget
		 * only at an instruction boundary, so an instruction that starts
		 * inside the budget runs to completion and its whole cost is
		 * reported. The timers therefore see a quantum that is sometimes a
		 * few cycles longer than the one that was asked for.
		 *
		 * THAT IS THE CORRECT ARGUMENT AND NOT AN ACCEPTED ERROR, and the
		 * reason is the part and not this code. An MCF5307's timers count
		 * cycles that ELAPSED. The cycles of the instruction that crossed the
		 * budget elapsed on the machine, so a timer that did not see them
		 * would be a timer running slow by exactly the overrun -- and the
		 * overrun is not noise: it recurs on every quantum whose last
		 * instruction is not free, so the error would ACCUMULATE rather than
		 * average out. Feeding `wantCycles` instead would make the timer's
		 * rate a function of the scheduler's quantum length, which no
		 * property of the hardware depends on.
		 *
		 * NOTHING IS DOUBLE-COUNTED. g2::runQuantum carries `spent - want`
		 * forward as the cycle debt and shortens the NEXT quantum's budget by
		 * it, so the same emulated cycle is executed once, fed to the timers
		 * once, and paid for once. */
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
		/* THE BOARD OWNS THE TEST AND THE SCHEDULER NEVER MAKES IT. The
		 * Scheduler calls this on every frame, unconditionally, and passes the
		 * authoritative virtual frame index; the 96:1 relation is a property of
		 * the USB device model rather than of the scheduler, so it is tested
		 * here. Design section 9.4.
		 *
		 * THE TRANSPORT IS PUMPED FIRST, AND THIS METHOD IS WHERE IT HAPPENS
		 * BECAUSE OF WHERE THE SCHEDULER ALREADY CALLS IT. scheduler.cpp calls
		 * this on EVERY frame, unconditionally, immediately before runMcu --
		 * design section 13.5's fixed point -- so it is already the
		 * per-quantum boundary a hub drain must sit on, and hooking it here
		 * adds no call to the Scheduler and edits no file the sched track
		 * owns. THE SOF TEST BELOW MUST NOT GATE IT: a drain that ran once
		 * every 96 quanta would make the hub's frame index count SOF frames
		 * rather than quanta, which is the one property transportHub.h states
		 * about it.
		 *
		 * THE ORDER IS DELIBERATE. The frames cross into the device BEFORE the
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
		BoardState s;
		s.version        = g_boardStateVersion;
		s.lastFrameIndex = m_lastFrameIndex;
		s.faulted        = m_faulted ? 1u : 0u;
		std::memcpy(dst, &s, sizeof s);
	}

	/* THE RESET. It reuses resetMcu rather than calling mcf5307_reset itself,
	 * so the core's reset and the clearing of this class's fault bit stay ONE
	 * decision in ONE place; a second call site here could drift from that one.
	 *
	 * THE VECTORS ARE ZERO BECAUSE THERE IS NO OTHER DEFENSIBLE VALUE. A reset
	 * that is not handed an initial stack pointer and an initial program
	 * counter has no source for either: the firmware's reset vector lives in a
	 * flash image this call is explicitly not reading, and a plausible-looking
	 * address invented here would be a machine configuration wearing the
	 * costume of a measurement. A caller that knows the vectors calls resetMcu
	 * with them.
	 *
	 * board.h states in full what this call does NOT zero, and why. */
	void Board::reset() noexcept
	{
		resetMcu(0u, 0u);

		m_lastFrameIndex = 0;

		m_dspSet.reset();
	}

	/* THE LOAD, AND THE VERSION WORD IS NOW READ. stateSave has written it
	 * since BRD-21; nothing read it, because a void return had nowhere to
	 * report a refusal to. SCH-21 step 4's reconciliation gives it one, and the
	 * comparison is what turns the word from decoration into a guard.
	 *
	 * THE COMPARISON IS BEFORE THE FIRST WRITE, so a refused load leaves this
	 * object exactly as it was -- the same rule ChainAdapter::stateLoad's
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
