// The test console.
//
// `g2TestConsole --boot` boots the Clavia OS image directly at 0x30000400 and
// prints display 0's 32 character cells. It asserts nothing; this program is
// the operator-facing window onto the same boot, and its output is meant to be
// read by a person bringing the machine up.
//
// Every address and every window below has the same provenance as
// `g2Lib/test/t1_boot.cpp` and is documented there. Two of them -- CS0's base
// and CS4's base -- are invented by this harness because no authority records
// them, and they are labelled at their site rather than presented as
// measured.

#include "board.h"
#include "executor.h"
#include "gdbStub.h"
#include "memoryMap.h"
#include "scheduler.h"
#include "status.h"
#include "artifactResolver.h"

#include "impulseOutcome.h"

// The DMA dump reads the registers of each position's peripheral set.
// board.h already reaches dma.h through dspSet.h and peripherals56311.h; the
// include is written out because this file names dsp56k::Dma and dsp56k::TWord.
#include "dsp56kEmu/dma.h"

#include "dsp56kBase/logging.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	// ------------------------------------------------ the ESAI underrun log filter
	//
	// dsp56kEmu's Esai::writeSlotToFrame calls LOG() once per transmit slot
	// whose data was never written, nothing drains the ESAIs until the codec
	// queues arrive, and this program turns hundreds of thousands of Scheduler
	// frames. Without the filter the two display lines a person ran this program
	// to read are buried.
	//
	// It hides the repetition of a real and expected condition and nothing else.
	// A quiet run is not evidence that the ESAIs are being drained; once the
	// codec queues arrive, a run that still reports underruns is reporting a
	// defect, and the kept lines are what makes that visible. Set
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

	// The count is reported rather than discarded, so "the log was silenced"
	// stays a statement about volume and not about evidence.
	void reportSuppressedLogLines()
	{
		const uint64_t seen = g_underrunLines.load();

		if(seen <= g_underrunLinesKept)
			return;

		std::cout << "note " << (seen - g_underrunLinesKept) << " further \""
		          << g_underrunMessage << "\" lines were suppressed; set "
		             "G2_LOG_ESAI_UNDERRUN to see every one of them"
		          << std::endl;
	}

	// ---------------------------------------------------------- the boot map

	// The display buffer base, confirmed at 0x30057040 as
	// `addil #808062392,%d0`.
	constexpr uint32_t g_displayBase   = 0x302A0DB8u;
	constexpr uint32_t g_displayStride = 298u;
	constexpr uint32_t g_lineWidth     = 16u;

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	// The register indices of the mcf5307 C ABI. 17 is the program counter, 18
	// is the vector base register.
	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	// The size a byte access presents to Board::onRead, in the core's unit.
	// mcf5307.h states it twice, once per callback typedef: `size` is a count of
	// bytes and never a width in bits.
	constexpr int g_byte = 1;

	// Measured: the loader's `movel #0x10000001,%d0` /
	// `movec %d0,%mbar` at loader offset 0x1E. The OS never writes MBAR, so a
	// direct boot of the OS image makes this the harness's job.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// Measured: CSAR2 = $1200 and CSMR2 = $007F0001 at
	// loader offsets 0x70 and 0x7c give 0x12000000..0x127FFFFF.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

	// Invented by this harness. No authority records CS0's or CS4's base, and
	// both are still open. Neither value below is a measurement and neither may
	// be copied into a shipped header.
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;

	// Measured from the workspace logbook: CS3 is a 64 KiB window,
	// derived from CSMR3 at 0x100000A8. The OS touches only 0x13000000 and
	// 0x13000010 inside it.
	constexpr uint32_t g_cs3Size = 0x00010000u;

	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;
	constexpr uint32_t g_sdramSize = 0x00800000u;

	// ------------------------------------------------------- the vector table
	//
	// Measured from the OS image: CODE_30000400.bin at 0x30058218 builds 256
	// identical longwords at 0x30000000 and points VBR at that base. Booting
	// CODE directly skips the code that would set it up, so the harness supplies
	// it.
	//
	// It is a floor and not a fix. The firmware has filled the table itself by
	// the time the first exception is taken, so omitting the table leaves the
	// run identical -- what is load-bearing is VBR. The table holds only for an
	// exception taken before the firmware's own fill.
	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	// ------------------------------------------------ what "display content" means
	//
	// The byte the display clear writes. Measured: the OS clears all four
	// displays to spaces before it composes anything, and 0x20 is the byte it
	// stores. It is named because it is the one value a "the firmware composed
	// something" predicate must refuse to be satisfied by: a predicate reading
	// `>= 0x20` is satisfied by a blank screen.
	constexpr uint8_t g_clearByte = 0x20u;

	// The G2 display does not hold ASCII. The display helper at 0x30056FEA is a
	// table-translating copy, and the firmware remaps these entries onto
	// the CGRAM alias range 0x08..0x0C --
	//
	//     'g' -> 0x08   'p' -> 0x09   'q' -> 0x0A   'y' -> 0x0B   'j' -> 0x0C
	//
	// -- the descenders, whose glyph bitmaps it then uploads. A cell
	// holding 0x09 is a correctly displayed 'p'. `Version 1.62 Exp` contains
	// one, so a console without this decode misreports a correct machine.
	constexpr uint8_t g_cgramFirst = 0x08u;
	constexpr uint8_t g_cgramLast  = 0x0Cu;

	// The CGRAM slot for each remapped character, and its inverse. Both are
	// written out rather than derived from each other, so a typo in one does not
	// silently agree with the other.
	constexpr char cgramToAscii(const uint8_t _byte)
	{
		return _byte == 0x08u ? 'g'
		     : _byte == 0x09u ? 'p'
		     : _byte == 0x0Au ? 'q'
		     : _byte == 0x0Bu ? 'y'
		     : _byte == 0x0Cu ? 'j'
		     : char(_byte);
	}

	constexpr bool isCgramGlyph(const uint8_t _byte)
	{
		return _byte >= g_cgramFirst && _byte <= g_cgramLast;
	}

	// A cell is content if it is an ordinary printable byte, or one of the
	// CGRAM glyphs. The second clause is what stops a line whose only printable
	// character is a descender from counting as blank.
	constexpr bool isDisplayContent(const uint8_t _byte)
	{
		return (_byte > g_clearByte && _byte < 0x7fu) || isCgramGlyph(_byte);
	}

	// ------------------------------------------------------------ the drive bounds

	/* The iteration bound is a stop so that a machine which never converges
	 * terminates rather than hanging; it is not a figure the firmware publishes.
	 * The real boot needs roughly 425,000 iterations to reach the patch browser,
	 * and the firmware's own handshake retry count of 0xFDE8 is far too small.
	 * Sized above that, and the run costs roughly ninety seconds. It is the same
	 * figure `t1_boot` uses, deliberately: the two programs boot the same
	 * firmware and a console that gave up earlier than the harness would report
	 * a failure the harness does not see. */
	constexpr uint32_t g_iterations = 500000u;

	/* One scheduler frame per iteration, and it is the whole drive. There is one
	 * core, the Board's, so the frame turns the DSP set, the chain, the panel and
	 * the MCU. No separate mcf5307_exec budget may be applied to the same core:
	 * it would double-count the cycles the scheduler already allocated, and those
	 * cycles feed the timers, so double-counting them would falsify a timer
	 * tick. */
	constexpr size_t g_framesPerIteration = 1;

	/* How long the firmware is given after the first printable character. The
	 * banner is written a character at a time, so a capture on the first content
	 * byte catches a partial line -- the measured symptom in `t1_boot` was line 0
	 * reading "Nord" and stopping. The latch therefore waits until the display
	 * has been quiet for this many iterations. */
	constexpr uint32_t g_bannerSettleIterations = 20000u;

	// The SDRAM the firmware executes from. board.cpp attaches its units and
	// leaves Region::Sdram with no target on purpose, so the store is the
	// harness's to supply. Big-endian, matching the part.
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		/* The banner observation lives here, on the write, and not on the bytes
		 * that survive. Every SDRAM access the core makes arrives at this object,
		 * so this is the firmware's own write path.
		 *
		 * A predicate that reads cells back asks whether a value is present,
		 * which is green whenever the bytes happen to be right, whoever put them
		 * there. A predicate over the write asks whether the firmware performed
		 * the transaction. It separates the two byte values that can arrive
		 * rather than counting writes, because "the cells were written" is
		 * exactly what the display clear does and exactly what must not count as
		 * a banner. */
		void watchCells(const uint32_t _offset, const uint32_t _length)
		{
			m_watchOffset   = _offset;
			m_watchLength   = _length;
			m_contentWrites = 0;
			m_clearWrites   = 0;
		}

		uint32_t contentWrites() const { return m_contentWrites; }
		uint32_t clearWrites() const { return m_clearWrites; }

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
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
				const auto byte = uint8_t((_value >> shift) & 0xffu);
				m_bytes[index] = byte;
				observeWrite(index, byte);
			}
		}

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

	private:
		// One byte that landed in the store, classified. A byte outside the
		// watched span is not a display cell and counts as neither.
		void observeWrite(const size_t _index, const uint8_t _byte)
		{
			if(m_watchLength == 0)
				return;
			if(_index < size_t(m_watchOffset) || _index >= size_t(m_watchOffset) + m_watchLength)
				return;

			if(isDisplayContent(_byte))
				++m_contentWrites;
			else if(_byte == g_clearByte)
				++m_clearWrites;
		}

		std::vector<uint8_t> m_bytes;

		uint32_t m_watchOffset   = 0;
		uint32_t m_watchLength   = 0;
		uint32_t m_contentWrites = 0;
		uint32_t m_clearWrites   = 0;
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

	// Reads through Board::onRead, which is the exact callback the Board hands
	// to mcf5307_create and therefore the path the core itself takes. The
	// CGRAM glyphs are decoded back to the characters they render; every other
	// byte passes through untouched, so a genuinely wrong cell still reads as
	// whatever it actually is.
	std::string readDisplayLine(g2::Board& _board, const uint32_t _display, const uint32_t _line)
	{
		const uint32_t base = g_displayBase + _display * g_displayStride + _line * g_lineWidth;

		std::string out;
		out.reserve(g_lineWidth);

		for(uint32_t col = 0; col < g_lineWidth; ++col)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t byte = g2::Board::onRead(&_board, base + col, g_byte, &status);
			out.push_back(cgramToAscii(uint8_t(byte & 0xffu)));
		}

		return out;
	}

	std::string escapedLine(const std::string& _line)
	{
		std::string out = "\"";
		for(const char c : _line)
		{
			const auto u = uint8_t(c);
			if(u >= 0x20 && u < 0x7f)
			{
				out.push_back(c);
				continue;
			}
			char buf[8];
			std::snprintf(buf, sizeof buf, "\\x%02X", unsigned(u));
			out += buf;
		}
		out += "\"";
		return out;
	}

	bool anyContent(const std::string& _line)
	{
		for(const char c : _line)
		{
			// The line has already been CGRAM-decoded, so a descender reads as
			// its ASCII letter here and the raw-cell clause is not needed.
			if(uint8_t(c) > g_clearByte && uint8_t(c) < 0x7fu)
				return true;
		}
		return false;
	}

	// ------------------------------------------------------------ the DMA check
	//
	// The constants below are read off the kernel's own MOVEP block:
	//
	//   Channel | DSR            | DDR                          | DCO
	//   2       | $FFFFA8 ESAI   | $001C00, $001C04 at the head | $007001, $001001 at the head
	//   4       | $001D00        | $FFFFA0 ESAI TX0             | $007001, $001001 at the tail
	//
	// All three registers are position-dependent, which is why this is a check
	// and not a dump: DDR2 and DCO2 single out the chain head, DCO4 singles out
	// the chain tail, and a firmware that programmed one uniform value
	// everywhere would satisfy a printer while failing the table.
	//
	// Head and tail are position 0 and position dspCount-1, read off the code
	// and not assumed. chainAdapter.cpp:206 states the head: injectCodecSource
	// writes mailbox 0's ingress frame and "the head's DMA then places them at
	// X:$001C04, not X:$001C00" -- the head is the position whose receive
	// callback reads mailbox 0, which audioRxCallback makes position 0.
	// chainAdapter.cpp:301 states the tail: "The tail position N - 1 therefore
	// writes mailbox N, which is the mailbox the egress phase reads."
	constexpr dsp56k::TWord g_ddr2Chain    = 0x001C00u;
	constexpr dsp56k::TWord g_ddr2Head     = 0x001C04u;
	constexpr dsp56k::TWord g_dcoChain     = 0x007001u;
	constexpr dsp56k::TWord g_dcoEndpoint  = 0x001001u;

	// Channel 2 is the ESAI RX0 receive channel and 4 is the ESAI TX0 transmit
	// channel.
	constexpr dsp56k::TWord g_dmaRxChannel = 2u;
	constexpr dsp56k::TWord g_dmaTxChannel = 4u;

	struct DspDmaExpectation
	{
		dsp56k::TWord ddr2;
		dsp56k::TWord dco2;
		dsp56k::TWord dco4;
	};

	// A position's row. `_count` rather than a literal 8, so the tail follows the
	// DSP set's own size and cannot disagree with it.

	// The firmware's own port ordering, read from the nine-entry table it builds
	// at 0x30116970 (set_hdi08_bases, 0x300391E8) rather than assumed from ours.
	// Entry i holds the CS1 address of the port at chain position i, and A3..A10
	// are eight active-low one-cold selects, so the port number is the index of
	// the single line pulled down.
	unsigned chainPositionOfPort(g2::Board& _board, const unsigned _port, const unsigned _count)
	{
		constexpr uint32_t g_portTableBase = 0x30116970u;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t entry =
				g2::Board::onRead(&_board, g_portTableBase + position * 4u, 4, &status);

			const uint8_t selects = uint8_t((entry >> 3) & 0xffu);
			const uint8_t low     = uint8_t(~selects);

			// Exactly one line low is a port; none low is the broadcast address
			// and belongs to no position.
			if(low == 0u || (low & uint8_t(low - 1u)) != 0u)
				continue;

			unsigned port = 0;
			for(uint8_t bit = low; bit > 1u; bit >>= 1)
				++port;

			if(port == _port)
				return position;
		}

		// A port the table does not name cannot be placed, and returning the
		// port itself makes that visible as a mismatch rather than hiding it.
		return _port;
	}

	constexpr DspDmaExpectation expectedDspDma(const unsigned _position, const unsigned _count)
	{
		const bool head = _position == 0u;
		const bool tail = _count > 0u && _position + 1u == _count;

		return DspDmaExpectation{
			head ? g_ddr2Head    : g_ddr2Chain,
			head ? g_dcoEndpoint : g_dcoChain,
			tail ? g_dcoEndpoint : g_dcoChain};
	}

	std::string dmaHex(const dsp56k::TWord _value)
	{
		char buf[16];
		std::snprintf(buf, sizeof buf, "$%06X", unsigned(_value));
		return buf;
	}

	/* DDR2 is a moving pointer and a terminal read of it answers the wrong
	 * question. This check establishes that the kernel programmed each
	 * position's DMA block correctly, and nothing about whether any sample has
	 * ever moved through it. DDR is the destination pointer of a receive channel
	 * and `DmaChannel::execTransfer` advances it on every transfer -- the
	 * `increment(m_ddr)`, `++m_ddr` and `m_ddr += m_dco + 1` sites in `dma.cpp`
	 * -- and the emulator saves no base beside it. After 500,000 frames it holds
	 * wherever the traversal has reached: $001D10 at the interior-or-tail
	 * positions and $001D08 at the head, against the $001C00 and $001C04 the
	 * kernel programmed. DCO does not move, so DCO2 and DCO4 stay correct.
	 *
	 * So DDR2 is latched at its first non-zero value, which is the value the
	 * kernel programmed. `t1_kernel_load` latches the same register the same
	 * way, at the same one-frame granularity this loop drives.
	 *
	 * What this does not claim: the sample is taken once per scheduler
	 * iteration, so it is the first iteration boundary at or after the write
	 * and not the writing instruction. A register written twice inside one
	 * iteration would be latched at the second value. `g_framesPerIteration` is
	 * 1, so that bound is the frame. */
	bool latchDdr2(g2::Board& _board, std::vector<dsp56k::TWord>& _latched)
	{
		bool complete = true;

		for(unsigned position = 0; position < _latched.size(); ++position)
		{
			if(_latched[position] != 0u)
				continue;

			_latched[position] =
				_board.dspSet().peripherals(position).getDMA().getDDR(g_dmaRxChannel);

			if(_latched[position] == 0u)
				complete = false;
		}

		return complete;
	}

	// Prints one line for each position and answers whether every position
	// matched. The verdict is the worst position: `all` is a conjunction over
	// the per-position results and never a separately computed summary, so a
	// single FAIL row cannot coexist with a PASS verdict.
	bool dumpDspDma(g2::Board& _board, const std::vector<dsp56k::TWord>& _ddr2Latched)
	{
		const unsigned count = _board.dspSet().dspCount();

		std::cout << "dsp-dma positions=" << count << std::endl;

		bool all = true;

		for(unsigned position = 0; position < count; ++position)
		{
			// The registers are read through the DSP set's own peripherals, so
			// this is the state the emulated kernel left behind and not a copy
			// the harness maintained beside it.
			dsp56k::Dma& dma = _board.dspSet().peripherals(position).getDMA();

			/* DDR2 comes from the latch and not from `dma`, for the reason the
			 * block above latchDdr2 states. A position that never latched
			 * carries zero, which no expected value matches, so it reports
			 * FAIL rather than passing on an unwritten register. */
			const dsp56k::TWord ddr2 = position < _ddr2Latched.size()
			                         ? _ddr2Latched[position]
			                         : dsp56k::TWord(0);
			const dsp56k::TWord dco2 = dma.getDCO(g_dmaRxChannel);
			const dsp56k::TWord dco4 = dma.getDCO(g_dmaTxChannel);

			/* The expectation is indexed by chain position and the loop walks
			 * hardware ports, and those are not the same order. The firmware's
			 * own nine-entry table at 0x30116970 maps chain position to port as
			 * 3, 7, 6, 5, 4, 2, 1, 0 -- so chain position 0, the head, is
			 * hardware port 3, and chain position 7, the tail, is port 0.
			 * Comparing a port's registers against that port's number as if it
			 * were a chain position reports mismatches against a correct
			 * machine. */
			const unsigned chainPosition = chainPositionOfPort(_board, position, count);
			const DspDmaExpectation expected = expectedDspDma(chainPosition, count);

			const bool ok = ddr2 == expected.ddr2
			             && dco2 == expected.dco2
			             && dco4 == expected.dco4;

			all = all && ok;

			std::cout << "  position " << position
			          << " DDR2=" << dmaHex(ddr2) << "/" << dmaHex(expected.ddr2)
			          << " DCO2=" << dmaHex(dco2) << "/" << dmaHex(expected.dco2)
			          << " DCO4=" << dmaHex(dco4) << "/" << dmaHex(expected.dco4)
			          << " " << (ok ? "PASS" : "FAIL") << std::endl;
		}

		// A set with no positions matched nothing and must not report a pass:
		// the loop would leave `all` true with no row behind it.
		if(count == 0)
			all = false;

		// The output says which sample each register is: DDR2 moves and the
		// other two do not.
		std::cout << "dsp-dma=" << (all ? "PASS" : "FAIL")
		          << " (measured/expected, expectations from design section 2.3;"
		             " DDR2 is LATCHED at its first written value and DCO2/DCO4"
		             " are read at the bound)"
		          << std::endl;

		return all;
	}

	int boot(const bool _dumpDspDma)
	{
		installLogFilter();

		g2::EnvArtifactResolver resolver;
		std::string why;

		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << why << std::endl;
			return 2;
		}

		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "CODE_30000400.bin is empty or unreadable under " << directory << std::endl;
			return 2;
		}

		g2::Board board(makeConfig());
		Ram ram(g_sdramSize);

		// The image goes where its name says it goes: 0x30000400, which is
		// offset 0x400 into the SDRAM window at 0x30000000.
		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "the image does not fit the configured SDRAM window" << std::endl;
			return 2;
		}

		// The vector table, big-endian, 256 identical longwords. It goes in
		// beside the image and before the watch below, so that none of it is
		// counted as the firmware's own traffic.
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
				std::cout << "the vector table does not fit the configured SDRAM window" << std::endl;
				return 2;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);

		// Installed before the core runs, so every count below is the firmware's
		// and none of it is the harness's own traffic.
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		/* The Board's own core is reset, and no second core is created. The
		 * Board already pointed its core at Board::onRead and Board::onWrite, so
		 * this composition is reached through the Board's own handle rather than
		 * through a copy. */
		board.resetMcu(g_entrySp, g_entryPc);

		/* VBR is placed after the reset and not before it, because the reset is
		 * what defines the machine's starting state and a value written ahead of
		 * it would depend on what the reset does not clear.
		 *
		 * The return is checked. setMcuReg answers false for an index the core
		 * refuses, and a core that refused this one would leave the table based
		 * at zero with nothing said about it. */
		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "the core refused VBR at register index " << g_regVbr << std::endl;
			return 2;
		}

		/* The Scheduler is declared after the Board so that it is destroyed
		 * before it: it borrows the Board's DSP set, and it installs chain
		 * callbacks into ESAIs the Board owns. The Executor is declared before
		 * the Scheduler for the same reason. */
		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return 2;
		}

		uint32_t iteration        = 0;
		uint32_t settleIterations = 0;
		bool     bannerLatched    = false;
		uint32_t pcAtBanner       = 0;

		std::string line0;
		std::string line1;

		/* The DDR2 latch, sized from the DSP set's own count so it cannot
		 * disagree with the loop that reads it back. It is armed only under the
		 * flag, so `--boot` alone drives exactly the machine it drove before. */
		std::vector<dsp56k::TWord> ddr2Latched;
		bool                       ddr2Complete = false;

		if(_dumpDspDma)
			ddr2Latched.resize(board.dspSet().dspCount(), 0u);

		for(; iteration < g_iterations; ++iteration)
		{
			scheduler->runFrames(g_framesPerIteration);

			if(_dumpDspDma && !ddr2Complete)
				ddr2Complete = latchDdr2(board, ddr2Latched);

			if(board.mcuHalted())
				break;

			/* The banner is latched when it appears because it is a transient:
			 * the firmware composes it, then boots on and the patch browser
			 * overwrites it. Measured at the full bound, line 0 reads
			 * "-:-       No Cat" and line 1 is blank, with the banner long gone.
			 *
			 * The settle counter is what makes the capture a whole line: the
			 * banner is written a character at a time, so latching on the first
			 * content byte catches a partial one. Reaching the settle does not
			 * stop the drive -- it freezes the banner and lets the machine run
			 * on, so the halted/faulted report below describes the machine at
			 * the bound and not the machine at the banner. */
			if(ram.contentWrites() > 0)
			{
				if(pcAtBanner == 0)
					pcAtBanner = board.mcuReg(g_regPc);

				if(++settleIterations >= g_bannerSettleIterations && !bannerLatched)
				{
					bannerLatched = true;
					line0 = readDisplayLine(board, 0, 0);
					line1 = readDisplayLine(board, 0, 1);
				}
			}
		}

		/* A run that never composed a banner latched nothing. readDisplayLine is
		 * called here only in that case, so the report shows what the display
		 * actually held rather than two empty strings. */
		if(!bannerLatched)
		{
			line0 = readDisplayLine(board, 0, 0);
			line1 = readDisplayLine(board, 0, 1);
		}

		const uint32_t pc = board.mcuReg(g_regPc);
		const bool halted = board.mcuHalted();
		const bool faulted = board.faulted();

		std::cout << "iterations=" << iteration
		          << " halted=" << (halted ? 1 : 0)
		          << " faulted=" << (faulted ? 1 : 0)
		          << " pc=0x" << std::hex << pc << std::dec << std::endl;

		// The two lines, printed untrimmed and byte for byte. The escaped form
		// is what makes a trailing space or a NUL visible to a reader; the plain
		// form is what a person actually wants to see.
		std::cout << "display0.line0=" << escapedLine(line0) << std::endl;
		std::cout << "display0.line1=" << escapedLine(line1) << std::endl;
		std::cout << line0 << std::endl;
		std::cout << line1 << std::endl;

		// Which sample the two lines are, said in the output rather than left to
		// a reader who knows this file. `banner=latched` means they are the
		// frozen first banner; `banner=none` means they are a terminal read of a
		// machine that never composed one.
		std::cout << "banner=" << (bannerLatched ? "latched" : "none")
		          << " contentWrites=" << ram.contentWrites()
		          << " clearWrites=" << ram.clearWrites()
		          << " pcAtBanner=0x" << std::hex << pcAtBanner << std::dec
		          << std::endl;

		const auto& log = board.memory().log();
		std::cout << "buslog=" << log.size() << std::endl;
		size_t printed = 0;
		for(const auto& line : log)
		{
			if(printed++ >= 40)
			{
				std::cout << "  (truncated after 40 lines)" << std::endl;
				break;
			}
			std::cout << "  " << line << std::endl;
		}

		reportSuppressedLogLines();

		// A boot that produced no banner is not a success, and this program must
		// not report one. A predicate reading `>= 0x20` is satisfied by a
		// screenful of the 0x20 spaces the display clear writes, so both clauses
		// below refuse that byte -- the latch is only set on a run that observed
		// writes the clear cannot produce, and anyContent refuses 0x20 itself.
		const bool bootOk = bannerLatched && anyContent(line0) && !halted && !faulted;

		/* The dump runs after the drive, on the same Board the drive turned, and
		 * it is the last thing the program does before it answers -- the
		 * registers it reads are the ones the firmware left at the bound. It is
		 * skipped entirely without the flag, so `--boot` alone answers on the
		 * banner predicate. */
		const bool dmaOk = _dumpDspDma ? dumpDspDma(board, ddr2Latched) : true;

		/* The verdict is the worse of the two clauses. A run whose
		 * DMA registers matched but which never composed a banner read those
		 * registers off a machine that did not boot, and must not exit 0. */
		if(!bootOk || !dmaOk)
			return 1;

		return 0;
	}

	/* Places the machine `--boot` places, drives it until the firmware has
	 * booted and every DSP has taken its program, enters the play phase, and
	 * then injects a known pattern at the CODEC SOURCE and reports the frame at
	 * which it reappears at the CODEC SINK.
	 *
	 * The expected delay is derived and never typed. The audio bus is a Line of
	 * dspCount + 1 mailboxes; the ingress phase writes mailbox 0's READ frame
	 * and the egress phase reads the last mailbox's WRITE frame, so neither
	 * codec edge carries a delay of its own and D_codec is 0. Each DSP-to-DSP
	 * hand-off costs one hop, and a chain of dspCount positions has
	 * dspCount - 1 of them:
	 *
	 *     D_chain + D_codec = (dspCount - 1) * hopFrames
	 *
	 * dspCount is read off the booted machine and hopFrames off the
	 * Scheduler::Config this program hands the factory.
	 *
	 * A report is not a verdict: this function exits non-zero unless the pattern
	 * arrived, arrived at exactly that frame, and arrived unchanged. */
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	/* The observer's known negative swaps these two, so a matcher stuck at true
	 * is caught only while they differ. */
	static_assert(g_impulseLeft != g_impulseRight,
		"The impulse slots must differ, or the swapped-slot control matches.");

	constexpr unsigned g_impulseOverrunQuanta = 1024u;

	/* The sentinel the arrival instrument's known positive places at the tail
	 * position's transmit source. It is deliberately neither of the two impulse
	 * words: a control that reused them could not be told apart from the
	 * measurement it qualifies.
	 *
	 * Bit 23 is clear, so `fromEsaiFrame`'s sign extension is the identity on
	 * it and the expected slot value is the written word itself. The
	 * expectation is derived from that here rather than typed twice. */
	constexpr uint32_t g_sinkControlWord     = 0x2B6D51u;
	constexpr int32_t  g_sinkControlExpected = int32_t(g_sinkControlWord);

	static_assert((g_sinkControlWord & 0x800000u) == 0u,
		"the sentinel's sign bit must be clear, or fromEsaiFrame's sign extension moves it");
	static_assert(g_sinkControlExpected != g_impulseLeft && g_sinkControlExpected != g_impulseRight,
		"the control's sentinel must not be either impulse word, or the control cannot be told from the measurement");

	/* The ESAI has six transmitters (TX0..TX5); the enabled mask decides which
	 * of them this writes. */
	constexpr uint32_t g_esaiTransmitters = 6u;

	/* The control's own bound. The tail's transmit callback writes mailbox N in
	 * the same quantum the egress phase reads it, so an arrival is due at
	 * quantum 0; this is headroom and not an expectation. */
	constexpr unsigned g_sinkControlQuanta = 64u;

	/* The outcome is printed on every exit path, including the ones that leave
	 * before a Board exists. A bare status leaves a reader to reconstruct the
	 * answer from figures, and "no artifact" and "the chain carried nothing"
	 * reconstruct to the same one. */
	void reportOutcome(const g2console::ImpulseOutcome _outcome, const std::string& _detail)
	{
		std::cout << "impulse: OUTCOME=" << g2console::name(_outcome) << std::endl;
		std::cout << "impulse: " << _detail << std::endl;
		std::cout << "impulse: this is a TRANSPORT probe. It reports whether the path carried the"
		             " injected sample, and it makes NO claim about audio: an unpatched Nord Modular"
		             " is silent by design, so a chain that carried nothing is a transport answer"
		             " and not a broken instrument" << std::endl;
	}

	/* The detail line says what the word means for this run, and it is derived
	 * from the record rather than typed beside it, so a figure cannot drift from
	 * the outcome it explains. */
	std::string detailFor(const g2console::ImpulseOutcome _outcome,
	                      const g2console::ImpulseObservation& _o)
	{
		switch(_outcome)
		{
		case g2console::ImpulseOutcome::DidNotRun:
			return "nothing about the chain was measured: the play phase was never reached";

		case g2console::ImpulseOutcome::InstrumentBlind:
			return "NO ANSWER. The observer could not observe, so the absence of an arrival says"
			       " nothing about the chain: framesPulled=" + std::to_string(_o.framesPulled)
			       + " observerSelfTest=" + std::to_string(_o.observerSelfTest ? 1 : 0)
			       + " sinkControlArrival=" + std::to_string(_o.sinkControlArrival)
			       + " sinkControlExact=" + std::to_string(_o.sinkControlExact ? 1 : 0);

		case g2console::ImpulseOutcome::Stopped:
			return "the observer received " + std::to_string(_o.framesPulled)
			       + " frames from the codec sink and the injected sample was in none of them, so"
			         " the path did not carry it to the sink. The arrival path is proven able to"
			         " REPORT one -- a sentinel placed in the tail position's ESAI transmit"
			         " register file and transmit-DMA source buffer came back out of the sink"
			         " unchanged at control quantum "
			       + std::to_string(_o.sinkControlArrival)
			       + " -- so this is a measured absence and not a blind one";

		case g2console::ImpulseOutcome::Propagated:
			return "the injected sample reached the codec sink unchanged at frame "
			       + std::to_string(_o.arrival) + ", which is the derived expectation";

		case g2console::ImpulseOutcome::PropagatedOffSpec:
			return "the injected sample reached the codec sink at frame " + std::to_string(_o.arrival)
			       + " against a derived expectation of " + std::to_string(_o.expectedArrival)
			       + ", unchanged=" + std::to_string(_o.arrivalExact ? 1 : 0)
			       + ", chain-health counters all zero=" + std::to_string(_o.countersZero ? 1 : 0);
		}

		return "unclassified";
	}

	int impulseDidNotRun(const std::string& _why)
	{
		std::cout << _why << std::endl;

		reportOutcome(g2console::ImpulseOutcome::DidNotRun,
			"nothing about the chain was measured: the play phase was never reached");

		return g2console::exitStatus(g2console::ImpulseOutcome::DidNotRun);
	}

	/* ------------------------------------------ the receive-buffer probe
	 *
	 * `--impulse --rx-probe` answers one question, and not the one `--impulse`
	 * answers: does the injected pattern reach the head DSP's ESAI receive path
	 * -- the words the receive DMA deposits in that DSP's X memory -- during the
	 * play phase?
	 *
	 * `--impulse` reports arrival at the codec sink. A silent sink has two
	 * causes that its three counters cannot separate: (a) the DSP receives the
	 * pattern and, unpatched, has no routing that would re-transmit it, and (b)
	 * the pattern never reaches the DSP's receive buffer at all. Both predict
	 * framesPulled == walkQuanta, underrun == 0 and arrival == -1. This probe
	 * reads the receive side directly, which is the observable the two
	 * hypotheses disagree about.
	 *
	 * No address here is assumed. The four DMA pointer registers of the two
	 * audio channels are latched at their first non-zero value during boot and
	 * printed, so the reader sees which address is the receive destination and
	 * which is the transmit source rather than being told. The census window
	 * below is wide enough to hold either and the wide scan does not use an
	 * address at all.
	 *
	 * A zero proves nothing without a known positive, so the probe plants a
	 * word this program owns at a known X address, runs the same scanner over
	 * it, prints the address the scanner returned, removes the plant and scans
	 * again. Both the positive and the negative are printed as values. */
	constexpr dsp56k::TWord g_rxProbeMask24 = 0x00FFFFFFu;

	// The census window: wide enough to hold the $001C00 receive area and the
	// $001D00 transmit area together, with margin on both sides. It is a
	// reporting window and not a claim -- the wide scan below covers all of
	// internal X and needs no window at all.
	constexpr dsp56k::TWord g_rxProbeWindowLow  = 0x001000u;
	constexpr dsp56k::TWord g_rxProbeWindowHigh = 0x002100u;

	// The wide scan: all of internal X memory, on the early quanta. The head is
	// chain position 0, so a pattern that reaches it at all reaches it within a
	// few quanta of the injection; 64 is that bound with two orders of margin.
	constexpr dsp56k::TWord g_rxProbeWideHigh   = 0x010000u;
	constexpr unsigned      g_rxProbeWideQuanta = 64u;

	// Where the known positive is planted. Inside the wide scan, far outside
	// the census window, and the old value is put back.
	constexpr dsp56k::TWord g_rxProbePlantWide   = 0x00FF00u;
	// A second plant, inside the census window, so the window's own addressing
	// is proven and not assumed.
	constexpr dsp56k::TWord g_rxProbePlantWindow = 0x001C10u;

	struct RxScanResult
	{
		bool          found    = false;
		dsp56k::TWord addr     = 0;
		int           which    = 0;   // 1 = the left word, 2 = the right word
		dsp56k::TWord value    = 0;
		unsigned      nonZero  = 0;
	};

	// One scanner, used by the measurement and by its known positive alike. A
	// known positive run through a different code path proves nothing about the
	// path that reported the absence.
	RxScanResult rxScan(const dsp56k::DSP& _dsp, const dsp56k::TWord _low,
		const dsp56k::TWord _high, const dsp56k::TWord _wantLeft,
		const dsp56k::TWord _wantRight)
	{
		RxScanResult r;

		const dsp56k::Memory& memory = _dsp.memory();

		for(dsp56k::TWord a = _low; a < _high; ++a)
		{
			const dsp56k::TWord w = memory.get(dsp56k::MemArea_X, a);

			if(w != 0u)
				++r.nonZero;

			if(r.found)
				continue;

			if(w == _wantLeft || w == _wantRight)
			{
				r.found = true;
				r.addr  = a;
				r.value = w;
				r.which = (w == _wantLeft) ? 1 : 2;
			}
		}

		return r;
	}

	// The four audio-channel DMA pointers of one position, each latched at its
	// first non-zero value. The latch is the point: DSR and DDR both move as
	// transfers run, so a terminal read reports wherever the traversal reached
	// and not the address the kernel programmed.
	struct RxProbeDma
	{
		dsp56k::TWord dsr2 = 0;   // receive channel source      (expect an ESAI register)
		dsp56k::TWord ddr2 = 0;   // receive channel destination (expect X memory)
		dsp56k::TWord dsr4 = 0;   // transmit channel source     (expect X memory)
		dsp56k::TWord ddr4 = 0;   // transmit channel destination(expect an ESAI register)

		/* The two control registers OR-accumulated over the boot drive, so a
		 * channel that was armed and later completed is told apart from one
		 * that was never armed at all. The play-phase accumulator is separate. */
		dsp56k::TWord dcr2Boot = 0;
		dsp56k::TWord dcr4Boot = 0;
	};

	/* How many positions have the ESAI receive DMA request registered.
	 *
	 * This is the audio path's own arming, read from the DMA controller that
	 * carries it, and it is the property `--impulse` waits for before it hands
	 * the machine to `beginPlayPhase()`. It is not a proxy: nothing here reads
	 * a program-load flag, a memory word or a cycle count.
	 *
	 * It is sticky, and only for a reason that could change. `finishTransfer`
	 * clears DE without calling `removeTriggerTarget`, so a channel that armed
	 * once stays registered for the rest of the run. Two things would make this
	 * predicate wrong:
	 *
	 *   - dsp56300 unregistering the target on completion. The predicate would
	 *     then go false between transfers and a healthy machine could run to
	 *     the bound.
	 *   - `setDCR` unregisters on any reconfiguration, so a kernel that armed
	 *     the channel and then rewrote DCR would show a window of false. The
	 *     drive polls every iteration and leaves on the first all-armed
	 *     reading, so it cannot miss such a window -- but what it saw would be
	 *     an arming that has since been withdrawn.
	 *
	 * And it reports registration, not traffic. A channel registered against a
	 * source that never asserts satisfies this forever. `--rx-probe` reads the
	 * destination buffers for that reason, and it is a separate instrument. */
	unsigned countRxArmed(g2::Board& _board, const unsigned _dspCount)
	{
		unsigned armed = 0;

		for(unsigned port = 0; port < _dspCount; ++port)
		{
			if(_board.dspSet().peripherals(port).getDMA().hasTrigger(
				dsp56k::DmaChannel::RequestSource::EsaiReceiveData))
				++armed;
		}

		return armed;
	}

	bool latchRxProbeDma(g2::Board& _board, std::vector<RxProbeDma>& _latched)
	{
		bool complete = true;

		for(unsigned port = 0; port < _latched.size(); ++port)
		{
			const dsp56k::Dma& dma = _board.dspSet().peripherals(port).getDMA();

			RxProbeDma& l = _latched[port];

			l.dcr2Boot |= dma.getDCR(g_dmaRxChannel);
			l.dcr4Boot |= dma.getDCR(g_dmaTxChannel);

			if(l.dsr2 == 0u) l.dsr2 = dma.getDSR(g_dmaRxChannel);
			if(l.ddr2 == 0u) l.ddr2 = dma.getDDR(g_dmaRxChannel);
			if(l.dsr4 == 0u) l.dsr4 = dma.getDSR(g_dmaTxChannel);
			if(l.ddr4 == 0u) l.ddr4 = dma.getDDR(g_dmaTxChannel);

			if(l.dsr2 == 0u || l.ddr2 == 0u || l.dsr4 == 0u || l.ddr4 == 0u)
				complete = false;
		}

		return complete;
	}

	int impulse(const bool _rxProbe)
	{
		installLogFilter();

		g2::EnvArtifactResolver resolver;
		std::string why;

		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			return impulseDidNotRun(why);
		}

		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			return impulseDidNotRun("CODE_30000400.bin is empty or unreadable under " + directory);
		}

		g2::Board board(makeConfig());
		Ram ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			return impulseDidNotRun("the image does not fit the configured SDRAM window");
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
				return impulseDidNotRun("the vector table does not fit the configured SDRAM window");
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			return impulseDidNotRun("the core refused VBR at register index " + std::to_string(g_regVbr));
		}

		g2::SerialExecutor          executor;
		g2::Status                  schedulerStatus{};
		const g2::Scheduler::Config config;

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, schedulerStatus);

		if(!scheduler)
		{
			return impulseDidNotRun("Scheduler::create returned no object; g2::Status = " + std::to_string(uint32_t(schedulerStatus)));
		}

		const unsigned dspCount = board.dspSet().dspCount();

		/* The drive leaves on a property of the audio path, and not on program
		 * loading.
		 *
		 * Leaving the moment every position reports `programLanded` is a fact
		 * about the kernel download and says nothing about audio: in this
		 * firmware `programLanded` goes true at boot iteration 44,515 and the
		 * ESAI receive DMA request is not armed on any position until 231,296 --
		 * more than five times later. At the moment of the boot-time DMA
		 * configuration the rx-arming code is not even resident;
		 * P:$000250-$000270 disassembles as all zeros, because it arrives in a
		 * later-loaded DSP program. That exit hands `beginPlayPhase()` a machine
		 * whose receive path is still dead, and the STOPPED verdict it produces
		 * is a statement about transport not yet existing rather than about
		 * routing.
		 *
		 * The predicate is `countRxArmed == dspCount`, bounded by g_iterations.
		 * A drive that reaches the bound without arming reports DID-NOT-RUN and
		 * not STOPPED: a machine whose receive path never came up did not
		 * measure the chain at all. `landed` is still reported -- it is the
		 * precondition for polling, not the exit. */
		uint32_t iteration    = 0;
		uint32_t settle       = 0;
		bool     booted       = false;
		bool     landed       = false;
		bool     rxArmed      = false;
		unsigned rxArmedPorts = 0;

		// The probe's DMA pointer latch, filled as the kernel programs each
		// position. Empty and untouched when the probe is off.
		std::vector<RxProbeDma> rxDma(_rxProbe ? dspCount : 0u);
		bool                    rxDmaComplete = false;

		for(; iteration < g_iterations; ++iteration)
		{
			scheduler->runFrames(g_framesPerIteration);

			/* Called on every iteration and not only until the latch fills: the
			 * pointer latch is idempotent once full, and the two control
			 * registers are OR-accumulated across the whole boot drive. */
			if(_rxProbe)
				rxDmaComplete = latchRxProbeDma(board, rxDma) || rxDmaComplete;

			if(board.mcuHalted())
				break;

			if(ram.contentWrites() == 0)
				continue;

			if(++settle < g_bannerSettleIterations)
				continue;

			booted = true;

			unsigned landedCount = 0;
			for(unsigned d = 0; d < dspCount; ++d)
			{
				const bool* const flag = board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landedCount;
			}

			if(landedCount == dspCount)
				landed = true;

			if(!landed)
				continue;

			if(countRxArmed(board, dspCount) == dspCount)
			{
				rxArmed = true;
				break;
			}
		}

		/* The reported count is read after the drive and not carried out of it,
		 * so a run that never reached the poll -- one that halted, or one whose
		 * programs never landed -- still reports a measured number rather than
		 * the initial zero. The registration is sticky within a run, so the
		 * reading here is the run's maximum. */
		rxArmedPorts = countRxArmed(board, dspCount);

		const bool halted  = board.mcuHalted();
		const bool faulted = board.faulted();

		scheduler->beginPlayPhase();

		/* The primed frames come off the sink before the walk starts, so the
		 * walk's own index is measured from the injection quantum and not from
		 * the lookahead. */
		std::vector<g2::Frame> primed(config.lookaheadFrames);
		const size_t primedPulled = scheduler->pull(primed.data(), primed.size());

		const unsigned expected = (dspCount > 0 ? dspCount - 1u : 0u) * config.hopFrames;
		const unsigned walk     = expected + g_impulseOverrunQuanta;

		g2::Frame impulseFrame{};
		impulseFrame.slot[0] = g_impulseLeft;
		impulseFrame.slot[1] = g_impulseRight;

		const g2::Frame silence{};

		/* The walk below and the self-test after it drive the same two
		 * predicates. Retyped copies let the self-test keep passing while the
		 * walk's own comparison drifts away from it. */
		const auto seen = [](const g2::Frame& _f)
		{
			return _f.slot[0] != 0 || _f.slot[1] != 0;
		};
		const auto matches = [](const g2::Frame& _f)
		{
			return _f.slot[0] == g_impulseLeft && _f.slot[1] == g_impulseRight;
		};

		int      arrival      = -1;
		bool     arrivalExact = false;
		unsigned framesPulled = 0;

		/* ---------------------------------- the receive-buffer probe's state.
		 *
		 * The head is found and not typed. chainPositionOfPort reads the
		 * firmware's own position-to-port table, so the port this probe reads
		 * is the machine's head and not this program's guess at it. */
		const dsp56k::TWord rxWantLeft  = dsp56k::TWord(g_impulseLeft)  & g_rxProbeMask24;
		const dsp56k::TWord rxWantRight = dsp56k::TWord(g_impulseRight) & g_rxProbeMask24;

		unsigned headPort      = 0;
		bool     headPortFound = false;

		if(_rxProbe)
		{
			for(unsigned port = 0; port < dspCount; ++port)
			{
				if(chainPositionOfPort(board, port, dspCount) != 0u)
					continue;

				headPort      = port;
				headPortFound = true;
				break;
			}
		}

		/* The known positive and its negative, run before the walk on the same
		 * scanner the walk uses. Each plant is written, scanned for, removed,
		 * and scanned for again, and every one of the four answers is printed
		 * as a value. A scanner that cannot see a word this program put there
		 * cannot report an absence of one the machine did not. */
		RxScanResult kpWidePlanted, kpWideRemoved, kpWindowPlanted, kpWindowRemoved;

		if(_rxProbe && headPortFound)
		{
			dsp56k::Memory& memory = board.dspSet().dsp(headPort).memory();

			const dsp56k::TWord savedWide   = memory.get(dsp56k::MemArea_X, g_rxProbePlantWide);
			const dsp56k::TWord savedWindow = memory.get(dsp56k::MemArea_X, g_rxProbePlantWindow);

			memory.set(dsp56k::MemArea_X, g_rxProbePlantWide, rxWantLeft);
			kpWidePlanted = rxScan(board.dspSet().dsp(headPort), 0u, g_rxProbeWideHigh,
				rxWantLeft, rxWantRight);
			memory.set(dsp56k::MemArea_X, g_rxProbePlantWide, savedWide);
			kpWideRemoved = rxScan(board.dspSet().dsp(headPort), 0u, g_rxProbeWideHigh,
				rxWantLeft, rxWantRight);

			memory.set(dsp56k::MemArea_X, g_rxProbePlantWindow, rxWantRight);
			kpWindowPlanted = rxScan(board.dspSet().dsp(headPort),
				g_rxProbeWindowLow, g_rxProbeWindowHigh, rxWantLeft, rxWantRight);
			memory.set(dsp56k::MemArea_X, g_rxProbePlantWindow, savedWindow);
			kpWindowRemoved = rxScan(board.dspSet().dsp(headPort),
				g_rxProbeWindowLow, g_rxProbeWindowHigh, rxWantLeft, rxWantRight);
		}

		// What the walk measures. -1 means the scanner ran and saw nothing.
		int           rxWideQuantum   = -1;
		dsp56k::TWord rxWideAddr      = 0;
		int           rxWideWhich     = 0;
		int           rxWindowQuantum = -1;
		dsp56k::TWord rxWindowAddr    = 0;
		unsigned      rxWindowPort    = 0;
		int           rxWindowWhich   = 0;

		// The census: the largest number of non-zero X words the census window
		// held at any quantum, at the head, with the quantum that carried it.
		unsigned rxMaxNonZero   = 0;
		int      rxMaxNonZeroAt = -1;

		// The receive DMA's destination pointer as it moves during the walk. A
		// pointer that moves is a receive channel that transferred.
		dsp56k::TWord rxDdr2First = 0;
		dsp56k::TWord rxDdr2Min   = 0xFFFFFFu;
		dsp56k::TWord rxDdr2Max   = 0;

		/* The delivery-level known positive, and it is not a planted one.
		 *
		 * Each position's receive buffer is the eight X words at the address its
		 * own latched DDR2 names, and its transmit buffer the eight at its
		 * latched DSR4. If any position's receive buffer ever holds a non-zero
		 * word, the ESAI-to-DMA-to-X-memory path demonstrably delivers audio on
		 * this machine, and the head's own answer is then an absence measured
		 * against a working mechanism rather than against an untested one. The
		 * transmit half is the same question asked of the other direction. */
		constexpr dsp56k::TWord kRxBufWords = 8u;

		std::vector<int>           rxBufFirstQ(_rxProbe ? dspCount : 0u, -1);
		std::vector<dsp56k::TWord> rxBufFirstAddr(_rxProbe ? dspCount : 0u, 0u);
		std::vector<dsp56k::TWord> rxBufFirstVal(_rxProbe ? dspCount : 0u, 0u);
		std::vector<unsigned>      rxBufMaxNz(_rxProbe ? dspCount : 0u, 0u);

		/* Whether the receive side ever requested a transfer at all, which is
		 * the question a buffer of zeros cannot answer on its own. The ESAI
		 * status register is OR-accumulated over the walk, so a bit that was
		 * set at any quantum is set here even if it was cleared before the next
		 * sample; M_RDF is the receive DMA's own request line and M_TDE is the
		 * transmit one, so the two directions are read off one instrument and
		 * the transmit column is the receive column's control. The two DMA
		 * pointers are tracked the same way, min and max over the walk. */
		std::vector<dsp56k::TWord> esaiSrOr(_rxProbe ? dspCount : 0u, 0u);
		/* The channel-enable bit is what `DmaChannel::execTransfer` gates on,
		 * so a channel whose De was clear at every sample transferred nothing
		 * however loudly its peripheral requested. OR-accumulated for the same
		 * reason the status register is: a bit set at any quantum is set here.
		 * The transmit channel is the receive channel's control. */
		std::vector<dsp56k::TWord> dcr2Or(_rxProbe ? dspCount : 0u, 0u);
		std::vector<dsp56k::TWord> dcr4Or(_rxProbe ? dspCount : 0u, 0u);

		/* The emulator's own record of "a channel is armed for this peripheral
		 * request". DmaChannel::arm registers the channel as a request target
		 * only when its DE bit is set, so this is the same question the DE bit
		 * answers, asked of a different structure -- and Dma::hasTrigger holds
		 * across a quantum rather than being sampled at its boundary, which is
		 * the exact weakness of the DE sample. The transmit source is the
		 * receive source's control, on the same call. */
		std::vector<uint8_t> rxTriggerEver(_rxProbe ? dspCount : 0u, 0u);
		std::vector<uint8_t> txTriggerEver(_rxProbe ? dspCount : 0u, 0u);
		std::vector<dsp56k::TWord> ddr2Min(_rxProbe ? dspCount : 0u, 0xFFFFFFu);
		std::vector<dsp56k::TWord> ddr2Max(_rxProbe ? dspCount : 0u, 0u);
		std::vector<dsp56k::TWord> dsr4Min(_rxProbe ? dspCount : 0u, 0xFFFFFFu);
		std::vector<dsp56k::TWord> dsr4Max(_rxProbe ? dspCount : 0u, 0u);

		std::vector<int>           txBufFirstQ(_rxProbe ? dspCount : 0u, -1);
		std::vector<dsp56k::TWord> txBufFirstAddr(_rxProbe ? dspCount : 0u, 0u);
		std::vector<dsp56k::TWord> txBufFirstVal(_rxProbe ? dspCount : 0u, 0u);
		std::vector<unsigned>      txBufMaxNz(_rxProbe ? dspCount : 0u, 0u);

		// Walks the eight words at `_base` and records the first non-zero and
		// the largest non-zero count. One body for both directions, so a
		// positive on one side and an absence on the other are the same
		// instrument's two answers.
		const auto censusBuffer = [&](const dsp56k::DSP& _dsp, const dsp56k::TWord _base,
			const unsigned _q, int& _firstQ, dsp56k::TWord& _firstAddr,
			dsp56k::TWord& _firstVal, unsigned& _maxNz)
		{
			if(_base == 0u)
				return;

			const dsp56k::Memory& memory = _dsp.memory();

			unsigned nz = 0;

			for(dsp56k::TWord i = 0; i < kRxBufWords; ++i)
			{
				const dsp56k::TWord w = memory.get(dsp56k::MemArea_X, _base + i);

				if(w == 0u)
					continue;

				++nz;

				if(_firstQ < 0)
				{
					_firstQ    = int(_q);
					_firstAddr = _base + i;
					_firstVal  = w;
				}
			}

			if(nz > _maxNz)
				_maxNz = nz;
		};

		for(unsigned q = 0; q < walk; ++q)
		{
			const g2::Frame& in = (q == 0) ? impulseFrame : silence;

			(void) scheduler->push(&in, 1);
			scheduler->runFrames(1);

			if(_rxProbe && headPortFound)
			{
				const dsp56k::TWord ddr2 =
					board.dspSet().peripherals(headPort).getDMA().getDDR(g_dmaRxChannel);

				if(q == 0)
					rxDdr2First = ddr2;
				if(ddr2 < rxDdr2Min)
					rxDdr2Min = ddr2;
				if(ddr2 > rxDdr2Max)
					rxDdr2Max = ddr2;

				// The census window, at every position and every quantum. A
				// pattern that stopped one DSP short of the head would show
				// here and nowhere else.
				for(unsigned port = 0; port < dspCount; ++port)
				{
					{
						dsp56k::Peripherals56311& p = board.dspSet().peripherals(port);

						esaiSrOr[port] |= p.getEsai().readStatusRegister();
						dcr2Or[port]   |= p.getDMA().getDCR(g_dmaRxChannel);
						dcr4Or[port]   |= p.getDMA().getDCR(g_dmaTxChannel);

						if(p.getDMA().hasTrigger(dsp56k::DmaChannel::RequestSource::EsaiReceiveData))
							rxTriggerEver[port] = 1u;
						if(p.getDMA().hasTrigger(dsp56k::DmaChannel::RequestSource::EsaiTransmitData))
							txTriggerEver[port] = 1u;

						const dsp56k::TWord d2 = p.getDMA().getDDR(g_dmaRxChannel);
						const dsp56k::TWord s4 = p.getDMA().getDSR(g_dmaTxChannel);

						if(d2 < ddr2Min[port]) ddr2Min[port] = d2;
						if(d2 > ddr2Max[port]) ddr2Max[port] = d2;
						if(s4 < dsr4Min[port]) dsr4Min[port] = s4;
						if(s4 > dsr4Max[port]) dsr4Max[port] = s4;
					}

					censusBuffer(board.dspSet().dsp(port), rxDma[port].ddr2, q,
						rxBufFirstQ[port], rxBufFirstAddr[port], rxBufFirstVal[port],
						rxBufMaxNz[port]);
					censusBuffer(board.dspSet().dsp(port), rxDma[port].dsr4, q,
						txBufFirstQ[port], txBufFirstAddr[port], txBufFirstVal[port],
						txBufMaxNz[port]);

					const RxScanResult w = rxScan(board.dspSet().dsp(port),
						g_rxProbeWindowLow, g_rxProbeWindowHigh, rxWantLeft, rxWantRight);

					if(port == headPort && w.nonZero > rxMaxNonZero)
					{
						rxMaxNonZero   = w.nonZero;
						rxMaxNonZeroAt = int(q);
					}

					if(rxWindowQuantum < 0 && w.found)
					{
						rxWindowQuantum = int(q);
						rxWindowAddr    = w.addr;
						rxWindowWhich   = w.which;
						rxWindowPort    = port;
					}
				}

				// The wide scan, head only, on the early quanta.
				if(q < g_rxProbeWideQuanta && rxWideQuantum < 0)
				{
					const RxScanResult wide = rxScan(board.dspSet().dsp(headPort),
						0u, g_rxProbeWideHigh, rxWantLeft, rxWantRight);

					if(wide.found)
					{
						rxWideQuantum = int(q);
						rxWideAddr    = wide.addr;
						rxWideWhich   = wide.which;
					}
				}
			}

			g2::Frame out{};

			/* The pull count is the pair for the zero. `out` is zero-initialised
			 * every quantum, so a sink that delivered nothing leaves exactly the
			 * bytes a sink that delivered silence leaves. Without this count
			 * `arrival=-1` reports "the chain did not carry it" and "the observer
			 * never saw a frame" in one value, and the second says nothing about
			 * the chain at all. */
			framesPulled += unsigned(scheduler->pull(&out, 1));

			if(arrival < 0 && seen(out))
			{
				arrival      = int(q);
				arrivalExact = matches(out);
			}
		}

		/* ----------------------------- the arrival instrument's known positive
		 *
		 * What the self-test below does not prove. It drives the detector's two
		 * predicates over `impulseFrame` and `silence`, two frames this program
		 * built on its own stack. Not one byte of the arrival path is on its
		 * evidence: it reads 1 with the tail's transmit callback deleted, with
		 * `fromEsaiFrame` returning zeros, with the mailbox swap frozen, with
		 * `extractCodecSink` reading the wrong mailbox and with `Scheduler::pull`
		 * copying nothing. So `arrival=-1` beside `observerSelfTest=1` has two
		 * readings -- a chain that carried nothing, and an arrival path that
		 * could not have reported anything -- and tells them apart nowhere.
		 *
		 * The control places a sentinel at the tail position's transmit source
		 * -- its ESAI transmit register file and the DSP-memory window the
		 * transmit DMA refills that register from -- and then reads that
		 * sentinel back out of the sink, through the same `pull` and the
		 * same comparator the walk above used. The sentinel goes in at the
		 * earliest point that still traverses the whole arrival path rather
		 * than at the reporting line: everything downstream of the transmit
		 * buffer is the machine's own code and none of it is bypassed.
		 *
		 * The links it traverses, and they are the links the walk's `arrival`
		 * depends on:
		 *
		 *   the tail DSP's X memory -> the transmit DMA -> m_tx, and
		 *   Esai::writeTX -> m_tx           the transmit register file
		 *   Esai::execTX  -> writeSlotToFrame -> m_txFrame    frame assembly
		 *   Esai::writeTXimpl -> the installed WriteTxCallback, which the DSP
		 *                        set bound to ChainAdapter::audioTxCallback(N-1)
		 *   fromEsaiFrame(in, kAudioReg)    the chain's Tx conversion point
		 *   m_audio[N].write()              the tail mailbox
		 *   ChainAdapter::advanceAll        the swap
		 *   ChainAdapter::extractCodecSink  the egress read
		 *   CodecSink::push / Scheduler::pull
		 *   the walk's own two predicates
		 *
		 * The links it does not traverse, stated so no reader credits it with
		 * them: no DSP core executes any part of it -- the sentinel is placed
		 * in the transmit buffer rather than computed into it -- and positions
		 * 0..N-2, every receive callback, the mailbox hop chain and
		 * `injectCodecSource` are all upstream of the tail and are not on its
		 * path. It is a control for the arrival instrument and not for the
		 * chain: it says the sink can report a frame the tail transmitted, and
		 * says nothing about whether anything reaches the tail.
		 *
		 * It runs after the walk, on the same machine, so it cannot move the
		 * measurement it qualifies: `arrival` and `framesPulled` are already
		 * latched above.
		 *
		 * Every enabled transmitter is written, not just register 0. Esai's
		 * underrun latch fires when the written mask does not cover the enabled
		 * mask, and a control that latched an underrun would be measuring the
		 * underrun path rather than the arrival path. */
		int  sinkControlArrival = -1;
		bool sinkControlExact   = false;
		int32_t sinkControlL    = 0;
		int32_t sinkControlR    = 0;
		unsigned sinkControlQuanta = 0;

		unsigned sinkControlPort = 0;
		bool     sinkControlPortFound = false;

		/* The tail is found and not typed, for the reason the head is: the
		 * chain adapter's position and the hardware port are not the same
		 * number. dspSet.cpp's attachChainCallbacks binds
		 * audioTxCallback(position) to peripherals(portOfPosition[position]),
		 * and portOfPosition comes from the firmware's own nine-entry table.
		 * On this machine position 7 is port 0, so a control that wrote
		 * peripherals(7) would be driving chain position 1 -- six hops
		 * upstream of the mailbox extractCodecSink reads -- and would report a
		 * dead arrival path while the path was healthy. */
		for(unsigned port = 0; port < dspCount; ++port)
		{
			if(chainPositionOfPort(board, port, dspCount) != dspCount - 1u)
				continue;

			sinkControlPort      = port;
			sinkControlPortFound = true;
			break;
		}

		if(sinkControlPortFound)
		{
			dsp56k::Esai& tailEsai = board.dspSet().peripherals(sinkControlPort).getEsai();

			for(unsigned q = 0; q < g_sinkControlQuanta && sinkControlArrival < 0; ++q)
			{
				++sinkControlQuanta;

				const dsp56k::TWord enabled = tailEsai.hasEnabledTransmitters();

				for(uint32_t reg = 0; reg < g_esaiTransmitters; ++reg)
				{
					if(enabled & (1u << reg))
						tailEsai.writeTX(reg, g_sinkControlWord);
				}

				/* And the buffer the transmit DMA refills that register from,
				 * because the register alone reaches one slot and the codec
				 * sink reads two. Esai::writeSlotToFrame copies the register
				 * file into the slot and then triggers the transmit DMA, which
				 * is serviced synchronously and overwrites the register before
				 * the next slot is assembled -- so a register-only injection
				 * arrives in slot 0 and slot 1 carries whatever the DSP's
				 * transmit buffer held. Planting the buffer as well makes the
				 * sentinel the value every slot of the frame is sourced from,
				 * and it puts the transmit DMA on the control's path rather
				 * than around it.
				 *
				 * The window is read off the DMA, not typed. DSR4 is the
				 * channel's live source address and the ESAI's own transmit
				 * word count gives the frame's length; two frames' worth from
				 * the frame-aligned base covers the half-buffer the DMA is
				 * reading and the one it moves to next. */
				{
					dsp56k::Dma& tailDma = board.dspSet().peripherals(sinkControlPort).getDMA();

					const dsp56k::TWord source     = tailDma.getDSR(g_dmaTxChannel);
					const dsp56k::TWord frameWords = tailEsai.getTxWordCount() + 1u;
					const dsp56k::TWord base       = source - (source % frameWords);

					dsp56k::Memory& tailMemory = board.dspSet().dsp(sinkControlPort).memory();

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

				sinkControlArrival = int(q);
				sinkControlL       = out.slot[0];
				sinkControlR       = out.slot[1];
				sinkControlExact   = out.slot[0] == g_sinkControlExpected
					&& out.slot[1] == g_sinkControlExpected;
			}
		}

		/* The observer's own known positive and known negative, run on the
		 * detector and not on the chain. The loop above reports an absence, and
		 * an absence reported by a detector that cannot detect is not evidence.
		 * This drives the same two predicates over a frame this program built,
		 * whose answers are known before the run: the injected pattern must be
		 * seen and must compare equal, and a zero frame must not be seen. */
		bool observerSelfTest = false;
		{
			g2::Frame mismatch{};
			mismatch.slot[0] = g_impulseRight;
			mismatch.slot[1] = g_impulseLeft;

			observerSelfTest = seen(impulseFrame) && matches(impulseFrame)
				&& !seen(silence)
				&& seen(mismatch) && !matches(mismatch);
		}

		std::cout << "impulse: dspCount=" << dspCount
		          << " hopFrames=" << config.hopFrames
		          << " lookaheadFrames=" << config.lookaheadFrames
		          << " D_chain=" << expected
		          << " D_codec=0" << std::endl;
		std::cout << "impulse: bootIterations=" << iteration
		          << " booted=" << (booted ? 1 : 0)
		          << " programsLanded=" << (landed ? 1 : 0)
		          << " halted=" << (halted ? 1 : 0)
		          << " faulted=" << (faulted ? 1 : 0) << std::endl;
		/* Why the drive stopped, in one word, beside the count that decided it.
		 * `rx-armed` and `bound` are the two answers the predicate itself can
		 * give; `halted` is the machine leaving under the caller. A reader who
		 * sees `bound` is looking at a run whose receive path never came up,
		 * which is a different fact from a chain that carried nothing. */
		std::cout << "impulse: bootExit="
		          << (rxArmed ? "rx-armed" : (halted ? "halted" : "bound"))
		          << " rxArmedPorts=" << rxArmedPorts << "/" << dspCount << std::endl;
		std::cout << "impulse: primedPulled=" << primedPulled
		          << " walkQuanta=" << walk
		          << " framesPulled=" << framesPulled
		          << " observerSelfTest=" << (observerSelfTest ? 1 : 0)
		          << " arrival=" << arrival
		          << " arrivalExact=" << (arrivalExact ? 1 : 0) << std::endl;
		/* The arrival instrument's known positive, on its own line, beside the
		 * measurement it qualifies. A reader who sees sinkControlArrival>=0 and
		 * sinkControlExact=1 is looking at an `arrival=-1` that means the chain
		 * carried nothing; a reader who sees sinkControlArrival=-1 is looking at
		 * an arrival path that could not report a frame it was handed, and the
		 * walk's -1 says nothing at all. */
		std::cout << "impulse: sinkControl tailPosition=" << (dspCount > 0 ? dspCount - 1u : 0u)
		          << " tailPort=" << (sinkControlPortFound ? int(sinkControlPort) : -1)
		          << " sentinel=$" << std::hex << g_sinkControlWord << std::dec
		          << " controlQuanta=" << sinkControlQuanta
		          << " sinkControlArrival=" << sinkControlArrival
		          << " sinkControlExact=" << (sinkControlExact ? 1 : 0)
		          << " sinkControlValue=" << sinkControlL << "/" << sinkControlR << std::endl;

		/* The chain-health counters. The per-position figures are reported as
		 * their maximum over the positions, with the position that carried it,
		 * so a reader is told which one moved. */
		uint64_t underrun = 0, secondUnderrun = 0, phaseError = 0;
		unsigned underrunAt = 0, secondUnderrunAt = 0, phaseErrorAt = 0;

		for(unsigned p = 0; p < dspCount; ++p)
		{
			if(scheduler->underrunFrames(p) > underrun)
			{
				underrun   = scheduler->underrunFrames(p);
				underrunAt = p;
			}
			if(scheduler->secondBusUnderrunFrames(p) > secondUnderrun)
			{
				secondUnderrun   = scheduler->secondBusUnderrunFrames(p);
				secondUnderrunAt = p;
			}
			if(scheduler->phaseErrorFrames(p) > phaseError)
			{
				phaseError   = scheduler->phaseErrorFrames(p);
				phaseErrorAt = p;
			}
		}

		const bool countersZero = underrun == 0 && secondUnderrun == 0 && phaseError == 0
			&& scheduler->starvedFrames() == 0 && scheduler->overflowFrames() == 0
			&& scheduler->droppedFrames() == 0 && scheduler->underflowFrames() == 0;

		std::cout << "impulse: underrun=" << underrun << "@" << underrunAt
		          << " secondBusUnderrun=" << secondUnderrun << "@" << secondUnderrunAt
		          << " phaseError=" << phaseError << "@" << phaseErrorAt
		          << " starved=" << scheduler->starvedFrames()
		          << " overflow=" << scheduler->overflowFrames()
		          << " dropped=" << scheduler->droppedFrames()
		          << " underflow=" << scheduler->underflowFrames() << std::endl;

		std::cout << "impulse: countersZero=" << (countersZero ? 1 : 0) << std::endl;

		/* ------------------------------------- the receive-buffer probe's report
		 *
		 * Every line prints the computed value beside its name. No sentence
		 * here is a conclusion the program decided in advance: the reader is
		 * handed the addresses, the counts and the two known-positive answers,
		 * and the narration is checkable against them. */
		if(_rxProbe)
		{
			std::cout << "rx-probe: headPortFound=" << (headPortFound ? 1 : 0)
			          << " headPort=" << headPort
			          << " dspCount=" << dspCount
			          << " dmaLatchComplete=" << (rxDmaComplete ? 1 : 0) << std::endl;

			for(unsigned port = 0; port < rxDma.size(); ++port)
			{
				const unsigned position = chainPositionOfPort(board, port, dspCount);

				std::cout << "rx-probe: port " << port
				          << " chainPosition=" << position
				          << " latched DSR2=" << dmaHex(rxDma[port].dsr2)
				          << " DDR2=" << dmaHex(rxDma[port].ddr2)
				          << " DSR4=" << dmaHex(rxDma[port].dsr4)
				          << " DDR4=" << dmaHex(rxDma[port].ddr4)
				          << " live DDR2=" << dmaHex(board.dspSet().peripherals(port)
				                                      .getDMA().getDDR(g_dmaRxChannel))
				          << " live DSR4=" << dmaHex(board.dspSet().peripherals(port)
				                                      .getDMA().getDSR(g_dmaTxChannel))
				          << " bootDCR2or=" << dmaHex(rxDma[port].dcr2Boot)
				          << " bootDE2everSet="
				          << ((rxDma[port].dcr2Boot >> dsp56k::DmaChannel::De) & 1u)
				          << " bootDCR4or=" << dmaHex(rxDma[port].dcr4Boot)
				          << " bootDE4everSet="
				          << ((rxDma[port].dcr4Boot >> dsp56k::DmaChannel::De) & 1u)
				          << std::endl;
			}

			std::cout << "rx-probe: KNOWN_POSITIVE wide plant addr=" << dmaHex(g_rxProbePlantWide)
			          << " value=" << dmaHex(rxWantLeft)
			          << " -> found=" << (kpWidePlanted.found ? 1 : 0)
			          << " at=" << dmaHex(kpWidePlanted.addr)
			          << " which=" << kpWidePlanted.which
			          << " nonZeroWordsInScan=" << kpWidePlanted.nonZero
			          << " ; after removal found=" << (kpWideRemoved.found ? 1 : 0)
			          << " nonZeroWordsInScan=" << kpWideRemoved.nonZero << std::endl;

			std::cout << "rx-probe: KNOWN_POSITIVE window plant addr=" << dmaHex(g_rxProbePlantWindow)
			          << " value=" << dmaHex(rxWantRight)
			          << " -> found=" << (kpWindowPlanted.found ? 1 : 0)
			          << " at=" << dmaHex(kpWindowPlanted.addr)
			          << " which=" << kpWindowPlanted.which
			          << " nonZeroWordsInScan=" << kpWindowPlanted.nonZero
			          << " ; after removal found=" << (kpWindowRemoved.found ? 1 : 0)
			          << " nonZeroWordsInScan=" << kpWindowRemoved.nonZero << std::endl;

			std::cout << "rx-probe: searchedFor left=" << dmaHex(rxWantLeft)
			          << " right=" << dmaHex(rxWantRight)
			          << " window=[" << dmaHex(g_rxProbeWindowLow) << ","
			          << dmaHex(g_rxProbeWindowHigh) << ") allPositions everyQuantum"
			          << " wide=[$000000," << dmaHex(g_rxProbeWideHigh) << ") headOnly firstQuanta="
			          << g_rxProbeWideQuanta << std::endl;

			std::cout << "rx-probe: RESULT windowQuantum=" << rxWindowQuantum
			          << " windowPort=" << rxWindowPort
			          << " windowAddr=" << dmaHex(rxWindowAddr)
			          << " windowWhich=" << rxWindowWhich
			          << " wideQuantum=" << rxWideQuantum
			          << " wideAddr=" << dmaHex(rxWideAddr)
			          << " wideWhich=" << rxWideWhich << std::endl;

			/* The delivery-level positive or its absence, per position and as
			 * values. `rxBufFirstQ` is the quantum at which that position's
			 * receive buffer first held any non-zero word; -1 means it never
			 * did. The transmit half is the same figure for the other
			 * direction. A reader compares the two columns rather than being
			 * told what they mean. */
			for(unsigned port = 0; port < rxDma.size(); ++port)
			{
				std::cout << "rx-probe: buffers port " << port
				          << " chainPosition=" << chainPositionOfPort(board, port, dspCount)
				          << " rxBuf@" << dmaHex(rxDma[port].ddr2)
				          << " firstNonZeroQ=" << rxBufFirstQ[port]
				          << " at=" << dmaHex(rxBufFirstAddr[port])
				          << " val=" << dmaHex(rxBufFirstVal[port])
				          << " maxNonZero=" << rxBufMaxNz[port] << "/8"
				          << " | txBuf@" << dmaHex(rxDma[port].dsr4)
				          << " firstNonZeroQ=" << txBufFirstQ[port]
				          << " at=" << dmaHex(txBufFirstAddr[port])
				          << " val=" << dmaHex(txBufFirstVal[port])
				          << " maxNonZero=" << txBufMaxNz[port] << "/8" << std::endl;
			}

			/* The request lines and the pointers, receive beside transmit. The
			 * transmit column is the receive column's control: the two are the
			 * same register and the same accumulator, so a transmit answer that
			 * moves while the receive answer does not is a difference in the
			 * machine and not in the instrument. */
			for(unsigned port = 0; port < rxDma.size(); ++port)
			{
				dsp56k::Peripherals56311& p = board.dspSet().peripherals(port);

				const dsp56k::TWord sr = esaiSrOr[port];

				std::cout << "rx-probe: esai port " << port
				          << " chainPosition=" << chainPositionOfPort(board, port, dspCount)
				          << " rxEnabled=" << p.getEsai().hasEnabledReceivers()
				          << " txEnabled=" << p.getEsai().hasEnabledTransmitters()
				          << " rxWordCount=" << p.getEsai().getRxWordCount()
				          << " txWordCount=" << p.getEsai().getTxWordCount()
				          << " srOr=" << dmaHex(sr)
				          << " RDFeverSet=" << ((sr >> dsp56k::Esai::M_RDF) & 1u)
				          << " ROEeverSet=" << ((sr >> dsp56k::Esai::M_ROE) & 1u)
				          << " TDEeverSet=" << ((sr >> dsp56k::Esai::M_TDE) & 1u)
				          << " DDR2=[" << dmaHex(ddr2Min[port]) << "," << dmaHex(ddr2Max[port])
				          << "] DSR4=[" << dmaHex(dsr4Min[port]) << "," << dmaHex(dsr4Max[port])
				          << "] DCR2or=" << dmaHex(dcr2Or[port])
				          << " DE2everSet=" << ((dcr2Or[port] >> dsp56k::DmaChannel::De) & 1u)
				          << " DCR4or=" << dmaHex(dcr4Or[port])
				          << " DE4everSet=" << ((dcr4Or[port] >> dsp56k::DmaChannel::De) & 1u)
				          << " rxRequestArmed=" << unsigned(rxTriggerEver[port])
				          << " txRequestArmed=" << unsigned(txTriggerEver[port])
				          << std::endl;
			}

			std::cout << "rx-probe: headCensus maxNonZeroWordsInWindow=" << rxMaxNonZero
			          << " atQuantum=" << rxMaxNonZeroAt
			          << " headDDR2 firstQuantum=" << dmaHex(rxDdr2First)
			          << " min=" << dmaHex(rxDdr2Min)
			          << " max=" << dmaHex(rxDdr2Max)
			          << " moved=" << ((rxDdr2Max > rxDdr2Min) ? 1 : 0) << std::endl;
		}

		reportSuppressedLogLines();

		/* The verdict is the worst of its clauses. A machine that never
		 * booted read its arrival off a chain that was not running, and a
		 * pattern that arrived at the wrong frame or changed on the way is a
		 * failure of the row and not a note beside it. */
		/* The verdict is the classifier's, and it is one word before it is a
		 * status. The clauses are the classifier's fields, so a reader is told
		 * which answer this was rather than handed a conjunction's false and
		 * left to reconstruct it.
		 *
		 * `rxArmed` is a condition of having reached the play phase at all, and
		 * that is where it belongs rather than in a sixth outcome arm. A drive
		 * that reached its bound with the receive path still dead did not
		 * measure the chain, so it is DID-NOT-RUN (status 2) and not STOPPED
		 * (status 1). Reporting it as STOPPED would be an absence of measurement
		 * wearing the word for a measured absence. */
		g2console::ImpulseObservation observation;
		observation.reachedPlayPhase = booted && landed && rxArmed && !halted && !faulted
			&& primedPulled == size_t(config.lookaheadFrames);
		observation.observerSelfTest = observerSelfTest;
		observation.framesPulled     = framesPulled;
		observation.arrival          = arrival;
		observation.arrivalExact     = arrivalExact;
		observation.expectedArrival  = expected;
		observation.countersZero     = countersZero;
		observation.sinkControlArrival = sinkControlArrival;
		observation.sinkControlExact   = sinkControlExact;

		const g2console::ImpulseOutcome outcome = g2console::classify(observation);

		reportOutcome(outcome, detailFor(outcome, observation));

		return g2console::exitStatus(outcome);
	}

	/* The GDB remote stub, opt-in and absent by default. `--gdb <port>` places
	 * the same machine `--boot` places -- the same image at the same entry, the
	 * same vector table, the same reset -- and then blocks until a debugger
	 * attaches instead of driving it. Nothing advances the machine here: the
	 * debugger's own `continue` and `stepi` are what run it, so
	 * a session that never attaches leaves a machine that has executed nothing.
	 *
	 * It drives the whole machine through the Scheduler, which is what `--boot`
	 * drives it with. The stub installs itself as that Scheduler's `McuRunner`,
	 * so the quantum order is turned by its one owning site and the stub adds
	 * only the decision point between two MCU instructions where a breakpoint
	 * compare has to happen. Without this the session steps `Board::runMcu`
	 * alone and stalls forever the first time the MCU waits on a DSP -- and
	 * reports a clean, plausible MISS rather than stalling visibly.
	 *
	 * There is no DSP56300 stub. The DSPs run; they are not instrumented.
	 * A breakpoint on a DSP program counter, a DSP register read or a DSP memory
	 * watchpoint is not available here. */
	int gdb(const uint16_t _port)
	{
		installLogFilter();

		g2::EnvArtifactResolver resolver;
		std::string why;

		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << why << std::endl;
			return 2;
		}

		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "CODE_30000400.bin is empty or unreadable under " << directory << std::endl;
			return 2;
		}

		g2::Board board(makeConfig());
		Ram ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "the image does not fit the configured SDRAM window" << std::endl;
			return 2;
		}

		// The vector table, exactly as the boot path places it: booting CODE
		// directly skips the code that builds it.
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
				std::cout << "the vector table does not fit the configured SDRAM window" << std::endl;
				return 2;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "the core refused VBR at register index " << g_regVbr << std::endl;
			return 2;
		}

		/* The Scheduler is declared after the Board so that it is destroyed
		 * before it: it borrows the Board's DSP set and installs chain callbacks
		 * into ESAIs the Board owns. The Executor is declared before the
		 * Scheduler for the same reason. Nothing here runs a frame -- the
		 * debugger's own `s` and `c` advance the machine. */
		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return 2;
		}

		/* The stub is constructed after the store is attached. Its watchpoint
		 * wrapper is interposed in front of whatever the memory map holds at that
		 * moment, so a target attached later would sit in front of the wrapper
		 * rather than behind it and its accesses would be invisible. */
		g2::GdbStub stub(board);

		/* The Scheduler is handed over before the socket opens, so no packet can
		 * be answered by an MCU-only session. The stub is destroyed before the
		 * Scheduler, which is what lets it remove its runner. */
		stub.attachScheduler(*scheduler);

		const uint16_t bound = stub.listenOn(_port);

		if(bound == 0)
		{
			std::cout << "the stub could not bind 127.0.0.1:" << _port << std::endl;
			return 2;
		}

		std::cout << "gdb stub listening on 127.0.0.1:" << bound << std::endl;
		std::cout << "connect with: m68k-elf-gdb -ex 'target remote 127.0.0.1:" << bound << "'"
		          << std::endl;
		std::cout << "the machine is at pc=0x" << std::hex << board.mcuReg(g_regPc) << std::dec
		          << " and has executed nothing; it runs when the debugger says so" << std::endl;

		if(!stub.waitForClient())
		{
			std::cout << "no debugger attached" << std::endl;
			return 2;
		}

		std::cout << "debugger attached" << std::endl;

		stub.serve();

		std::cout << "the debugger detached; pc=0x" << std::hex << board.mcuReg(g_regPc) << std::dec
		          << " halted=" << (board.mcuHalted() ? 1 : 0)
		          << " faulted=" << (board.faulted() ? 1 : 0) << std::endl;

		return 0;
	}

	/* The listing is machine-readable because a check reads it.
	 *
	 * `subcommands:` opens a block of one name per line, two spaces then the
	 * name; the block ends at the first line that is not of that shape.
	 * t0_console_subcommands compares this block against the
	 * `command == "--name"` comparisons in main() below and requires the two sets
	 * to be equal, so a subcommand dispatched without a line here is red, and a
	 * line here whose subcommand is not dispatched is red.
	 *
	 * A modifier is not a subcommand and sits under `options:` for that reason:
	 * --dump-dsp-dma is read out of a later argv entry by --boot and is never
	 * compared against argv[1]. */
	void usage()
	{
		std::cout << "usage: g2TestConsole <subcommand> [option ...]" << std::endl;
		std::cout << "subcommands:" << std::endl;
		std::cout << "  --boot           boot CODE_30000400.bin from NMG2_ARTIFACTS and print"
		             " display 0's 32 character cells" << std::endl;
		std::cout << "  --gdb <port>     place the same machine and serve a GDB remote session on"
		             " 127.0.0.1:<port>, blocking until a debugger attaches; 0 asks for a free"
		             " port. The debugger drives the whole machine through the Scheduler, so a"
		             " continue crosses a DSP handshake; breakpoints are MCF5307-side only"
		          << std::endl;
		std::cout << "  --help           print this listing and exit 0" << std::endl;
		std::cout << "  --impulse        TRANSPORT probe, and NOT an audio claim: boot, enter the"
		             " play phase, inject a known pattern at the codec source and report whether"
		             " and where it propagates, against (dspCount - 1) * hopFrames; also report"
		             " the seven chain-health counters. It prints one OUTCOME word -- PROPAGATED"
		             " (exit 0), PROPAGATED-OFF-SPEC or STOPPED (exit 1), DID-NOT-RUN (exit 2),"
		             " INSTRUMENT-BLIND (exit 3) -- so a chain that carried nothing, a machine that"
		             " never ran and an observer that saw nothing are told apart. An unpatched Nord"
		             " Modular is silent by design, so STOPPED is the expected answer with no patch"
		             " loaded and is not a defect in the transport. It also runs the ARRIVAL"
		             " instrument's own known positive: a sentinel placed at the tail position's"
		             " transmit source and read back out of the codec sink, so an `arrival=-1` is a"
		             " measured absence rather than a silence of unknown cause. A control that does"
		             " not come back is INSTRUMENT-BLIND" << std::endl;
		std::cout << "options:" << std::endl;
		std::cout << "  --dump-dsp-dma   modifier of --boot: additionally print each DSP"
		             " position's DDR2, DCO2 and DCO4 and check them against design section 2.3;"
		             " a mismatch exits non-zero" << std::endl;
		std::cout << "  --rx-probe       modifier of --impulse: additionally read the DSPs' ESAI"
		             " RECEIVE side during the play phase -- the four audio-channel DMA pointers"
		             " latched at the value the kernel programmed, and a scan of X memory for the"
		             " injected pattern at every position on every quantum plus a whole-internal-X"
		             " scan at the head. It plants a known word and scans for it first, and prints"
		             " that answer beside the measurement, so an absence is a measured one. It"
		             " prints and does not judge: the exit status is --impulse's own" << std::endl;
	}

	/* A refusal names the subcommand that refused, and the usage text is not a
	 * diagnostic. The listing prints every name, so "the output mentions the
	 * name" would be true of any refusal that printed usage and would say
	 * nothing about which subcommand failed. Every diagnostic therefore opens
	 * at column 0 with this prefix, which no indented listing line can. */
	void diagnose(const std::string& _subject, const std::string& _reason)
	{
		std::cerr << "g2TestConsole: " << _subject << ": " << _reason << std::endl;
	}

	// A subcommand that ran and failed must not be silent about which one it
	// was. The exit status is passed through unchanged; only the naming is
	// added, so no caller's meaning moves.
	int named(const std::string& _name, const int _result)
	{
		if(_result != 0)
			diagnose(_name, "failed with exit status " + std::to_string(_result));

		return _result;
	}
}

