// The Board is the MCU substrate the Scheduler runs on. Every CS0 to CS5
// device and the TransportHub hang off it.
//
// The Board is concrete, and that is a requirement, not an accident. The
// Scheduler's job array and the ChainAdapter's callbacks point into it, so it
// must never move, be copied, or participate in dynamic dispatch; `final` plus
// the static_asserts below make that a compile-time property rather than a
// convention.
//
// G2_MCU_CORE_CLOCK_HZ's 45,000,000 is a placeholder -- the lowest in-spec
// catalog speed grade -- and the Board logs one line at construction saying so.
// No golden reference and no capture may be recorded until the real value is
// measured.

// Where every window comes from. The CS1, CS3, CS5 and SDRAM bases are recorded
// in AGENTS.md and live as constants in memoryMap.h. CS0's, CS2's and CS4's
// bases are recorded by no authority, and no authority records a size for any
// window. Every base and every size is therefore configuration, and BoardConfig
// below is how a caller supplies it. This file invents no address and ships no
// default layout, for the reason memoryMap.h already gives: some bases have
// nothing to take a default from. A default-constructed Board therefore answers
// nowhere, which is the honest answer for a board nobody has configured.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <mcf5407.h>

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
#include "transportHub.h"
#include "uart0.h"

namespace rg2
{
	/* The panel board's analogue controls, in the order the converter scans
	 * them. The two channels the firmware also scans past these are tied to
	 * ground on the panel PCB, so they are not controls and are not named. */
	enum class PanelControl : uint8_t
	{
		MasterVolume = 0,
		ControlPedal = 1,
		Aftertouch   = 2,
		PitchStick   = 3,
		ModWheel     = 4
	};

	constexpr size_t g_panelControlCount = 5u;

	/* The potentials the panel board presents to the converter when nobody is
	 * touching anything. See the definition for where each figure comes from. */
	Max1039Config panelAdcConfig();

	/* Where a control rests, as a fraction of the reference, so that a caller
	 * driving a control from a normalised position starts from the same place
	 * panelAdcConfig() puts it rather than from a second copy of the figure. */
	float panelControlRestPosition(PanelControl _control);

	/* The whole board layout a caller supplies. It carries no default address
	 * of its own: `memory` starts with every window absent (size zero), which
	 * answers at no address at all, and a caller fills in the windows the
	 * machine it is modelling actually has. */
	struct BoardConfig
	{
		MemoryMapConfig memory;

		/* Which machine of the family the straps present. It reaches two units
		 * that are decoded far apart -- the CS5 latch the panel identifier
		 * lives in, and the SIM's UIPCR byte -- and detect_model() reads both,
		 * so setting only one presents a machine that does not exist. This
		 * field is what makes that pair inseparable. */
		Model model = Model::G2X;

		/* The populated-port set of the HDI08 array. The expanded machine is the
		 * default; it reaches no address unless the caller also gives CS1 a
		 * window. */
		Hdi08Decode hdi08{g_hdi08ExpandedPorts};

		/* The one two-wire slave the machine carries. Its potentials start at
		 * zero: the part has no opinion about what a board connects to it, and
		 * a Board is composed by callers that model different amounts of the
		 * machine. panelAdcConfig() above is the panel's own description, and a
		 * caller that means to model the panel assigns it here.
		 *
		 * It is not the default, and the reason is measured rather than
		 * stylistic. A panel at rest converts to values the firmware's boot
		 * calibration accepts, so the firmware leaves that calibration and runs
		 * on into code that reads the panel's static RAM. A Board composed
		 * without that window then faults there. Wiring the panel and mapping
		 * what the panel needs are one decision, and this is the field that
		 * would silently take half of it. panelSram.h carries the other half:
		 * the bank, and the CS4 window that reaches it. */
		Max1039Config adc;

