// Task BRD-4. UART0, the MCF5307 DUART module on the board side.
//
// Plan section 13.1, BRD-4. Design sections 6.4, 14.5.
// Logbook: AGENTS.md section 2.2.
//
// ---------------------------------------------------------------------------
// PROVENANCE. Plan section 13.1 requires a record of what this task took and
// where each fact was read. This is that record.
//
// NO FILE OF MegabytePhreak/qemu-mcf5307 WAS OPENED FOR THIS TASK, and no
// value below was checked against it. AGENTS.md section 4.2 forbids taking
// expression from that repository; this module is written from the manual
// alone.
//
// THE MANUAL. MCF5307 ColdFire Integrated Microprocessor User's Manual,
// Motorola, 1998. 456 pages, 27,240,768 bytes, sha256
// 86cbcc8c9caa933fe10275a975a78d914df86771df9f0bc22d03de8b1aff91fa. The same
// copy task BRD-2's sim.cpp documents and BRD-3's interruptController.cpp
// cites. Every citation below was opened and read in it.
//
// SOURCE: MCF5307 User's Manual, section 14 "UART Module".
//
//   * The MCF5307 carries two UART modules, each an MC68681-compatible DUART
//     with only channel A implemented: section 14.1.3.
//   * The register address map -- UART0 at MBAR+$1C0, UART1 at MBAR+$200,
//     the four-byte stride and the read/write naming of each address: Table
//     14-1, "UART Module Programming Model", p. 14-17.
//   * The single bus-width rule for the whole UART block: section 14.3.7,
//     "Bus Operation": "All UART module registers must be accessed as
//     bytes."
//   * UMR1: section 14.4.1.1. B/C[1:0] (bits 1:0) is the bits-per-character
//     encoding of Table 14-3; PM[1:0] (bits 4:3) is the parity-mode encoding
//     of Table 14-2. The mode-register pointer (reset to UMR1, advanced to
//     UMR2 after an access to UMR1) is described in the same subsection.
//   * UMR2: section 14.4.1.2. SB[3:0] is the stop-bit-length encoding of
//     Table 14-5; for a 6..8 bit character, SB=0111 selects one stop bit.
//   * USR: section 14.4.1.3. Bit 3 TxEMP, bit 2 TxRDY, bit 1 FFULL, bit 0
//     RxRDY, and the error bits RB, FE, PE, OE.
//   * UCSR: section 14.4.1.4. RCS[3:0] / TCS[3:0]; $DD selects the system
//     bus clock for both.
//   * UCR: section 14.4.1.5. MISC[2:0] (bits 6:4), TC[1:0] (bits 3:2),
//     RC[1:0] (bits 1:0), and Tables 14-8, 14-9, 14-10. The reset-receiver,
//     reset-transmitter, reset-mode-pointer and reset-error-status commands
//     are the ones this model honours.
//   * URB/UTB: sections 14.4.1.6 and 14.4.1.7. The transmitter buffer is
//     write-only and the receiver buffer read-only, at the same +0x0C
//     address.
//   * UIPCR/UACR: sections 14.4.1.8 and 14.4.1.9. UIPCR is read-only at
//     +0x10; on the G2 its bit 0 reads low (the Engine strap, AGENTS.md
//     section 4.1), which is why this model resets it to $0E and not the
//     manual's all-high value.
//   * UISR/UIMR: sections 14.4.1.10 and 14.4.1.11. The UART interrupt output
//     is asserted when a UISR flag is set and its UIMR mask bit is set.
//   * UBG1/UBG2: sections 14.4.1.12 and 14.4.1.13. Both are WRITE ONLY and
//     cannot be read by the CPU. Their concatenation is the baud-rate
//     preload; the minimum value is $0002.
//   * UIVR: section 14.4.1.14. Reset $0F, an uninitialised interrupt
//     condition. The observed G2 value is 0x42 (AGENTS.md section 2.2).
//   * UIP and UOP: sections 14.4.1.15 and 14.4.1.16. CTS state and the
//     address-triggered output commands.
//
// SOURCE: the SIM interrupt-assignment table, Table 8-2, p. 8-5: ICR4 belongs
// to the UART at MBAR+$1C0. BRD-3's controller calls that internal source
// index 4. The manual names the module UART1 (one-indexed); this task and
// AGENTS.md name it UART0. The index is 4 either way.
//
// AGENTS.md CARRY-RULES (section 2.2): UART0 at MBAR+0x1C0, vector 0x42,
// divider 0x36, 8N1; UART1 unused reads reset values; the 54 MHz clock
// derived from the divider is REFUTED and must not return.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#include "uart0.h"

