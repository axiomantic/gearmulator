#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <coldfire.h>

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

#include "hardwareLib/isp1181.h"

namespace rg2
{
	// Panel board analog controls in scan order
	enum class PanelControl : uint8_t
	{
		MasterVolume = 0,
		ControlPedal = 1,
		Aftertouch = 2,
		PitchStick = 3,
		ModWheel = 4
	};

	constexpr size_t g_panelControlCount = 5u;

	Max1039Config panelAdcConfig();
	float panelControlRestPosition(PanelControl _control);

	// Board memory and peripheral configuration
	struct BoardConfig
	{
		MemoryMapConfig memory;

		// Model strapped by CS5 latch and SIM UIPCR
		Model model = Model::G2X;

		// HDI08 port decode configuration
		Hdi08Decode hdi08{g_hdi08ExpandedPorts};

		// Panel ADC configuration (default 0; use panelAdcConfig() for panel)
		Max1039Config adc;

		// ISP1181 protocol endpoint (default 3 OUT per G2 protocol)
		int usbProtocolEndpoint = 3;

		// Bulk transfer packet size in bytes.
		// Datasheet: ISP1362 Rev. 06 Table 16 / Table 109 sets bulk max packet size to 64 bytes.
		std::size_t usbMaxPacketBytes = 64;

		// Optional zero-length packet termination for exact-multiple transfers
		bool usbTerminateWithZeroLengthPacket = false;
	};

	// The Board substrate hosting the ColdFire MCU, memory map, peripherals, and DSP bridge.
	class Board final
	{
	public:
		/* Creates the ColdFire core context, initialises the Nim runtime once,
		 * and logs the G2_MCU_CORE_CLOCK_HZ placeholder line exactly once. */
		Board();

		/* Builds the units from `_config`, attaches each to the region it
		 * answers, and points the ColdFire core's bus callbacks at the decode.
		 * Every base and every size comes from `_config`; this class chooses
		 * none of them. */
		explicit Board(const BoardConfig& _config);

		~Board();

		Board(const Board&) = delete;
		Board& operator=(const Board&) = delete;
		Board(Board&&) = delete;
		Board& operator=(Board&&) = delete;

		// Executes up to wantCycles on the ColdFire core; returns actual cycles consumed.
		uint32_t runMcu(uint32_t wantCycles) noexcept;

		// True if the ColdFire core trapped on an exception or bus error
		bool faulted() const noexcept;

		void resetMcu(uint32_t initialSp, uint32_t initialPc) noexcept;
		uint32_t mcuReg(int index) const noexcept;
		bool setMcuReg(int index, uint32_t value) noexcept;

		// True if the core is halted (normal stop or faulted)
		bool mcuHalted() const noexcept;

		// 1 kHz USB start-of-frame tick (advances SOF every 96 frames)
		void tickSofIfDue(uint64_t frameIndex) noexcept;

		size_t stateSize() const noexcept;
		void stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		// Resets ColdFire core, snapshot state, and DSP set
		void reset() noexcept;

		// Memory-map bus routing (size in bits: 8, 16, 32)
		uint32_t busRead(uint32_t _address, int _size, cf_bus_status& _status);
		void busWrite(uint32_t _address, int _size, uint32_t _value, cf_bus_status& _status);

		// Core bus callbacks (size in bytes: 1, 2, 4)
		static uint32_t onRead(void* user, uint32_t addr, int size, cf_bus_status* status);
		static void onWrite(void* user, uint32_t addr, int size, uint32_t value, cf_bus_status* status);

		TransportHub& transport() noexcept { return m_transport; }

		// Drains transport hub and forwards packets to the USB peripheral
		void pumpTransport() noexcept;

		struct UsbTransportStats
		{
			uint64_t pumps = 0;
			uint64_t drained = 0;
			uint64_t offered = 0;
			uint64_t accepted = 0;
			uint64_t refused = 0;
			uint64_t completed = 0;
			uint64_t undeliverable = 0;
			uint64_t stallReports = 0;
			uint64_t heldAttempts = 0;
			size_t heldOffset = 0;
			size_t heldSize = 0;
			bool held = false;
		};

		UsbTransportStats usbTransport() const noexcept;

		static void onUsbTx(void* user, int endpoint, const uint8_t* data, size_t len);
		static void onUsbIrq(void* user, int asserted);
		static uint16_t onPortARead(void* user);