		/* The ISP1181 endpoint the G2 protocol runs over. It is configuration
		 * and not a constant this file invents; a caller may name another one.
		 *
		 * The default is a measurement of the emulated firmware: booting the
		 * Clavia image, handing a real `.pch2` to one endpoint and recording
		 * every byte the firmware writes to the CS3 command port. On endpoint 3
		 * it answers read-interrupt-register `0xC0`, endpoint-3 status `0x54`,
		 * read endpoint 3's buffer `0x14`, clear endpoint 3's buffer `0x74` --
		 * the authority's OUT sequence, a drain -- and the
		 * DcEndpointConfiguration bytes it writes have EPDIR clear on that
		 * endpoint's slot, so the firmware declared the buffer host-to-device.
		 * Endpoint 0 in the same run answers `0xC0 0x50` and no more.
		 *
		 * The buffer table cannot discriminate an endpoint, being firmware
		 * configuration rather than a property of the part: ISP1362 Rev. 06
		 * pp.51-53 put the size in FFOSZ[3:0] and the buffering in DBLBUF, both
		 * fields of a register the firmware writes.
		 *
		 * TODO(usb-descriptors): a capture of the real device's descriptors
		 * would be a better authority than this measurement; none has been
		 * taken. */
		int usbProtocolEndpoint = 3;

		/* The maximum packet size of that endpoint, in bytes -- the largest
		 * single packet the Board may hand to `isp1181_rx`.
		 *
		 * A wire constraint, and the Board is the side that must honour it: on a
		 * real G2 the host controller splits a bulk transfer into
		 * max-packet-size packets and the firmware reassembles them, so the
		 * peripheral never sees a packet larger than the buffer it configured.
		 * This Board stands where that host controller stands.
		 *
		 * Configuration rather than a constant, because the size is a field the
		 * firmware writes -- FFOSZ[3:0] of the `0x20+n` byte (ISP1362 Rev. 06
		 * Table 110, p.107), with endpoint 0 fixed at 64 and endpoints 1 to 14
		 * programmable (Table 15, p.51), which states the consequence: "The size
		 * of the buffer memory determines the maximum packet size that the
		 * hardware can support for a given endpoint."
		 *
		 * 64 is also the ceiling, so the default cannot be raised: Table 16
		 * (p.52) allows 8, 16, 32 and 64 and reserves the rest, and Table 109
		 * (p.105) agrees with "interrupt/bulk: N <= 64 bytes". Section 15.2.1
		 * (p.113) contradicts both with "N <= 32"; 64 is used because the two
		 * tables agree. No configuration makes the 862-byte object of a real
		 * `.pch2` deliverable whole, so the split is not a workaround for one
		 * image.
		 *
		 * The device model configures the same 64 and exposes no query for it, so
		 * the figure is duplicated and kept in step by hand; pumpTransport
		 * documents how a drift in either direction shows up.
		 *
		 * TODO(usb-abi): give the device model a max-packet query, so this
		 * figure is read rather than duplicated. */
		std::size_t usbMaxPacketBytes = 64;

		/* Whether a frame whose length is an exact multiple of
		 * `usbMaxPacketBytes` is followed by a zero-length packet.
		 *
		 * The answer is unknown, and the default is the one that invents
		 * nothing. Terminating such a transfer with a zero-length packet is a
		 * USB bulk convention, and only a convention here: neither ISP1362
		 * Rev. 06 nor AN10008-01 contains it, "zero-length" does not appear in
		 * the data sheet at all, and neither document describes an
		 * exact-multiple bulk transfer. AN10008-01 pp.74 and 88 do send an
		 * empty packet, but on the control IN endpoint to end a control read --
		 * a different endpoint, direction and transfer type.
		 *
		 * What the data sheet does say is about DMA and not framing: section
		 * 12.4.3.1 (p.56) and Table 20 (p.57) make a short packet an
		 * end-of-transfer condition for a DMA-driven OUT endpoint, gated by
		 * SHORTP in DcDMAConfiguration (p.111) -- which the firmware may or may
		 * not have enabled, and this Board cannot see which. If it ends a
		 * message on the first short packet, an exact-multiple frame with no
		 * trailing empty packet never ends; if it counts bytes from the object's
		 * own 2-byte length header, that packet is spurious. The two readings
		 * want opposite defaults, which is why this is a flag.
		 *
		 * `false` sends no extra packet: the whole corpus is reachable without
		 * one, and a packet the firmware did not ask for is traffic this project
		 * invented. */
		bool usbTerminateWithZeroLengthPacket = false;
	};

	// The Board. Concrete, final, neither copyable nor movable. Constructed by
	// the Device subclass before the Scheduler and destroyed after it; the
	// Scheduler borrows the Board and never destroys it.
	class Board final
	{
	public:
		/* Creates the MCF5407 core context, initialises the Nim runtime once,
		 * and logs the G2_MCU_CORE_CLOCK_HZ placeholder line exactly once. */
		Board();