#include <cstddef>

namespace g2
{
	namespace
	{
		// The register offsets within a 0x40-byte UART module block, from
		// Table 14-1.
		constexpr uint32_t kMode = 0x00u;
		constexpr uint32_t kStatusOrClock = 0x04u; // USR read / UCSR write
		constexpr uint32_t kCommand = 0x08u;       // UCR write
		constexpr uint32_t kBuffer = 0x0Cu;        // URB read / UTB write
		constexpr uint32_t kStrapOrAux = 0x10u;    // UIPCR read / UACR write
		constexpr uint32_t kIntStatusOrMask = 0x14u; // UISR read / UIMR write
		constexpr uint32_t kBaudMsb = 0x18u;       // UBG1, write only
		constexpr uint32_t kBaudLsb = 0x1Cu;       // UBG2, write only
		constexpr uint32_t kIntVector = 0x30u;     // UIVR
		constexpr uint32_t kInputPort = 0x34u;     // UIP, read only
		constexpr uint32_t kOutputSet = 0x38u;     // UOP1, write only
		constexpr uint32_t kOutputReset = 0x3Cu;   // UOP0, write only

		constexpr int kRxFifoDepth = 4;

		// MISC[2:0] and the enable/disable encodings of Tables 14-8..14-10.
		constexpr uint8_t kMiscResetModePointer = 0x01u;
		constexpr uint8_t kMiscResetReceiver = 0x02u;
		constexpr uint8_t kMiscResetTransmitter = 0x03u;
		constexpr uint8_t kMiscResetErrorStatus = 0x04u;

		constexpr uint8_t kEnable = 0x01u;
		constexpr uint8_t kDisable = 0x02u;
	}

	Uart0::Uart0(InterruptController* _interrupts)
		: m_interrupts(_interrupts)
	{
		if(m_interrupts)
			m_interrupts->setInternalVector(gUart0InterruptIndex, gUart0Vector);
	}

	Uart0::UartLoc Uart0::locate(const uint32_t _offset) const
	{
		UartLoc loc;
		if(_offset >= gUart0Base && _offset < gUart0Base + gUartModuleSize)
			loc.inModule = true;
		else if(_offset >= gUart1Base && _offset < gUart1Base + gUartModuleSize)
		{
			loc.inModule = true;
			loc.uart1 = true;
		}
		if(loc.inModule)
			loc.local = _offset - (loc.uart1 ? gUart1Base : gUart0Base);
		return loc;
	}

	uint8_t Uart0::usr() const
	{
		uint8_t value = 0;
		if(m_txEnabled && !m_txHoldingValid)
			value |= 0x0Cu;                 // TxEMP (bit 3) and TxRDY (bit 2)
		if(m_rxFifoCount >= kRxFifoDepth)
			value |= 0x02u;                 // FFULL (bit 1)
		if(m_rxFifoCount > 0)
			value |= 0x01u;                 // RxRDY (bit 0)
		return value;
	}

