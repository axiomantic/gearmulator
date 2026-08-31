/* t0_midi_out.cpp -- the check of task PLG-7. Design sections 14.5 and
 * 17 row 7.30.
 *
 * WHAT THE CHECK OWNS: readMidiOut carries ONLY what the machine originated.
 * The source is the emulated UART0 transmit register -- a byte the firmware
 * writes to UTB is delivered through Uart0's MidiOutFn callback to the
 * Device's parser, and readMidiOut drains the completed events. Design
 * section 14.5: "There is no unsolicited SysEx. Nothing is periodic. There is
 * no keepalive. The emulated MIDI port has nothing to volunteer."
 *
 * THE THREE CASE GROUPS:
 *
 *  1. With no machine running -- the state every device is in between
 *     construction and PLG-12's boot -- readMidiOut appends nothing, however
 *     many times it is called. Nothing is periodic, so repeated calls must
 *     not grow the output either.
 *
 *  2. The UART0 model's TX side is driven exactly the way the machine would
 *     drive it: Uart0::setMidiOut installs the Device's own callback
 *     (uart0MidiOut, the very function pointer PLG-12 hands
 *     Board::uart0().setMidiOut), the transmitter is enabled, and bytes are
 *     written to UTB through Uart0's bus interface, as t0_uart0 case group 5
 *     does. readMidiOut must then carry exactly those bytes, as completed
 *     SMidiEvents, in order, with the Device source -- and NOTHING ELSE. A
 *     byte written while the transmitter is disabled is not machine output
 *     (Uart0 withholds it, UM 14.4.1.3 TxRDY), so readMidiOut must not
 *     carry it either.
 *
 *  3. THE NO-KEEPALIVE PROPERTY IS TESTED, NOT STATED. The required-red
 *     mutation plants a keepalive -- one extra event pushed into the parser
 *     before every drain -- and case group 1 turns red: readMidiOut called
 *     repeatedly with no UART traffic must return an empty vector every
 *     time, so any periodic source the implementation volunteers shows up
 *     here and not in production.
 *
 * THE HARNESS reaches readMidiOut through the subclass route the plan's
 * qualification names, because readMidiOut is protected. The same harness
 * exposes the uart0MidiOut sink, so case group 2 installs the real function
 * pointer the boot task will install, not a private copy of it.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() and nothing here depends
 * on NDEBUG.
 */

#include "g2JucePlugin/g2Device.h"

#include "uart0.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

	/* The harness reaches the protected surface: readMidiOut itself, and the
	 * static uart0MidiOut sink whose function pointer Uart0 stores. */
	class MidiHarness final : public g2::Device
	{
	public:
		MidiHarness() : g2::Device(synthLib::DeviceCreateParams{}) {}

		using g2::Device::readMidiOut;
		using g2::Device::uart0MidiOut;
	};

	// The register facts, written out by hand from the MCF5307 UM Table 14-1
	// and the UCR encodings, as t0_uart0 does: the test drives the bus the
	// firmware drives, and does not import uart0.h's answers.
	constexpr uint32_t kUart0Base = 0x1C0u;
	constexpr uint32_t kBuffer    = 0x0Cu;   // URB read / UTB write
	constexpr uint32_t kCommand   = 0x08u;   // UCR, write only

	constexpr uint8_t kCrEnableTransmitter = (0x01u << 2);

	// UM section 14.3.7: every UART register is accessed as a byte, and this
	// model's width unit is BITS, not bytes -- 8 is the byte access.
	constexpr int g_byteWidth = 8;

	mcf5307_bus_status g_status = MCF5307_BUS_OK;

	void writeByte(g2::Uart0& _uart, const uint32_t _off, const uint32_t _val)
	{
		g_status = MCF5307_BUS_OK;
		_uart.write(_off, g_byteWidth, _val, g_status);
	}
}

