// Drives the expanded per-DSP addresses through the decode, the two paths a
// firmware word can arrive by -- a 32-bit store at register offset 4, and a
// byte-at-a-time TXH/TXM/TXL sequence -- and the broadcast.
//
// The addresses below are the expected input of a decode, not a table the
// emulator carries: they are the values `set_hdi08_bases(expanded)` writes at
// boot. This test reads them only to compute CS1-relative offsets for the
// adapter.

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

	// The array is the caller's, and it is an out-parameter rather than a
	// return value on purpose. The per-port lambdas outlive this call and hold a
	// reference to whatever they were given. Returning a local by value would
	// bind them to storage this function owns, which survives the return only
	// while a compiler chooses NRVO -- an optimisation it is permitted to
	// decline. Taking the caller's own object makes the lifetime a guarantee of
	// the language instead.
	void installCaptures(g2::Hdi08Adapter& _adapter, std::array<Capture, g2::g_hdi08PortCount>& _captures)
	{
		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			_adapter.port(p).setWriteTxCallback(
				[&_captures, p](const uint32_t _word)
				{
					_captures[p].word = _word;
					++_captures[p].count;
				});
		}
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
		std::array<Capture, g2::g_hdi08PortCount> captures;
		installCaptures(adapter, captures);

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
	// Case group 1. The broadcast.
	//
	// An offset of zero drives every populated select low. A longword at
	// register offset 4 pushes the same word to every port.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures;
		installCaptures(adapter, captures);
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
		std::array<Capture, g2::g_hdi08PortCount> captures;
		installCaptures(adapter, captures);

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
	// Case group 3. The 16-bit path.
	//
	// A 16-bit store at register offset 6 writes TXM then TXL, so the word
	// completes with the low 16 bits of the value and a zero high byte. This
	// is the same "word completes on TXL" rule seen from a third width.
	{
		std::array<Capture, g2::g_hdi08PortCount> captures;
		installCaptures(adapter, captures);
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

		std::array<Capture, g2::g_hdi08PortCount> captures;
		installCaptures(adapter, captures);
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

	// -----------------------------------------------------------------------
	// Case group 5. The INIT bit clears itself.
	//
	// A DSP56300 host port drives ICR's INIT bit low again once the interface
	// initialisation it requested has completed, so a host that writes INIT and
	// polls for it to clear is polling for the DSP's hardware and not for its
	// own store. `mc68k::Hdi08::write8` stores the byte verbatim and clears
	// nothing; the port that owns the register has to answer through the init
	// callback, which is the seam the Nord Lead 2X uses for the same reason.
	//
	// Every assertion below is an exact register value and not a bit test. The
	// two shapes this must tell apart -- INIT cleared, and INIT cleared along
	// with bits the host set in the same byte -- differ only in the bits a
	// masked test would discard.
	{
		const g2::Hdi08Decode initDecode(g2::g_hdi08ExpandedPorts);
		g2::Hdi08Adapter initAdapter(initDecode);

		// The byte the OS writes: INIT alone, with HREQ and TREQ clear.
		const uint32_t icrInitOnly = mc68k::Hdi08::Init;

		// The ISR a port reads before and after initialisation. Txde stands in
		// both because `Hdi08::isr()` raises it on every read; Trdy is the bit
		// the initialisation adds, so the two bytes differ by exactly it.
		const uint32_t isrBeforeInit = mc68k::Hdi08::Txde;
		const uint32_t isrAfterInit  = mc68k::Hdi08::Txde | mc68k::Hdi08::Trdy;

		// Case 5a -- one selected port. 0x110007b8 selects port 3, which is the
		// port the firmware's own initialisation loop reaches first.
		{
			const uint32_t portBase = 0x110007b8u;
			const int portIndex = 3;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			initAdapter.write(portBase - g2::g_cs1Base, 8, icrInitOnly, status);

			checkEqual(status, uint32_t(MCF5307_BUS_OK), "the ICR init write completes OK");

			checkEqual(initAdapter.port(portIndex).icr(), uint32_t(0),
				"an ICR write of INIT alone reads back with INIT cleared");
			checkEqual(initAdapter.port(portIndex).isr(), isrAfterInit,
				"the initialised port raises Trdy beside Txde");

			for(int p = 0; p < g2::g_hdi08PortCount; ++p)
			{
				if(p == portIndex)
					continue;
				checkEqual(initAdapter.port(p).isr(), isrBeforeInit,
					std::string("a single-port ICR init leaves port ") + std::to_string(p)
						+ " uninitialised");
			}
		}

		// Case 5b -- only INIT clears. The host sets HF0 and TREQ in the same
		// byte and both must survive.
		{
			const uint32_t portBase = 0x110007f0u; // port 0.
			const int portIndex = 0;

			const uint32_t written = mc68k::Hdi08::Init | mc68k::Hdi08::Hf0 | mc68k::Hdi08::Treq;
			const uint32_t expected = mc68k::Hdi08::Hf0 | mc68k::Hdi08::Treq;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			initAdapter.write(portBase - g2::g_cs1Base, 8, written, status);

			checkEqual(initAdapter.port(portIndex).icr(), expected,
				"an ICR write of INIT with HF0 and TREQ clears INIT and keeps the rest");
		}

		// Case 5c -- an ICR write WITHOUT init changes no status bit. The clear
		// must be the port answering an initialisation request and not something
		// every ICR write does.
		{
			const uint32_t portBase = 0x110005f8u; // port 6.
			const int portIndex = 6;

			const uint32_t written = mc68k::Hdi08::Hf1;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			initAdapter.write(portBase - g2::g_cs1Base, 8, written, status);

			checkEqual(initAdapter.port(portIndex).icr(), written,
				"an ICR write without INIT is stored verbatim");
			checkEqual(initAdapter.port(portIndex).isr(), isrBeforeInit,
				"an ICR write without INIT raises no Trdy");
		}

		// Case 5d -- the broadcast initialises all eight. CS1 offset zero drives
		// every populated select low, which is how one store initialises the
		// whole array.
		{
			const g2::Hdi08Decode broadcastDecode(g2::g_hdi08ExpandedPorts);
			g2::Hdi08Adapter broadcastAdapter(broadcastDecode);

			mcf5307_bus_status status = MCF5307_BUS_OK;
			broadcastAdapter.write(0u, 8, icrInitOnly, status);

			for(int p = 0; p < g2::g_hdi08PortCount; ++p)
			{
				checkEqual(broadcastAdapter.port(p).icr(), uint32_t(0),
					std::string("the broadcast ICR init clears INIT at port ") + std::to_string(p));
				checkEqual(broadcastAdapter.port(p).isr(), isrAfterInit,
					std::string("the broadcast ICR init raises Trdy at port ") + std::to_string(p));
			}
		}
	}

	// -----------------------------------------------------------------------
	// Case group 6. The rx-empty callback has a default.
	//
	// `mc68k::Hdi08::readRX` calls the rx-empty callback on the first byte of a
	// read sequence without testing it first, so a usable callback is part of
	// the object's invariant rather than an optional decoration. A port holding
	// an empty `std::function` therefore terminates the process on the first RX
	// read instead of reading an empty register as zero, which is what the
	// hardware does.
	//
	// An empty function must be substituted and not stored, so a caller that
	// clears the callback and keeps the port alive does not re-arm the abort.
	// That is why clearing is asserted apart from never installing: a
	// constructor-only default would satisfy one and leave the other armed.
	{
		// ICR is zero on a fresh port, so HLEND is clear and TXH is the first
		// byte of a read sequence -- the one byte that reaches the callback.
		const auto firstRxByte = mc68k::PeriphAddress::HdiTXH;

		// Case 6a -- nothing was ever installed.
		{
			mc68k::Hdi08 hdi08;

			checkEqual(hdi08.read8(firstRxByte), uint32_t(0),
				"a default-constructed port reads an empty RX register as zero");
		}

		// Case 6b -- the callback cleared with an empty function.
		{
			mc68k::Hdi08 hdi08;
			hdi08.setRxEmptyCallback({});

			checkEqual(hdi08.read8(firstRxByte), uint32_t(0),
				"a port whose rx-empty callback was cleared reads an empty RX register as zero");
		}

		// Case 6c -- an installed callback is still the one that runs.
		{
			mc68k::Hdi08 hdi08;

			int calls = 0;
			int needMoreDataCalls = 0;

			hdi08.setRxEmptyCallback([&calls, &needMoreDataCalls](const bool _needMoreData)
			{
				++calls;
				if(_needMoreData)
					++needMoreDataCalls;
			});

			checkEqual(hdi08.read8(firstRxByte), uint32_t(0),
				"an installed rx-empty callback that supplies no data still reads as zero");
			checkEqual(uint32_t(calls), uint32_t(1),
				"the installed rx-empty callback runs once for the read");
			checkEqual(uint32_t(needMoreDataCalls), uint32_t(1),
				"the installed rx-empty callback is told the RX register needs data");
		}
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