		/* Builds the units from `_config`, attaches each to the region it
		 * answers, and points the MCF5407 core's bus callbacks at the decode.
		 * Every base and every size comes from `_config`; this class chooses
		 * none of them. */
		explicit Board(const BoardConfig& _config);

		~Board();

		Board(const Board&)            = delete;
		Board& operator=(const Board&) = delete;
		Board(Board&&)                 = delete;
		Board& operator=(Board&&)      = delete;

		/* One quantum of the MCU context. Returns the emulated cycles spent --
		 * exactly what mcf5407_exec returns. It forwards directly to
		 * mcf5407_exec, which already takes a cycle budget. uint32_t, not
		 * int64_t: it returns exactly what the core returned, and the Scheduler
		 * widens at the call site.
		 *
		 * The return may exceed `wantCycles`, by up to the cost of one
		 * instruction, because mcf5407_exec finishes the instruction it
		 * started. That overrun is not a defect to absorb here: it is what
		 * rg2::runQuantum's cycle debt exists to carry, and clamping it in this
		 * method would make the debt identically zero. */
		uint32_t runMcu(uint32_t wantCycles) noexcept;

		/* True when the MCF5407 core stopped because an instruction trapped --
		 * a bus error, an illegal instruction word, an illegal effective address
		 * for the opcode, an illegal operand size or a divide by zero.
		 *
		 * Fault and halt are different flags and this method reports the fault.
		 * mcf5407.h is the authority: a valid
		 * opcode with no implemented semantics halts without faulting, and a
		 * faulted core is always also halted. mcuHalted() below is the wider
		 * condition. */
		bool faulted() const noexcept;

		/* A Board whose core pointer is nil answers a defined value rather than
		 * dereferencing it, in the shape runMcu uses for that case.
		 *
		 * mcuReg and setMcuReg take the register file's own index: 0 to 7 are
		 * d0 to d7, 8 to 15 are a0 to a7, 16 is the status register and 17 is
		 * the program counter. mcf5407.h owns that mapping and this class
		 * restates none of it. setMcuReg answers false for an out-of-range index
		 * and for a nil core, which is what the C call already answers. */
		void     resetMcu(uint32_t initialSp, uint32_t initialPc) noexcept;
		uint32_t mcuReg(int index) const noexcept;
		bool     setMcuReg(int index, uint32_t value) noexcept;

		/* True when the core will run no further instruction until the next
		 * reset. It is the strictly wider condition of the two the core reports:
		 * every faulted core is halted and a halted core need not be faulted, so
		 * this is the one a caller asking "may I run more" must ask, and
		 * faulted() is the one a caller asking "did this instruction trap" must
		 * ask. */
		bool mcuHalted() const noexcept;

		/* The 1 kHz USB start-of-frame tick. The Board owns the test: the
		 * Scheduler calls this on every frame, unconditionally, passing the
		 * authoritative virtual frame index, and the Board tests
		 * frameIndex % 96 == 0 itself. On each due frame it advances m_usb by
		 * exactly one SOF frame. */
		void tickSofIfDue(uint64_t frameIndex) noexcept;

		/* The MCU context's determinism-relevant state, embedded in the
		 * Scheduler snapshot. This serialises the Board's own state only; the
		 * core's mcf5407_state_* and isp1181_state_* blocks are not folded in.
		 *
		 * stateLoad reports Status::Ok, or Status::BadStateImage for an image
		 * whose version word is not the one this build writes. An exception is
		 * forbidden and a release build removes an assertion, so the return
		 * value is the whole channel.
		 *
		 * The guard is before the first write, so a refused load changes
		 * nothing. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* The reset covers the MCF5407 core, through the same mcf5407_reset the
		 * resetMcu above drives; this class's own snapshot state -- the fault
		 * bit and the last frame index; and the DSP set, through DspSet::reset.
		 *
		 * The bus targets attached to this board are not zeroed by this call:
		 * the flash images, the SDRAM window, the latches, the panel surface,
		 * the MBAR block and the USB device all keep what they held. BusTarget
		 * declares read and write and nothing else, and MemoryMap hands out a
		 * BusTarget* and offers no walk over the attached set. A caller that
		 * needs a cleared SDRAM must still reconstruct the board. */
		void reset() noexcept;

