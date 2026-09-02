// This test needs the Clavia firmware artifacts and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// The board does not program the DMA; the firmware does, through the kernel it
// downloads. So this file boots the real firmware on the real composition and
// then reads, out of the emulated DSPs' own peripherals, the four DMA channels
// the kernel programs. Nothing here writes a DMA register. A test that
// programmed the channels it then checked would assert that this file can
// spell.
//
// The three things it asserts, and where each figure comes from:
//
//   1. Eight DSPs each receive one whole kernel image. The word count is not a
//      literal here: it is read out of the firmware's own container header and
//      only then compared against 573. See findKernelImages below for why the
//      header is self-identifying and why a scan for it cannot land on a
//      coincidence.
//
//   2. The four channels carry the documented constants, position by position.
//
//   3. The DCR request source is the hardware field and not the library
//      enumerator. The two number spaces agree for the primary ESAI (11 and
//      12) and differ for ESAI_1, where the hardware writes 21 and 22 while the
//      library names Esai1ReceiveData = 22 and Esai1TransmitData = 23. A check
//      that took the library value for the wire value would demand a transmit
//      source on the receive channel and call the result an acceptance
//      criterion.
//
// Chain position is not hardware port, and this file derives the map rather
// than carrying it. The map is the firmware's own nine-entry table at
// 0x30116970, and readChainPositions below reads it out of the booted machine.
// The ordering itself is deliberately not written into this file as data: a
// copy of it here would be a second definition site that a firmware change
// could not move.
//
// Every verdict is an observable and not an assert(). The default build is
// Release with NDEBUG, which deletes every assert(), so a predicate spelled as
// one is a predicate the shipped build does not have. Nothing below calls
// assert(); the static_assert uses are compile-time and survive NDEBUG by
// construction.

#include "gatedFixture.h"

#include "../board.h"
#include "../executor.h"
#include "../hdi08Decode.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