	uint8_t Uart0::readUart0(const uint32_t _local)
	{
		switch(_local)
		{
		case kMode:
		{
			// The mode-register pointer: reset to UMR1; an access to UMR1
			// advances it to UMR2; an access to UMR2 leaves it there. UM
			// section 14.4.1.1.
			uint8_t value = m_modeUmr1 ? m_umr1 : m_umr2;
			m_modeUmr1 = false;
			return value;
		}
		case kStatusOrClock:
			return usr();
		case kBuffer:
		{
			if(m_rxFifoCount > 0)
			{
				const uint8_t byte = m_rxFifo[(m_rxFifoHead - m_rxFifoCount + kRxFifoDepth) % kRxFifoDepth];
				--m_rxFifoCount;
				recomputeInterrupt();
				return byte;
			}
			return 0x00u; // reading an empty receiver returns zero
		}
		case kStrapOrAux:
			// UIPCR. Read-only. On this machine bit 0 reads low (the Engine
			// strap, AGENTS.md section 4.1), so the reset value is $0E and
			// not the manual's all-high figure.
			return 0x0Eu;
		case kIntStatusOrMask:
		{
			uint8_t value = 0;
			if(m_rxFifoCount > 0)
				value |= 0x02u;             // RxRDY duplicate (bit 1)
			if(m_txEnabled && !m_txHoldingValid)
				value |= 0x01u;             // TxRDY duplicate (bit 0)
			return value;
		}
		case kBaudMsb:
		case kBaudLsb:
			// UBG1/UBG2 are WRITE ONLY and cannot be read by the CPU. UM
			// section 14.4.1.13. A read returns zero.
			return 0x00u;
		case kIntVector:
			return m_uivr;
		case kInputPort:
			// UIP bit 0 = CTS, idle high. No modem line is driven in this
			// model, so it reads 1.
			return 0x01u;
		default:
			return 0x00u; // DO NOT ACCESS and untouched gaps
		}
	}

	void Uart0::writeUart0(const uint32_t _local, const uint8_t _value)
	{
		switch(_local)
		{
		case kMode:
		{
			if(m_modeUmr1)
			{
				m_umr1 = _value;
				m_modeUmr1 = false;
			}
			else
				m_umr2 = _value;
			return;
		}
		case kStatusOrClock:
			m_ucsr = _value; // UCSR, write only
			return;
		case kCommand:
		{
			// UCR is a command register: one write performs the commands in
			// its three fields. UM section 14.4.1.5 and Tables 14-8..14-10.
			const uint8_t misc = (_value >> 4) & 0x07u;
			const uint8_t tc   = (_value >> 2) & 0x03u;
			const uint8_t rc   =  _value        & 0x03u;

			switch(misc)
			{
			case kMiscResetModePointer: m_modeUmr1 = true; break;
			case kMiscResetReceiver:
				m_rxEnabled = false;
				m_rxFifoCount = 0;
				m_rxFifoHead = 0;
				break;
			case kMiscResetTransmitter:
				m_txEnabled = false;
				m_txHoldingValid = false;
				break;
			case kMiscResetErrorStatus: break; // no error bits are modelled
			default: break;                      // break-control and no-op
			}

			if(tc == kEnable) m_txEnabled = true;
			else if(tc == kDisable) { m_txEnabled = false; m_txHoldingValid = false; }

			if(rc == kEnable) m_rxEnabled = true;
			else if(rc == kDisable) { m_rxEnabled = false; m_rxFifoCount = 0; }

			recomputeInterrupt();
			return;
		}
		case kBuffer:
		{
			// The transmitter buffer. A character loaded while the
			// transmitter is disabled is NOT transmitted (UM section
			// 14.4.1.3, TxRDY). This model delivers the byte to the MIDI-out
			// consumer only when the transmitter is enabled. The byte is
			// emitted at once because the emulated transmitter has no bit
			// timing; the ShiftDuration belongs to the scheduler.
			m_txHolding = _value;
			m_txHoldingValid = true;
			if(m_txEnabled && m_midiOut)
				m_midiOut(m_midiOutUser, _value);
			recomputeInterrupt();
			return;
		}
		case kStrapOrAux:
			m_uacr = _value; // UACR, write only
			return;
		case kIntStatusOrMask:
			m_uimr = _value; // UIMR, write only
			recomputeInterrupt();
			return;
		case kBaudMsb:
			// UBG1: the upper byte of the baud-rate-generator preload. This
			// is where the observed divider 0x36 is stored (AGENTS.md section
			// 2.2). It is DATA ONLY: nothing here turns it into a frequency.
			m_ubg1 = _value;
			return;
		case kBaudLsb:
			m_ubg2 = _value;
			return;
		case kIntVector:
			m_uivr = _value;
			return;
		case kOutputSet:
		case kOutputReset:
			// UOP1/UOP0 are address-triggered output commands. No MODEM output
			// is driven in this model, so the commands are accepted and do
			// nothing.
			return;
		default:
			logLine("UNMODELLED", true, 8, gUart0Base + _local);
			return;
		}
	}

