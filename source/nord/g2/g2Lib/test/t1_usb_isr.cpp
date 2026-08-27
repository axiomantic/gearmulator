// THE END-TO-END CHECK: does a delivered packet reach the firmware's USB
// interrupt service routine, and does that routine issue the command that takes
// the interrupt bit back?
//
// Tier T1: it boots the Clavia firmware and reads one file out of the artifact
// corpus, so it SKIPS with a reason when NMG2_ARTIFACTS names no directory.
//
// WHAT THIS FILE ANSWERS AND t1_patch_running DOES NOT. t1_patch_running proves
// a byte of a real `.pch2` sits in the device register file of a machine that
// has really booted, and it reports that the firmware NEVER TOOK it out. It
// says nothing about WHY, because it observes neither the interrupt line nor
// the command port. This file observes both.
//
//   1. THE COMMAND STREAM. Every byte the firmware writes to the CS3 COMMAND
//      port, in order. The port split is the device model's own: the chip's A0
//      is wired to CPU A4, so bit 4 of the CS3-relative offset is the
//      command/data select and a write with that bit set is a command byte.
//      The recorder is a BusTarget that WRAPS the Board's own CS3 target and
//      forwards every cycle to it unchanged, so it is on the firmware's own
//      path and is not a second door.
//
//   2. THE INTERRUPT LINE. `Board::onUsbIrq` calls
//      `InterruptController::setExternalPending(ExternalPin::Irq3, ...)`, and
//      the controller DERIVES the level and the autovector bit from IRQPAR and
//      AVR, which the firmware programs through the MBAR window. So the level
//      and the autovector bit are read back off the controller and are not
//      written here.
//
//   3. THE SERVICE ROUTINE. A counter on 16-bit SDRAM reads at 0x30053C38,
//      which is the address the CODE image installs with
//      install_autovector(3, ...). The instrument is t1_patch_running's,
//      unchanged and for its reasons: the MCF5307 core fetches every
//      instruction word through the bus read callback as a 16-bit access at
//      the instruction's own address, so a counter on 16-bit reads at one SDRAM
//      offset is an instruction-fetch counter for that address.
//
// THE INSTRUMENT'S CONTROLS, BOTH FROM THE SAME POPULATION, because a zero from
// a counter that never fires is not a measurement.
//
//   known positive   the address the MACHINE ITSELF is sitting at when the
//                    window opens, read off Board::mcuReg(17) at that instant.
//                    It is not chosen by this file.
//   known negative   an address inside the vector TABLE. Vectors are read as
//                    32-bit longwords and never fetched as instruction words,
//                    so the same counter must read 0 there.
//
// THE TWO RUNS DELIVER TO DIFFERENT ENDPOINTS, and the endpoint is a Board
// configuration value rather than anything this file reaches past the Board to
// set. Run A uses the shipped default, endpoint 2, and hands over a REAL
// `.pch2`. Run B moves `BoardConfig::usbProtocolEndpoint` to 0 and hands over
// the small in-process container, because the model gives endpoint 0 a
// SINGLE 64-byte OUT buffer and the corpus's largest framed object is 2492
// bytes: a real patch frame would be REFUSED at that endpoint and would raise
// no bit at all, so a zero from it would measure the buffer and not the wire.
//
// EVERY VERDICT IS AN OBSERVABLE AND NOT AN assert(). A release build deletes
// assert(), so a predicate spelled as one is a predicate the shipped build does
// not have. Nothing below calls assert() and nothing catches an exception.

#include "gatedFixture.h"