		/* The bus, as the memory map sees it. onRead and onWrite forward here,
		 * so this is the routing itself; a caller may drive it without running
		 * a program.
		 *
		 * `_size` here is a width in BITS -- 8, 16 or 32 -- which is the
		 * MemoryMap's unit and not the core's. The two callbacks below take the
		 * core's unit and convert; this pair is below that conversion. */
		uint32_t busRead(uint32_t _address, int _size, mcf5407_bus_status& _status);
		void     busWrite(uint32_t _address, int _size, uint32_t _value,
		                  mcf5407_bus_status& _status);

		/* The installed callbacks, public on purpose: these are the exact
		 * function pointers handed to mcf5407_create, so they are the path the
		 * core takes.
		 *
		 * They are not a second route into the Board. Each one forwards to
		 * busRead or busWrite and converts the one argument whose unit differs
		 * between the two sides, and does nothing else.
		 *
		 * `size` here is a count of BYTES -- 1, 2 or 4 -- because that is what
		 * mcf5407.h hands an mcf5407_read_fn and an mcf5407_write_fn, and these
		 * two are that pair. busRead and busWrite above take bits, and the
		 * conversion between the two units happens here and nowhere else. A
		 * caller that drives these directly must therefore supply 1, 2 or 4; any
		 * other value is refused as MCF5407_BUS_SIZE_ILLEGAL, 8, 16 and 32
		 * included, because those are legal widths in the other unit. */
		static uint32_t onRead(void* user, uint32_t addr, int size,
		                       mcf5407_bus_status* status);
		static void     onWrite(void* user, uint32_t addr, int size,
		                        uint32_t value, mcf5407_bus_status* status);

		/* The Board's transport hub. The attachments -- the internal
		 * client, the forked G2-Edit socket and the usbip adapter -- share one
		 * hub. It is a member and not a pointer, so there is no state in which
		 * a Board has no hub and no order in which an attachment can reach one
		 * that does not exist yet.
		 *
		 * Attaching is the caller's and not this class's. The Board attaches
		 * nothing: an attachment the Board created would be a component of the
		 * Board rather than a peer of the other two. */
		TransportHub& transport() noexcept { return m_transport; }

		/* One quantum's worth of attachment-to-device traffic. It drains the
		 * hub exactly once and hands every drained frame to the USB device.
		 *
		 * It is called from tickSofIfDue, the one Board method the Scheduler
		 * calls unconditionally on every frame, immediately before runMcu: the
		 * fixed per-quantum boundary this drain must sit on. It is reachable on
		 * its own so that a check can drive one quantum's transport without
		 * driving a SOF tick.
		 *
		 * It allocates nothing. The drain target is sized once, with the hub,
		 * in the constructor's member initialiser list.
		 *
		 * A frame the device refuses is held and re-offered, not discarded.
		 * `isp1181_rx` answers 0 for a NAK, and mcf5407.h states what that
		 * costs: "THE PACKET IS GONE IN EVERY ONE OF THOSE CASES - a refusal
		 * here is a dropped packet and not a deferred one". So the deferral
		 * has to live on this side of the call, and it does: the refused
		 * bytes are copied into Board-owned storage and offered again at the
		 * next quantum, ahead of anything still queued. usbTransport() is how
		 * a caller reads what that cost.
		 *
		 * A `usbMaxPacketBytes` larger than the device's own buffer shows up
		 * here: every oversized packet is refused forever, because `Fifo.accept`
		 * rejects on length whatever the occupancy, so draining never clears it
		 * and the stall line below fires every 255 ms of emulated time. One too
		 * small makes every packet short instead, which a firmware ending a
		 * transfer on the first short packet reads as a whole message -- not
		 * detectable from here, which is why that figure is stated rather than
		 * derived downwards. */
		void pumpTransport() noexcept;

		/* What the device did with the bytes this Board handed it, per Board.
		 * Members and not a file-scope tally: a file-scope counter pools two
		 * Boards in one process into one figure, leaving a reader to subtract
		 * one arm from the other by hand before the number means anything.
		 *
		 * The no-loss invariant, which holds at every quantum boundary:
		 * `drained == completed + undeliverable + (held ? 1 : 0)`. Nothing this
		 * Board takes out of the hub can go anywhere except into the device,
		 * into the hold, or into `undeliverable`. */
		struct UsbTransportStats
		{
			uint64_t pumps         = 0;
			uint64_t drained       = 0;

