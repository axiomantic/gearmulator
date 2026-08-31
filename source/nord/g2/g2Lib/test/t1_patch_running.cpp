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
//   known positive   the address the machine itself is sitting at when the
//                    window opens, read off Board::mcuReg(17) and installed as
//                    a probe at that instant. It is not chosen by this file,
//                    and it is fetched through the identical counter.
//   known negative   an address inside the vector table. Vectors are read as
//                    32-bit longwords and never fetched as instruction words,
//                    so the same counter must read 0 there.
//
// And the control that makes the answer an answer: the whole run happens twice
// on the same code path, once without a patch and once with one. A probe count
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

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
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

	// The largest framed object in a `.pch2`, counting its 3-byte header, and
	// how many objects it holds. Both are computed from the file this run loaded
	// and neither is written here as a literal, so a different patch reports its
	// own figures.
	void measureObjects(const std::vector<uint8_t>& _file, unsigned& _count, size_t& _largest,
		uint8_t& _firstType)
	{
		_count     = 0;
		_largest   = 0;
		_firstType = 0;

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

	// The known negative. An address inside the vector table this file writes.
	// Vectors are read as 32-bit longwords, never fetched as instruction words,
	// so the 16-bit counter must read 0 there. It is offset 4 rather than 0 so
	// that it is not the reset vector either.
	constexpr uint32_t g_probeNegative = g_vectorTableBase + 4u;

	// How many quanta the machine runs after the patch is handed over. A
	// 4000-tick deferred rebuild timer schedules the routine under test, so a
	// window shorter than that could report a routine that had not been given
	// the chance to run. This is more than ten times it.
	constexpr uint32_t g_observeQuanta = 50000u;

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		// One instruction-fetch counter. `absolute` is an SDRAM address; the
		// counter fires on a 16-bit read at exactly that offset.
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

		// Zeroes every counter, so that a window's counts are the window's and
		// not the boot's.
		void resetProbes()
		{
			for(Probe& p : m_probes)
				p.hits = 0;
			m_wordFetches = 0;
		}

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

				for(Probe& p : m_probes)
				{
					if(p.offset == _offset)
						++p.hits;
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
		std::vector<uint8_t> m_bytes;
		std::vector<Probe>   m_probes;
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

		uint32_t windowPc       = 0;   // the known positive's address
		uint64_t windowFetches  = 0;

		// The three CS3 readings, in the order they are taken.
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
	// measurement and must reach the assertions rather than a bail-out.
	bool runOnce(const std::string& _directory, const std::vector<uint8_t>& _patch,
		const bool _deliver, RunResult& _r)
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

		const size_t iNegative = ram.addProbe(g_probeNegative);
		const size_t iTarget   = ram.addProbe(g_probeTarget);
		const size_t iCallerA  = ram.addProbe(g_probeCallerA);
		const size_t iCallerB  = ram.addProbe(g_probeCallerB);
		const size_t iAssembly = ram.addProbe(g_probeAssembly);

		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

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
			// The known positive is read off the machine here, not chosen above:
			// it is the address the core is sitting at at this instant, so it is
			// an address the machine itself demonstrably reaches.
			_r.windowPc = board.mcuReg(g_regPc);

			const size_t iPositive = ram.addProbe(_r.windowPc);

			ram.resetProbes();

			for(uint32_t i = 0; i < g_observeQuanta; ++i)
				scheduler->runFrames(1);

			_r.windowFetches     = ram.wordFetches();
			_r.hitsKnownPositive = ram.probe(iPositive).hits;
			_r.hitsKnownNegative = ram.probe(iNegative).hits;
			_r.hitsTarget        = ram.probe(iTarget).hits;
			_r.hitsCallerA       = ram.probe(iCallerA).hits;
			_r.hitsCallerB       = ram.probe(iCallerB).hits;
			_r.hitsAssembly      = ram.probe(iAssembly).hits;

			// The second reading. If the first was non-zero and this one is
			// 0x00, the firmware took the packet out during the window; if both
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
		          << " windowPc=" << hex32(_r.windowPc)
		          << " windowWordFetches=" << _r.windowFetches << std::endl;
		std::cout << _label << ": probe " << hex32(_r.windowPc)
		          << " (known positive) = " << _r.hitsKnownPositive
		          << " | probe " << hex32(g_probeNegative)
		          << " (known negative) = " << _r.hitsKnownNegative << std::endl;
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

		unsigned objectCount   = 0;
		size_t   largestObject = 0;
		uint8_t  firstType     = 0;
		measureObjects(patch, objectCount, largestObject, firstType);

		std::cout << "patch: " << patchPath << " (" << patch.size() << " bytes, "
		          << objectCount << " objects, largest framed object "
		          << largestObject << " bytes, first object type "
		          << hex32(firstType) << ")" << std::endl;

		// ---------------------------------------------------------- the control
		//
		// The unpatched run is first and it runs the identical code path. It is
		// what makes any probe count in the patched run a statement about the
		// patch rather than about the firmware's ordinary idle.
		RunResult control;
		if(!runOnce(directory, patch, false, control))
			return false;
		report("control", control);

		// -------------------------------------------------------- the measurement
		RunResult patched;
		if(!runOnce(directory, patch, true, patched))
			return false;
		report("patched", patched);

		// The instrument, asserted before anything is read off it.
		check(control.windowFetches > 0,
			"the fetch counter saw instruction words at all during the control window");
		check(control.hitsKnownPositive > 0,
			"KNOWN POSITIVE: the address the machine itself was sitting at is counted by the same probe");
		check(control.hitsKnownNegative == 0,
			"KNOWN NEGATIVE: an address inside the vector table is never fetched as an instruction word");
		check(patched.hitsKnownPositive > 0,
			"KNOWN POSITIVE, patched run: the same probe fires there too");
		check(patched.hitsKnownNegative == 0,
			"KNOWN NEGATIVE, patched run: the vector-table address is still never fetched");

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

		std::cout << "verdict: the MCU instruction-fetch stream is "
		          << (patched.windowFetches == control.windowFetches &&
		              patched.hitsKnownPositive == control.hitsKnownPositive
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

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