		InterruptController& interrupts() { return m_interrupts; }
		Flash& flash() { return m_flash; }
		Panel& panel() { return m_panel; }
		const Panel& panel() const { return m_panel; }
		Latches& latches() { return m_latches; }
		Hdi08Adapter& hdi08() { return m_hdi08; }
		Sim& sim() { return m_sim; }
		Uart0& uart0() { return m_uart0; }
		MBus& mbus() { return m_mbus; }
		Max1039& adc() { return m_adc; }
		MemoryMap& memory() { return m_memory; }
		DspSet& dspSet() { return m_dspSet; }
		hwLib::Isp1181& usb() { return m_usb; }
		const hwLib::Isp1181& usb() const { return m_usb; }

	private:
		// BusTarget adapter routing CS0 and CS2 windows to Flash
		class FlashWindow final : public BusTarget
		{
		public:
			FlashWindow(Flash& _flash, const MemoryMap& _map, const Region _region) :
				m_flash(_flash), m_map(_map), m_region(_region)
			{
			}

			uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

		private:
			uint32_t absolute(uint32_t _offset) const;

			Flash& m_flash;
			const MemoryMap& m_map;
			Region m_region;
		};

		// BusTarget routing MBAR accesses to internal peripherals (UART, MBus, ICR, SIM)
		class MbarWindow final : public BusTarget
		{
		public:
			MbarWindow(cf_ctx*& _mcu, MBus& _mbus, InterruptController& _interrupts, Sim& _sim, Uart0& _uart0) :
				m_mcu(_mcu), m_mbus(_mbus), m_interrupts(_interrupts), m_sim(_sim), m_uart0(_uart0)
			{
			}

			uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

			static bool isUartOwned(uint32_t _offset);
			static bool isMbusOwned(uint32_t _offset);
			static bool isInterruptOwned(uint32_t _offset);

		private:
			cf_ctx*& m_mcu;
			MBus& m_mbus;
			InterruptController& m_interrupts;
			Sim& m_sim;
			Uart0& m_uart0;
		};

		// BusTarget routing CS3 window to ISP1181 USB controller
		class Isp1181Window final : public BusTarget
		{
		public:
			explicit Isp1181Window(hwLib::Isp1181& _usb) : m_usb(_usb) {}

			uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

		private:
			hwLib::Isp1181& m_usb;
		};

		static constexpr uint32_t g_simUartStrapOffset = g_simUipcrOffset;

		void attachUnits();

		static void onInterruptAck(void* user, int level, uint8_t vector);
		static void onInterruptPresent(void* user, int level, uint8_t vector, int autovector);

		// Core and interrupt controller declared first for callback readiness during child initialization
		cf_ctx* m_mcu;
		InterruptController m_interrupts;

		MemoryMap m_memory;

		Flash m_flash;
		Panel m_panel;
		Latches m_latches;
		Hdi08Adapter m_hdi08;
		Sim m_sim;
		Uart0 m_uart0;

		Max1039 m_adc;
		MBus m_mbus;

		FlashWindow m_flashCs0;
		FlashWindow m_flashCs2;
		MbarWindow m_mbar;

		hwLib::Isp1181 m_usb;
		Isp1181Window m_usbCs3;

		int m_usbProtocolEndpoint;
		size_t m_usbMaxPacketBytes;
		bool m_usbZeroLengthTerminator;

		// Transport hub buffers sized at construction to avoid allocation during quantum
		TransportHub m_transport;
		std::vector<StampedFrame> m_drained;

		std::vector<uint8_t> m_heldBytes;
		size_t m_heldSize = 0;
		bool m_heldValid = false;
		uint64_t m_heldAttempts = 0;
		size_t m_heldOffset = 0;
		bool m_heldNeedsZlp = false;

		UsbTransportStats m_usbStats;

		uint64_t m_lastFrameIndex = 0;
		bool m_faulted = false;

		// DspSet declared last so destruction uninstalls bridges before m_hdi08 teardown
		DspSet m_dspSet;
	};

	// Concreteness as a compile-time property, so that "nothing derives from
	// it, no virtual, neither copyable nor movable" cannot be silently lost.
	static_assert(!std::is_polymorphic_v<Board>, "Board must be concrete: no virtual method and no vtable");
	static_assert(!std::is_copy_constructible_v<Board>, "Board must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<Board>, "Board must not be copy assignable");
	static_assert(!std::is_move_constructible_v<Board>, "Board must not be move constructible");
	static_assert(!std::is_move_assignable_v<Board>, "Board must not be move assignable");
} // namespace rg2
