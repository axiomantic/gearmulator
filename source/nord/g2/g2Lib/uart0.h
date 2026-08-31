// UART0, the MCF5307 DUART module on the board side.
//
// The MCF5307 carries two UART modules, and each one is a Motorola
// MC68681-compatible DUART with only channel A implemented (MCF5307 UM section
// 14.1.3). UART0 sits at MBAR+0x1C0 and UART1 at MBAR+0x200.
//
//   * UART0 base MBAR+0x1C0, vector 0x42, divider 0x36, 8N1.
//   * Vector 0x42 is 66, which lands in the user-defined range 64..255, so
//     UART0 is a VECTORED source and not an autovectored one. The autovectors
//     are vectors 25 to 31, and 66 is nowhere near them.
//   * UART1 is unused on the G2 and reads back its reset values.
//   * The transmitter buffer is the source for readMidiOut in the Device
//     subclass: a byte the firmware writes to UTB leaves the machine on the
//     MIDI-out callback.
//   * The divider 0x36 is a baud-rate-generator preload value and nothing
//     more. This file names no clock-rate quantity and derives no clock from
//     it, because the MCF5307 has two clock domains that can never be equal
//     and no rate is established.
//
// UM section 14.3.7 states one rule for the whole UART block: all UART module
// registers must be accessed as bytes. A 16-bit or 32-bit access to any UART
// offset is rejected with MCF5307_BUS_SIZE_ILLEGAL and one log line.
//
// REGISTER MAP, MCF5307 um table 14-1. Offsets are relative to each UART
// base (0x1C0 / 0x200). The modules are MC68681-compatible, so each register
// is one byte and the stride between them is four:
//
//     +0x00  MODE  (UMR1, UMR2)              read/write, mode-register pointer
//     +0x04  USR read / UCSR write           status / clock-select
//     +0x08  --    read / UCR write          command register
//     +0x0C  URB  read / UTB write           receiver / transmitter buffer
//     +0x10  UIPCR read / UACR write         input-port change / aux control
//     +0x14  UISR read / UIMR write          interrupt status / interrupt mask
//     +0x18  UBG1 (write only)               baud-rate prescale MSB
//     +0x1C  UBG2 (write only)               baud-rate prescale LSB
//     +0x30  UIVR (read/write, reset $0F)    interrupt vector register
//     +0x34  UIP  read / -- write            input port
//     +0x38  -- read / UOP1 write            output port bit-set
//     +0x3C  -- read / UOP0 write            output port bit-reset
//
// Everything between those rows is "do not access" -- factory-test space
// whose reads produce undesired effects. This model returns the benign reset
// value for the reads.
//
// UART0's interrupt is an internal source of the SIM's two-tier controller:
// UM Table 8-2 assigns ICR4 to the UART at MBAR+0x1C0. When a maskable
// interrupt condition becomes active (the interrupt status register AND-ed
// with the interrupt mask is non-zero) this model asserts internal source
// index 4 on the connected controller, and deasserts when the condition
// clears. The autovector/vector selection that follows ICR4's AVEC bit is the
// arbiter's business; this model supplies the vector 0x42.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "interruptController.h"
#include "memoryMap.h"

namespace g2
{
	class Uart0 final : public BusTarget
	{
	public:
		// Offsets are MBAR-relative; the decode produces them.
		//
		// UART0 at MBAR+0x1C0, UART1 (unused) at MBAR+0x200. MCF5307 um
		// table 14-1. The manual names them UART1 and UART2 (one-indexed);
		// this model names the 0x1C0 module UART0 and the unused 0x200 module
		// UART1. The internal interrupt source is index 4 either way
		// (UM Table 8-2, ICR4).
		static constexpr uint32_t gUart0Base = 0x1C0u;
		static constexpr uint32_t gUart1Base = 0x200u;
		static constexpr uint32_t gUartModuleSize = 0x40u;

		// The observed UART0 interrupt vector. 0x42 = 66 is in the
		// user-defined range 64..255 and is NOT an autovector (those are
		// vectors 25..31), so UART0 is a vectored source.
		static constexpr uint8_t gUart0Vector = 0x42u;
		static constexpr int gUart0InterruptIndex = 4;   // ICR4, UM Table 8-2

		// The observed baud-rate-generator divider, 0x36. It is data: stored as
		// the firmware programmed it, and not a clock rate. Nothing here
		// derives a frequency from it.
		static constexpr uint16_t gBaudDivider = 0x0036u;