#include "dsp56kBase/logging.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
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

	// The ESAI underrun log filter. The underruns are real and expected in the
	// boot regime, because nothing drains the ESAIs until the codec queues
	// arrive. This hides the repetition and nothing else. Set
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

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase = 0x10000000u;

	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;
	constexpr uint32_t g_cs3Size = 0x00010000u;
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;
	constexpr uint32_t g_sdramSize = 0x00800000u;
	constexpr uint32_t g_cs1Size = 0x00010000u;
	constexpr uint32_t g_cs5Size = 0x00000010u;

	// The SDRAM store the harness supplies. board.cpp leaves Region::Sdram with
	// no target on purpose, so a firmware image has to live somewhere. A plain
	// big-endian byte array with no decode of its own; every address it answers
	// has already been decoded by the MemoryMap.
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

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
				m_bytes[index] = uint8_t((_value >> shift) & 0xffu);
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
		std::vector<uint8_t> m_bytes;
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

		config.memory.cs0   = {g_cs0Base,     g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base, g_cs1Size};
		config.memory.cs2   = {g_cs2Base,     g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base, g_cs3Size};
		config.memory.cs4   = {g_cs4Base,     g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base, g_cs5Size};
		config.memory.mbar  = {g_mbarBase,    g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};

		return config;
	}

	// Each DSP kernel image in the OS image is a 10-byte header followed by the
	// code:
	//
	//   +0x00  4  pointer to the first code word: the image address plus 0x0A
	//   +0x04  4  zero, most probably the P-memory load address
	//   +0x08  2  the byte count of the code, three bytes for each 24-bit word
	//   +0x0A  n  the code, most significant byte first
	//
	// The scan is not a search for a magic number. The +0x00 field is a
	// self-reference: it must equal the load address of the scan position plus
	// 0x0A. A run of bytes that happens to look like a header has to also carry,
	// four bytes earlier, the exact address of where it sits in a machine it
	// knows nothing about. That is the identification; the byte-count field is
	// then read and never matched against.
	//
	// The word count is derived and not typed: it is the header's byte count
	// divided by three. The documented figure of 573 is asserted against that
	// derivation at the assertion site, which is the direction that makes the
	// firmware the authority and the document the claim under test.
	//
	// The scan finds seven containers and not three. Measured over
	// CODE_30000400.bin: seven containers stand in one contiguous run at
	// 0x30108150, 0x30108814, 0x30108ED8, 0x3010959C, 0x301096AA, 0x30109750 and
	// 0x30109832, carrying 573, 573, 573, 86, 51, 71 and 19 words. The first
	// three are the kernel images. The next three are P-code overlays --
	// 0x30038D1E downloads 86 words to DSP 0, 51 to the middle DSPs and 71 to
	// DSP N-1 -- which arrive at patch-build time and not at boot, and the
	// seventh, of 19 words, is named by nothing this file could find.
	//
	// So the scan is deliberately not narrowed to three. Narrowing it by word
	// count would make the 573 this file exists to check into the thing that
	// selects what gets checked, and a check that picks its own evidence by the
	// answer is no check. Which container is a kernel is decided by the machine:
	// the eight booted DSPs are asked which container their program memory
	// holds, and the word count is read off whichever one each of them names.
	constexpr uint32_t g_containerHeaderSize = 0x0Au;
	constexpr uint32_t g_bytesPerWord        = 3u;

	// 573 words of 24-bit code. The one place this file names the figure, and it
	// appears as the right-hand side of a comparison against a count read out of
	// the firmware.
	constexpr uint32_t g_designWordCount = 573u;

	// The OS carries three kernel images, and they are not copies of each other.
	constexpr size_t g_designImageCount = 3u;

	struct KernelImage
	{
		uint32_t              fileOffset = 0;   // of the header
		uint32_t              loadAddress = 0;  // of the header
		uint32_t              byteCount = 0;    // the +0x08 field, verbatim
		std::vector<uint32_t> words;            // the code, unpacked to 24-bit words
	};

	uint32_t beU32(const std::vector<uint8_t>& _d, const size_t _at)
	{
		return (uint32_t(_d[_at]) << 24) | (uint32_t(_d[_at + 1]) << 16) |
		       (uint32_t(_d[_at + 2]) << 8) | uint32_t(_d[_at + 3]);
	}

	uint32_t beU16(const std::vector<uint8_t>& _d, const size_t _at)
	{
		return (uint32_t(_d[_at]) << 8) | uint32_t(_d[_at + 1]);
	}

	std::vector<KernelImage> findKernelImages(const std::vector<uint8_t>& _code, const uint32_t _loadAddress)
	{
		std::vector<KernelImage> out;

		if(_code.size() < g_containerHeaderSize)
			return out;

		for(size_t off = 0; off + g_containerHeaderSize <= _code.size(); ++off)
		{
			// The self-reference. See the block comment above.
			if(beU32(_code, off) != _loadAddress + uint32_t(off) + g_containerHeaderSize)
				continue;

			if(beU32(_code, off + 4) != 0u)
				continue;

			const uint32_t byteCount = beU16(_code, off + 8);

			if(byteCount == 0u || off + g_containerHeaderSize + byteCount > _code.size())
				continue;

			if(byteCount % g_bytesPerWord != 0u)
				continue;

			KernelImage image;
			image.fileOffset  = uint32_t(off);
			image.loadAddress = _loadAddress + uint32_t(off);
			image.byteCount   = byteCount;

			const uint32_t words = byteCount / g_bytesPerWord;
			image.words.reserve(words);

			for(uint32_t w = 0; w < words; ++w)
			{
				const size_t at = off + g_containerHeaderSize + size_t(w) * g_bytesPerWord;
				image.words.push_back((uint32_t(_code[at]) << 16) |
				                      (uint32_t(_code[at + 1]) << 8) |
				                       uint32_t(_code[at + 2]));
			}

			out.push_back(std::move(image));
		}

		return out;
	}

	// The firmware builds a nine-entry table at 0x30116970 (`set_hdi08_bases`,
	// 0x300391E8) at boot. Entry i holds the CS1 address of the port at chain
	// position i, and CS1's A3 to A10 are eight active-low one-cold selects, so
	// the port is the index of the single line pulled down. The ninth entry is
	// the broadcast address and belongs to no position.
	//
	// It is read out of the booted machine, which is the whole point: this is
	// the firmware's own answer to "which port is which", taken without
	// inference from anything on our side of the boundary.
	constexpr uint32_t g_portTableBase = 0x30116970u;

	// Returns the number of ports the table named, and fills _positionOfPort so
	// that _positionOfPort[port] is that port's chain position. A port the table
	// does not name is left at _count, which is out of range and reads as a
	// mismatch rather than as position zero.
	unsigned readChainPositions(g2::Board& _board, const unsigned _count, std::vector<unsigned>& _positionOfPort)
	{
		_positionOfPort.assign(_count, _count);

		unsigned named = 0;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t entry =
				g2::Board::onRead(&_board, g_portTableBase + position * 4u, 4, &status);

			if(status != MCF5307_BUS_OK)
				continue;

			const auto selects = uint8_t((entry >> 3) & 0xffu);
			const auto low     = uint8_t(~selects);

			// Exactly one line low is a port; none low is the broadcast address.
			if(low == 0u || (low & uint8_t(low - 1u)) != 0u)
				continue;

			unsigned port = 0;
			for(uint8_t bit = low; bit > 1u; bit >>= 1)
				++port;

			if(port >= _count)
				continue;

			_positionOfPort[port] = position;
			++named;
		}

		return named;
	}

	// The four channels the kernel programs, each in the order the MOVEP block
	// writes them: DSR, DDR, DCO, DCR.
	//
	// The request source is the hardware DCR field. Reading these as library
	// `RequestSource` enumerators would demand 22 and 23 on channels 3 and 5 --
	// and library 22 is Esai1ReceiveData, so channel 5, the ESAI_1 transmit
	// channel, would be asserted to sit on a receive source.
	constexpr dsp56k::TWord g_esaiRx0   = 0xFFFFA8u;  // ESAI RX0
	constexpr dsp56k::TWord g_esai1Rx0  = 0xFFFF88u;  // ESAI_1 RX0
	constexpr dsp56k::TWord g_esaiTx0   = 0xFFFFA0u;  // ESAI TX0
	constexpr dsp56k::TWord g_esai1Tx2  = 0xFFFF82u;  // ESAI_1 TX2

	constexpr dsp56k::TWord g_ddr2Chain = 0x001C00u;
	constexpr dsp56k::TWord g_ddr2Head  = 0x001C04u;
	constexpr dsp56k::TWord g_ddr3      = 0x001C10u;
	constexpr dsp56k::TWord g_dsr4      = 0x001D00u;
	constexpr dsp56k::TWord g_dsr5      = 0x001D10u;

	// The DCO count field. $007001 gives 8 outer slots, the inter-DSP bus;
	// $001001 gives 2, the stereo pair at the chain's two ends.
	constexpr dsp56k::TWord g_dcoChain    = 0x007001u;
	constexpr dsp56k::TWord g_dcoEndpoint = 0x001001u;

	// The hardware DCR request source.
	constexpr dsp56k::TWord g_rsEsaiRx  = 11u;
	constexpr dsp56k::TWord g_rsEsai1Rx = 21u;
	constexpr dsp56k::TWord g_rsEsaiTx  = 12u;
	constexpr dsp56k::TWord g_rsEsai1Tx = 22u;

	// The request source occupies DCR bits 15 to 11 -- DmaChannel::DcrBits names
	// them Drs0 to Drs4. The shift is taken from that enumeration rather than
	// typed, so a library that renumbered the field would move this with it.
	constexpr uint32_t g_drsShift = uint32_t(dsp56k::DmaChannel::DcrBits::Drs0);
	constexpr uint32_t g_drsMask  = 0x1Fu;

	static_assert(uint32_t(dsp56k::DmaChannel::DcrBits::Drs4) ==
	              uint32_t(dsp56k::DmaChannel::DcrBits::Drs0) + 4u,
	              "the DMA request source must be five contiguous DCR bits");

	// The library's own values for the ESAI_1 pair. They are named here only so
	// that the difference this file exists to hold apart is visible in the
	// source, and they are never used as an expectation.
	static_assert(uint32_t(dsp56k::DmaChannel::RequestSource::Esai1ReceiveData) != g_rsEsai1Rx &&
	              uint32_t(dsp56k::DmaChannel::RequestSource::Esai1TransmitData) != g_rsEsai1Tx,
	              "The two number spaces have collapsed: the hardware DCR field and the "
	              "library enumerator now agree for ESAI_1, so this file's distinction is stale");

	// The MOVEP immediate encoding is `08 F4 xx` plus one immediate word. The
	// byte `xx` is the field `1Spppppp`. The bit `S` selects the X or the Y
	// space. The six bits `pppppp` give the address as $FFFFC0 + pppppp.
	//
	// This exists beside the register reads and does not replace them. Two of
	// the sixteen registers cannot be read back off a running DSP: see the
	// transmit-source clause at the assertion site. For those two the kernel's
	// own instruction is the only place the write is still visible, and for the
	// others the register read is the stronger evidence because it proves the
	// emulator acted on the write.
	constexpr uint32_t g_movepImmediateOpcode = 0x08F4u;
	constexpr uint32_t g_movepPeripheralBase  = 0xFFFFC0u;
	constexpr uint32_t g_movepAddressMask     = 0x3Fu;
	constexpr uint32_t g_movepYSpaceBit       = 0x40u;
	constexpr uint32_t g_movepImmediateBit    = 0x80u;

	// The first X-space immediate written to each peripheral address. A later
	// write to the same address is deliberately not taken: what is recorded is
	// the programming and not what the running kernel does with the register
	// afterwards.
	std::map<uint32_t, dsp56k::TWord> decodeMovepImmediates(const KernelImage& _image)
	{
		std::map<uint32_t, dsp56k::TWord> out;

		const std::vector<uint32_t>& words = _image.words;

		for(size_t w = 0; w + 1 < words.size(); ++w)
		{
			if((words[w] >> 8) != g_movepImmediateOpcode)
				continue;

			const uint32_t xx = words[w] & 0xFFu;

			if((xx & g_movepImmediateBit) == 0u || (xx & g_movepYSpaceBit) != 0u)
				continue;

			const uint32_t address = g_movepPeripheralBase + (xx & g_movepAddressMask);

			out.emplace(address, dsp56k::TWord(words[w + 1]));

			// The immediate is the instruction's second word and is not itself an
			// opcode, so the scan steps over it.
			++w;
		}

		return out;
	}

	// The four register addresses of one DMA channel, derived from the library's
	// own XIO enumeration rather than typed. The block descends: channel 5 is
	// lowest and each channel is four registers, DCR, DCO, DDR, DSR ascending.
	struct ChannelRegisters
	{
		uint32_t dsr;
		uint32_t ddr;
		uint32_t dco;
		uint32_t dcr;
	};

	constexpr uint32_t g_dmaRegistersPerChannel = 4u;
	constexpr uint32_t g_dmaHighestChannel      = 5u;

	constexpr ChannelRegisters channelRegisters(const dsp56k::TWord _channel)
	{
		const uint32_t dcr = uint32_t(dsp56k::XIO_DCR5) +
			(g_dmaHighestChannel - uint32_t(_channel)) * g_dmaRegistersPerChannel;

		return ChannelRegisters{dcr + 3u, dcr + 2u, dcr + 1u, dcr};
	}

	// The derivation is held against the enumeration at both ends of the block,
	// so a library that renumbered it fails the build instead of moving the
	// addresses this file reads.
	static_assert(channelRegisters(5u).dcr == uint32_t(dsp56k::XIO_DCR5) &&
	              channelRegisters(5u).dsr == uint32_t(dsp56k::XIO_DSR5) &&
	              channelRegisters(2u).dcr == uint32_t(dsp56k::XIO_DCR2) &&
	              channelRegisters(2u).dsr == uint32_t(dsp56k::XIO_DSR2) &&
	              channelRegisters(4u).dco == uint32_t(dsp56k::XIO_DCO4) &&
	              channelRegisters(3u).ddr == uint32_t(dsp56k::XIO_DDR3),
	              "the DMA register block is not four descending channels of DCR, DCO, DDR, DSR");

	// Where the peripheral block starts, so "this address is a peripheral" is the
	// library's own boundary and not a literal.
	constexpr uint32_t g_peripheralSpaceFirst = uint32_t(dsp56k::XIO_Reserved_High_First);

	struct ChannelExpectation
	{
		dsp56k::TWord channel;
		dsp56k::TWord dsr;
		dsp56k::TWord ddr;
		dsp56k::TWord dco;
		dsp56k::TWord requestSource;
	};

	// One position's four rows of the expectation. `_count` and not a literal
	// eight, so the tail follows the DSP set's own size.
	std::vector<ChannelExpectation> expectedChannels(const unsigned _position, const unsigned _count)
	{
		const bool head = _position == 0u;
		const bool tail = _count > 0u && _position + 1u == _count;

		return {
			{2u, g_esaiRx0,  head ? g_ddr2Head : g_ddr2Chain, head ? g_dcoEndpoint : g_dcoChain, g_rsEsaiRx },
			{3u, g_esai1Rx0, g_ddr3,                          g_dcoChain,                        g_rsEsai1Rx},
			{4u, g_dsr4,     g_esaiTx0,                       tail ? g_dcoEndpoint : g_dcoChain, g_rsEsaiTx },
			{5u, g_dsr5,     g_esai1Tx2,                      g_dcoChain,                        g_rsEsai1Tx}};
	}

	std::string hex6(const uint32_t _value)
	{
		char buf[16];
		std::snprintf(buf, sizeof buf, "$%06X", unsigned(_value));
		return buf;
	}

	// Written out rather than read from the object under test: DspSet holds a
	// fixed array and dspCount() returns its size, so a comparison against that
	// same accessor would agree with itself whatever the array became.
	constexpr unsigned g_expectedDspCount = 8u;

	// The iteration bound is a stop and not a figure the firmware publishes, so
	// a machine that never converges fails rather than hanging the suite. The
	// loop leaves early on the convergence predicate, so this number bounds a
	// failure and does not price a pass.
	constexpr uint32_t g_iterationBound = 500000u;

	constexpr size_t g_framesPerIteration = 1;

	// What one boot produced. Every assertion reads from here, so the machine
	// runs once and no assertion can quietly get a second answer.
	struct LoadResult
	{
		bool imageLoaded = false;

		uint32_t iterations = 0;
		bool     halted     = false;
		bool     faulted    = false;
		bool     converged  = false;

		unsigned dspCount = 0;

		// Indexed by hardware port throughout. The chain position of port i is
		// positionOfPort[i]; see readChainPositions.
		std::vector<unsigned> positionOfPort;
		unsigned              positionsNamed = 0;

		std::vector<bool> landed;

		struct Registers
		{
			dsp56k::TWord dsr = 0;
			dsp56k::TWord ddr = 0;
			dsp56k::TWord dco = 0;
			dsp56k::TWord dcr = 0;
			bool          latched = false;
		};

		// [port][channel], at the instant the channel was armed. This is the set
		// the assertions read; see latchChannels for why a terminal read is the
		// wrong instant for two of the four registers.
		std::vector<std::vector<Registers>> armed;

		// [port][channel] at the end of the run, reported and never asserted, so
		// that the pointers the DMA moved stay visible instead of being hidden by
		// the latch.
		std::vector<std::vector<Registers>> terminal;

		// Which discovered kernel image each port's P memory holds, or -1.
		std::vector<int> imageOfPort;
	};

	constexpr dsp56k::TWord g_channelCount = 6u;

	bool everyChannelProgrammed(g2::Board& _board, const unsigned _count)
	{
		for(unsigned port = 0; port < _count; ++port)
		{
			dsp56k::Dma& dma = _board.dspSet().peripherals(port).getDMA();

			for(const auto& e : expectedChannels(0u, _count))
			{
				if(dma.getDCR(e.channel) == 0u)
					return false;
			}
		}

		return true;
	}

	/* The instant is part of the observation, and a terminal read is the wrong
	 * one for two of the four registers. A terminal read of channel 4's DSR
	 * gives $001E0C and of channel 5's gives $001E1C, against the documented
	 * $001D00 and $001D10. Neither is a defect: DSR is the source pointer of a
	 * transmit channel and `DmaChannel::execTransfer` advances it on every
	 * transfer (`dma.cpp`, the `increment(m_dsr)` and `m_dsr += ...` sites), so
	 * after eighteen thousand frames it holds wherever the traversal has
	 * reached and not what the kernel wrote. The receive channels' DSR is a
	 * fixed peripheral address and does not move, which is exactly why the
	 * defect showed on two rows and not on eight.
	 *
	 * The kernel's own MOVEP block settles it without inference from either
	 * side: decoding the three 573-word images out of CODE_30000400.bin gives
	 * `MOVEP #$001D00,X:$FFFFDF` and `MOVEP #$001D10,X:$FFFFDB` in all three --
	 * DSR4 and DSR5 -- so the documented table is right and the register had
	 * simply moved on.
	 *
	 * So each register is latched at its own first non-zero value, and the four
	 * are not sampled together. Sampling all four at the instant the DCR first
	 * goes non-zero reports $001E00 and $001E10 -- the documented pair plus
	 * exactly $100, identically on all eight ports -- because DCR4 is one of the
	 * three register-form MOVEP writes and lands later than the immediate block,
	 * by which time the kernel has already moved DSR to the second transmit
	 * bank. A per-register latch reads the first write each register takes.
	 *
	 * What this does not claim: the sample is taken once per ESAI frame, so it
	 * is the first frame boundary at or after the write and not the writing
	 * instruction itself. A register written twice inside one frame would be
	 * latched at the second value.
	 *
	 * A register whose first written value is zero never latches, and the DCR
	 * clause below is what reports that rather than letting a zero read as an
	 * expectation that happened to be zero. */
	bool latchFirst(dsp56k::TWord& _slot, const dsp56k::TWord _now)
	{
		if(_slot != 0u || _now == 0u)
			return false;

		_slot = _now;
		return true;
	}

	void latchChannels(g2::Board& _board, LoadResult& _result)
	{
		for(unsigned port = 0; port < _result.dspCount; ++port)
		{
			dsp56k::Dma& dma = _board.dspSet().peripherals(port).getDMA();

			for(dsp56k::TWord channel = 0; channel < g_channelCount; ++channel)
			{
				LoadResult::Registers& r = _result.armed[port][channel];

				latchFirst(r.dsr, dma.getDSR(channel));
				latchFirst(r.ddr, dma.getDDR(channel));
				latchFirst(r.dco, dma.getDCO(channel));

				if(latchFirst(r.dcr, dma.getDCR(channel)))
					r.latched = true;
			}
		}
	}

	bool everyProgramLanded(g2::Board& _board, const unsigned _count)
	{
		for(unsigned port = 0; port < _count; ++port)
		{
			const bool* const landed = _board.dspSet().programLanded(port);

			if(landed == nullptr || !*landed)
				return false;
		}

		return true;
	}

	bool runLoad(const std::string& _directory, const std::vector<KernelImage>& _images, LoadResult& _result)
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

		// The vector table: big-endian, 256 identical longwords.
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
		_result.imageLoaded = true;

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		_result.dspCount = board.dspSet().dspCount();

		_result.armed.assign(_result.dspCount, std::vector<LoadResult::Registers>(g_channelCount));

		/* The drive leaves on the property the assertions read, and that is
		 * deliberate. The convergence predicate is "every slot has taken a
		 * program and every slot's four DMA channels carry a non-zero DCR", so
		 * the registers this file goes on to check are the registers that made
		 * the loop stop. A fixed iteration count would either cost the suite the
		 * full bound on every run or sample a machine mid-download.
		 *
		 * It is not the same predicate as the acceptance. Convergence asks only
		 * that a DCR is non-zero; the acceptance asks what is in it. A firmware
		 * that armed four channels with the wrong sources would satisfy the loop
		 * and fail the assertions, which is the direction that keeps the exit
		 * condition from deciding the verdict. */
		for(uint32_t i = 0; i < g_iterationBound; ++i)
		{
			_result.iterations = i + 1;

			scheduler->runFrames(g_framesPerIteration);

			// Sampled before the exit tests, so the iteration that satisfies the
			// convergence predicate still contributes its observation.
			latchChannels(board, _result);

			if(everyProgramLanded(board, _result.dspCount) &&
			   everyChannelProgrammed(board, _result.dspCount))
			{
				_result.converged = true;
				break;
			}

			if(board.mcuHalted())
				break;
		}

		_result.halted  = board.mcuHalted();
		_result.faulted = board.faulted();

		_result.positionsNamed = readChainPositions(board, _result.dspCount, _result.positionOfPort);

		_result.landed.assign(_result.dspCount, false);
		_result.imageOfPort.assign(_result.dspCount, -1);
		_result.terminal.assign(_result.dspCount, std::vector<LoadResult::Registers>(g_channelCount));

		for(unsigned port = 0; port < _result.dspCount; ++port)
		{
			const bool* const landed = board.dspSet().programLanded(port);
			_result.landed[port] = landed != nullptr && *landed;

			dsp56k::Dma& dma = board.dspSet().peripherals(port).getDMA();

			for(dsp56k::TWord channel = 0; channel < g_channelCount; ++channel)
			{
				_result.terminal[port][channel] =
					LoadResult::Registers{dma.getDSR(channel), dma.getDDR(channel),
					                      dma.getDCO(channel), dma.getDCR(channel), true};
			}

			// Which image landed, read out of the DSP's own P memory. A whole
			// image matching word for word is what "received 573 words" means
			// operationally: a download that stopped short leaves the remainder
			// unwritten and cannot match.
			const dsp56k::Memory& memory = board.dspSet().dsp(port).memory();

			// The longest match wins and not the first. A short container is a
			// prefix of a long one whenever their first words agree, so a
			// first-match rule would let a 19-word overlay claim a slot holding a
			// 573-word kernel and the word-count assertion below would then be
			// held to the overlay's header.
			size_t best = 0;

			for(size_t index = 0; index < _images.size(); ++index)
			{
				const std::vector<uint32_t>& words = _images[index].words;

				if(words.size() <= best)
					continue;

				bool same = true;

				for(uint32_t w = 0; w < words.size() && same; ++w)
					same = memory.get(dsp56k::MemArea_P, dsp56k::TWord(w)) == dsp56k::TWord(words[w]);

				if(same)
				{
					_result.imageOfPort[port] = int(index);
					best = words.size();
				}
			}
		}

		return true;
	}

	void report(const LoadResult& _r, const std::vector<KernelImage>& _images)
	{
		std::cout << "kernel-load: iterations=" << _r.iterations
		          << " converged=" << (_r.converged ? 1 : 0)
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0)
		          << " dspCount=" << _r.dspCount
		          << " positionsNamed=" << _r.positionsNamed
		          << std::endl;

		for(size_t i = 0; i < _images.size(); ++i)
		{
			std::cout << "kernel-load: image " << i
			          << " at " << hex6(_images[i].loadAddress)
			          << " byteCount=" << _images[i].byteCount
			          << " words=" << _images[i].words.size()
			          << std::endl;
		}

		for(unsigned port = 0; port < _r.dspCount; ++port)
		{
			const unsigned position = _r.positionOfPort[port];

			std::cout << "kernel-load: port " << port
			          << " position=" << position
			          << " landed=" << (_r.landed[port] ? 1 : 0)
			          << " image=" << _r.imageOfPort[port]
			          << std::endl;

			for(const auto& e : expectedChannels(position, _r.dspCount))
			{
				const LoadResult::Registers& m = _r.armed[port][e.channel];
				const LoadResult::Registers& t = _r.terminal[port][e.channel];

				std::cout << "kernel-load:   channel " << e.channel
				          << " armed=" << (m.latched ? 1 : 0)
				          << " DSR=" << hex6(m.dsr) << "/" << hex6(e.dsr)
				          << " DDR=" << hex6(m.ddr) << "/" << hex6(e.ddr)
				          << " DCO=" << hex6(m.dco) << "/" << hex6(e.dco)
				          << " DCR=" << hex6(m.dcr)
				          << " rs=" << ((m.dcr >> g_drsShift) & g_drsMask) << "/" << e.requestSource
				          << std::endl;

				// The pointers the running DMA moved, printed beside the armed
				// sample so the difference between the two instants is visible.
				std::cout << "kernel-load:     at end  DSR=" << hex6(t.dsr)
				          << " DDR=" << hex6(t.ddr)
				          << " DCO=" << hex6(t.dco)
				          << " DCR=" << hex6(t.dcr)
				          << std::endl;
			}
		}
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

		// The word count the machine is going to be held to is read out of the
		// firmware first, so a failure here is read as "the container is not
		// where it is documented to be" and never as "the DSPs did not receive
		// the kernel".
		const std::vector<uint8_t> code = readFile(directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << directory << std::endl;
			return false;
		}

		const std::vector<KernelImage> images = findKernelImages(code, g_entryPc);

		if(images.empty())
		{
			std::cout << "FAIL the container scan found no self-identifying container at all, "
			             "so nothing below could be held to a word count" << std::endl;
			return false;
		}

		// Three kernel images, and they are not copies of each other. The count
		// is taken over the containers that carry the documented word count; the
		// containers that do not are the patch-time overlays and are reported
		// rather than hidden.
		{
			std::vector<const KernelImage*> kernels;

			for(const KernelImage& image : images)
			{
				if(image.words.size() == g_designWordCount)
					kernels.push_back(&image);
			}

			check(kernels.size() == g_designImageCount,
			      "the OS image carries " + std::to_string(g_designImageCount) + " containers of " +
			      std::to_string(g_designWordCount) + " words; it carries " +
			      std::to_string(kernels.size()) + " of " + std::to_string(images.size()) +
			      " self-identifying containers in all");

			bool allDifferent = true;

			for(size_t i = 0; i < kernels.size(); ++i)
			{
				for(size_t j = i + 1; j < kernels.size(); ++j)
					allDifferent = allDifferent && kernels[i]->words != kernels[j]->words;
			}

			check(kernels.size() == g_designImageCount && allDifferent,
			      "those containers hold images that differ from one another; a "
			      "scan that found the same bytes three times found one image and two coincidences");
		}

		// Every container's byte count is a whole number of 24-bit words. The scan
		// already refuses one that is not, so this states the property the scan
		// enforces rather than re-testing it -- what it can catch is a scan
		// narrowed later in a way that stops enforcing it.
		for(size_t i = 0; i < images.size(); ++i)
		{
			check(images[i].byteCount % g_bytesPerWord == 0u,
			      "container " + std::to_string(i) + " at " + hex6(images[i].loadAddress) +
			      " has a byte count of " + std::to_string(images[i].byteCount) +
			      ", a whole number of 24-bit words");
		}

		LoadResult result;

		if(!runLoad(directory, images, result))
			return false;

		report(result, images);

		reportSuppressedLogLines();

		check(result.converged,
		      "every slot took a program and armed its four DMA channels within the "
		      "iteration bound of " + std::to_string(g_iterationBound) +
		      "; the run used " + std::to_string(result.iterations));

		check(!result.halted && !result.faulted,
		      "the MCU neither halted nor faulted while the kernel was downloaded");

		check(result.dspCount == g_expectedDspCount,
		      "the board attached " + std::to_string(g_expectedDspCount) + " DSPs; it attached " +
		      std::to_string(result.dspCount));

		// The map is asserted to be a permutation before it is used. A table that
		// named one position twice would silently make two ports share a row of
		// the expectation, and the second of them would be checked against the
		// first one's constants.
		check(result.positionsNamed == result.dspCount,
		      "the firmware's table at " + hex6(g_portTableBase) + " names a chain position for "
		      "every one of the " + std::to_string(result.dspCount) + " ports; it named " +
		      std::to_string(result.positionsNamed));

		{
			std::vector<bool> seen(result.dspCount, false);
			bool permutation = result.positionsNamed == result.dspCount;

			for(unsigned port = 0; port < result.dspCount && permutation; ++port)
			{
				const unsigned position = result.positionOfPort[port];

				if(position >= result.dspCount || seen[position])
					permutation = false;
				else
					seen[position] = true;
			}

			check(permutation,
			      "the port-to-position map read out of the booted machine is a permutation, so "
			      "no two ports are checked against the same row of the port-to-position table");
		}

		// Clause 1, the kernel download.
		for(unsigned port = 0; port < result.dspCount; ++port)
		{
			check(result.landed[port],
			      "port " + std::to_string(port) + " completed its bootstrap download");

			check(result.imageOfPort[port] >= 0,
			      "port " + std::to_string(port) + "'s program memory holds one of the firmware's "
			      "own containers word for word, which is what receiving a whole download means; a "
			      "download that stopped short leaves the remainder unwritten and matches nothing");

			// The word count is the machine's answer held against the document's
			// claim. Which container this slot holds was decided by comparing
			// program memory; how many words that container carries was read out
			// of its own header; and "all eight DSPs receive 573 words" is the
			// right-hand side. Nothing here selected a container by its length.
			const size_t words = result.imageOfPort[port] >= 0
				? images[size_t(result.imageOfPort[port])].words.size()
				: 0u;

			check(words == g_designWordCount,
			      "port " + std::to_string(port) + " received " + std::to_string(words) +
			      " words, and all eight DSPs receive " +
			      std::to_string(g_designWordCount));
		}

		// Clause 2, the DMA constants.
		for(unsigned port = 0; port < result.dspCount; ++port)
		{
			const unsigned position = result.positionOfPort[port];

			// The MOVEP block of the image this port actually holds, so the
			// instruction and the register belong to the same DSP.
			const std::map<uint32_t, dsp56k::TWord> movep =
				result.imageOfPort[port] >= 0
					? decodeMovepImmediates(images[size_t(result.imageOfPort[port])])
					: std::map<uint32_t, dsp56k::TWord>{};

			for(const auto& e : expectedChannels(position, result.dspCount))
			{
				const LoadResult::Registers& m = result.armed[port][e.channel];
				const dsp56k::TWord requestSource = (m.dcr >> g_drsShift) & g_drsMask;

				const std::string where =
					"chain position " + std::to_string(position) + " (hardware port " +
					std::to_string(port) + ") channel " + std::to_string(e.channel) + " ";

				check(m.latched, where + "was observed armed, so the registers below carry the "
				                         "kernel's writes and are not an unwritten zero");

				check(m.ddr == e.ddr, where + "DDR is " + hex6(e.ddr) + "; it is " + hex6(m.ddr));
				check(m.dco == e.dco, where + "DCO is " + hex6(e.dco) + "; it is " + hex6(m.dco));

				check(requestSource == e.requestSource,
				      where + "DCR request source is the HARDWARE field " +
				      std::to_string(e.requestSource) + " and not the library enumerator; it is " +
				      std::to_string(requestSource));

				/* The source register is two different kinds of thing and only
				 * one of them survives to be read back, so this row is routed by
				 * what the expectation is and never by which reading passes.
				 *
				 * On channels 2 and 3 the source is a peripheral address -- ESAI
				 * RX0 and ESAI_1 RX0 -- which nothing moves, so the DSP's own
				 * register is read and it is the stronger evidence: it shows the
				 * emulator took the write and kept it.
				 *
				 * On channels 4 and 5 the source is a data-memory pointer into
				 * the transmit buffers, and the kernel banks it. Measured on all
				 * eight ports, at the first frame boundary at which the register
				 * is non-zero and again at the end of an 18,058-iteration run:
				 * DSR4 reads $001E00 and DSR5 reads $001E10, which is the
				 * documented pair plus exactly $100, with the tail's channel 4
				 * among them although its DCR carries DE clear and it has
				 * therefore transferred nothing. A value that appears without a
				 * transfer is a write, so the kernel moves the pointer to the
				 * second bank inside the frame in which it programs it, and no
				 * frame-boundary read of this emulator can catch the first bank.
				 *
				 * So the row is checked against the kernel's own instruction and
				 * it says so in its own failure message. That is weaker than the
				 * other rows -- it proves the DSP was sent the write and not that
				 * the emulated DMA acted on it -- and the clause under it is what
				 * keeps the weakening from being vacuous: the live register must
				 * still hold a written, data-space pointer. */
				if(e.dsr >= g_peripheralSpaceFirst)
				{
					check(m.dsr == e.dsr, where + "DSR is the peripheral address " + hex6(e.dsr) +
					      "; the DSP's own register holds " + hex6(m.dsr));
					continue;
				}

				const uint32_t address = channelRegisters(e.channel).dsr;
				const auto     it      = movep.find(address);

				check(it != movep.end(),
				      where + "kernel image carries a MOVEP immediate to DSR at X:" + hex6(address));

				check(it != movep.end() && it->second == e.dsr,
				      where + "DSR is written as " + hex6(e.dsr) + " by the kernel's own MOVEP at X:" +
				      hex6(address) + "; it writes " +
				      (it == movep.end() ? std::string("nothing") : hex6(it->second)) +
				      " -- THIS ROW IS CHECKED AGAINST THE INSTRUCTION AND NOT AGAINST THE DSP'S "
				      "REGISTER, because the kernel banks this transmit pointer inside the frame it "
				      "programs it in and a frame-boundary read cannot see the first bank");

				check(m.dsr != 0u && m.dsr < g_peripheralSpaceFirst,
				      where + "the DSP's own DSR holds a written data-space pointer, which is what "
				      "keeps the instruction-side check above from passing over a channel whose "
				      "source register the emulator never took; it holds " + hex6(m.dsr));
			}
		}

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