int main(int _argc, char** _argv)
{
	// No argument is not a success. A program that exits 0 and prints nothing is
	// exactly how a milestone check passes against a program that does nothing,
	// so an unrecognised invocation is an error here.
	if(_argc < 2)
	{
		usage();
		return 2;
	}

	const std::string command = _argv[1];

	if(command == "--boot")
	{
		/* An unrecognised modifier is an error and not a shrug: a `--boot` that
		 * read nothing past argv[1] would exit 0 for an option it does not
		 * implement. */
		bool dumpDspDma = false;

		for(int i = 2; i < _argc; ++i)
		{
			if(std::string(_argv[i]) == "--dump-dsp-dma")
			{
				dumpDspDma = true;
				continue;
			}

			diagnose("--boot", std::string("unrecognised option '") + _argv[i] + "'");
			usage();
			return 2;
		}

		return named("--boot", boot(dumpDspDma));
	}

	/* The port is required and it is not guessed at. A `--gdb` with no number, or
	 * with a number outside a port, is an error here rather than a default the
	 * operator did not choose. */
	if(command == "--gdb")
	{
		if(_argc != 3)
		{
			diagnose("--gdb", "expects exactly one port number");
			usage();
			return 2;
		}

		char*             end   = nullptr;
		const long        value = std::strtol(_argv[2], &end, 10);

		if(end == _argv[2] || *end != '\0' || value < 0 || value > 65535)
		{
			diagnose("--gdb", std::string("'") + _argv[2] + "' is not a port number in 0..65535");
			usage();
			return 2;
		}

		return named("--gdb", gdb(uint16_t(value)));
	}

	/* It takes no option, and an option handed to it is an error rather than a
	 * shrug, for the reason `--boot`'s modifier loop states. */
	if(command == "--impulse")
	{
		/* The modifier loop is `--boot`'s: an option this subcommand does not
		 * know is an error and not a shrug. */
		bool rxProbe = false;

		for(int i = 2; i < _argc; ++i)
		{
			if(std::string(_argv[i]) == "--rx-probe")
			{
				rxProbe = true;
				continue;
			}

			diagnose("--impulse", std::string("unrecognised option '") + _argv[i] + "'");
			usage();
			return 2;
		}

		return named("--impulse", impulse(rxProbe));
	}

	/* `--help` is a handled word and not a fall-through: reaching the usage text
	 * as an unrecognised flag exits 2, which is a defensible exit for an unknown
	 * flag and not for a --help. */
	if(command == "--help")
	{
		usage();
		return 0;
	}

	/* Nothing unimplemented exits 0, and the refusal says what it refused. A
	 * name this binary does not dispatch lands here, and an operator reads which
	 * argument was rejected rather than that something was. */
	diagnose("unrecognised argument", "'" + command + "'");
	usage();
	return 2;
}