		// The 8N1 mode the firmware programs: 8 data bits, no parity, one
		// stop bit, as UMR1 = 0x0B (B/C=11, PM=10) and UMR2 = 0x07
		// (SB=0111, one stop bit for a 6..8 bit character). UM Tables 14-3,
		// 14-5 and 14-2.
		static constexpr uint8_t gUmr18n1 = 0x0Bu;
		static constexpr uint8_t gUmr28n1 = 0x07u;

		// The MIDI-out callback -- the emulated UART0 transmitter buffer is
		// the source for readMidiOut in the Device subclass. A byte the
		// firmware writes to UTB at +0x0C is delivered here, in order.
		using MidiOutFn = void (*)(void* _user, uint8_t _byte);

		// _interrupts is the interrupt controller to assert on, or nullptr for
		// a standalone model that records its own interrupt condition.
		Uart0(InterruptController* _interrupts = nullptr);

		// The single restricted width rule of UM section 14.3.7: every UART
		// register is a byte. A 16-bit or 32-bit access is rejected with
		// MCF5307_BUS_SIZE_ILLEGAL and one log line.
		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// The MIDI-out consumer (readMidiOut's source). A byte is delivered
		// when the firmware writes the transmitter buffer.
		void setMidiOut(MidiOutFn _fn, void* _user);

		// The receive side (MIDI in). _byte lands in the receiver FIFO, sets
		// RxRDY, and re-evaluates the interrupt condition. The scheduler or
		// the Device feeds this on the emulated receive path.
		void receive(uint8_t _byte);

		// Advance the transmitter: the holding register transfers to the
		// shift register and empties, so TxRDY and TxEMP re-assert. Called by
		// the scheduler when emulated time advances the transmitter.
		void transmitComplete();

		// The current USR (status register) byte, computed from state.
		uint8_t usr() const;
		// True when the UART's interrupt output is asserted: the interrupt
		// status AND the mask is non-zero. Independent of whether a
		// controller is attached, so a standalone test can read it.
		bool interruptAsserted() const;

		// The internal interrupt source index asserted on the controller
		// (ICR4), and the vector 0x42 the firmware programs into UIVR.
		uint8_t uivr() const { return m_uivr; }

		// One line for every rejected access, in the shape the board track
		// uses elsewhere.
		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		struct UartLoc
		{
			bool inModule = false;   // a byte inside a UART module block
			bool uart1 = false;      // true for the unused UART1 block
			uint32_t local = 0;      // offset within the 0x40-byte block
		};

		UartLoc locate(uint32_t _offset) const;
		bool isByteAccess(int _size) const { return _size == 8; }
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		uint8_t readUart0(uint32_t _local);
		void writeUart0(uint32_t _local, uint8_t _value);
		uint8_t readReset(uint32_t _local) const;

		void recomputeInterrupt();
		void setPending(bool _asserted);

		// The receiver FIFO. The DUART is quadruple-buffered.
		uint8_t m_rxFifo[4];
		int m_rxFifoCount = 0;
		int m_rxFifoHead = 0;

		// Transmitter state: one holding register over a shift register.
		bool m_txEnabled = false;
		bool m_txHoldingValid = false;
		uint8_t m_txHolding = 0;

		// Receiver state.
		bool m_rxEnabled = false;

		// The register file. Reset values follow Table 14-1 and the register
		// descriptions: UIVR resets to $0F (uninitialized interrupt condition,
		// UM 14.4.1.14) and UIPCR resets with CTS high at $0E (bit 0 low on
		// the G2 strap).
		uint8_t m_umr1 = 0x00u;
		uint8_t m_umr2 = 0x00u;
		bool m_modeUmr1 = true;   // the mode-register pointer, reset to UMR1
		uint8_t m_ucsr = 0x00u;
		uint8_t m_uacr = 0x00u;
		uint8_t m_uimr = 0x00u;
		uint8_t m_ubg1 = 0x00u;
		uint8_t m_ubg2 = 0x00u;
		uint8_t m_uivr = 0x0Fu;

		bool m_interruptAsserted = false;

		InterruptController* m_interrupts;
		MidiOutFn m_midiOut = nullptr;
		void* m_midiOutUser = nullptr;

		std::vector<std::string> m_log;
	};
}