			// Counted in packets, not frames: pumpTransport splits a frame into
			// max-packet-size packets, so one drained frame can cost many
			// offers, and re-offers of a held frame count too. They partition
			// exactly: offered == accepted + refused.
			uint64_t offered       = 0;
			uint64_t accepted      = 0;
			uint64_t refused       = 0;

			// The frame-shaped counter, and the one the invariant above uses. A
			// frame is completed when its last packet is accepted.
			uint64_t completed     = 0;

			// A defect report rather than a mode: a frame too large for the hold
			// buffer that left the hub and can never be offered. The hub refuses
			// such a frame before the drain, so a non-zero reading means that
			// guarantee broke. Not subtracted from `drained`, because the frame
			// did leave the hub; not `refused`, because the device never saw it.
			uint64_t undeliverable = 0;

			// The loud lines written for a frame the device would not take
			// within the datasheet's own NAK retry window.
			uint64_t stallReports  = 0;
			uint64_t heldAttempts  = 0;

			// How many bytes of the held frame the device has already taken, so
			// a partly-delivered frame reads as one rather than as undelivered,
			// and that frame's whole length. Both 0 when nothing is held.
			size_t   heldOffset    = 0;
			size_t   heldSize      = 0;
			bool     held          = false;
		};

		UsbTransportStats usbTransport() const noexcept;

		/* The device's transmit callback, public for the reason onRead and
		 * onWrite above are public: it is the exact function pointer handed to
		 * isp1181_create, so it is the path the device takes.
		 *
		 * `endpoint` is accepted and not filtered. The hub carries bytes and
		 * names no endpoint, the protocol's framing is in the payload itself,
		 * and no authority in this project records which endpoints the device
		 * transmits on. */
		static void     onUsbTx(void* user, int endpoint, const uint8_t* data,
		                        size_t len);

		/* The device's service request, public for the reason onUsbTx above is
		 * public: it is the exact function pointer handed to isp1181_create.
		 *
		 * It names the pin and nothing else. The level, the autovector bit and
		 * the vector are the interrupt controller's to derive -- from IRQPAR
		 * and from AVR, both of which the firmware programs through the MBAR
		 * window. Naming a level here would freeze at construction a value the
		 * firmware is still free to move. */
		static void     onUsbIrq(void* user, int asserted);

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
		 * absolutely and tells them apart by address. This adapter is the join.
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

			uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

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

			uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		private:
			// True when the offset belongs to UART0's model rather than the
			// SIM's. The one strap offset the SIM answers is excluded here and
			// nowhere else, so the rule has a single site.
			static bool isUartOwned(uint32_t _offset);

			// True when the offset belongs to the M-Bus module. The bound comes
			// from mbus.h, so this file states no register address of its own.
			static bool isMbusOwned(uint32_t _offset);

			/* True when the offset is one of the register groups the
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

			uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		private:
			isp1181_ctx*& m_usb;
		};

		// The strap offset the SIM answers inside the UART block. sim.cpp's
		// register table carries it as UIPCR.
		static constexpr uint32_t g_simUartStrapOffset = g_simUipcrOffset;

		/* Attach every unit to the region it answers. It is called from the
		 * constructor and takes nothing: an absent window (size zero) decodes
		 * to no region at all, so attaching unconditionally is what makes a
		 * default-constructed Board answer nowhere without a second code path
		 * that could disagree with this one. */
		void attachUnits();

		/* The acknowledge callback stays private: only the core drives it, and
		 * its body says why it clears nothing. */
		static void     onInterruptAck(void* user, int level, uint8_t vector);

		/* The controller's present callback hands the whole current state to
		 * the sink and does nothing else: no arbitration, no pending bit and no
		 * priority decision. The `autovector` argument is forwarded and not
		 * decided -- the controller has already read the AVEC bit of the
		 * winning source's ICR, and a present function that chose either form
		 * here would silently override the bit the firmware programmed.
		 *
		 * It is a no-op while the core handle is null. The controller exists
		 * before `mcf5407_create` returns, and `Uart0`'s constructor programs
		 * its vector into the controller, which presents; that presentation
		 * has no core to reach. */
		static void     onInterruptPresent(void* user, int level, uint8_t vector,
		                                   int autovector);

