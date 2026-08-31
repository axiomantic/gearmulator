// Drives the eight expanded addresses through the decode and asserts the
// selected port set for each, then exercises the two paths a firmware word can
// arrive by -- a 32-bit store at register offset 4, and a byte-at-a-time
// TXH/TXM/TXL sequence -- and asserts the same 24-bit word comes out either
// way. It also drives the broadcast and asserts the word arrives at every
// populated port.
//
// The addresses below are the expected input of a decode, not a table the
// emulator carries: they are the values `set_hdi08_bases(expanded)` writes at
// boot, and hdi08Adapter.h and hdi08Decode.h hold none of them. This test
// reads them only to compute CS1-relative offsets for the adapter.

#include "hdi08Adapter.h"
#include "hdi08Decode.h"
#include "memoryMap.h"

#include <array>
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

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <0x" << std::hex << _expected
			<< ">, got <0x" << _actual << ">" << std::dec << std::endl;
		++g_failures;
	}

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	// The expanded address table. Each per-DSP address drives one select low;
	// `ports` is the bit that is set, which is the single port that must
	// receive a word addressed there.
	struct RecordedEntry
	{
		int index;
		uint32_t address;
		uint8_t expectedPort;
		const char* note;
	};

	const RecordedEntry g_expandedTable[] =
	{
		{0, 0x110007b8u, 3, "index 0 drives A6 low and selects port 3"},
		{1, 0x110003f8u, 7, "index 1 drives A10 low and selects port 7"},
		{2, 0x110005f8u, 6, "index 2 drives A9 low and selects port 6"},
		{3, 0x110006f8u, 5, "index 3 drives A8 low and selects port 5"},
		{4, 0x11000778u, 4, "index 4 drives A7 low and selects port 4"},
		{5, 0x110007d8u, 2, "index 5 drives A5 low and selects port 2"},
		{6, 0x110007e8u, 1, "index 6 drives A4 low and selects port 1"},
		{7, 0x110007f0u, 0, "index 7 drives A3 low and selects port 0"},
	};

	// One captured transmit word per port, so the test can say exactly which
	// port received a word and what the word was.
	struct Capture
	{
		uint32_t word = 0;
		int count = 0;
	};

	std::array<Capture, g2::g_hdi08PortCount> captureAll(g2::Hdi08Adapter& _adapter)
	{
		std::array<Capture, g2::g_hdi08PortCount> captures;
		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			_adapter.port(p).setWriteTxCallback(
				[&captures, p](const uint32_t _word)
				{
					captures[p].word = _word;
					++captures[p].count;
				});
		}
		return captures;
	}
}

