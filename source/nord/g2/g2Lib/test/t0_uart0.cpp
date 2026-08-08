// Task BRD-4. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.1, BRD-4. Design sections 6.4, 14.5.
// Logbook: AGENTS.md section 2.2.
//
// THE CARRY-RULES THIS TEST PINS:
//
//   1. UART0 sits at MBAR+0x1C0 and UART1 (unused) at MBAR+0x200. MCF5307 UM
//      Table 14-1. The manual names them UART1/UART2; AGENTS.md and this
//      task named the 0x1C0 module UART0.
//   2. UART0's vector is 0x42. 0x42 = 66 is in the user-defined range
//      64..255 and is NOT in the autovector range 25..31, so UART0 is a
//      VECTORED source and not an autovectored one. This test drives UART0's
//      interrupt through BRD-3's arbiter and asserts the presented argument
//      is (vector 0x42, autovector 0) when ICR4's AVEC bit is clear.
//   3. The divider 0x36 is observed and it stands; the 54 MHz clock derived
//      from it is refuted and must not return. This model names no clock
//      rate, and the test asserts the observed divider value stands and that
//      the baud registers are DATA (write-only, no frequency built on them).
//   4. UART0 is 8N1: UMR1 = 0x0B (8 data bits, no parity) and UMR2 = 0x07
//      (one stop bit for a 6..8 bit character), UM Tables 14-2, 14-3, 14-5.
//   5. UART1 is unused and reads back its reset values.
//   6. The transmitter buffer is the source for readMidiOut in the Device
//      (design section 14.5): a byte written to UTB is delivered, in order,
//      to the MIDI-out callback.
//
// THE ONE RESTRICTED WIDTH RULE, UM SECTION 14.3.7: "All UART module
// registers must be accessed as bytes." A 16-bit or 32-bit access to any
// UART offset is rejected with MCF5307_BUS_SIZE_ILLEGAL and one log line, on
// BOTH the read and the write path.
//
// THE REGISTER FACTS ARE WRITTEN OUT AGAIN BY HAND. The offsets come from
// MCF5307 UM Table 14-1 and the reset values from the register descriptions.
// A test that imported uart0.h's own constants would answer that the model
// equals itself; this test names the manual's values so the two sides move
// independently.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG.