		/* Declaration order is initialisation order and it is load-bearing
		 * here. m_memory is declared before the adapters because they bind
		 * references to it, and the units are declared before the adapters
		 * that forward to them.
		 *
		 * m_mcu and m_interrupts are first, and that position is the reason
		 * onInterruptPresent can read them at all. Constructing m_uart0 asks
		 * the controller to record its vector, and the controller presents on
		 * every change -- so the present callback runs while the members below
		 * it are still raw storage. m_mcu is the member it reads, so it is
		 * initialised before any unit that can present exists. */
		mcf5407_ctx*        m_mcu;
		InterruptController m_interrupts;

		MemoryMap    m_memory;

		Flash        m_flash;
		Panel        m_panel;
		Latches      m_latches;
		Hdi08Adapter m_hdi08;
		Sim          m_sim;
		Uart0        m_uart0;

		/* The slave is declared before the controller that points at it, for
		 * the same reason the units are declared before the adapters: the
		 * controller binds its address at construction. */
		Max1039      m_adc;
		MBus         m_mbus;

		FlashWindow  m_flashCs0;
		FlashWindow  m_flashCs2;
		MbarRouter   m_mbar;

		/* Declared before m_usb, so it is constructed while that handle is
		 * still null. It binds a reference to the handle rather than copying
		 * it, which is what makes the order harmless: the assignment in the
		 * constructor body is what every later read sees. Storing the handle
		 * by value here would capture the null instead. */
		Isp1181Window m_usbCs3;

		/* The ISP1181 USB device this Board owns; tickSofIfDue is what advances
		 * it. The Board creates it in the constructor and destroys it in the
		 * destructor, so its lifetime is exactly the Board's. */
		isp1181_ctx* m_usb;

		/* The endpoint pumpTransport delivers on, taken from BoardConfig
		 * because no authority records it. */
		int          m_usbProtocolEndpoint;

		/* The wire constraint, held as a member so that no literal appears in
		 * pumpTransport. BoardConfig::usbMaxPacketBytes carries the whole
		 * argument for the figure and for the flag beside it. */
		size_t       m_usbMaxPacketBytes;
		bool         m_usbZeroLengthTerminator;

		/* The hub, and the drain target it fills. Both are sized in the
		 * constructor's member initialiser list and neither grows again:
		 * pumpTransport runs on the scheduler thread inside a quantum boundary,
		 * where allocation is forbidden. m_drained holds kMaxEndpoints x
		 * queueDepth entries, which is every frame the hub can possibly hand
		 * out in one drain, so a drain can never be cut short by this
		 * buffer. */
		TransportHub              m_transport;
		std::vector<StampedFrame> m_drained;

		/* The one refused frame this Board still owns, and its own copy of
		 * the bytes. It cannot be a pointer into the hub: transportHub.h
		 * gives a drained frame a lifetime that ends at the NEXT drain, and
		 * pumpTransport drains on every quantum whether it takes a frame or
		 * not, so a held pointer would dangle exactly one quantum later. The
		 * buffer is sized with the hub in the constructor's member
		 * initialiser list and never grows, because allocation at a quantum
		 * boundary is forbidden. */
		std::vector<uint8_t> m_heldBytes;
		size_t               m_heldSize     = 0;
		bool                 m_heldValid    = false;
		uint64_t             m_heldAttempts = 0;

		/* How many bytes of the held frame the device has already taken. It is
		 * the cursor the split runs on: the next packet starts here, and the
		 * frame is finished when this reaches m_heldSize (and, when the
		 * zero-length terminator is enabled and the length was an exact
		 * multiple, when that final empty packet has been taken too).
		 *
		 * It is a byte offset and not a packet index, because the last packet
		 * of a frame is usually short and a packet index could not express
		 * where it ends. */
		size_t               m_heldOffset   = 0;

		/* True when the only thing left to send for the held frame is the
		 * terminating empty packet. Without this flag `m_heldOffset ==
		 * m_heldSize` would mean two different states -- "finished" and "all
		 * the bytes are across but the terminator is not" -- and a frame in
		 * the second would be reported completed while a packet it still owes
		 * had never been offered. */
		bool                 m_heldNeedsZlp = false;

		UsbTransportStats    m_usbStats;

		uint64_t     m_lastFrameIndex = 0;
		bool         m_faulted        = false;

		/* Last, and that position is destruction order and not construction
		 * order. The set borrows nothing at construction -- the bridges are
		 * attached from the constructor body, after every member exists -- but
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