int main()
{
	/* ------------- Case group 1. NO MACHINE, NOTHING ORIGINATED. A device
	 * between construction and boot (the PLG-1 state) volunteers nothing, on
	 * the first call and on every call after it. */
	{
		MidiHarness device;

		std::vector<synthLib::SMidiEvent> midiOut;
		device.readMidiOut(midiOut);
		check(midiOut.empty(), "a never-booted device's first readMidiOut appends nothing");

		bool grew = false;
		for(int i = 0; i < 32; ++i)
		{
			device.readMidiOut(midiOut);
			if(!midiOut.empty())
			{
				grew = true;
				break;
			}
		}
		check(!grew, "repeated readMidiOut calls with no machine volunteer nothing (nothing is periodic, no keepalive)");
	}

	/* ------------- Case group 2. THE UART0 TRANSMIT REGISTER IS THE SOURCE.
	 * The sink is installed on the UART0 model through setMidiOut, the
	 * exact call PLG-12 performs on Board::uart0(); the bytes enter through
	 * UTB bus writes, the path the firmware takes. */
	{
		MidiHarness device;

		g2::Uart0 uart;
		uart.setMidiOut(&MidiHarness::uart0MidiOut, &device);

		// Enable the transmitter, as the firmware's UCR write does, then
		// originate one complete note-on (3 bytes) and one complete program
		// change (2 bytes).
		writeByte(uart, kUart0Base + kCommand, kCrEnableTransmitter);
		writeByte(uart, kUart0Base + kBuffer, 0x90);
		writeByte(uart, kUart0Base + kBuffer, 0x3C);
		writeByte(uart, kUart0Base + kBuffer, 0x40);
		writeByte(uart, kUart0Base + kBuffer, 0xC5);
		writeByte(uart, kUart0Base + kBuffer, 0x20);

		std::vector<synthLib::SMidiEvent> midiOut;
		device.readMidiOut(midiOut);

		checkEqual(midiOut.size(), size_t(2), "two machine-originated MIDI messages are carried by readMidiOut");
		if(midiOut.size() == 2)
		{
			checkEqual(uint32_t(midiOut[0].a), uint32_t(0x90),
				"the first carried event's status byte is the machine's 0x90, byte for byte");
			checkEqual(uint32_t(midiOut[0].b), uint32_t(0x3C),
				"the note number 0x3C survives the UART path unchanged");
			checkEqual(uint32_t(midiOut[1].a), uint32_t(0xC5),
				"the second carried event's status byte is the machine's 0xC5, in order");
			checkEqual(uint32_t(midiOut[0].source), uint32_t(synthLib::MidiEventSource::Device),
				"a machine-originated event carries MidiEventSource::Device");
			checkEqual(uint32_t(midiOut[1].source), uint32_t(synthLib::MidiEventSource::Device),
				"the second event carries the same source");
		}

		// The drain is real: a second readMidiOut with nothing new carries
		// nothing, so the parser does not replay drained events.
		std::vector<synthLib::SMidiEvent> again;
		device.readMidiOut(again);
		check(again.empty(), "a drained event is not delivered twice");

		// A byte the transmitter never sent is NOT machine-originated. Uart0
		// refuses to deliver it (UM section 14.4.1.3, TxRDY), so readMidiOut
		// must not carry it either.
		MidiHarness silent;
		g2::Uart0 uartDisabled;
		uartDisabled.setMidiOut(&MidiHarness::uart0MidiOut, &silent);
		writeByte(uartDisabled, kUart0Base + kBuffer, 0x90);
		std::vector<synthLib::SMidiEvent> fromDisabled;
		silent.readMidiOut(fromDisabled);
		check(fromDisabled.empty(),
			"a byte written while the transmitter is disabled never reaches readMidiOut");
	}

	/* ------------- Case group 3. NO UNSOLICITED SYSEX. A machine-originated
	 * SysEx dump IS carried (design 14.5: the G2 sends SysEx on the DIN
	 * port), but one the machine did not originate is not: a device that
	 * inserts its own SysEx into the stream shows up here. */
	{
		MidiHarness device;

		std::vector<synthLib::SMidiEvent> midiOut;
		device.readMidiOut(midiOut);
		bool sysexSeen = false;
		for(const auto& ev : midiOut)
		{
			if(!ev.sysex.empty())
				sysexSeen = true;
		}
		check(!sysexSeen, "no unsolicited SysEx arrives when the machine has sent nothing");
	}

	if(g_failures)
	{
		std::cout << "t0_midi_out: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_midi_out: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