#include "uart0.h"
#include "interruptController.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	// -----------------------------------------------------------------------
	// THE HAND-WRITTEN FACTS FROM THE MANUAL. Offsets are MBAR-relative and
	// come from Table 14-1; the UART block stride is four bytes and each
	// register is one byte.
	constexpr uint32_t gUart0Base = 0x1C0u;
	constexpr uint32_t gUart1Base = 0x200u;
	constexpr uint32_t gMode       = 0x00u; // UMR1/UMR2
	constexpr uint32_t gCommand    = 0x08u; // UCR, write only
	constexpr uint32_t gBuffer     = 0x0Cu; // URB read / UTB write
	constexpr uint32_t gIntVec     = 0x30u; // UIVR
	constexpr uint32_t gBaudMsb    = 0x18u; // UBG1, write only
	constexpr uint32_t gBaudLsb    = 0x1Cu; // UBG2, write only

	// The 8N1 configuration, UM Tables 14-2, 14-3, 14-5.
	constexpr uint8_t gUmr18n1 = 0x0Bu; // B/C=11 (8 bits), PM=10 (no parity)
	constexpr uint8_t gUmr28n1 = 0x07u; // SB=0111 (one stop bit, 6..8 bits)

	// UCR command encodings, Tables 14-8..14-10.
	constexpr uint8_t gCrEnableReceiver    = 0x01u;
	constexpr uint8_t gCrEnableTransmitter = (0x01u << 2);
	constexpr uint8_t gCrResetModePointer  = (0x01u << 4);

	// UIMR mask bit for the RxRDY interrupt, UM section 14.4.1.11.
	constexpr uint8_t gImrRxRdy = 0x02u;
	constexpr uint8_t gImrTxRdy = 0x01u;

	mcf5307_bus_status g_status = MCF5307_BUS_OK;

	uint32_t rd(g2::Uart0& u, const uint32_t _off)
	{
		g_status = MCF5307_BUS_OK;
		return u.read(_off, 8, g_status);
	}

	void wr(g2::Uart0& u, const uint32_t _off, const uint32_t _val)
	{
		g_status = MCF5307_BUS_OK;
		u.write(_off, 8, _val, g_status);
	}

	uint8_t writeMode(g2::Uart0& u, const uint8_t _b1, const uint8_t _b2)
	{
		// Program UMR1 then UMR2 through the mode-register pointer: the
		// first access at +0x00 hits UMR1 and advances the pointer; the
		// second hits UMR2.
		wr(u, gUart0Base + gMode, _b1);
		wr(u, gUart0Base + gMode, _b2);
		return rd(u, gUart0Base + gMode); // reads UMR2, leaves pointer on UMR2
	}

	// The receive interrupt recorder (the BRD-3 present callback).
	struct PresentRecorder
	{
		int level = -999;
		uint8_t vector = 0;
		int autovector = -999;
	};

	void recordPresent(void* _user, const int _level, const uint8_t _vector, const int _autovector)
	{
		auto* recorder = static_cast<PresentRecorder*>(_user);
		recorder->level = _level;
		recorder->vector = _vector;
		recorder->autovector = _autovector;
	}

	constexpr uint32_t gIcrBase = 0x04Cu;

	// An ICR byte: AVEC at bit 7, level at bits 4:2 (IL[2:0]), IP at bits 1:0,
	// from BRD-3 / UM Table 8-2.
	uint8_t makeIcr(const int _level, const bool _avec)
	{
		uint8_t value = uint8_t((_level << 2) & 0x7cu);
		if(_avec)
			value |= 0x80u;
		return value;
	}

	// The MIDI-out consumer (the readMidiOut source).
	struct MidiRecorder
	{
		uint8_t bytes[16];
		int count = 0;
	};

	void recordMidi(void* _user, const uint8_t _byte)
	{
		auto* rec = static_cast<MidiRecorder*>(_user);
		if(rec->count < 16)
			rec->bytes[rec->count] = _byte;
		++rec->count;
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. THE BASES AND THE VECTOR NUMBER, AS FACTS THE LATER BOARD
	// TASK DEPENDS ON. Vector 0x42 = 66 is in the user-defined range 64..255,
	// and the autovectors are 25..31, so the UART is a vectored source and
	// not an autovectored one.
	{
		checkEqual(uint32_t(g2::Uart0::gUart0Base), gUart0Base,
			"UART0 sits at MBAR+0x1C0");
		checkEqual(uint32_t(g2::Uart0::gUart1Base), gUart1Base,
			"UART1 sits at MBAR+0x200");
		checkEqual(uint32_t(g2::Uart0::gUart0Vector), 0x42u,
			"the observed UART0 vector is 0x42");
		check(uint32_t(g2::Uart0::gUart0Vector) >= 64
			&& uint32_t(g2::Uart0::gUart0Vector) <= 255,
			"vector 0x42 is in the user-defined range 64..255");
		check(uint32_t(g2::Uart0::gUart0Vector) < 25
			|| uint32_t(g2::Uart0::gUart0Vector) > 31,
			"vector 0x42 is not an autovector (those are vectors 25..31), so UART0 is a vectored source");
		checkEqual(int(g2::Uart0::gUart0InterruptIndex), 4,
			"UART0 is internal interrupt source index 4 (ICR4, UM Table 8-2)");
	}

	// -----------------------------------------------------------------------
	// Case group 2. THE OBSERVED DIVIDER STANDS AS DATA, AND THE REFUTED
	// 54 MHz CLOCK DOES NOT EXIST IN THE MODEL. gBaudDivider is a compile-time
	// pinned value and the model builds no frequency from it.
	{
		static_assert(g2::Uart0::gBaudDivider == 0x0036u,
			"the observed UART0 baud divider is 0x36 and must stay 0x36");
		checkEqual(uint32_t(g2::Uart0::gBaudDivider), 0x0036u,
			"the observed UART0 divider 0x36 stands");

		// The baud registers are DATA. They are WRITE ONLY: a read returns
		// zero (UM section 14.4.1.13) and a write stores what the firmware
		// programmed. The divider's high byte is stored through UBG1.
		g2::Uart0 uart;
		wr(uart, gUart0Base + gBaudMsb, 0x00);
		wr(uart, gUart0Base + gBaudLsb, 0x36);
		checkEqual(rd(uart, gUart0Base + gBaudMsb), uint32_t(0x00),
			"UBG1 is write-only: a read returns zero");
		checkEqual(rd(uart, gUart0Base + gBaudLsb), uint32_t(0x00),
			"UBG2 is write-only: a read returns zero");
		checkEqual(uint32_t(g2::Uart0::gBaudDivider), 0x0036u,
			"the observed divider 0x36 is the ONLY number built on the baud data; no 54 MHz clock rate exists in the model");
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE ONE RESTRICTED WIDTH RULE (UM SECTION 14.3.7): every
	// UART register is a byte, and a 16-bit or 32-bit access is rejected with
	// MCF5307_BUS_SIZE_ILLEGAL and one log line, on both paths.
	{
		g2::Uart0 uart;
		const uint32_t logAt = gUart0Base + gBuffer;

		mcf5307_bus_status st = MCF5307_BUS_OK;
		uart.read(logAt, 16, st);
		check(st == MCF5307_BUS_SIZE_ILLEGAL,
			"a 16-bit read of a UART register is rejected with SIZE_ILLEGAL");
		st = MCF5307_BUS_OK;
		uart.read(logAt, 32, st);
		check(st == MCF5307_BUS_SIZE_ILLEGAL,
			"a 32-bit read of a UART register is rejected with SIZE_ILLEGAL");
		check(!uart.log().empty(), "a rejected width writes one log line");

		uart.clearLog();
		st = MCF5307_BUS_OK;
		uart.write(logAt, 16, 0x1234u, st);
		check(st == MCF5307_BUS_SIZE_ILLEGAL,
			"a 16-bit write to a UART register is rejected with SIZE_ILLEGAL");
		check(!uart.log().empty(), "a rejected write width writes one log line");
	}

	// -----------------------------------------------------------------------
	// Case group 4. THE 8N1 MODE IS STORED AND READ BACK THROUGH THE MODE
	// POINTER. UMR1 = 0x0B is 8 data bits, no parity; UMR2 = 0x07 is one stop
	// bit for a 6..8 bit character.
	{
		g2::Uart0 uart;
		// Reset the mode pointer through the UCR command register, then
		// program UMR1 and UMR2 in order.
		wr(uart, gUart0Base + gCommand, gCrResetModePointer);
		writeMode(uart, gUmr18n1, gUmr28n1);
		checkEqual(rd(uart, gUart0Base + gMode), uint32_t(gUmr28n1),
			"after programming the two mode registers, reading +0x00 returns UMR2 (8N1 stop bits)");
		// Reset the pointer again and verify UMR1 reads back.
		wr(uart, gUart0Base + gCommand, gCrResetModePointer);
		checkEqual(rd(uart, gUart0Base + gMode), uint32_t(gUmr18n1),
			"after a reset-mode-pointer command, reading +0x00 returns UMR1 (8N1 data/parity)");
	}

	// -----------------------------------------------------------------------
	// Case group 5. THE TRANSMITTER BUFFER IS THE SOURCE FOR readMidiOut. A
	// byte written to UTB while the transmitter is enabled is delivered, in
	// order, to the MIDI-out callback; TxRDY clears while the holding register
	// is loaded and re-asserts when the transmitter completes.
	{
		g2::Uart0 uart;
		MidiRecorder midi;
		uart.setMidiOut(recordMidi, &midi);

		// Enable the transmitter. TxRDY is now set.
		wr(uart, gUart0Base + gCommand, gCrEnableTransmitter | gCrResetModePointer);
		check((uart.usr() & 0x04u) != 0, "with the transmitter enabled, USR TxRDY (bit 2) is set");

		// Write two bytes to UTB. Both are delivered, in order, to the
		// MIDI-out consumer.
		wr(uart, gUart0Base + gBuffer, 0x90);
		wr(uart, gUart0Base + gBuffer, 0x3C);
		checkEqual(midi.count, 2, "two bytes written to UTB are delivered to the MIDI-out consumer");
		checkEqual(midi.bytes[0], uint8_t(0x90), "the first transmitted byte is delivered unchanged");
		checkEqual(midi.bytes[1], uint8_t(0x3C), "the second transmitted byte is delivered unchanged, in order");
		check((uart.usr() & 0x04u) == 0, "after a byte is written, TxRDY clears while the holding register is loaded");

		// Complete the transmission: the holding register empties and TxRDY
		// re-asserts.
		uart.transmitComplete();
		check((uart.usr() & 0x04u) != 0, "after transmitComplete, TxRDY re-asserts");
		check((uart.usr() & 0x08u) != 0, "after transmitComplete, USR TxEMP (bit 3) is set");
	}

	// -----------------------------------------------------------------------
	// Case group 6. THE RECEIVE PATH (MIDI IN): a byte injected while the
	// receiver is enabled lands in the FIFO, sets RxRDY, and is read back from
	// the receiver buffer.
	{
		g2::Uart0 uart;
		wr(uart, gUart0Base + gCommand, gCrEnableReceiver);
		check((uart.usr() & 0x01u) == 0, "before any receive, USR RxRDY (bit 0) is clear");

		uart.receive(0x7Eu);
		check((uart.usr() & 0x01u) != 0, "after an injected byte, USR RxRDY (bit 0) is set");

		checkEqual(rd(uart, gUart0Base + gBuffer), uint32_t(0x7E),
			"reading the receiver buffer returns the injected byte");
		check((uart.usr() & 0x01u) == 0, "reading the last byte in the FIFO clears RxRDY");
	}

	// -----------------------------------------------------------------------
	// Case group 7. THE VECTORED SOURCE, END TO END THROUGH BRD-3'S ARBITER.
	// When UART0 asserts its interrupt and ICR4's AVEC bit is clear, the board
	// presents (vector 0x42, autovector 0) at ICR4's programmed level. This is
	// the property that makes UART0 a vectored source.
	{
		PresentRecorder recorder;
		g2::InterruptController ic(&recorder, recordPresent);
		g2::Uart0 uart(&ic);

		// Program ICR4 for UART0 at level 5, with AVEC clear (vectored).
		ic.writeRegister(gIcrBase + 4, makeIcr(5, false));

		// Enable the receiver and mask the RxRDY interrupt.
		wr(uart, gUart0Base + gCommand, gCrEnableReceiver);
		wr(uart, gUart0Base + 0x14, gImrRxRdy);

		// Inject a byte: the interrupt condition becomes active.
		uart.receive(0x22);
		check(uart.interruptAsserted(), "a received byte with the RxRDY mask set asserts the UART interrupt");
		checkEqual(recorder.level, 5, "the presented interrupt level is ICR4's programmed level 5");
		checkEqual(recorder.vector, uint8_t(0x42), "the presented vector is 0x42, so UART0 is a vectored source");
		checkEqual(recorder.autovector, 0, "with ICR4's AVEC bit clear, the presented autovector is 0");

		// Reading the byte drops the condition and deasserts.
		checkEqual(rd(uart, gUart0Base + gBuffer), uint32_t(0x22),
			"the received byte is read from the receiver buffer");
		check(!uart.interruptAsserted(), "after the FIFO drains, the UART interrupt deasserts");
		checkEqual(recorder.level, 0, "after deassert, the arbiter presents no pending source");
	}

	// -----------------------------------------------------------------------
	// Case group 8. UART1 IS UNUSED AND READS BACK ITS RESET VALUES. The
	// register-file reset values are UIVR = $0F (uninitialised interrupt
	// condition) and the general registers zero. Writes have no effect.
	{
		g2::Uart0 uart;
		checkEqual(rd(uart, gUart1Base + gIntVec), uint32_t(0x0F),
			"UART1 is unused: UIVR reads its reset value $0F");
		checkEqual(rd(uart, gUart1Base + gBuffer), uint32_t(0x00),
			"UART1 is unused: the receiver buffer reads its reset value 0");
		checkEqual(rd(uart, gUart1Base + gMode), uint32_t(0x00),
			"UART1 is unused: the mode register reads its reset value 0");

		// A write to UART1 has no effect on the reset read-back.
		wr(uart, gUart1Base + gIntVec, 0x42);
		wr(uart, gUart1Base + gBuffer, 0x90);
		checkEqual(rd(uart, gUart1Base + gIntVec), uint32_t(0x0F),
			"a write to UART1's UIVR does not change the reset read-back (unused)");
		checkEqual(rd(uart, gUart1Base + gBuffer), uint32_t(0x00),
			"a write to UART1's transmitter buffer has no effect (unused)");
	}

	// -----------------------------------------------------------------------
	// Case group 9. THE UART0 MODULE GROUNDS ITS OWN ACCESS.
	//
	// An access completely outside the two UART blocks is refused as
	// UNMAPPED rather than silently answered, so that a firmware fault that
	// addresses the wrong MBAR offset is loud rather than hidden. MBAR+0x240
	// is just past UART1's block (0x200..0x23F) and belongs to no UART module.
	// (An offset INSIDE a module block, including its DO NOT ACCESS gaps, is
	// a benign reset-value read, not a fault.)
	{
		g2::Uart0 uart;
		mcf5307_bus_status st = MCF5307_BUS_OK;
		uart.read(0x240, 8, st);
		check(st == MCF5307_BUS_UNMAPPED,
			"an access past the UART blocks is refused as UNMAPPED");
		st = MCF5307_BUS_OK;
		uart.write(0x240, 8, 0x00, st);
		check(st == MCF5307_BUS_UNMAPPED,
			"a write past the UART blocks is refused as UNMAPPED");
	}

	if(g_failures)
	{
		std::cout << "t0_uart0: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_uart0: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
