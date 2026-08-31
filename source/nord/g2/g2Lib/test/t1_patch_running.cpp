// A real `.pch2` delivered to booted, running firmware.
// Tier T1: it needs the Clavia-derived artifacts and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// t0_usb_ingress_byte proves a patch byte reaches the device register file the
// firmware reads, on a Board with no firmware in it. t1_boot and t1_egress boot
// the firmware and load no patch. So `--impulse`'s `arrival=-1` is a statement
// about an unpatched machine and says nothing about a patched one. This file is
// the join.
//
// The two questions it makes answerable.
//
//   1. Does MCU routine 0x30032A82 fire on a real patch load? Nothing in the
//      emulator names that address, so the only way to reach it is to let the
//      firmware run the path.
//
//   2. Audio. `arrival=-1` on an unpatched machine is not a claim about a
//      patched one.
//
// The instrument needs no production change. The MCF5307 core fetches every
// instruction word through the bus read callback -- `cpu.nim`'s
// `ctx.readFn(ctx.user, ctx.pc, 2, addr status)` -- and Board::onRead routes
// that to the MemoryMap, which routes it to the BusTarget attached at
// Region::Sdram. That target is this file's Ram. So a counter on 16-bit reads
// at one SDRAM offset is an instruction-fetch counter for that address, built
// entirely inside the test.
//
// The instrument's controls, both from the same population. A zero from a
// counter that never fires is not a measurement.
//
//   known positive   the most-visited address of the window: the argmax of a
//                    histogram this file keeps over every 16-bit read in the
//                    window. It is not named anywhere in this file; it is
//                    whatever address this run read most, counted by the same
//                    counter as every probe.
//   known negative   an address inside the vector TABLE. Vectors are read as
//                    32-bit longwords and never fetched as instruction words,
//                    so the same counter must read 0 there.
//
// The argmax, and not the address the core happened to sit at when the window
// opened: one instant of the machine is not a measure of how hard the counter
// can fire, and the window can open inside an interrupt handler at an address
// the window then reads once.
//
// The argmax is not certified to be an instruction -- it is the most-read
// 16-bit location, and a hot 16-bit DATA read wins it just as legitimately. Its
// job is to answer how large a count this counter can produce on THIS arm, so
// that a zero elsewhere has a scale to be read against.
//
// Because the known positive is the maximum, `knownPositive >= hitsTarget`
// holds by construction for every probe in this file. No such comparison is
// asserted below, and none would mean anything if it were.
//
// AND THE CONTROL THAT MAKES THE ANSWER AN ANSWER: the whole run happens TWICE
// on the same code path, once WITHOUT a patch and once WITH one. A probe count
// that is non-zero in both runs says the routine fires anyway; non-zero only in
// the patched run is the patch load reaching it.
//
// Every verdict is an observable and not an assert(). A release build deletes
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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// The underruns are real and expected in the boot regime, because nothing
	// drains the ESAIs until the codec queues arrive. This hides the repetition
	// and nothing else. Set
	// G2_LOG_ESAI_UNDERRUN to install no filter at all.
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

	// ---------------------------------------------------- the machine placement

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

	// ------------------------------------------------- the CS3 peek instrument
	//
	// t0_usb_ingress_byte's instrument: the part's own peek command (0xD2)
	// issued at the CS3 command port and read back at the CS3 data port, with
	// the peek target selected by the endpoint-configuration command (0x20 +
	// the endpoint's configuration slot, which is not its number). It reads the
	// head byte of the OUT buffer the given endpoint delivers into, and answers
	// the model's benign 0x00 when that buffer holds nothing.
	//
	// t0_usb_ingress_byte runs it on a Board with no firmware in it. On a
	// booted machine the same reading answers a different question: whether the
	// firmware ever took the packet out.
	constexpr uint32_t g_dataPort    = g2::g_cs3Base + 0x00u;
	constexpr uint32_t g_commandPort = g2::g_cs3Base + 0x10u;

	constexpr int g_byteWidth = 1;

	constexpr uint8_t g_endpointConfigBase = 0x20u;
	constexpr uint8_t g_peekCommand        = 0xD2u;

	// The configuration slot order, which is not the endpoint number. ISP1362
	// Rev. 06 section 15.1.1 orders the sixteen `0x20`..`0x2F` slots control
	// OUT, control IN, then endpoints 1 to 14, so endpoint 0 is slot 0,
	// endpoint 1 is slot 2, endpoint 2 is slot 3 and endpoint 3 is slot 4. The
	// peek command answers about the buffer the last configuration command
	// selected, and that operand is one of these. Passing an endpoint number
	// straight through selects a buffer one place low for every endpoint above
	// 0, the read still succeeds, and the wrong answer arrives looking exactly
	// like the right one.
	constexpr int g_bufferSlotOfEndpoint[4] = {0, 2, 3, 4};

	int bufferSlotOfEndpoint(const int _endpoint)
	{
		if(_endpoint < 0 || _endpoint >= 4)
			return -1;
		return g_bufferSlotOfEndpoint[_endpoint];
	}

	// The synthetic object the known positive delivers. Its type byte is not
	// 0x00, so a reading of it cannot be confused with the benign answer; it is
	// also not 0x21, which is the type byte of the first object in every file
	// of the corpus, so a reading of it cannot be confused with the PATCH's
	// either. Its whole framed length is 3 + 15 = 18 bytes, well inside the
	// 64-byte capacity the model gives the protocol endpoint.
	constexpr uint8_t g_probeObjectType   = 0x4Au;
	constexpr size_t  g_probeObjectLength = 15u;

	uint8_t peekHeadByte(g2::Board& _board, const int _endpoint)
	{
		const int slot = bufferSlotOfEndpoint(_endpoint);

		if(slot < 0)
			return 0x00u;

		mcf5307_bus_status status = MCF5307_BUS_OK;

		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_endpointConfigBase) + uint32_t(slot), &status);
		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_peekCommand), &status);

		const uint32_t value = g2::Board::onRead(&_board, g_dataPort, g_byteWidth, &status);

		return uint8_t(value & 0xffu);
	}

	// The largest FRAMED object in a `.pch2`, counting its 3-byte header, and
	// how many objects it holds. Both are COMPUTED from the file this run
	// loaded and neither is written here as a literal, so a different patch
	// reports its own figures.
	//
	// IT ALSO COUNTS WHAT THE SPLIT COSTS AND WHAT THE CORPUS CANNOT ANSWER.
	// `_packets` is how many max-packet-size packets the whole container takes,
	// and `_exactMultiples` is how many of its framed objects have a length
	// that is an EXACT MULTIPLE of that packet size. The second figure is the
	// one that decides whether this file can say anything about the trailing
	// zero-length packet at all: the convention only ever applies to an exact
	// multiple, so a corpus containing none of them cannot exercise it, and
	// saying so as a computed number is the difference between a measurement
	// and an assumption. Both are COMPUTED from the file this run loaded.
	void measureObjects(const std::vector<uint8_t>& _file, unsigned& _count, size_t& _largest,
		uint8_t& _firstType, size_t _packetSize, unsigned& _packets, unsigned& _exactMultiples)
	{
		_count           = 0;
		_largest         = 0;
		_firstType       = 0;
		_packets         = 0;
		_exactMultiples  = 0;

		size_t at = 0;
		while(at < _file.size() && _file[at] != 0)
			++at;

		if(at >= _file.size())
			return;

		at += 1 + 2;                       // the NUL, then the 2-byte binary header
		const size_t end = _file.size() >= 2 ? _file.size() - 2 : 0;   // the stored CRC

		while(at + 3 <= end)
		{
			const size_t length = (size_t(_file[at + 1]) << 8) | size_t(_file[at + 2]);
			const size_t framed = 3 + length;

			if(at + framed > end)
				return;

			if(framed > _largest)
				_largest = framed;

			if(_count == 0)
				_firstType = _file[at];

			if(_packetSize != 0)
			{
				// A frame of N bytes costs ceil(N / packetSize) packets, and a
				// frame of ZERO bytes still costs one -- it is an empty packet,
				// not an absent one.
				_packets += unsigned(framed == 0 ? 1
				                   : (framed + _packetSize - 1) / _packetSize);

				if(framed != 0 && (framed % _packetSize) == 0)
					++_exactMultiples;
			}

			++_count;
			at += framed;
		}
	}

	// ------------------------------------------------------------ the probe set
	//
	// 0x30032A82 is the load-bearing unknown; 0x3001D85C and 0x3001DAD8 are its
	// two callers, and they are probed too because "the caller never ran" and
	// "the caller ran and did not reach it" are different findings about the
	// machine. 0x30032254 is the three-piece assembly 0x30032A82 reaches.
	constexpr uint32_t g_probeTarget   = 0x30032A82u;
	constexpr uint32_t g_probeCallerA  = 0x3001D85Cu;
	constexpr uint32_t g_probeCallerB  = 0x3001DAD8u;
	constexpr uint32_t g_probeAssembly = 0x30032254u;

	// SCAFFOLD (temporary, reverted after the measurement). 0x300039C8 is the
	// `beqs` the scan at 0x30003982 takes when the ring is empty; every register
	// the scan was handed is live at its fetch. 0x302A2700 is the longword the
	// four call sites push as the descriptor pointer.
	constexpr uint32_t g_ringProbePc = 0x300039C8u;
	constexpr uint32_t g_ringGlobal  = 0x302A2700u;

	// THE KNOWN NEGATIVE. An address inside the vector TABLE this file writes.
	// Vectors are read as 32-bit longwords, never fetched as instruction words,
	// so the 16-bit counter must read 0 there. It is offset 4 rather than 0 so
	// that it is not the reset vector either.
	constexpr uint32_t g_probeNegative = g_vectorTableBase + 4u;

	// How many quanta the machine runs after the patch is handed over. The
	// findings name a 4000-tick deferred rebuild timer as the scheduler of the
	// routine under test, so a window shorter than that could report a routine
	// that had not been given the chance to run. This is more than ten times it.
	// SCAFFOLD (temporary, reverted after the measurement): the window length is
	// read from the environment so the same binary can be run at several window
	// sizes. Absent the variable it is the committed 50000.
	inline uint32_t observeQuantaFromEnv()
	{
		const char* const v = std::getenv("G2_OBSERVE_QUANTA");
		if(v == nullptr || *v == '\0')
			return 50000u;
		return uint32_t(std::strtoul(v, nullptr, 10));
	}

	const uint32_t g_observeQuanta = observeQuantaFromEnv();

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size)
			: m_bytes(_size, 0u)
			, m_wordHits(_size / 2u, 0u)
			, m_firstQuantum(_size / 2u, 0u)  // SCAFFOLD
		{
		}

		// SCAFFOLD (temporary): the window's quantum index, stepped by the
		// observation loop, so a first fetch can be dated. 0 means "never".
		uint32_t scaffoldQuantum = 0;

		// SCAFFOLD: the longword at an absolute SDRAM address, read straight
		// out of the backing store WITHOUT touching the histogram, so that
		// reading a pointer table cannot alter the measurement it qualifies.
		uint32_t peekLong(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;
			const size_t off = size_t(_absolute - g2::g_sdramBase);
			if(off + 4u > m_bytes.size())
				return 0u;
			return (uint32_t(m_bytes[off]) << 24) | (uint32_t(m_bytes[off + 1]) << 16)
			     | (uint32_t(m_bytes[off + 2]) << 8) | uint32_t(m_bytes[off + 3]);
		}

		// SCAFFOLD: the 16-bit word at an absolute SDRAM address, same backing
		// store, same no-histogram rule as peekLong above.
		uint32_t peekWord(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;
			const size_t off = size_t(_absolute - g2::g_sdramBase);
			if(off + 2u > m_bytes.size())
				return 0u;
			return (uint32_t(m_bytes[off]) << 8) | uint32_t(m_bytes[off + 1]);
		}

		uint32_t peekByte(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;
			const size_t off = size_t(_absolute - g2::g_sdramBase);
			if(off >= m_bytes.size())
				return 0u;
			return m_bytes[off];
		}

		// SCAFFOLD: the DATA probe. When the core fetches the instruction word
		// at m_ringProbePc the machine's registers already hold everything the
		// scan at 0x30003982 was handed, so this is the moment the ring
		// descriptor and the key are both live. The snapshot is formatted here
		// and printed after the window; nothing is printed from inside the bus
		// callback.
		const g2::Board* scaffoldBoard = nullptr;

		void armRingProbe(const uint32_t _pc, const uint32_t _globalPtr, const size_t _maxSnaps)
		{
			m_ringProbePc     = _pc;
			m_ringProbeGlobal = _globalPtr;
			m_ringProbeMax    = _maxSnaps;
			m_ringLog.clear();
			m_ringHits = 0;
		}

		uint64_t ringHits() const { return m_ringHits; }
		const std::vector<std::string>& ringLog() const { return m_ringLog; }

		// THE COUNTER IS A HISTOGRAM AND NOT A PROBE LIST, and that is what
		// makes the known positive a property of the RUN. A fixed probe list can
		// only answer about addresses this file names; a histogram over every
		// 16-bit read lets the file ASK the run which address it read most, and
		// take that answer as its control. One counter per 16-bit word of SDRAM.
		struct Hottest
		{
			uint32_t absolute = 0;
			uint64_t hits     = 0;
		};

		// The count at one absolute SDRAM address. Zero outside the window and
		// zero at an odd offset, which gets no counter.
		uint64_t hitsAt(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0;

			const uint32_t offset = _absolute - g2::g_sdramBase;

			if((offset & 1u) != 0u)
				return 0;

			const size_t index = size_t(offset) >> 1;

			return index < m_wordHits.size() ? uint64_t(m_wordHits[index]) : 0u;
		}

		// The argmax over every counter: the address this run read more times
		// than any other. Ties go to the lowest address, so the selection is
		// deterministic across runs.
		Hottest hottest() const
		{
			return hottestInRange(g2::g_sdramBase,
				g2::g_sdramBase + uint32_t(m_wordHits.size() << 1));
		}

		// The same argmax over one half-open address range; the caller passes the
		// extent of the firmware image it just read from disk.
		//
		// The unrestricted argmax can land past the end of the loaded image, where
		// the machine reaches it as a 16-bit DATA read and not as an instruction
		// fetch. The probes are instruction addresses, so such a control belongs to
		// a different population and proves only that the counter runs.
		Hottest hottestInRange(const uint32_t _loAbsolute, const uint32_t _hiAbsolute) const
		{
			Hottest best;

			if(_hiAbsolute <= _loAbsolute || _loAbsolute < g2::g_sdramBase)
				return best;

			const size_t lo = (size_t(_loAbsolute - g2::g_sdramBase) + 1u) >> 1;
			const size_t hi = std::min(size_t(_hiAbsolute - g2::g_sdramBase) >> 1, m_wordHits.size());

			for(size_t index = lo; index < hi; ++index)
			{
				if(uint64_t(m_wordHits[index]) <= best.hits)
					continue;

				best.hits     = uint64_t(m_wordHits[index]);
				best.absolute = g2::g_sdramBase + uint32_t(index << 1);
			}

			return best;
		}

		// SCAFFOLD (temporary, reverted after the measurement). Writes every
		// 16-bit SDRAM word inside [lo, hi) that was fetched at least once in
		// the window, as "absolute hits", one per line. The unit is one 16-bit
		// SDRAM word fetched at least once inside the window.
		void dumpExecuted(const std::string& _file, const uint32_t _loAbsolute, const uint32_t _hiAbsolute) const
		{
			std::ofstream out(_file);
			if(!out)
				return;

			const size_t lo = (size_t(_loAbsolute - g2::g_sdramBase) + 1u) >> 1;
			const size_t hi = std::min(size_t(_hiAbsolute - g2::g_sdramBase) >> 1, m_wordHits.size());

			for(size_t index = lo; index < hi; ++index)
			{
				if(m_wordHits[index] == 0u)
					continue;
				out << (g2::g_sdramBase + uint32_t(index << 1)) << ' '
				    << uint64_t(m_wordHits[index]) << ' '
				    << m_firstQuantum[index] << '\n';
			}
		}

		// Zeroes every counter, so that a window's counts are the WINDOW's and
		// not the boot's.
		void resetProbes()
		{
			std::fill(m_wordHits.begin(), m_wordHits.end(), 0u);
			std::fill(m_firstQuantum.begin(), m_firstQuantum.end(), 0u);  // SCAFFOLD
			m_wordFetches  = 0;
			m_oddWordReads = 0;
		}

		// 16-bit reads at an odd offset, which get no histogram counter. Reported
		// rather than dropped: if this were ever large, the histogram would be
		// missing reads it should be seeing.
		uint64_t oddWordReads() const { return m_oddWordReads; }

		// Every 16-bit read in the window, whatever its address. It is what
		// separates "the probed address was not fetched" from "the counter was
		// never reached at all".
		uint64_t wordFetches() const { return m_wordFetches; }

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			// The count is taken on 16-bit reads and on nothing else, because
			// that is the width the core fetches an instruction word at.
			if(_size == 16)
			{
				++m_wordFetches;

				// An odd offset gets no counter. Folding it into the even bucket
				// below it would be worse than dropping it: the histogram picks a
				// maximum, so two addresses sharing one bucket would inflate that
				// bucket and could hand the argmax to an address that was never
				// read that many times. Instruction words are even-aligned, so
				// this drops no fetch, and the count is reported so that claim is
				// checkable rather than assumed.
				if((_offset & 1u) != 0u)
				{
					++m_oddWordReads;
				}
				else
				{
					const size_t index = size_t(_offset) >> 1;
					if(index < m_wordHits.size())
					{
						// SCAFFOLD: date the FIRST fetch of this word.
						if(m_wordHits[index] == 0u)
							m_firstQuantum[index] = scaffoldQuantum;
						++m_wordHits[index];
					}

					// SCAFFOLD: the data probe fires on the instruction fetch
					// at the armed PC and reads the backing store only.
					if(m_ringProbePc != 0u && (g2::g_sdramBase + _offset) == m_ringProbePc)
						captureRing();
				}
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

				// A content write is one that is not the display clear, which
				// writes 0x20 and only 0x20. Counting 0x20 as content reports a
				// blank screen as a booted machine.
				const int shift = int(8u * (count - 1u - i));
				const uint8_t byte = uint8_t((_value >> shift) & 0xffu);

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
		// SCAFFOLD (temporary, reverted after the measurement). One snapshot of
		// the ring the scan at 0x30003982 was handed, taken at the fetch of the
		// armed PC. Register indices are the register FILE's own: 9 is a1, 10 is
		// a2, 12 is a4, 7 is d7 -- board.h states that mapping and this restates
		// none of it beyond the four it reads.
		void captureRing()
		{
			++m_ringHits;

			if(scaffoldBoard == nullptr || m_ringLog.size() >= m_ringProbeMax)
				return;

			const uint32_t a1 = scaffoldBoard->mcuReg(9);
			const uint32_t a2 = scaffoldBoard->mcuReg(10);
			const uint32_t a4 = scaffoldBoard->mcuReg(12);
			const uint32_t d7 = scaffoldBoard->mcuReg(7);

			const uint32_t head = peekWord(a2 + 0u);
			const uint32_t tail = peekWord(a2 + 2u);
			const uint32_t size = peekWord(a2 + 4u);
			const uint32_t endP = peekLong(a2 + 6u);
			const uint32_t bufP = peekLong(a2 + 10u);

			std::ostringstream o;
			o << "ring q=" << scaffoldQuantum
			  << " a2=" << hex32(a2)
			  << " a4=" << hex32(a4)
			  << " key=" << hex32(a4 & 0xffffu)
			  << " a1=" << hex32(a1)
			  << " d7=" << hex32(d7)
			  << " head=" << head
			  << " tail=" << tail
			  << " size=" << size
			  << " end=" << hex32(endP)
			  << " buf=" << hex32(bufP)
			  << " global[" << hex32(m_ringProbeGlobal) << "]=" << hex32(peekLong(m_ringProbeGlobal))
			  // SELF-CONSISTENCY POSITIVE, and it comes from the firmware's own
			  // arithmetic at 0x30003A16..0x30003A24: end == buf + size*12. If
			  // this does not hold, a2 is not a ring descriptor and every field
			  // above is a misread rather than a measurement.
			  << " end==buf+size*12:" << ((bufP + size * 12u) == endP ? "yes" : "no");

			// The ring's contents. The unit is ONE 12-BYTE RING SLOT; the loop
			// at 0x300039D8 compares slot word0 against the key, so word0 is
			// printed for every slot and the full 12 bytes for each.
			const uint32_t slots = size <= 64u ? size : 64u;
			for(uint32_t i = 0; i < slots; ++i)
			{
				const uint32_t slot = bufP + i * 12u;
				o << "\n  slot[" << i << "] @" << hex32(slot)
				  << " word0=" << hex32(peekWord(slot))
				  << " bytes=";
				for(uint32_t b = 0; b < 12u; ++b)
				{
					static const char* const digits = "0123456789abcdef";
					const uint32_t v = peekByte(slot + b);
					o << digits[(v >> 4) & 0xfu] << digits[v & 0xfu];
					if(b == 3u || b == 7u)
						o << ' ';
				}
				const bool live = (head <= tail) ? (i >= head && i < tail)
				                                 : (i >= head || i < tail);
				o << (i == head ? " <-head" : "") << (i == tail ? " <-tail" : "")
				  << (live ? " LIVE" : "");
			}

			m_ringLog.push_back(o.str());
		}

		uint32_t m_ringProbePc     = 0;   // SCAFFOLD
		uint32_t m_ringProbeGlobal = 0;   // SCAFFOLD
		size_t   m_ringProbeMax    = 0;   // SCAFFOLD
		uint64_t m_ringHits        = 0;   // SCAFFOLD
		std::vector<std::string> m_ringLog;  // SCAFFOLD

		std::vector<uint8_t>  m_bytes;
		std::vector<uint32_t> m_wordHits;
		std::vector<uint32_t> m_firstQuantum;  // SCAFFOLD
		uint64_t             m_oddWordReads  = 0;
		uint64_t             m_wordFetches   = 0;
		uint32_t             m_watchBase     = 0;
		uint32_t             m_watchLength   = 0;
		uint64_t             m_contentWrites = 0;
	};

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig()
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

		return config;
	}

	// ------------------------------------------------------ the impulse pattern
	//
	// t1_egress's two values: they differ from
	// each other so a chain that carried slot 0 into both slots fails rather
	// than passes, and neither is a power of two.
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	constexpr unsigned g_overrunQuanta = 1024u;

	/* ------------------------------- the arrival instrument's known positive
	 *
	 * Without it, `arrival=-1` is printed by an instrument nothing has ever
	 * shown a frame to, so a chain that carried nothing and an arrival path
	 * that could not report anything produce the same figure.
	 *
	 * The control places a sentinel at the tail position's transmit source and
	 * reads it back out of the codec sink, through the same `pull` and the same
	 * comparator the walk uses. Its sentinel is neither impulse word, so it
	 * cannot be mistaken for the measurement it qualifies, and bit 23 is clear
	 * so fromEsaiFrame's sign extension is the identity on it. */
	constexpr uint32_t g_sinkControlWord     = 0x2B6D51u;
	constexpr int32_t  g_sinkControlExpected = int32_t(g_sinkControlWord);

	static_assert((g_sinkControlWord & 0x800000u) == 0u,
		"the sentinel's sign bit must be clear, or fromEsaiFrame's sign extension moves it");
	static_assert(g_sinkControlExpected != g_impulseLeft && g_sinkControlExpected != g_impulseRight,
		"the control's sentinel must not be either impulse word");

	constexpr uint32_t g_esaiTransmitters  = 6u;
	constexpr unsigned g_sinkControlQuanta = 64u;
	constexpr dsp56k::TWord g_dmaTxChannel = 4u;

	/* The tail is found and not typed. The chain adapter's position and the
	 * hardware port are not the same number: dspSet.cpp binds
	 * audioTxCallback(position) to peripherals(portOfPosition[position]), and
	 * portOfPosition comes from the nine-entry table the firmware builds at
	 * 0x30116970. Entry i holds the CS1 address of the port at chain position
	 * i, and A3..A10 are eight ACTIVE-LOW one-cold selects, so the port number
	 * is the index of the single line pulled down. */
	unsigned portOfChainPosition(g2::Board& _board, const unsigned _wanted, const unsigned _count)
	{
		constexpr uint32_t g_portTableBase = 0x30116970u;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t entry =
				g2::Board::onRead(&_board, g_portTableBase + position * 4u, 4, &status);

			const uint8_t selects = uint8_t((entry >> 3) & 0xffu);
			const uint8_t low     = uint8_t(~selects);

			if(low == 0u || (low & uint8_t(low - 1u)) != 0u)
				continue;

			unsigned port = 0;
			for(uint8_t bit = low; bit > 1u; bit >>= 1)
				++port;

			if(position == _wanted && port < _count)
				return port;
		}

		return _count;
	}

	// A one-object `.pch2` whose single object is small enough for the endpoint
	// the model gives it. It carries no Clavia byte -- every byte of it is this
	// process's own.
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

	struct RunResult
	{
		bool     patchOffered   = false;
		size_t   patchBytes     = 0;
		unsigned patchObjects   = 0;
		g2::Pch2LoadResult loadResult = g2::Pch2LoadResult::Loaded;
		bool     loadReturned   = false;

		bool     booted         = false;
		bool     programsLanded = false;
		bool     halted         = false;
		bool     faulted        = false;
		uint32_t bootQuanta     = 0;
		unsigned dspCount       = 0;
		unsigned hopFrames      = 0;
		unsigned lookaheadFrames = 0;

		uint32_t windowPc       = 0;   // where the core sat when the window opened
		uint64_t windowPcHits   = 0;   // and how often the window read THAT address
		uint64_t windowFetches  = 0;
		uint64_t oddWordReads   = 0;

		// The known positive of this arm: the argmax of this arm's own histogram.
		// Each arm selects its own, because each arm is a separate run of the
		// machine and one arm's figure says nothing about the other.
		uint32_t knownPositiveAddr = 0;
		uint64_t knownPositiveHits = 0;

		// The population-matched known positive: the same argmax restricted to
		// the bytes the firmware image supplied. The probes are instruction
		// addresses inside that image, so this is the control that shares their
		// population.
		uint32_t imageBase           = 0;
		uint32_t imageEnd            = 0;
		uint32_t codeKnownPositiveAddr = 0;
		uint64_t codeKnownPositiveHits = 0;

		// One address supplied by the caller, measured on this arm. It is how the
		// two arms are compared at a common address: each arm's argmax is a
		// different address, so their counts are not comparable to each other.
		uint32_t crossAddr = 0;
		uint64_t crossHits = 0;

		// The three CS3 readings, in the order they are taken.
		// THE BOARD'S OWN ACCOUNT OF WHAT THE DEVICE DID WITH THE BYTES, read
		// off the Board that produced it. It is per-Board and needs no
		// subtraction: an earlier file-scope diagnostic pooled this arm with
		// the control arm and every reader had to undo the pooling by hand.
		g2::Board::UsbTransportStats usb;

		uint8_t  peekAfterHandover = 0;  // one quantum after pch2Load returned
		uint8_t  peekAfterWindow   = 0;  // g_observeQuanta later
		uint8_t  peekProbeObject   = 0;  // after a SMALL object goes the same way
		bool     probeLoaded       = false;

		uint64_t hitsKnownPositive = 0;
		uint64_t hitsKnownNegative = 0;
		uint64_t hitsTarget        = 0;
		uint64_t hitsCallerA       = 0;
		uint64_t hitsCallerB       = 0;
		uint64_t hitsAssembly      = 0;

		size_t   primedPulled   = 0;
		unsigned walkQuanta     = 0;
		int      arrival        = -1;
		bool     arrivalExact   = false;
		uint64_t nonZeroFrames  = 0;
		int32_t  firstNonZeroL  = 0;
		int32_t  firstNonZeroR  = 0;

		// The arrival instrument's known positive, run after the walk on the
		// same machine so it cannot move the measurement it qualifies.
		unsigned sinkControlPort    = 0;
		bool     sinkControlPortFound = false;
		unsigned sinkControlQuanta  = 0;
		int      sinkControlArrival = -1;
		bool     sinkControlExact   = false;
		int32_t  sinkControlL       = 0;
		int32_t  sinkControlR       = 0;
	};

	// Boots one machine, optionally hands it `_patch`, opens the observation
	// window and then walks the codec. Returns false only when the machine could
	// not be placed at all; a machine that ran and moved nothing returns true
	// with a result that says so, because "the machine is silent" is a
	// MEASUREMENT and must reach the assertions rather than a bail-out.
	// `_crossAddress` is 0 on the first arm and the FIRST arm's known positive
	// on the second, so the two arms can be compared at one common address.
	bool runOnce(const std::string& _directory, const std::vector<uint8_t>& _patch,
		const bool _deliver, const uint32_t _crossAddress, RunResult& _r)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig());
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

		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		// SCAFFOLD (temporary, reverted after the measurement). The probe reads
		// the register file, so it needs the machine; the Board outlives the Ram
		// in this scope and neither owns the other.
		ram.scaffoldBoard = &board;
		if(std::getenv("G2_RING_PROBE") != nullptr)
			ram.armRingProbe(g_ringProbePc, g_ringGlobal, 6u);

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

		_r.dspCount        = board.dspSet().dspCount();
		_r.hopFrames       = config.hopFrames;
		_r.lookaheadFrames = config.lookaheadFrames;

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

		// ------------------------------------------------- the patch hand-over
		//
		// The client is attached to the Board's own hub and to nothing else, so
		// what leaves it is drained by Board::pumpTransport at the next quantum
		// boundary and handed to the device with isp1181_rx. There is no second
		// path: this is the same `pch2Load` the plugin calls.
		{
			// The two inbox sizes bound the device-to-plugin direction only; the
			// bound on what may be originated is the hub's and the hub reports
			// it. 4096 clears the largest object in the corpus, measured at
			// 2492 bytes.
			g2::InternalClient client(board.transport(), 4096, 4);

			if(_deliver)
			{
				_r.patchOffered = true;
				_r.patchBytes   = _patch.size();
				_r.loadResult   = g2::pch2Load(_patch.data(), _patch.size(), client);
				_r.loadReturned = true;
			}

			// The pump, with no MCU cycle after it, and that ordering is the
			// measurement. Board::pumpTransport drains what pch2Load put in the
			// hub and hands each frame to the device with isp1181_rx; it is
			// public because tickSofIfDue calls it, and calling it directly
			// delivers the frame without letting the core run.
			//
			// Running one quantum first cannot tell two worlds apart: the pump
			// and the firmware's service both happen inside that quantum, so
			// "the packet arrived and the firmware drained it" and "the packet
			// never arrived" both leave the buffer empty and both read 0x00.
			// This reading is the arrival; the reading after the window is the
			// drain.
			board.pumpTransport();
			_r.peekAfterHandover = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// The window below expects a machine that has serviced the packet.
			scheduler->runFrames(1);

			// ------------------------------------------------ the window opens
			//
			// The address the core is sitting at at this instant. Recorded and
			// printed, but not the known positive: the two arms open their
			// windows in different places.
			_r.windowPc = board.mcuReg(g_regPc);

			// SCAFFOLD (temporary): the BOOT-INCLUSIVE histogram, dumped before
			// the clear. A region absent from the post-boot window is not
			// thereby a region that never ran; the clear masks boot by
			// construction. This is the unmasked companion.
			if(const char* const prefix = std::getenv("G2_EXEC_DUMP"))
			{
				ram.dumpExecuted(std::string(prefix) + (_deliver ? ".bootpatched" : ".bootcontrol"),
					g_entryPc, g_entryPc + uint32_t(code.size()));
			}

			// SCAFFOLD (temporary): the ISP1181 per-endpoint callback table the
			// ISR dispatches through, READ AFTER BOOT, straight out of the
			// backing store so the read does not disturb the histogram. Sixteen
			// longwords from 0x30119C62; entry k is endpoint k+1, so endpoint 3
			// is 0x30119C6A. Two same-read known positives accompany it.
			if(std::getenv("G2_CALLBACK_TABLE") != nullptr)
			{
				const char* const arm = _deliver ? "patched" : "control";

				for(uint32_t k = 0; k < 16u; ++k)
				{
					const uint32_t addr = 0x30119C62u + 4u * k;
					std::cout << "cbtable " << arm << " entry[" << k << "] endpoint " << (k + 1)
					          << " at " << hex32(addr) << " = " << hex32(ram.peekLong(addr))
					          << std::endl;
				}

				// KNOWN POSITIVE 1, same read path, same image: the operand of
				// the `pea 0x30053C38` that installs the handler. If this does
				// not read back as the ISR's own entry address, the read is
				// looking at the wrong address space and a table of zeros
				// proves nothing.
				std::cout << "cbtable " << arm << " KNOWN POSITIVE handler-install operand at "
				          << hex32(0x30055BF8u) << " = " << hex32(ram.peekLong(0x30055BF8u))
				          << " (expect " << hex32(0x30053C38u) << ")" << std::endl;

				// KNOWN POSITIVE 2: the first longword of the image, which this
				// run itself placed from CODE_30000400.bin.
				uint32_t head = 0;
				for(int b = 0; b < 4; ++b)
					head = (head << 8) | (b < int(code.size()) ? code[size_t(b)] : 0u);
				std::cout << "cbtable " << arm << " KNOWN POSITIVE image head at "
				          << hex32(g_entryPc) << " = " << hex32(ram.peekLong(g_entryPc))
				          << " (expect " << hex32(head) << ")" << std::endl;
			}

			// SCAFFOLD: the BOOT-INCLUSIVE reading, reported before the window's
			// arming clears it.
			if(std::getenv("G2_RING_PROBE") != nullptr)
			{
				const char* const arm = _deliver ? "patched" : "control";
				std::cout << "ringboot " << arm << " scanEntries=" << ram.ringHits() << std::endl;
				for(const auto& line : ram.ringLog())
					std::cout << "ringboot " << arm << " " << line << std::endl;
			}

			ram.resetProbes();

			if(std::getenv("G2_RING_PROBE") != nullptr)
				ram.armRingProbe(g_ringProbePc, g_ringGlobal, 24u);

			for(uint32_t i = 0; i < g_observeQuanta; ++i)
			{
				ram.scaffoldQuantum = i + 1u;  // SCAFFOLD: 1-based; 0 means never
				scheduler->runFrames(1);
			}

			const Ram::Hottest hottest = ram.hottest();

			_r.knownPositiveAddr = hottest.absolute;
			_r.knownPositiveHits = hottest.hits;

			_r.imageBase = g_entryPc;
			_r.imageEnd  = g_entryPc + uint32_t(code.size());

			// SCAFFOLD (temporary, reverted after the measurement).
			if(const char* const prefix = std::getenv("G2_EXEC_DUMP"))
			{
				ram.dumpExecuted(std::string(prefix) + (_deliver ? ".patched" : ".control"),
					_r.imageBase, _r.imageEnd);
			}

			const Ram::Hottest inImage = ram.hottestInRange(_r.imageBase, _r.imageEnd);

			_r.codeKnownPositiveAddr = inImage.absolute;
			_r.codeKnownPositiveHits = inImage.hits;

			_r.crossAddr = _crossAddress;
			_r.crossHits = _crossAddress != 0 ? ram.hitsAt(_crossAddress) : 0u;

			_r.windowFetches     = ram.wordFetches();
			_r.oddWordReads      = ram.oddWordReads();
			_r.windowPcHits      = ram.hitsAt(_r.windowPc);
			_r.hitsKnownPositive = hottest.hits;
			_r.hitsKnownNegative = ram.hitsAt(g_probeNegative);
			_r.hitsTarget        = ram.hitsAt(g_probeTarget);
			_r.hitsCallerA       = ram.hitsAt(g_probeCallerA);
			_r.hitsCallerB       = ram.hitsAt(g_probeCallerB);
			_r.hitsAssembly      = ram.hitsAt(g_probeAssembly);

			// SCAFFOLD (temporary, reverted after the measurement).
			if(std::getenv("G2_RING_PROBE") != nullptr)
			{
				const char* const arm = _deliver ? "patched" : "control";

				std::cout << "ring " << arm << " scanEntries=" << ram.ringHits() << std::endl;
				for(const auto& line : ram.ringLog())
					std::cout << "ring " << arm << " " << line << std::endl;

				// The scan's neighbourhood, as instruction-fetch counts inside
				// the window. The unit is ONE 16-BIT WORD FETCHED. Every one of
				// these is an address disassembled from the image this run
				// loaded, not a guess: the four call sites, the loop's match
				// target, the loop's no-match exit, and the caller's two arms.
				static const struct { uint32_t addr; const char* what; } probes[] =
				{
					{0x30003982u, "scanEntry"},
					{0x300039C8u, "scanEmptyBranch"},
					{0x300039D8u, "scanLoopHead"},
					{0x300039F0u, "scanLoadWord0"},
					{0x300039F6u, "scanMatchBranch"},
					{0x30003A2Eu, "scanMatchTarget"},
					{0x30003A0Au, "scanNoMatchExit"},
					{0x30004D5Eu, "callSite_4D5E"},
					{0x30004DAEu, "callSite_4DAE"},
					{0x30004E04u, "callSite_4E04"},
					{0x30004ED4u, "callSite_4ED4"},
					{0x30004E0Au, "caller4E04_afterCall"},
					{0x30004E12u, "caller4E04_foundArm"},
					{0x30004E1Eu, "caller4E04_allocArm"},
				};

				for(const auto& p : probes)
					std::cout << "ringfetch " << arm << " " << p.what << " " << hex32(p.addr)
					          << " hits=" << ram.hitsAt(p.addr) << std::endl;
			}

			// THE SECOND READING. If the first was non-zero and this one is
			// 0x00, the firmware TOOK the packet out during the window; if both
			// carry the same byte, it never did.
			_r.peekAfterWindow = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// ------------------------------------------- the wire's known positive
			//
			// A one-object container this process built, small enough for the
			// buffer the model gives the endpoint, delivered through the same
			// client, the same hub, the same pump and read back through the same
			// two bus writes and one bus read. It is what makes a 0x00 above a
			// measurement of the patch rather than of a dead wire on a booted
			// machine.
			{
				const std::vector<uint8_t> probeFile = buildProbeContainer();

				_r.probeLoaded =
					g2::pch2Load(probeFile.data(), probeFile.size(), client) == g2::Pch2LoadResult::Loaded;

				board.pumpTransport();

				_r.peekProbeObject = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

				scheduler->runFrames(1);
			}
		}

		_r.halted  = board.mcuHalted();
		_r.faulted = board.faulted();

		// ------------------------------------------------- the play transition
		scheduler->beginPlayPhase();

		{
			std::vector<g2::Frame> primed(_r.lookaheadFrames);
			_r.primedPulled = scheduler->pull(primed.data(), primed.size());
		}

		// ------------------------------------------------------------ the walk
		const unsigned expected = (_r.dspCount > 0 ? _r.dspCount - 1u : 0u) * _r.hopFrames;
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
					_r.arrivalExact  = out.slot[0] == g_impulseLeft && out.slot[1] == g_impulseRight;
					_r.firstNonZeroL = out.slot[0];
					_r.firstNonZeroR = out.slot[1];
				}
			}
		}

		_r.walkQuanta = walk;

		// ---------------------------- the arrival instrument's known positive
		//
		// The links it traverses: the tail DSP's X memory, its transmit DMA,
		// the ESAI transmit register file, ESAI frame assembly, the installed
		// WriteTxCallback (which is ChainAdapter::audioTxCallback(N-1)),
		// fromEsaiFrame, mailbox N, ChainAdapter::advanceAll,
		// extractCodecSink, CodecSink::push, Scheduler::pull, and the walk's
		// own two predicates.
		//
		// The links it does not: no DSP core executes any part of it, and
		// positions 0..N-2, every receive callback, the mailbox hop chain and
		// injectCodecSource are all upstream of the tail. It says the sink can
		// report a frame the tail transmitted; it says nothing about whether
		// anything reaches the tail.
		{
			const unsigned tailPort = portOfChainPosition(board, _r.dspCount - 1u, _r.dspCount);

			_r.sinkControlPortFound = tailPort < _r.dspCount;
			_r.sinkControlPort      = _r.sinkControlPortFound ? tailPort : 0u;

			if(_r.sinkControlPortFound)
			{
				dsp56k::Peripherals56311& p = board.dspSet().peripherals(_r.sinkControlPort);
				dsp56k::Esai&             tailEsai = p.getEsai();

				for(unsigned q = 0; q < g_sinkControlQuanta && _r.sinkControlArrival < 0; ++q)
				{
					++_r.sinkControlQuanta;

					const dsp56k::TWord enabled = tailEsai.hasEnabledTransmitters();

					for(uint32_t reg = 0; reg < g_esaiTransmitters; ++reg)
					{
						if(enabled & (1u << reg))
							tailEsai.writeTX(reg, g_sinkControlWord);
					}

					// And the buffer the transmit DMA refills that register
					// from. writeSlotToFrame copies the register file into the
					// slot and then triggers the transmit DMA, which is
					// serviced synchronously and overwrites the register before
					// the next slot is assembled, so a register-only injection
					// reaches one slot and the codec sink reads two. The window
					// is read off the DMA's own source register and the ESAI's
					// own transmit word count, never typed.
					{
						const dsp56k::TWord source     = p.getDMA().getDSR(g_dmaTxChannel);
						const dsp56k::TWord frameWords = tailEsai.getTxWordCount() + 1u;
						const dsp56k::TWord base       = source - (source % frameWords);

						dsp56k::Memory& tailMemory = board.dspSet().dsp(_r.sinkControlPort).memory();

						for(dsp56k::TWord i = 0; i < frameWords * 2u; ++i)
							tailMemory.set(dsp56k::MemArea_X, base + i, g_sinkControlWord);
					}

					(void) scheduler->push(&silence, 1);
					scheduler->runFrames(1);

					g2::Frame out{};

					if(scheduler->pull(&out, 1) == 0)
						continue;

					if(out.slot[0] == 0 && out.slot[1] == 0)
						continue;

					_r.sinkControlArrival = int(q);
					_r.sinkControlL       = out.slot[0];
					_r.sinkControlR       = out.slot[1];
					_r.sinkControlExact   = out.slot[0] == g_sinkControlExpected
						&& out.slot[1] == g_sinkControlExpected;
				}
			}
		}

		// LAST, SO IT COVERS EVERY QUANTUM THIS ARM RAN. The Board is still
		// alive here; the figures are its own and belong to this arm alone.
		_r.usb = board.usbTransport();

		return true;
	}

	void report(const char* const _label, const RunResult& _r)
	{
		std::cout << _label << ": patchOffered=" << (_r.patchOffered ? 1 : 0)
		          << " patchBytes=" << _r.patchBytes
		          << " loadResult="
		          << (_r.loadReturned ? g2::pch2LoadResultName(_r.loadResult) : "(not offered)")
		          << std::endl;
		std::cout << _label << ": bootQuanta=" << _r.bootQuanta
		          << " booted=" << (_r.booted ? 1 : 0)
		          << " programsLanded=" << (_r.programsLanded ? 1 : 0)
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0)
		          << " dspCount=" << _r.dspCount << std::endl;
		std::cout << _label << ": windowQuanta=" << g_observeQuanta
		          << " windowWordFetches=" << _r.windowFetches
		          << " oddWordReads=" << _r.oddWordReads << std::endl;

		std::cout << _label << ": windowOpenPc=" << hex32(_r.windowPc)
		          << " readsAtWindowOpenPc=" << _r.windowPcHits
		          << "  (this was the OLD known positive)" << std::endl;
		std::cout << _label << ": KNOWN POSITIVE, any address (most-read of this run) "
		          << hex32(_r.knownPositiveAddr) << " = " << _r.knownPositiveHits
		          << (_r.knownPositiveAddr >= _r.imageEnd || _r.knownPositiveAddr < _r.imageBase
		              ? "  (OUTSIDE the firmware image: a 16-bit DATA read, not an instruction fetch)"
		              : "  (inside the firmware image)")
		          << std::endl;
		std::cout << _label << ": KNOWN POSITIVE, in-image (image " << hex32(_r.imageBase)
		          << ".." << hex32(_r.imageEnd) << ") " << hex32(_r.codeKnownPositiveAddr)
		          << " = " << _r.codeKnownPositiveHits
		          << "  <- the probes' own population" << std::endl;
		std::cout << _label << ": probe " << hex32(g_probeNegative)
		          << " (known negative) = " << _r.hitsKnownNegative << std::endl;

		if(_r.crossAddr != 0)
			std::cout << _label << ": at the CONTROL arm's known positive "
			          << hex32(_r.crossAddr) << " this arm read " << _r.crossHits
			          << std::endl;
		std::cout << _label << ": probe " << hex32(g_probeTarget) << " = " << _r.hitsTarget
		          << " | " << hex32(g_probeCallerA) << " = " << _r.hitsCallerA
		          << " | " << hex32(g_probeCallerB) << " = " << _r.hitsCallerB
		          << " | " << hex32(g_probeAssembly) << " = " << _r.hitsAssembly << std::endl;
		std::cout << _label << ": cs3 peek endpoint " << g2::BoardConfig{}.usbProtocolEndpoint
		          << " -- afterHandover=0x" << std::hex << unsigned(_r.peekAfterHandover)
		          << " afterWindow=0x" << unsigned(_r.peekAfterWindow)
		          << " afterProbeObject=0x" << unsigned(_r.peekProbeObject)
		          << std::dec << " (probe object type " << hex32(g_probeObjectType)
		          << ", probeLoaded=" << (_r.probeLoaded ? 1 : 0) << ")" << std::endl;
		std::cout << _label << ": usb pumps=" << _r.usb.pumps
		          << " drained=" << _r.usb.drained
		          << " offered=" << _r.usb.offered
		          << " accepted=" << _r.usb.accepted
		          << " refused=" << _r.usb.refused
		          << " completed=" << _r.usb.completed
		          << " stallReports=" << _r.usb.stallReports
		          << " held=" << (_r.usb.held ? 1 : 0)
		          << " heldAttempts=" << _r.usb.heldAttempts
		          << " heldOffset=" << _r.usb.heldOffset
		          << "/" << _r.usb.heldSize
		          << std::endl;
		std::cout << _label << ": primedPulled=" << _r.primedPulled
		          << " walkQuanta=" << _r.walkQuanta
		          << " arrival=" << _r.arrival
		          << " arrivalExact=" << (_r.arrivalExact ? 1 : 0)
		          << " nonZeroFrames=" << _r.nonZeroFrames
		          << " firstNonZero=" << _r.firstNonZeroL << "/" << _r.firstNonZeroR
		          << std::endl;
		std::cout << _label << ": sinkControl tailPosition="
		          << (_r.dspCount > 0 ? _r.dspCount - 1u : 0u)
		          << " tailPort=" << (_r.sinkControlPortFound ? int(_r.sinkControlPort) : -1)
		          << " controlQuanta=" << _r.sinkControlQuanta
		          << " sinkControlArrival=" << _r.sinkControlArrival
		          << " sinkControlExact=" << (_r.sinkControlExact ? 1 : 0)
		          << " sinkControlValue=" << _r.sinkControlL << "/" << _r.sinkControlR
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

		// THE PACKET SIZE IS READ FROM BoardConfig AND NOT WRITTEN HERE. The
		// Board the arms below construct takes it from the same default, so a
		// change to that default moves the expectation and the machine
		// together instead of turning this file red.
		const size_t packetSize = g2::BoardConfig{}.usbMaxPacketBytes;

		unsigned objectCount    = 0;
		size_t   largestObject  = 0;
		uint8_t  firstType      = 0;
		unsigned packetsNeeded  = 0;
		unsigned exactMultiples = 0;
		measureObjects(patch, objectCount, largestObject, firstType,
			packetSize, packetsNeeded, exactMultiples);

		std::cout << "patch: " << patchPath << " (" << patch.size() << " bytes, "
		          << objectCount << " objects, largest framed object "
		          << largestObject << " bytes, first object type "
		          << hex32(firstType) << ")" << std::endl;

		std::cout << "patch: at a " << packetSize << "-byte max packet size the "
		          << objectCount << " objects take " << packetsNeeded
		          << " packets, and " << exactMultiples
		          << " of them have a length that is an exact multiple of it"
		          << std::endl;

		// THE LARGEST OBJECT AGAINST THE PART'S OWN CEILING, ASSERTED SO THAT
		// THE SPLIT CANNOT BE ARGUED AWAY AS AN ARTEFACT OF ONE CONFIGURATION.
		// ISP1362 Rev. 06 Table 16 (p.52) gives a non-isochronous endpoint
		// exactly four legal buffer sizes -- 8, 16, 32 and 64 bytes, with
		// `0100` to `1111` reserved -- and Table 109 (p.105) states the same
		// bound as "interrupt/bulk: N <= 64 bytes". So no DcEndpointConfiguration
		// byte exists that would make this object deliverable in one packet,
		// and the only question the split answers is whether this stack cuts
		// it up or drops it.
		check(largestObject > packetSize,
			std::string("this container really does carry an object no legal bulk"
			            " endpoint could take whole: largest framed object ")
			+ std::to_string(largestObject) + " bytes against a "
			+ std::to_string(packetSize) + "-byte maximum packet");

		// ---------------------------------------------------------- the control
		//
		// The unpatched run is first and it runs the identical code path. It is
		// what makes any probe count in the patched run a statement about the
		// patch rather than about the firmware's ordinary idle.
		RunResult control;
		if(!runOnce(directory, patch, false, 0u, control))
			return false;
		report("control", control);

		// -------------------------------------------------------- the measurement
		//
		// The control's known positive is handed to the patched arm so the two
		// can be read at one COMMON address. Each arm still selects its own.
		RunResult patched;
		if(!runOnce(directory, patch, true, control.knownPositiveAddr, patched))
			return false;
		report("patched", patched);

		// The instrument, asserted before anything is read off it.
		check(control.windowFetches > 0,
			"the fetch counter saw instruction words at all during the control window");
		check(control.hitsKnownNegative == 0,
			"KNOWN NEGATIVE: an address inside the vector table is never fetched as an instruction word");
		check(patched.hitsKnownNegative == 0,
			"KNOWN NEGATIVE, patched run: the vector-table address is still never fetched");

		// ------------------------------------------------- the sensitivity gate
		//
		// A floor and not a presence test, asserted per arm. A `> 0` gate passes
		// on a known positive of 1, and every zero on such an arm is then read
		// against a counter that fired once.
		//
		// The floor is a property of this file and the address is not: the file
		// may say how large a known positive has to be, not where that count is
		// to be found. This value fires on an instrument that has gone blind and
		// not on one that has merely moved.
		constexpr uint64_t g_sensitivityFloor = 1000u;

		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			const std::string label = run.first;

			check(run.second.knownPositiveHits >= g_sensitivityFloor,
				label + ": KNOWN POSITIVE, the most-read address of this run ("
				+ hex32(run.second.knownPositiveAddr) + "), is read at least "
				+ std::to_string(g_sensitivityFloor)
				+ " times, so a zero on this arm is read against a real count; observed "
				+ std::to_string(run.second.knownPositiveHits));

			// The one the probes are actually weighed against. The unrestricted
			// argmax can be satisfied by a hot DATA read; this one cannot,
			// because it is confined to the bytes the image supplied.
			check(run.second.codeKnownPositiveHits >= g_sensitivityFloor,
				label + ": KNOWN POSITIVE, in-image, the most-read address inside the loaded"
				" firmware image (" + hex32(run.second.codeKnownPositiveAddr)
				+ "), is read at least " + std::to_string(g_sensitivityFloor)
				+ " times, so a zero at an instruction address on this arm is read against a"
				" count from the SAME population; observed "
				+ std::to_string(run.second.codeKnownPositiveHits));
		}

		// The preconditions of the measurement, asserted before the measurement,
		// so that a silent machine is reported as the machine's failure and not
		// as the patch's.
		// ---------------- the arrival instrument's known positive, both runs
		//
		// Without a control, a chain that carried nothing and an arrival path
		// that could not report anything both print `arrival=-1`, and nothing
		// here tells them apart. Both runs are asserted because both print an
		// arrival figure, and a control that held on one run would say nothing
		// about the other.
		//
		// Every field is pinned. The port is the one the firmware's table puts
		// at chain position N-1 and is not the position number -- on this
		// machine it is 0 against a position of 7, and a control that assumes
		// otherwise drives chain position 1 and reports a dead path on a
		// healthy machine. The arrival is quantum 0 because the tail writes
		// mailbox N in the same quantum the egress phase reads it. Both slots
		// carry the sentinel, so neither line of extractCodecSink is passing a
		// value the other one supplied.
		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			const std::string label = run.first;

			check(run.second.sinkControlPortFound,
				label + ": the firmware's port table names a port at chain position "
				+ std::to_string(run.second.dspCount - 1u));
			check(run.second.sinkControlArrival == 0,
				label + ": the sink control arrived on the first control quantum; observed "
				+ std::to_string(run.second.sinkControlArrival));
			check(run.second.sinkControlExact,
				label + ": the sink control arrived unchanged in BOTH codec slots; observed "
				+ std::to_string(run.second.sinkControlL) + "/"
				+ std::to_string(run.second.sinkControlR) + " against "
				+ std::to_string(g_sinkControlExpected));
		}

		check(control.programsLanded, "control: every DSP position took its program");
		check(patched.programsLanded, "patched: every DSP position took its program");
		check(!patched.halted,  "patched: the core is not halted when the window closes");
		check(!patched.faulted, "patched: the board reports no fault when the window closes");

		// -------------------------------------------------- the load is accepted
		//
		// A real `.pch2` is accepted by the Board's own transport hub. Every
		// other measurement in this file is downstream of it: a load that was
		// refused originated some frames and not others, and a probe count taken
		// after a partial hand-over is a count about neither machine.
		check(patched.loadResult == g2::Pch2LoadResult::Loaded,
			std::string("a real 18-object `.pch2` loads through the BOARD'S OWN hub into the running"
			            " machine; pch2Load answered ") +
			g2::pch2LoadResultName(patched.loadResult));

		// ------------------------------------------- the wire, on a booted machine
		//
		// The known positive for every 0x00 reported above. Same board, same
		// client, same hub, same pump, same three bus calls -- only the object
		// is different, and the difference is its size.
		check(patched.probeLoaded, "the one-object probe container loads through the same hub");
		check(control.probeLoaded, "the one-object probe container loads on the control run too");

		// The known positive is taken on the control run and only there, and
		// that is a property of the instrument rather than a weakening of it.
		// The peek reads the head of the endpoint's OUT FIFO, and on the control
		// run the FIFO is empty when the probe object arrives, so the head is
		// the probe object -- with no firmware service in the story at all. The
		// patched run's own reading is reported in the verdict below rather than
		// asserted here, because on that run the head depends on whether the
		// firmware has serviced the patch packet yet, and that is the thing this
		// file is measuring rather than a precondition it may assume.
		check(control.peekProbeObject == g_probeObjectType,
			std::string("KNOWN POSITIVE, on a BOOTED machine: a small object delivered through the"
			            " same client, hub, pump and bus calls is readable at the CS3 data port as"
			            " its own type byte ") + hex32(g_probeObjectType) + "; read " +
			hex32(control.peekProbeObject));

		// And the control that makes that known positive mean something: on the
		// run where no patch was offered, the same reading before the probe
		// object is the benign 0x00.
		check(control.peekAfterHandover == 0x00u,
			"CONTROL: with no patch offered, the endpoint buffer is empty at the same instant");

		// The property this file exists to hold. A byte of a real `.pch2` is in
		// the device register file of a machine that has really booted. The
		// expected value is computed from the file this run read and is not
		// written here as a literal, so the case cannot pass against a device
		// that answers a fixed byte that happens to match.
		check(patched.peekAfterHandover == firstType,
			std::string("a byte of a REAL `.pch2` is readable at the CS3 data port of a BOOTED"
			            " machine as the patch's own first object type ") + hex32(firstType) +
			"; read " + hex32(patched.peekAfterHandover));

		// ---------------------------------------- the transport loses NOTHING
		//
		// THE NO-LOSS INVARIANT, ASSERTED AND NOT NARRATED. Every frame this
		// Board took out of the hub sits in the device, is still held for
		// another offer, or is counted undeliverable; nowhere else. That is the
		// whole content of the repair, and before it these two cases were BOTH red:
		// the pump handed each drained frame to `isp1181_rx`, discarded the
		// answer, and 17 of 19 frames on this arm vanished with every visible
		// signal reading healthy.
		//
		// THEY ARE ASSERTED ON BOTH ARMS. A control arm that offers one frame
		// and a patched arm that offers tens of thousands are the same
		// invariant, and an arithmetic slip that held only for the busy arm
		// would be a real defect.
		for(const RunResult* const arm : { &control, &patched })
		{
			const g2::Board::UsbTransportStats& u = arm->usb;

			check(u.offered == u.accepted + u.refused,
				std::string("every offer to the device is either accepted or refused: ")
				+ std::to_string(u.offered) + " == " + std::to_string(u.accepted)
				+ " + " + std::to_string(u.refused));

			/* THE INVARIANT READS `completed` AND NOT `accepted`, AND THAT IS
			 * A UNIT REPAIR RATHER THAN A WEAKENING. `accepted` counts
			 * PACKETS since pumpTransport began splitting a frame into
			 * max-packet-size pieces, and `drained` has always counted FRAMES;
			 * the old form compared the two directly and went red on this arm
			 * the moment the split landed -- 19 frames left the hub against 70
			 * accepted packets. `completed` is incremented exactly once, when a
			 * frame's LAST packet is taken, so it is the frame-shaped figure
			 * this equality needs. `undeliverable` is the third destination a
			 * drained frame can reach: it left the hub and was never offered.
			 * board.h states the same thing at the declaration. */
			check(u.drained == u.completed + u.undeliverable + (u.held ? 1u : 0u),
				std::string("NOTHING DRAINED IS LOST: ") + std::to_string(u.drained)
				+ " frames left the hub, " + std::to_string(u.completed)
				+ " crossed to the device whole, " + std::to_string(u.undeliverable)
				+ " were too large to offer and " + (u.held ? "1 is" : "0 are")
				+ " still held for another offer");

			// Separate from the invariant on purpose. The invariant stays true
			// whatever this reads, and this says the hub's size guarantee held:
			// a frame too large for the hold buffer should never reach the
			// drain at all.
			check(u.undeliverable == 0u,
				std::string("no frame left the hub too large to be offered to the device: ")
				+ std::to_string(u.undeliverable));
		}

		// ------------------------------------ HOW MUCH OF THE PATCH ARRIVED
		//
		// THE QUESTION THIS WHOLE CHANGE EXISTS TO ANSWER, ASSERTED WITH ITS
		// UNIT NAMED. ONE unit here is ONE PROTOCOL FRAME the hub handed the
		// Board -- one `.pch2` object, plus the one-object probe container the
		// arm also loads -- and NOT one packet, one object of the file, or one
		// byte. Before the split this arm drained 2 frames and completed 1;
		// the other 17 objects never left the hub, because a frame the device
		// would not take blocked the drain behind it for the rest of the run.
		//
		// IT IS A FRACTION AND IT IS ASSERTED AS ONE. A count on its own would
		// go green on a run that pushed fewer frames, which is the failure this
		// project has paid for before: the denominator is what the arm really
		// offered, read from the same struct as the numerator.
		{
			const g2::Board::UsbTransportStats& u = patched.usb;

			check(u.drained > 0,
				std::string("frames really left the hub on the patched arm, so the"
				            " fraction below is read against a real denominator; ")
				+ std::to_string(u.drained) + " frames");

			check(u.completed == u.drained && !u.held,
				std::string("EVERY frame the hub handed this Board reached the device"
				            " WHOLE: ") + std::to_string(u.completed) + " of "
				+ std::to_string(u.drained) + " frames completed, "
				+ std::to_string(u.accepted) + " packets accepted and "
				+ std::to_string(u.refused) + " NAKed and retried, "
				+ (u.held ? "1 frame is still held" : "nothing is still held"));

			std::cout << "measurement: " << u.completed << " of " << u.drained
			          << " protocol frames crossed to the firmware whole, in "
			          << u.accepted << " packets; " << u.refused
			          << " packets were NAKed and re-offered, and "
			          << u.stallReports << " frames outlived the NAK retry ceiling"
			          << std::endl;
		}

		// -------------------------------------------------------- the verdict lines
		//
		// Reported and not asserted, deliberately. Each names a state of the
		// machine as it is now, and an assertion on it would go red on the day
		// the machine improves.
		std::cout << "verdict: the firmware "
		          << (patched.peekAfterWindow == patched.peekAfterHandover
		              ? "NEVER TOOK the packet out of the endpoint buffer"
		              : "TOOK the packet out of the endpoint buffer")
		          << " across " << g_observeQuanta << " quanta" << std::endl;

		// The two arms are compared at a common address and not at their own.
		// Each arm's known positive is a different address, so comparing the two
		// arms' known-positive counts compares two different things and would
		// report "DIFFERENT" for a machine that did the same work.
		std::cout << "verdict: the MCU instruction-fetch stream is "
		          << (patched.windowFetches == control.windowFetches &&
		              patched.crossHits == control.knownPositiveHits
		              ? "IDENTICAL with and without the patch, so the patch changed nothing the core did"
		              : "DIFFERENT with and without the patch")
		          << std::endl;

		std::cout << "verdict: after a SECOND object was delivered, the head of the endpoint buffer is "
		          << hex32(patched.peekProbeObject) << " on the patched run and "
		          << hex32(control.peekProbeObject) << " on the control run"
		          << std::endl;

		std::cout << "verdict: routine " << hex32(g_probeTarget) << " "
		          << (patched.hitsTarget > 0 ? "FIRED" : "DID NOT FIRE")
		          << " on the patched run and "
		          << (control.hitsTarget > 0 ? "FIRED" : "DID NOT FIRE")
		          << " on the control run" << std::endl;

		// The scale every zero above is read against, per arm and never pooled:
		// quoting one arm's known positive beside the other arm's zero measures
		// nothing.
		for(const std::pair<const char*, const RunResult&> run :
			{std::pair<const char*, const RunResult&>{"control", control},
			 std::pair<const char*, const RunResult&>{"patched", patched}})
		{
			std::cout << "verdict: on the " << run.first << " arm a zero at "
			          << hex32(g_probeTarget)
			          << " is weighed against an IN-IMAGE known positive of "
			          << run.second.codeKnownPositiveHits << " at "
			          << hex32(run.second.codeKnownPositiveAddr)
			          << " (and an any-address known positive of "
			          << run.second.knownPositiveHits << " at "
			          << hex32(run.second.knownPositiveAddr) << "), out of "
			          << run.second.windowFetches << " word reads in the window" << std::endl;
		}

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
