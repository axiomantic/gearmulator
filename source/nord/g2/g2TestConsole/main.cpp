// Task INT-1. The test console.
//
// Plan section 15 (INT-1), section 7.4.2. This directory is SHARED BY ORDER and
// not by ownership: INT-1 creates this file in Wave 4a, INT-2 extends it in
// Wave 5b, PLG-14 in Wave 6 and PERF-1 in Wave 7, and no two writers share a
// wave.
//
// WHAT THIS PROGRAM IS. `g2TestConsole --boot` boots the Clavia OS image
// directly at 0x30000400 and PRINTS display 0's 32 character cells. It asserts
// nothing; this program is the operator-facing window onto the same boot, and
// its output is meant to be read by a person bringing the machine up.
//
// IT EXISTS BECAUSE ITS ABSENCE WAS ITSELF A DEFECT. Until this file existed
// `g2TestConsole/CMakeLists.txt` generated a placeholder translation unit whose
// `main` returned 0 immediately, so the milestone's own acceptance command
// exited 0 and printed nothing -- plan section 24.6 row W3-95, the project's
// signature defect sitting on a milestone definition. The generator stops
// firing the moment this file is present.
//
// EVERY ADDRESS AND EVERY WINDOW BELOW HAS THE SAME PROVENANCE AS
// `g2Lib/test/t1_boot.cpp` AND IS DOCUMENTED THERE. Two of them -- CS0's base
// and CS4's base -- are INVENTED BY THIS HARNESS because no authority records
// them, and they are labelled at their site rather than presented as measured.

#include "board.h"
#include "executor.h"
#include "gdbStub.h"
#include "memoryMap.h"
#include "scheduler.h"
#include "status.h"
#include "artifactResolver.h"

#include "impulseOutcome.h"