int main()
{
	// The expanded machine.
	const g2::Hdi08Decode decode(g2::g_hdi08ExpandedPorts);
	g2::Hdi08Adapter adapter(decode);

	// -----------------------------------------------------------------------
	// Case group 0. The eight per-DSP addresses, by longword.
	//
	// A 32-bit store at register offset 4 pushes one 24-bit word to the single
	// port the address selects. The word's low 24 bits must arrive there and
	// nowhere else.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures = captureAll(adapter);

		// One distinct word per port, so a word landing on the wrong port is
		// visible in both the count and the value.
		const uint32_t baseWord = 0x11 << 20; // 0x110000 + port nibble in the top.

		for(const RecordedEntry& e : g_expandedTable)
		{
			for(Capture& c : captures) { c.word = 0; c.count = 0; }

			const uint32_t word = baseWord | (uint32_t(e.index) << 12); // distinct per row.
			const uint32_t offset = (e.address - g2::g_cs1Base) + 4u;   // register offset 4.

			mcf5307_bus_status status = MCF5307_BUS_OK;
			adapter.write(offset, 32, word, status);

			checkEqual(status, uint32_t(MCF5307_BUS_OK),
				std::string("longword at ") + hex32(e.address) + "+4 completes OK: " + e.note);

			checkEqual(uint32_t(captures[e.expectedPort].count), uint32_t(1),
				std::string("longword at ") + hex32(e.address) + " reaches port "
					+ std::to_string(e.expectedPort) + " once");
			checkEqual(captures[e.expectedPort].word, word & 0x00ffffffu,
				std::string("the word pushed at ") + hex32(e.address)
					+ " is the low 24 bits of the longword");

			for(int p = 0; p < g2::g_hdi08PortCount; ++p)
			{
				if(p == e.expectedPort)
					continue;
				checkEqual(uint32_t(captures[p].count), uint32_t(0),
					std::string("longword at ") + hex32(e.address)
						+ " reaches no other port (port " + std::to_string(p) + " stays silent)");
			}
		}
	}

	// -----------------------------------------------------------------------
	// Case group 1. THE BROADCAST.
	//
	// An offset of zero drives every populated select low. A longword at
	// register offset 4 pushes the same word to every port.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures = captureAll(adapter);
		for(Capture& c : captures) { c.word = 0; c.count = 0; }

		const uint32_t word = 0x0055aaffu;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		// CS1 offset zero is the broadcast; +4 is the longword register offset.
		adapter.write(0x4u, 32, word, status);

		checkEqual(status, uint32_t(MCF5307_BUS_OK), "the broadcast longword completes OK");

		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			checkEqual(uint32_t(captures[p].count), uint32_t(1),
				std::string("the broadcast reaches port ") + std::to_string(p) + " once");
			checkEqual(captures[p].word, word,
				std::string("the broadcast word at port ") + std::to_string(p)
					+ " is the pushed word");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 2. The byte-at-a-time path.
	//
	// The adapter must not assume the CPU issued a longword. Writing TXH, TXM
	// and TXL one byte at a time must assemble the same word the longword
	// store did, and the word must complete only when TXL is written.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures = captureAll(adapter);

		// Port 3, the address 0x110007b8 (register offset 0).
		const uint32_t portBase = 0x110007b8u;
		const int portIndex = 3;
		const uint32_t word = 0x00abcdefu;

		// TXH first: no word yet.
		mcf5307_bus_status status = MCF5307_BUS_OK;
		adapter.write((portBase - g2::g_cs1Base) + 5u, 8, (word >> 16) & 0xffu, status);
		checkEqual(uint32_t(captures[portIndex].count), uint32_t(0),
			"writing TXH alone does not complete a word");

		// TXM: still no word.
		adapter.write((portBase - g2::g_cs1Base) + 6u, 8, (word >> 8) & 0xffu, status);
		checkEqual(uint32_t(captures[portIndex].count), uint32_t(0),
			"writing TXH and TXM still does not complete a word");

		// TXL: the word completes now, and it is the same word the longword
		// path pushed.
		adapter.write((portBase - g2::g_cs1Base) + 7u, 8, word & 0xffu, status);
		checkEqual(uint32_t(captures[portIndex].count), uint32_t(1),
			"the word completes when TXL is written");
		checkEqual(captures[portIndex].word, word,
			"the byte-at-a-time path assembles the same word as the longword path");
		checkEqual(status, uint32_t(MCF5307_BUS_OK),
			"each byte of the byte-at-a-time path completes OK");
		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			if(p == portIndex) continue;
			checkEqual(uint32_t(captures[p].count), uint32_t(0),
				std::string("byte-at-a-time path reaches no other port (port ")
					+ std::to_string(p) + " stays silent)");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE 16-BIT PATH.
	//
	// A 16-bit store at register offset 6 writes TXM then TXL, so the word
	// completes with the low 16 bits of the value and a zero high byte. This
	// is the same "word completes on TXL" rule seen from a third width.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures = captureAll(adapter);
		for(Capture& c : captures) { c.word = 0; c.count = 0; }

		const uint32_t portBase = 0x110007f0u; // port 0.
		const int portIndex = 0;
		const uint32_t half = 0x3f2au;

		mcf5307_bus_status status = MCF5307_BUS_OK;
		adapter.write((portBase - g2::g_cs1Base) + 6u, 16, half, status);

		checkEqual(uint32_t(captures[portIndex].count), uint32_t(1),
			"a 16-bit store at register offset 6 completes a word on TXL");
		checkEqual(captures[portIndex].word, half,
			"the 16-bit store pushes the low 16 bits of the value");
	}

	// -----------------------------------------------------------------------
	// Case group 4. The adapter rides the bus.
	//
	// The adapter presents the BusTarget the MemoryMap attaches to CS1, so a
	// write through the map lands on the selected port just as a direct write
	// did.
	{
		g2::MemoryMapConfig config;
		config.cs1 = {g2::g_cs1Base, 0x800u};

		g2::MemoryMap map(config);
		map.attach(g2::Region::Cs1, &adapter);

		std::array<Capture, g2::g_hdi08PortCount> captures = captureAll(adapter);
		for(Capture& c : captures) { c.word = 0; c.count = 0; }

		const uint32_t word = 0x00bead00u;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		// 0x110007F0 = port 0 (register offset 0); +4 is the longword offset.
		map.write(0x110007f4u, 32, word, status);

		checkEqual(status, uint32_t(MCF5307_BUS_OK), "a write through the MemoryMap completes OK");
		check(map.decode(0x110007f4u) == g2::Region::Cs1, "0x110007F4 decodes to CS1");
		checkEqual(uint32_t(captures[0].count), uint32_t(1),
			"a write through the MemoryMap reaches the decoded port");
		checkEqual(captures[0].word, word,
			"a write through the MemoryMap pushes the word");
	}

	if(g_failures)
	{
		std::cout << "t0_hdi08_adapter: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_hdi08_adapter: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