#include "../board.h"
#include "../crc16.h"
#include "../frame.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"
#include "../transportHub.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "dsp56kBase/logging.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const std::string& _what)
	{
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	std::string hex32(const uint32_t _value)
	{
		static const char* const digits = "0123456789ABCDEF";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	std::string hex8(const uint8_t _value)
	{
		static const char* const digits = "0123456789ABCDEF";
		std::string result = "0x";
		result += digits[(_value >> 4) & 0xfu];
		result += digits[_value & 0xfu];
		return result;
	}

	// ---------------------------------------------- the ESAI underrun log filter
	//
	// INT-1's filter, with INT-1's limit: the underruns are REAL and expected in
	// the boot regime, because nothing drains the ESAIs until the codec queues
	// arrive. This hides the REPETITION and nothing else.
	const char* const g_underrunMessage = "ESAI transmit underrun";

	constexpr uint64_t g_underrunLinesKept = 4;

	std::atomic<uint64_t> g_underrunLines{0};

	void filterLog(const std::string& _s)
	{
		if(_s.find(g_underrunMessage) != std::string::npos &&
		   g_underrunLines.fetch_add(1) >= g_underrunLinesKept)
			return;

		std::cout << _s << '\n';
	}

	void installLogFilter()
	{
		if(std::getenv("G2_LOG_ESAI_UNDERRUN"))
			return;

		Logging::setLogFunc(&filterLog);
	}

	// ------------------------------------------------- INT-1's machine placement
	//
	// COPIED RATHER THAN SHARED, for plan section 1.3 rule 1 -- the reason
	// t1_egress, t1_kernel_load and t1_patch_running each carry their own: a
	// harness's configuration lives at its own site.

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase = 0x10000000u;

	constexpr uint32_t g_cs2Base   = 0x12000000u;
	constexpr uint32_t g_cs2Size   = 0x00800000u;
	constexpr uint32_t g_cs3Size   = 0x00010000u;
	constexpr uint32_t g_cs0Base   = 0x00000000u;
	constexpr uint32_t g_cs0Size   = 0x00020000u;
	constexpr uint32_t g_cs4Base   = 0x14000000u;
	constexpr uint32_t g_cs4Size   = 0x00010000u;
	constexpr uint32_t g_sdramSize = 0x00800000u;
	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;

	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;

	constexpr uint32_t g_bootQuantumBound   = 500000u;
	constexpr uint32_t g_bannerSettleQuanta = 20000u;

	// ------------------------------------------------------ the CS3 port split
	//
	// The device model's own: "the chip's A0 is wired to CPU A4, so bit 4 of
	// the address is the chip's command/data select". Both constants are
	// CS3-window-relative, which is the offset a BusTarget attached at
	// Region::Cs3 receives.
	constexpr uint32_t g_commandSelect = 0x10u;

	constexpr uint32_t g_dataPortAbs    = g2::g_cs3Base + 0x00u;
	constexpr uint32_t g_commandPortAbs = g2::g_cs3Base + 0x10u;

	constexpr int g_byteWidth = 1;

	// The opcodes this file NAMES. Every one of them is already numbered by
	// `src/isp1181/commands.nim`; nothing here invents an opcode, and the 111
	// bytes that file leaves `ccUnspecified` stay unspecified.
	constexpr uint8_t g_endpointConfigBase = 0x20u;
	constexpr uint8_t g_peekCommand        = 0xD2u;
	constexpr uint8_t g_writeIntEnable     = 0xC2u;
	constexpr uint8_t g_readIntRegister    = 0xC0u;

	// THE STATUS FAMILY, WHICH IS THE WHOLE QUESTION. `0x50` is control OUT
	// status, `0x51` is control IN status and `0x52`..`0x54` are endpoints 1
	// to 3. A read of one of these is the route the interrupt bit leaves the
	// interrupt register by, per ISP1362 Rev. 06 p.53, and it is what the
	// device model implements.
	constexpr uint8_t g_statusFirst = 0x50u;
	constexpr uint8_t g_statusLast  = 0x54u;

	// The three further opcodes the measured stream contains, numbered by
	// `src/isp1181/commands.nim` and named here so the expected sequence below
	// reads as words rather than as bytes. The endpoint forms are the family
	// base plus (endpoint - 1), which is that file's own arithmetic: the status
	// family's endpoint base is 0x52 for endpoint 1, so endpoint 2 is 0x53; the
	// buffer-write family's is 0x02, so endpoint 2 is 0x03; the validate
	// family's is 0x62, so endpoint 2 is 0x63.
	constexpr uint8_t g_statusEndpoint2     = 0x53u;
	constexpr uint8_t g_writeEndpoint2In    = 0x03u;
	constexpr uint8_t g_validateEndpoint2In = 0x63u;

	std::string describeOpcodes(const std::vector<uint8_t>& _opcodes)
	{
		if(_opcodes.empty())
			return "(none)";

		std::string result;
		for(const uint8_t opcode : _opcodes)
		{
			if(!result.empty())
				result += ' ';
			result += hex8(opcode);
		}
		return result;
	}

	// ------------------------------------------------------------ the probe set
	//
	// THE SERVICE ROUTINE. CODE reaches install_autovector(3, 0x30053C38) at
	// that helper's ONLY call site in the image, so 0x30053C38 is the address
	// the firmware itself installs as the level-3 handler. board.cpp's
	// onUsbIrq comment is the authority and this file states no address of its
	// own beyond copying it.
	constexpr uint32_t g_probeIsr = 0x30053C38u;

	// THE HANDLER THIS HARNESS ITSELF WRITES INTO EVERY VECTOR. If the firmware
	// never installs its own, an exception taken through the table lands here
	// instead, so a count at this address separates "no exception was taken"
	// from "an exception was taken and it did not go to the firmware's USB
	// handler". It is probed with the SAME 16-bit counter as g_probeIsr, so the
	// two readings are from one population.
	constexpr uint32_t g_probeBlanket = g_vectorHandler;

	// THE LEVEL-3 AUTOVECTOR ENTRY. The ColdFire autovector formula is 24 +
	// level, so level 3 is vector 27 and its table entry is VBR + 27*4. The
	// core reads a vector as a 32-bit LONGWORD, which is exactly the access the
	// 16-bit counter above cannot see, so this is a separate counter on 32-bit
	// reads. board.cpp's onUsbIrq comment is the authority for the vector
	// number and this file restates the arithmetic rather than the number.
	constexpr uint32_t g_usbVectorNumber = 27u;
	constexpr uint32_t g_usbVectorEntry  = g_vectorTableBase + g_usbVectorNumber * 4u;

	// THE KNOWN NEGATIVE. An address inside the vector TABLE this file writes.
	// Vectors are read as 32-bit longwords, never fetched as instruction words,
	// so the 16-bit counter must read 0 there. It is offset 4 rather than 0 so
	// that it is not the reset vector either.
	constexpr uint32_t g_probeNegative = g_vectorTableBase + 4u;

	// How many quanta the machine runs after the packet is handed over.
	//
	// IT IS BOUNDED AND THAT IS DELIBERATE. If the firmware enters the handler
	// and never issues the status read, the line never deasserts and the
	// handler re-enters for as long as the machine is allowed to run. A bound
	// turns that from a hang into a MEASUREMENT: the probe count comes back
	// enormous and the status opcode is absent from the stream.
	constexpr uint32_t g_observeQuanta = 20000u;

	// ------------------------------------------------------- the AVR and IRQPAR
	//
	// MBAR-relative, from interruptController.h's own register map. They are
	// read back rather than written, so this file asserts what the FIRMWARE
	// programmed and never programs anything itself.
	constexpr uint32_t g_avrOffset    = 0x04Bu;
	constexpr uint32_t g_irqparOffset = 0x006u;

	// ------------------------------------------------------ the SDRAM instrument

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		struct Probe
		{
			uint32_t absolute = 0;
			uint32_t offset   = 0;
			uint64_t hits     = 0;
		};

		size_t addProbe(const uint32_t _absolute)
		{
			Probe p;
			p.absolute = _absolute;
			p.offset   = _absolute - g2::g_sdramBase;
			m_probes.push_back(p);
			return m_probes.size() - 1;
		}

		const Probe& probe(const size_t _index) const { return m_probes[_index]; }

		// THE VECTOR COUNTER, AND IT IS A SEPARATE COUNTER BECAUSE IT COUNTS A
		// SEPARATE ACCESS. A vector is fetched as a 32-bit LONGWORD, which the
		// 16-bit instruction-fetch counter above cannot see -- that is exactly
		// what makes the vector table this file's known negative for THAT
		// counter. Both counts are taken over the same span so that "the entry
		// for level 3 was never read" has a control from its own population:
		// the number of longword reads anywhere in the table.
		void watchVectorTable(const uint32_t _base, const uint32_t _entries,
			const uint32_t _watchedEntry)
		{
			m_vectorBase    = _base - g2::g_sdramBase;
			m_vectorLength  = _entries * 4u;
			m_vectorWatched = _watchedEntry - g2::g_sdramBase;
		}

		uint64_t vectorReadsAnywhere() const { return m_vectorReadsAnywhere; }
		uint64_t vectorReadsWatched()  const { return m_vectorReadsWatched; }

		// The longword the store currently holds at an absolute address. It is
		// how this file asks whether the FIRMWARE installed its own handler
		// over the blanket one this harness wrote.
		uint32_t peekLong(const uint32_t _absolute) const
		{
			const size_t index = size_t(_absolute - g2::g_sdramBase);
			uint32_t value = 0;
			for(uint32_t i = 0; i < 4u; ++i)
			{
				value <<= 8;
				if(index + i < m_bytes.size())
					value |= m_bytes[index + i];
			}
			return value;
		}

		void resetProbes()
		{
			for(Probe& p : m_probes)
				p.hits = 0;
			m_wordFetches         = 0;
			m_vectorReadsAnywhere = 0;
			m_vectorReadsWatched  = 0;
		}

		uint64_t wordFetches() const { return m_wordFetches; }

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			// THE COUNT IS TAKEN ON 16-BIT READS AND ON NOTHING ELSE, because
			// that is the width the core fetches an instruction word at.
			if(_size == 16)
			{
				++m_wordFetches;

				for(Probe& p : m_probes)
				{
					if(p.offset == _offset)
						++p.hits;
				}
			}

			// THE VECTOR FETCH. A 32-bit read inside the table span is the core
			// taking an exception through it.
			if(_size == 32 && m_vectorLength != 0 &&
			   _offset >= m_vectorBase && _offset < m_vectorBase + m_vectorLength)
			{
				++m_vectorReadsAnywhere;
				if(_offset == m_vectorWatched)
					++m_vectorReadsWatched;
			}

			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t value = 0u;

			for(uint32_t i = 0; i < count; ++i)
			{
				value <<= 8;
				const size_t index = size_t(_offset) + i;
				if(index < m_bytes.size())
					value |= m_bytes[index];
			}

			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			const uint32_t count = uint32_t(_size) / 8u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
				if(index >= m_bytes.size())
					continue;

				const int shift = int(8u * (count - 1u - i));
				const uint8_t byte = uint8_t((_value >> shift) & 0xffu);

				// A CONTENT WRITE IS ONE THAT IS NOT THE DISPLAY CLEAR, which
				// writes 0x20 and only 0x20.
				if(m_watchLength != 0 && index >= m_watchBase && index < m_watchBase + m_watchLength
					&& byte != 0x20u && byte != 0x00u)
					++m_contentWrites;

				m_bytes[index] = byte;
			}
		}

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

		void watchCells(const uint32_t _offset, const uint32_t _length)
		{
			m_watchBase   = _offset;
			m_watchLength = _length;
		}

		uint64_t contentWrites() const { return m_contentWrites; }

	private:
		std::vector<uint8_t> m_bytes;
		std::vector<Probe>   m_probes;
		uint64_t             m_wordFetches   = 0;
		uint32_t             m_watchBase     = 0;
		uint32_t             m_watchLength   = 0;
		uint64_t             m_contentWrites = 0;

		uint32_t             m_vectorBase          = 0;
		uint32_t             m_vectorLength        = 0;
		uint32_t             m_vectorWatched       = 0;
		uint64_t             m_vectorReadsAnywhere = 0;
		uint64_t             m_vectorReadsWatched  = 0;
	};

	// -------------------------------------------------- the command-port recorder
	//
	// IT WRAPS THE BOARD'S OWN CS3 TARGET AND FORWARDS EVERY CYCLE UNCHANGED.
	// The Board attaches its Isp1181Window in its constructor; this object is
	// attached over it afterwards and holds the one it displaced, so the device
	// still answers every read and keeps every write. A recorder that answered
	// cycles ITSELF would be a second device model and would measure nothing
	// about the first.
	//
	// IT RECORDS THE COMMAND PORT AND NOT THE DATA PORT. A command byte is a
	// WRITE with the command-select bit set; a data-port write is an operand of
	// the command already pending and is not itself a command. Recording both
	// would put operand bytes in the opcode stream, and an operand that happens
	// to equal 0x50 would then answer this file's whole question wrongly.
	class Cs3Recorder final : public g2::BusTarget
	{
	public:
		explicit Cs3Recorder(g2::BusTarget* const _inner) : m_inner(_inner) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			if(m_inner == nullptr)
			{
				_status = MCF5307_BUS_OK;
				return 0u;
			}
			return m_inner->read(_offset, _size, _status);
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
			mcf5307_bus_status& _status) override
		{
			if((_offset & g_commandSelect) != 0 && m_recording)
			{
				const uint8_t opcode = uint8_t(_value & 0xffu);

				++m_counts[opcode];
				++m_total;

				// THE FLAT SEQUENCE, so that a SLICE of it can be compared
				// against an exact expected list. It is capped, and a stream
				// that overruns the cap leaves `m_sequenceDropped` non-zero --
				// which is itself the finding, because the only thing that
				// produces thousands of command bytes here is a handler that
				// never stops re-entering.
				if(m_sequence.size() < m_sequenceCap)
					m_sequence.push_back(opcode);
				else
					++m_sequenceDropped;

				// RUN-LENGTH, so a handler that re-enters a million times is
				// still reportable IN ORDER without a million lines. The ORDER
				// is preserved exactly; only the repetition is folded.
				if(!m_runs.empty() && m_runs.back().first == opcode)
					++m_runs.back().second;
				else if(m_runs.size() < m_runCap)
					m_runs.emplace_back(opcode, uint64_t(1));
				else
					++m_runsDropped;
			}

			if(m_inner == nullptr)
			{
				_status = MCF5307_BUS_OK;
				return;
			}

			m_inner->write(_offset, _size, _value, _status);
		}

		// The peek instrument below drives the command port itself, and its two
		// writes are THIS FILE'S and not the firmware's. Recording them would
		// put bytes in the stream that no firmware issued.
		void setRecording(const bool _on) { m_recording = _on; }

		uint64_t total() const { return m_total; }
		uint64_t countOf(const uint8_t _opcode) const
		{
			const auto it = m_counts.find(_opcode);
			return it == m_counts.end() ? 0u : it->second;
		}
		const std::map<uint8_t, uint64_t>& counts() const { return m_counts; }
		const std::vector<std::pair<uint8_t, uint64_t>>& runs() const { return m_runs; }
		uint64_t runsDropped() const { return m_runsDropped; }

		// The recorded opcodes from `_from` onwards, in order. `_from` is a
		// count taken earlier off `total()`, so the slice is "everything issued
		// after that moment".
		std::vector<uint8_t> sequenceFrom(const uint64_t _from) const
		{
			if(_from >= m_sequence.size())
				return {};
			return std::vector<uint8_t>(m_sequence.begin() + size_t(_from), m_sequence.end());
		}

		uint64_t sequenceDropped() const { return m_sequenceDropped; }

		// Every distinct opcode in `[first, last]` that appears at all.
		std::vector<uint8_t> presentInRange(const uint8_t _first, const uint8_t _last) const
		{
			std::vector<uint8_t> found;
			for(const auto& entry : m_counts)
			{
				if(entry.first >= _first && entry.first <= _last && entry.second > 0)
					found.push_back(entry.first);
			}
			return found;
		}

	private:
		g2::BusTarget* m_inner;
		bool           m_recording = true;

		std::map<uint8_t, uint64_t>               m_counts;
		std::vector<std::pair<uint8_t, uint64_t>> m_runs;
		std::vector<uint8_t>                      m_sequence;
		uint64_t                                  m_total           = 0;
		uint64_t                                  m_runsDropped     = 0;
		uint64_t                                  m_sequenceDropped = 0;

		static constexpr size_t m_runCap      = 4096;
		static constexpr size_t m_sequenceCap = 4096;
	};

	// ------------------------------------------------------- the CS3 peek instrument
	//
	// t0_usb_ingress_byte's instrument, unchanged and for its reasons. It is
	// driven through Board::onWrite/onRead, so it reaches the recorder as well
	// as the device -- which is why the recorder is switched off around it.
	uint8_t peekHeadByte(g2::Board& _board, Cs3Recorder& _recorder, const int _endpoint)
	{
		_recorder.setRecording(false);

		mcf5307_bus_status status = MCF5307_BUS_OK;

		g2::Board::onWrite(&_board, g_commandPortAbs, g_byteWidth,
			uint32_t(g_endpointConfigBase) + uint32_t(_endpoint), &status);
		g2::Board::onWrite(&_board, g_commandPortAbs, g_byteWidth,
			uint32_t(g_peekCommand), &status);

		const uint32_t value = g2::Board::onRead(&_board, g_dataPortAbs, g_byteWidth, &status);

		_recorder.setRecording(true);

		return uint8_t(value & 0xffu);
	}

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig(const int _endpoint)
	{
		g2::BoardConfig config;

		config.memory.cs0   = {g_cs0Base,       g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,   g_cs1Size};
		config.memory.cs2   = {g_cs2Base,       g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base,   g_cs3Size};
		config.memory.cs4   = {g_cs4Base,       g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,   g_cs5Size};
		config.memory.mbar  = {g_mbarBase,      g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		config.usbProtocolEndpoint = _endpoint;

		return config;
	}

	// The small in-process container t0_usb_ingress_byte builds, for its
	// reason: a one-object `.pch2` whose single object is small enough for the
	// 64-byte single buffer the model gives endpoint 0. It carries no Clavia
	// byte -- every byte of it is this process's own.
	constexpr uint8_t g_probeObjectType   = 0x4Au;
	constexpr size_t  g_probeObjectLength = 15u;

	std::vector<uint8_t> buildProbeContainer()
	{
		std::vector<uint8_t> file;

		const char* const ascii = "Version=Nord Modular G2 File Format 1\n";
		for(const char* p = ascii; *p != 0; ++p)
			file.push_back(static_cast<uint8_t>(*p));
		file.push_back(0);

		const size_t binaryHeader = file.size();

		file.push_back(0x17);
		file.push_back(0x00);

		file.push_back(g_probeObjectType);
		file.push_back(uint8_t((g_probeObjectLength >> 8) & 0xffu));
		file.push_back(uint8_t(g_probeObjectLength & 0xffu));

		for(size_t i = 0; i < g_probeObjectLength; ++i)
			file.push_back(uint8_t(31u + i * 7u + 1u));

		file.push_back(0);
		file.push_back(0);

		const uint16_t crc = g2::crc16File(file.data(), file.size(), binaryHeader);
		g2::crc16Store(file.data() + file.size() - 2, crc);

		return file;
	}

	// ------------------------------------------------------------- the result

	struct RunResult
	{
		std::string label;
		int      endpoint       = 0;

		bool     booted         = false;
		bool     programsLanded = false;
		bool     halted         = false;
		bool     faulted        = false;
		uint32_t bootQuanta     = 0;
		unsigned dspCount       = 0;

		bool     loadReturned   = false;
		g2::Pch2LoadResult loadResult = g2::Pch2LoadResult::Loaded;

		uint8_t  peekAfterHandover = 0;
		uint8_t  peekAfterWindow   = 0;

		uint32_t windowPc          = 0;
		uint64_t windowFetches     = 0;
		uint64_t hitsKnownPositive = 0;
		uint64_t hitsKnownNegative = 0;
		uint64_t hitsIsr           = 0;
		uint64_t hitsBlanket       = 0;

		uint64_t vectorReadsAnywhere = 0;
		uint64_t vectorReadsUsb      = 0;
		uint32_t usbVectorContent    = 0;

		// The interrupt line, sampled once per quantum inside the window.
		bool     irqEverPresented    = false;
		int      irqMaxLevel         = 0;
		int      irqAutovectorAtMax  = 0;
		uint8_t  irqVectorAtMax      = 0;
		uint64_t irqQuantaPresented  = 0;

		// What the FIRMWARE programmed into the controller, read back.
		uint8_t  avr    = 0;
		uint8_t  irqpar = 0;

		// The command stream.
		uint64_t commandBytesBoot   = 0;
		uint64_t commandBytesPump   = 0;
		uint64_t commandBytesTotal  = 0;
		uint64_t statusReads        = 0;
		std::vector<uint8_t> statusOpcodesSeen;
		std::vector<uint8_t> commandsAfterBoot;
		uint64_t sequenceDropped    = 0;
		std::map<uint8_t, uint64_t>               commandCounts;
		std::vector<std::pair<uint8_t, uint64_t>> commandRuns;
		uint64_t commandRunsDropped = 0;

		// Audio.
		size_t   primedPulled  = 0;
		unsigned walkQuanta    = 0;
		int      arrival       = -1;
		uint64_t nonZeroFrames = 0;
		int32_t  firstNonZeroL = 0;
		int32_t  firstNonZeroR = 0;
	};

	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	constexpr unsigned g_overrunQuanta = 1024u;

	bool runOnce(const std::string& _directory, const std::vector<uint8_t>& _object,
		RunResult& _r)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig(_r.endpoint));
		Ram ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit the configured SDRAM window" << std::endl;
			return false;
		}

		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);

			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
			{
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] =
						uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			}

			if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit the configured SDRAM window" << std::endl;
				return false;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);

		// THE RECORDER GOES ON OVER THE BOARD'S OWN TARGET AND KEEPS IT. A
		// recorder attached with a null inner would silence the device, and
		// every reading below would then be a reading of the recorder.
		g2::BusTarget* const boardCs3 = board.memory().target(g2::Region::Cs3);

		if(boardCs3 == nullptr)
		{
			std::cout << "FAIL the Board attached no target at Region::Cs3, so there is nothing to wrap"
			          << std::endl;
			return false;
		}

		Cs3Recorder recorder(boardCs3);
		board.memory().attach(g2::Region::Cs3, &recorder);

		const size_t iNegative = ram.addProbe(g_probeNegative);
		const size_t iIsr      = ram.addProbe(g_probeIsr);
		const size_t iBlanket  = ram.addProbe(g_probeBlanket);

		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);
		ram.watchVectorTable(g_vectorTableBase, g_vectorTableEntries, g_usbVectorEntry);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::SerialExecutor          executor;
		g2::Status                  schedulerStatus{};
		const g2::Scheduler::Config config;

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		_r.dspCount = board.dspSet().dspCount();

		// ---------------------------------------------------------- the boot
		uint32_t settle = 0;

		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			_r.bootQuanta = i + 1;

			scheduler->runFrames(1);

			if(board.mcuHalted())
				break;

			if(ram.contentWrites() == 0)
				continue;

			if(++settle < g_bannerSettleQuanta)
				continue;

			_r.booted = true;

			unsigned landed = 0;
			for(unsigned d = 0; d < _r.dspCount; ++d)
			{
				const bool* const flag = board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}

			if(landed == _r.dspCount)
			{
				_r.programsLanded = true;
				break;
			}
		}

		_r.commandBytesBoot = recorder.total();

		// ------------------------------------------------- the packet hand-over
		{
			// ------------------------------------------------ the window opens
			//
			// IT OPENS BEFORE THE HAND-OVER AND BEFORE THE FIRST QUANTUM AFTER
			// IT, and that placement is the whole difference between a
			// measurement and a zero. An earlier form of this file reset the
			// counters AFTER the pump quantum -- and the pump quantum is
			// exactly the quantum in which the firmware services the packet, so
			// every count of that service was thrown away and the file reported
			// a handler that "was not entered" while the command stream showed
			// it plainly. The counters now cover the hand-over itself.
			//
			// THE KNOWN POSITIVE IS READ OFF THE MACHINE HERE, not chosen above:
			// it is the address the core is sitting at at this instant, so it is
			// an address the machine itself demonstrably reaches.
			_r.windowPc = board.mcuReg(g_regPc);

			const size_t iPositive = ram.addProbe(_r.windowPc);

			ram.resetProbes();

			g2::InternalClient client(board.transport(), 4096, 4);

			_r.loadResult   = g2::pch2Load(_object.data(), _object.size(), client);
			_r.loadReturned = true;

			for(uint32_t i = 0; i < g_observeQuanta; ++i)
			{
				// THE FIRST QUANTUM IS THE PUMP QUANTUM: Board::pumpTransport
				// drains what pch2Load put in the hub and hands each frame to
				// the device with isp1181_rx at the CONFIGURED endpoint.
				scheduler->runFrames(1);

				const int level = board.interrupts().presentedLevel();

				if(level != 0)
				{
					++_r.irqQuantaPresented;
					_r.irqEverPresented = true;

					if(level > _r.irqMaxLevel)
					{
						_r.irqMaxLevel        = level;
						_r.irqAutovectorAtMax = board.interrupts().presentedAutovector();
						_r.irqVectorAtMax     = board.interrupts().presentedVector();
					}
				}

				if(i == 0)
				{
					_r.commandBytesPump  = recorder.total();
					_r.peekAfterHandover = peekHeadByte(board, recorder, _r.endpoint);
				}
			}

			_r.windowFetches     = ram.wordFetches();
			_r.hitsKnownPositive = ram.probe(iPositive).hits;
			_r.hitsKnownNegative = ram.probe(iNegative).hits;
			_r.hitsIsr           = ram.probe(iIsr).hits;
			_r.hitsBlanket       = ram.probe(iBlanket).hits;

			_r.vectorReadsAnywhere = ram.vectorReadsAnywhere();
			_r.vectorReadsUsb      = ram.vectorReadsWatched();
			_r.usbVectorContent    = ram.peekLong(g_usbVectorEntry);

			_r.peekAfterWindow = peekHeadByte(board, recorder, _r.endpoint);
		}

		_r.avr    = board.interrupts().readRegister(g_avrOffset);
		_r.irqpar = board.interrupts().readRegister(g_irqparOffset);

		_r.halted  = board.mcuHalted();
		_r.faulted = board.faulted();

		_r.commandBytesTotal    = recorder.total();
		_r.commandCounts        = recorder.counts();
		_r.commandRuns          = recorder.runs();
		_r.commandRunsDropped   = recorder.runsDropped();
		_r.statusOpcodesSeen    = recorder.presentInRange(g_statusFirst, g_statusLast);
		_r.commandsAfterBoot    = recorder.sequenceFrom(_r.commandBytesBoot);
		_r.sequenceDropped      = recorder.sequenceDropped();

		_r.statusReads = 0;
		for(const uint8_t opcode : _r.statusOpcodesSeen)
			_r.statusReads += recorder.countOf(opcode);

		// ------------------------------------------------- the play transition
		scheduler->beginPlayPhase();

		{
			std::vector<g2::Frame> primed(config.lookaheadFrames);
			_r.primedPulled = scheduler->pull(primed.data(), primed.size());
		}

		// ------------------------------------------------------------ the walk
		const unsigned expected = (_r.dspCount > 0 ? _r.dspCount - 1u : 0u) * config.hopFrames;
		const unsigned walk     = expected + g_overrunQuanta;

		g2::Frame impulse{};
		impulse.slot[0] = g_impulseLeft;
		impulse.slot[1] = g_impulseRight;

		const g2::Frame silence{};

		for(unsigned q = 0; q < walk; ++q)
		{
			const g2::Frame& in = (q == 0) ? impulse : silence;

			(void) scheduler->push(&in, 1);
			scheduler->runFrames(1);

			g2::Frame out{};
			(void) scheduler->pull(&out, 1);

			if(out.slot[0] != 0 || out.slot[1] != 0)
			{
				++_r.nonZeroFrames;

				if(_r.arrival < 0)
				{
					_r.arrival       = int(q);
					_r.firstNonZeroL = out.slot[0];
					_r.firstNonZeroR = out.slot[1];
				}
			}
		}

		_r.walkQuanta = walk;

		// The Board's CS3 target is restored before the recorder dies, so no
		// dangling pointer outlives this scope.
		board.memory().attach(g2::Region::Cs3, boardCs3);

		return true;
	}

	void report(const RunResult& _r)
	{
		const std::string& l = _r.label;

		std::cout << l << ": endpoint=" << _r.endpoint
		          << " bootQuanta=" << _r.bootQuanta
		          << " booted=" << (_r.booted ? 1 : 0)
		          << " programsLanded=" << (_r.programsLanded ? 1 : 0)
		          << " dspCount=" << _r.dspCount
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0) << std::endl;

		std::cout << l << ": loadResult="
		          << (_r.loadReturned ? g2::pch2LoadResultName(_r.loadResult) : "(not offered)")
		          << " peekAfterHandover=" << hex8(_r.peekAfterHandover)
		          << " peekAfterWindow=" << hex8(_r.peekAfterWindow) << std::endl;

		std::cout << l << ": windowQuanta=" << g_observeQuanta
		          << " windowPc=" << hex32(_r.windowPc)
		          << " windowWordFetches=" << _r.windowFetches << std::endl;

		std::cout << l << ": probe " << hex32(_r.windowPc) << " (known positive) = "
		          << _r.hitsKnownPositive
		          << " | probe " << hex32(g_probeNegative) << " (known negative) = "
		          << _r.hitsKnownNegative
		          << " | probe " << hex32(g_probeIsr) << " (the ISR) = "
		          << _r.hitsIsr
		          << " | probe " << hex32(g_probeBlanket) << " (this harness's blanket handler) = "
		          << _r.hitsBlanket << std::endl;

		std::cout << l << ": vector longword reads -- anywhere in the table = "
		          << _r.vectorReadsAnywhere
		          << ", at " << hex32(g_usbVectorEntry) << " (level 3, vector "
		          << g_usbVectorNumber << ") = " << _r.vectorReadsUsb
		          << ", and that entry now holds " << hex32(_r.usbVectorContent)
		          << " (this harness wrote " << hex32(g_vectorHandler)
		          << ", the CODE image installs " << hex32(g_probeIsr) << ")" << std::endl;

		std::cout << l << ": irq presented in " << _r.irqQuantaPresented << " of "
		          << g_observeQuanta << " quanta, maxLevel=" << _r.irqMaxLevel
		          << " autovector=" << _r.irqAutovectorAtMax
		          << " vector=" << unsigned(_r.irqVectorAtMax)
		          << " | firmware AVR=" << hex8(_r.avr)
		          << " IRQPAR=" << hex8(_r.irqpar) << std::endl;

		std::cout << l << ": command bytes -- boot=" << _r.commandBytesBoot
		          << " throughPumpQuantum=" << _r.commandBytesPump
		          << " total=" << _r.commandBytesTotal
		          << " distinct=" << _r.commandCounts.size()
		          << " statusReads(0x50-0x54)=" << _r.statusReads << std::endl;

		std::cout << l << ": command counts:";
		for(const auto& entry : _r.commandCounts)
			std::cout << ' ' << hex8(entry.first) << "x" << entry.second;
		std::cout << std::endl;

		std::cout << l << ": command stream in order (run-length, "
		          << _r.commandRuns.size() << " runs, " << _r.commandRunsDropped
		          << " dropped past the cap):" << std::endl;

		{
			std::string line = "    ";
			size_t onLine = 0;
			for(const auto& run : _r.commandRuns)
			{
				line += hex8(run.first);
				if(run.second > 1)
					line += "*" + std::to_string(run.second);
				line += ' ';
				if(++onLine == 12)
				{
					std::cout << line << std::endl;
					line = "    ";
					onLine = 0;
				}
			}
			if(onLine != 0)
				std::cout << line << std::endl;
		}

		std::cout << l << ": commands issued AFTER the boot: "
		          << describeOpcodes(_r.commandsAfterBoot)
		          << " (" << _r.sequenceDropped << " dropped past the sequence cap)" << std::endl;

		std::cout << l << ": primedPulled=" << _r.primedPulled
		          << " walkQuanta=" << _r.walkQuanta
		          << " arrival=" << _r.arrival
		          << " nonZeroFrames=" << _r.nonZeroFrames
		          << " firstNonZero=" << _r.firstNonZeroL << "/" << _r.firstNonZeroR
		          << std::endl;
	}
}