	uint8_t Uart0::readReset(const uint32_t _local) const
	{
		switch(_local)
		{
		case kIntVector: return 0x0Fu; // UIVR resets to the uninitialised $0F
		case kStrapOrAux: return 0x0Eu; // UIPCR with the Engine strap
		default: return 0x00u;
		}
	}

	uint32_t Uart0::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isByteAccess(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const UartLoc loc = locate(_offset);
		if(!loc.inModule)
		{
			_status = MCF5307_BUS_UNMAPPED;
			logLine("UNMAPPED", false, _size, _offset);
			return 0;
		}

		if(loc.uart1)
			return readReset(loc.local); // UART1 is unused: reset values only

		return readUart0(loc.local);
	}

	void Uart0::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isByteAccess(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const UartLoc loc = locate(_offset);
		if(!loc.inModule)
		{
			_status = MCF5307_BUS_UNMAPPED;
			logLine("UNMAPPED", true, _size, _offset);
			return;
		}

		// UART1 is unused: writes are accepted and have no effect, exactly as
		// a module no G2 signal reaches.
		if(!loc.uart1)
			writeUart0(loc.local, uint8_t(_value & 0xffu));
	}

	void Uart0::setMidiOut(const MidiOutFn _fn, void* _user)
	{
		m_midiOut = _fn;
		m_midiOutUser = _user;
	}

	void Uart0::receive(const uint8_t _byte)
	{
		if(!m_rxEnabled)
			return;

		if(m_rxFifoCount < kRxFifoDepth)
		{
			m_rxFifo[m_rxFifoHead] = _byte;
			m_rxFifoHead = (m_rxFifoHead + 1) % kRxFifoDepth;
			++m_rxFifoCount;
		}
		// A FIFO already full: the new character is lost (overrun), which the
		// T0 model does not track beyond leaving OE at zero.
		recomputeInterrupt();
	}

	void Uart0::transmitComplete()
	{
		m_txHoldingValid = false;
		recomputeInterrupt();
	}

	bool Uart0::interruptAsserted() const
	{
		const uint8_t uisr =
			((m_rxFifoCount > 0) ? 0x02u : 0x00u)
			| ((m_txEnabled && !m_txHoldingValid) ? 0x01u : 0x00u);
		return (uisr & m_uimr) != 0;
	}

	void Uart0::recomputeInterrupt()
	{
		setPending(interruptAsserted());
	}

	void Uart0::setPending(const bool _asserted)
	{
		if(_asserted == m_interruptAsserted)
			return;

		m_interruptAsserted = _asserted;

		if(m_interrupts)
			m_interrupts->setInternalPending(gUart0InterruptIndex, _asserted);
	}

	void Uart0::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
	{
		static const char* digits = "0123456789abcdef";
		std::string hex = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			hex += digits[(_offset >> shift) & 0xfu];
		m_log.push_back(std::string("uart0: ") + _reason
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at MBAR+" + hex);
	}
}