// TASK M4 CLAUSE 1 reads the DMA registers of each position's peripheral set.
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

	// ---------------------------------------------------------------- section 6.6

	// Plan section 6.6.4 clause 1: the display buffer base, confirmed at
	// 0x30057040 as `addil #808062392,%d0`.
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

	// MEASURED, plan section 6.6.3: the loader's `movel #0x10000001,%d0` /
	// `movec %d0,%mbar` at loader offset 0x1E. The OS never writes MBAR, so a
	// direct boot of the OS image makes this the harness's job.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// MEASURED, plan section 6.6.9: CSAR2 = $1200 and CSMR2 = $007F0001 at
	// loader offsets 0x70 and 0x7c give 0x12000000..0x127FFFFF.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

	// INVENTED BY THIS HARNESS. No authority records CS0's or CS4's base; plan
	// section 4.2 register row 18 is still open on both. Neither value below is
	// a measurement and neither may be copied into a shipped header.
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;

	// MEASURED, the workspace logbook section 3.8: CS3 is a 64 KiB window,
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
	// table-translating copy, and the firmware remaps exactly five entries onto
	// the CGRAM alias range 0x08..0x0C --
	//
	//     'g' -> 0x08   'p' -> 0x09   'q' -> 0x0A   'y' -> 0x0B   'j' -> 0x0C
	//
	// -- the five descenders, whose glyph bitmaps it then uploads. A cell
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

	// A cell is content if it is an ordinary printable byte, or one of the five
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

	// The SDRAM the firmware executes from. board.cpp attaches the seven units
	// plan section 24.6 row W3-115 names and leaves Region::Sdram with no target
	// on purpose, so the store is the harness's to supply. Big-endian, matching
	// the part.
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
	// to mcf5307_create and therefore the path the core itself takes. The five
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
	// The four constants below are read off the kernel's own MOVEP block:
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
	 * question. This check establishes that the kernel PROGRAMMED each
	 * position's DMA block correctly, and nothing about whether any sample has
	 * ever moved through it. DDR is the DESTINATION pointer of a receive channel
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

	// Prints one line for each position and answers whether EVERY position
	// matched. THE VERDICT IS THE WORST POSITION: `all` is a conjunction over
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

		/* TASK M4 CLAUSE 1. The dump runs AFTER the drive, on the same Board the
		 * drive turned, and it is the last thing the program does before it
		 * answers -- the registers it reads are the ones the firmware left at
		 * the bound.
		 *
		 * IT IS SKIPPED ENTIRELY WITHOUT THE FLAG, so `--boot` alone answers on
		 * the banner predicate exactly as it did before this task. */
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
	 * A report is not a verdict: this function exits NON-ZERO unless the pattern
	 * arrived, arrived at exactly that frame, and arrived unchanged. */
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	/* The observer's known negative swaps these two, so a matcher stuck at true
	 * is caught only while they differ. */
	static_assert(g_impulseLeft != g_impulseRight,
		"The impulse slots must differ, or the swapped-slot control matches.");

	constexpr unsigned g_impulseOverrunQuanta = 1024u;

	/* The outcome is printed on every exit path, including the ones that leave
	 * before a board exists. Reconstructing an answer from the figures alone
	 * gives "no artifact" and "the chain carried nothing" the same one. */
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
			       + " observerSelfTest=" + std::to_string(_o.observerSelfTest ? 1 : 0);

		case g2console::ImpulseOutcome::Stopped:
			return "the observer received " + std::to_string(_o.framesPulled)
			       + " frames from the codec sink and the injected sample was in none of them, so"
			         " the path did not carry it to the sink. The observer is proven able to see"
			         " one (observerSelfTest=1), so this is a measured absence and not a blind one";

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

	int impulse()
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

		/* The drive leaves on the properties the play phase needs: display
		 * content composed, the settle window served, and every DSP position's
		 * program landed. A fixed count would either cost the full bound every
		 * run or hand beginPlayPhase a machine still downloading kernels. */
		uint32_t iteration = 0;
		uint32_t settle    = 0;
		bool     booted    = false;
		bool     landed    = false;

		for(; iteration < g_iterations; ++iteration)
		{
			scheduler->runFrames(g_framesPerIteration);

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
			{
				landed = true;
				break;
			}
		}

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

		/* The walk below and the self-test after it drive THE SAME two
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

		for(unsigned q = 0; q < walk; ++q)
		{
			const g2::Frame& in = (q == 0) ? impulseFrame : silence;

			(void) scheduler->push(&in, 1);
			scheduler->runFrames(1);

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

		/* The observer's own controls, run on the detector and not on the chain.
		 * The loop above reports an absence, and an absence reported by a
		 * detector that cannot detect is not evidence.
		 *
		 * THE SWAPPED-SLOT FRAME IS THE CONTROL THAT COSTS SOMETHING. A
		 * `matches` wired to a constant true survives both other controls:
		 * silence never reaches it, because `seen` rejects the zero frame
		 * first, and the injected frame is supposed to match. Only a frame that
		 * is non-zero AND wrong separates a matcher from a matcher that always
		 * says yes -- which is the exact failure that would report every
		 * arriving frame as arrivalExact. */
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
		std::cout << "impulse: primedPulled=" << primedPulled
		          << " walkQuanta=" << walk
		          << " framesPulled=" << framesPulled
		          << " observerSelfTest=" << (observerSelfTest ? 1 : 0)
		          << " arrival=" << arrival
		          << " arrivalExact=" << (arrivalExact ? 1 : 0) << std::endl;

		/* The chain-health counters. The per-position figures are reported as
		 * their maximum over the positions, with the position that carried it,
		 * so a reader is told WHICH one moved. */
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

		reportSuppressedLogLines();

		/* THE VERDICT, AND IT IS THE WORST OF ITS CLAUSES. A machine that never
		 * booted read its arrival off a chain that was not running, and a
		 * pattern that arrived at the wrong frame or changed on the way is a
		 * failure of the row and not a note beside it. */
		/* The verdict is the classifier's, and it is one word before it is a
		 * status: a reader is told which answer this was rather than handed a
		 * conjunction's false and left to reconstruct it. */
		g2console::ImpulseObservation observation;
		observation.reachedPlayPhase = booted && landed && !halted && !faulted
			&& primedPulled == size_t(config.lookaheadFrames);
		observation.observerSelfTest = observerSelfTest;
		observation.framesPulled     = framesPulled;
		observation.arrival          = arrival;
		observation.arrivalExact     = arrivalExact;
		observation.expectedArrival  = expected;
		observation.countersZero     = countersZero;

		const g2console::ImpulseOutcome outcome = g2console::classify(observation);

		reportOutcome(outcome, detailFor(outcome, observation));

		return g2console::exitStatus(outcome);
	}

	/* TASK TOOL-13. THE GDB REMOTE STUB, AND IT IS OPT-IN AND ABSENT BY DEFAULT.
	 * `--gdb <port>` places the same machine `--boot` places -- the same image at
	 * the same entry, the same vector table, the same reset -- and then BLOCKS
	 * until a debugger attaches instead of driving it. NOTHING ADVANCES THE
	 * MACHINE HERE: the debugger's own `continue` and `stepi` are what run it, so
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
	 * There is still no DSP56300 stub. The DSPs run; they are not instrumented.
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
		 * BEFORE it: it borrows the Board's DSP set and installs chain callbacks
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

		/* THE STUB IS CONSTRUCTED AFTER THE STORE IS ATTACHED. Its watchpoint
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
		             " loaded and is not a defect in the transport" << std::endl;
		std::cout << "options:" << std::endl;
		std::cout << "  --dump-dsp-dma   modifier of --boot: additionally print each DSP"
		             " position's DDR2, DCO2 and DCO4 and check them against design section 2.3;"
		             " a mismatch exits non-zero" << std::endl;
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
	// NO ARGUMENT IS NOT A SUCCESS. The placeholder this file replaces exited 0
	// and printed nothing, which is exactly how a milestone check passes against
	// a program that does nothing. An unrecognised invocation is an error here.
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
		if(_argc != 2)
		{
			diagnose("--impulse", "takes no option");
			usage();
			return 2;
		}

		return named("--impulse", impulse());
	}

	/* TASK PLG-14. `--help` IS A HANDLED WORD AND NOT A FALL-THROUGH. Before
	 * this task it printed the usage text only because it was UNRECOGNISED, and
	 * it exited 2 -- a defensible exit for an unknown flag and not a --help. */
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