int main()
{
	installLogFilter();

	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		const std::string patchPath = directory + "/" + G2_PATCH_RELATIVE_PATH;
		const std::vector<uint8_t> patch = readFile(patchPath);

		if(patch.empty())
		{
			std::cout << "FAIL the patch is empty or unreadable at " << patchPath << std::endl;
			return false;
		}

		std::cout << "patch: " << patchPath << " (" << patch.size() << " bytes)" << std::endl;

		// -------------------------------------------- run A: the real path
		RunResult real;
		real.label    = "ep2-patch";
		real.endpoint = 2;
		if(!runOnce(directory, patch, real))
			return false;
		report(real);

		// ------------------------------- run B: a packet the control endpoint fits
		const std::vector<uint8_t> probeFile = buildProbeContainer();

		RunResult control0;
		control0.label    = "ep0-small";
		control0.endpoint = 0;
		if(!runOnce(directory, probeFile, control0))
			return false;
		report(control0);

		// ----------------------------------------- the instrument, asserted first
		check(real.windowFetches > 0,
			"ep2-patch: the fetch counter saw instruction words at all during the window");
		check(real.hitsKnownPositive > 0,
			"ep2-patch KNOWN POSITIVE: the address the machine itself was sitting at is counted by the same probe");
		check(real.hitsKnownNegative == 0,
			"ep2-patch KNOWN NEGATIVE: an address inside the vector table is never fetched as an instruction word");
		check(control0.hitsKnownPositive > 0,
			"ep0-small KNOWN POSITIVE: the same probe fires there too");
		check(control0.hitsKnownNegative == 0,
			"ep0-small KNOWN NEGATIVE: the vector-table address is still never fetched");

		// The command-port recorder's own known positive: the firmware DOES
		// drive the command port. A stream of zero bytes would make every
		// absence below a statement about the recorder.
		check(real.commandBytesBoot > 0,
			std::string("KNOWN POSITIVE for the recorder: the firmware wrote command bytes to the CS3"
			            " command port during the boot; counted ") +
			std::to_string(real.commandBytesBoot));

		// The preconditions.
		check(real.programsLanded, "ep2-patch: every DSP position took its program");
		check(control0.programsLanded, "ep0-small: every DSP position took its program");
		check(!real.halted, "ep2-patch: the core is not halted when the window closes");
		check(!real.faulted, "ep2-patch: the board reports no fault when the window closes");

		// ------------------------------------------------------------- THE ROWS

		// 1. The packet is in the device.
		check(real.peekAfterHandover != 0x00u,
			std::string("ep2-patch: a byte of the real `.pch2` is at the CS3 data port after the"
			            " hand-over; read ") + hex8(real.peekAfterHandover));
		check(control0.peekAfterHandover != 0x00u,
			std::string("ep0-small: a byte of the small container is at the CS3 data port of"
			            " ENDPOINT 0 after the hand-over; read ") + hex8(control0.peekAfterHandover));

		// 2. The line asserts, AND THE EVIDENCE IS THE TAKEN EXCEPTION AND NOT
		//    THE SAMPLED LEVEL.
		//
		//    presentedLevel() IS SAMPLED AT QUANTUM BOUNDARIES AND THAT IS NOT
		//    FINE ENOUGH. The whole transaction -- the device raising the bit,
		//    the controller presenting level 3, the core taking the exception
		//    and the handler clearing the bit -- completes INSIDE one quantum,
		//    so a sample taken at the boundary sees the machine's ordinary
		//    level-1 traffic and never the level-3 presentation. That is a
		//    property of the sampling and not of the machine, and it is why the
		//    sampled figures below are REPORTED and only this is asserted.
		//
		//    THE LEVEL-3 AUTOVECTOR ENTRY IS READ AS A LONGWORD EXACTLY WHEN
		//    THE CORE TAKES THE EXCEPTION, and the ColdFire autovector formula
		//    24+level puts level 3 at vector 27. Its control comes from the
		//    same population and the same counter: the longword reads anywhere
		//    else in the same table, which the machine makes in the thousands.
		check(control0.vectorReadsAnywhere > 0,
			std::string("ep0-small KNOWN POSITIVE for the vector counter: the core reads vector-table"
			            " longwords at all; counted ") + std::to_string(control0.vectorReadsAnywhere));
		check(control0.vectorReadsUsb == 1,
			std::string("ep0-small: the LEVEL-3 autovector entry at ") + hex32(g_usbVectorEntry) +
			" is read EXACTLY ONCE, so the core took exactly one level-3 exception; counted " +
			std::to_string(control0.vectorReadsUsb));
		check(control0.usbVectorContent == g_probeIsr,
			std::string("ep0-small: the firmware installed its OWN handler over the blanket one this"
			            " harness wrote -- the level-3 entry holds ") +
			hex32(control0.usbVectorContent) + " and not " + hex32(g_vectorHandler));

		// 3. The handler is entered, and it is entered ONCE.
		//
		//    ONCE IS THE LOAD-BEARING NUMBER AND NOT A DETAIL. If the bit never
		//    left the interrupt register the line would never deassert, the
		//    handler would re-enter for every quantum the machine is given, and
		//    this count would be in the thousands rather than 1. `> 0` would be
		//    green in BOTH worlds and would say nothing about the route.
		check(control0.hitsIsr == 1,
			std::string("ep0-small: the service routine at ") + hex32(g_probeIsr) +
			" is entered EXACTLY ONCE across " + std::to_string(g_observeQuanta) +
			" quanta -- so the line DEASSERTED and the handler did not re-enter; the same counter"
			" that read " + std::to_string(control0.hitsKnownPositive) +
			" at the known positive read " + std::to_string(control0.hitsIsr) + " there");
		check(control0.hitsBlanket == 0,
			std::string("ep0-small: the blanket handler this harness wrote is never entered, so the"
			            " count above is the FIRMWARE'S handler and not an exception landing in the"
			            " harness's own filler; counted ") + std::to_string(control0.hitsBlanket));
		check(real.hitsIsr == 1,
			std::string("ep2-patch: the same routine is entered exactly once on the endpoint the"
			            " machine actually uses; counted ") + std::to_string(real.hitsIsr));

		// 4. THE ANSWER. The clearing route, and the opcode tracks the ENDPOINT.
		//
		//    THE TWO RUNS ARE EACH OTHER'S CONTROL. A model whose status family
		//    were mis-decoded, or a firmware that issued one fixed opcode, would
		//    put the SAME byte in both streams. 0x50 is control OUT status and
		//    0x53 is endpoint 2's, and each appears in the run that delivered to
		//    that endpoint.
		check(control0.statusOpcodesSeen == std::vector<uint8_t>{ g_statusFirst },
			std::string("ep0-small: the firmware issues status-read ") + hex8(g_statusFirst) +
			" -- control OUT status, the route the model takes bit 8 back by -- and no other opcode"
			" of the family; saw " + describeOpcodes(control0.statusOpcodesSeen));
		check(real.statusOpcodesSeen == std::vector<uint8_t>{ g_statusEndpoint2 },
			std::string("ep2-patch: the firmware issues status-read ") + hex8(g_statusEndpoint2) +
			" -- endpoint 2's status, the route the model takes bit 11 back by -- and no other opcode"
			" of the family; saw " + describeOpcodes(real.statusOpcodesSeen));

		// 5. THE ORDER. A status read that arrived before the interrupt-register
		//    read would not be a service of that interrupt.
		check(real.commandsAfterBoot == std::vector<uint8_t>{
				g_readIntRegister, g_statusEndpoint2, g_writeEndpoint2In, g_validateEndpoint2In },
			std::string("ep2-patch: the commands the firmware issues AFTER the boot are exactly"
			            " read-interrupt-register, endpoint-2 status, write endpoint-2 IN buffer,"
			            " validate endpoint-2 IN buffer -- a complete service that reads the register,"
			            " clears the bit and replies; saw ") + describeOpcodes(real.commandsAfterBoot));
		check(control0.commandsAfterBoot == std::vector<uint8_t>{
				g_readIntRegister, g_statusFirst },
			std::string("ep0-small: the commands after the boot are exactly read-interrupt-register"
			            " then control OUT status, and no reply follows; saw ") +
			describeOpcodes(control0.commandsAfterBoot));

		// ------------------------------------------------------- the verdict lines
		std::cout << "verdict: status-read opcodes 0x50-0x54 are "
		          << (control0.statusOpcodesSeen.empty() ? "ABSENT from" : "PRESENT in")
		          << " the ep0-small command stream" << std::endl;

		std::cout << "verdict: status-read opcodes 0x50-0x54 are "
		          << (real.statusOpcodesSeen.empty() ? "ABSENT from" : "PRESENT in")
		          << " the ep2-patch command stream" << std::endl;

		std::cout << "verdict: write-interrupt-enable " << hex8(g_writeIntEnable) << " was issued "
		          << real.commandCounts.count(g_writeIntEnable) << " distinct-opcode times ("
		          << (real.commandCounts.count(g_writeIntEnable) ? "PRESENT" : "ABSENT")
		          << "), read-interrupt-register " << hex8(g_readIntRegister) << " is "
		          << (real.commandCounts.count(g_readIntRegister) ? "PRESENT" : "ABSENT")
		          << std::endl;

		std::cout << "verdict: the ISR at " << hex32(g_probeIsr) << " was "
		          << (real.hitsIsr > 0 ? "ENTERED" : "NOT ENTERED") << " on ep2-patch and "
		          << (control0.hitsIsr > 0 ? "ENTERED" : "NOT ENTERED") << " on ep0-small"
		          << std::endl;

		std::cout << "verdict: the firmware "
		          << (control0.peekAfterWindow == control0.peekAfterHandover
		              ? "NEVER TOOK the packet out of the endpoint 0 buffer"
		              : "TOOK the packet out of the endpoint 0 buffer")
		          << " across " << g_observeQuanta << " quanta" << std::endl;

		std::cout << "verdict: audio -- ep2-patch arrival=" << real.arrival
		          << " nonZeroFrames=" << real.nonZeroFrames
		          << ", ep0-small arrival=" << control0.arrival
		          << " nonZeroFrames=" << control0.nonZeroFrames << std::endl;

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
