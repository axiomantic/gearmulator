// zz_audio_out.cpp -- DIAGNOSTIC INSTRUMENT, not a registered test.
//
// The question: with a patch delivered through the composing loader and the
// machine run long enough to be genuinely booted, does audio reach the output?
//
// It is zz_patch_store's machine placement (SRAM mapped through the stretched
// CS4 window, one InternalClient sized for the largest frame either arm
// originates) joined to t1_patch_running's codec walk.
//
// Arms, selected by G2_AUDIO_MODE:
//   none      no delivery at all                        (the negative control)
//   objects   g2::pch2Load, one frame per object
//   framed    g2::pch2LoadFramed, the composed message  (default)
//
// Controls for the audio instrument itself:
//   the tail-ESAI sentinel injection, run AFTER the walk on the same machine,
//   which drives a value the walk's own pull and comparator must report. A zero
//   walk beside a non-zero sentinel is a measurement; a zero walk beside a zero
//   sentinel is a probe that cannot see audio.
//
// Three probes were added to answer "where does a voice DSP's audio go":
//   (always on) a dump of all six DMA channels of all eight DSPs, before the
//               walk and after it, with the DCR decoded. Every prior reading of
//               the transmit path went through getDSR(4) alone.
//   G2_AUDIO_YWIN     extends the window fill, the read-only sampler and the
//                     per-quantum watch to the same range in Y space.
//   G2_AUDIO_SCRATCH  samples, read-only and per quantum, the pages BELOW the
//                     transmit window in both X and Y, counting both non-zero
//                     words and words that CHANGED since the previous quantum.
//                     The change count is the one that matters: a patch's
//                     uploaded coefficients are non-zero and sit still.
//
// Nothing here asserts. Every verdict is an observable.
#include "gatedFixture.h"

#include "../board.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"
#include "../transportHub.h"
#include "../crc16.h"
#include "../uart0.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "dsp56kEmu/debuggerinterface.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dma.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kBase/logging.h"
#include "baseLib/logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
	std::atomic<uint64_t> g_logLines{0};

	void countLog(const std::string&)
	{
		++g_logLines;
	}

	constexpr uint32_t g_entryPc = 0x30000400u;

	// One DSP's observable state at a moment. The note phase and the patch
	// upload are then measured by the SAME instrument in the SAME run, which is
	// the whole point: a note phase measured with a different probe than the one
	// that sees the upload compares two things that were never comparable.
	struct DspSnap
	{
		uint64_t instr = 0;
		uint64_t nz[3] = {0, 0, 0};
		uint64_t hash[3] = {0, 0, 0};
	};
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase  = 0x10000000u;
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

	// t1_egress's two impulse words, kept so an optional pass-through arm can
	// use exactly the pattern that file's arrival instrument uses.
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	// The tail-ESAI sentinel, from t1_patch_running: bit 23 clear so the frame
	// conversion's sign extension is the identity on it, and neither impulse
	// word so it cannot be mistaken for the measurement it qualifies.
	constexpr uint32_t g_sinkControlWord     = 0x2B6D51u;
	constexpr int32_t  g_sinkControlExpected = int32_t(g_sinkControlWord);

	static_assert((g_sinkControlWord & 0x800000u) == 0u, "the sentinel's sign bit must be clear");
	static_assert(g_sinkControlExpected != g_impulseLeft && g_sinkControlExpected != g_impulseRight,
		"the sentinel must not be either impulse word");

	constexpr uint32_t g_esaiTransmitters  = 6u;
	constexpr unsigned g_sinkControlQuanta = 256u;
	constexpr dsp56k::TWord g_dmaTxChannel = 4u;

	std::string hex32(const uint32_t _v)
	{
		static const char* const d = "0123456789ABCDEF";
		std::string r = "0x";
		for(int s = 28; s >= 0; s -= 4)
			r += d[(_v >> s) & 0xfu];
		return r;
	}

	/* ------------------------------------------------- the write collector
	 *
	 * Every probe this instrument has carried is a SAMPLE: it reads memory once
	 * per audio frame and infers what happened in between. That cannot tell a
	 * store that fires with a constant operand from a store that never fires at
	 * all, and the corpus names that gap explicitly.
	 *
	 * This is not a sample. dsp56k routes every DSP memory write through
	 * Memory::dspWrite when the JIT is configured with memoryWritesCallCpp, and
	 * dspWrite calls DebuggerInterface::onMemoryWrite when the emulator is built
	 * with DSP56300_DEBUGGER=1. Both are existing, unmodified mechanisms; this
	 * instrument only turns them on. The result is an EXACT per-address write
	 * event count for X and Y below g_traceHi, per DSP, over the walk.
	 *
	 * Armed only for the duration of the walk, because the instrument's own
	 * poison fill and the patch upload also go through Memory::set and would
	 * otherwise be counted as machine writes.
	 */
	/* The upper bound of the traced range, settable because the control arm's
	 * own trace showed 64,848 X writes per voice DSP ABOVE $2000 -- a region no
	 * probe in this corpus has ever sampled per quantum. A bound chosen to match
	 * the existing samplers would have reported those writes as "out of range"
	 * and nothing would have said so. G2_AUDIO_TRACEHI raises it. */
	dsp56k::TWord g_traceHi = 0x2000u;

	struct WriteCell
	{
		uint64_t            writes  = 0;
		uint64_t            changes = 0;   // writes whose value differed from the last one written
		bool                seen    = false;
		dsp56k::TWord       first   = 0;
		dsp56k::TWord       last    = 0;
		dsp56k::TWord       minVal  = 0xffffffu;
		dsp56k::TWord       maxVal  = 0;
	};

	class WriteCollector final : public dsp56k::DebuggerInterface
	{
	public:
		explicit WriteCollector(dsp56k::DSP& _dsp) : DebuggerInterface(_dsp)
		{
			m_cells[0].resize(g_traceHi);
			m_cells[1].resize(g_traceHi);
		}

		void arm(const bool _on) { m_armed = _on; }

		void onMemoryWrite(const dsp56k::EMemArea _area, const dsp56k::TWord _addr, const dsp56k::TWord _value) override
		{
			if(!m_armed)
				return;
			const unsigned a = _area == dsp56k::MemArea_X ? 0u : (_area == dsp56k::MemArea_Y ? 1u : 2u);
			if(a > 1u)
			{
				++m_writesP;
				return;
			}
			++m_writesTotal[a];
			if(_addr >= g_traceHi)
			{
				++m_writesAbove[a];
				return;
			}
			WriteCell& c = m_cells[a][_addr];
			if(!c.seen)
			{
				c.seen  = true;
				c.first = _value;
				c.minVal = _value;
				c.maxVal = _value;
			}
			else if(_value != c.last)
			{
				++c.changes;
				if(_value < c.minVal) c.minVal = _value;
				if(_value > c.maxVal) c.maxVal = _value;
			}
			c.last = _value;
			++c.writes;
		}

		const std::vector<WriteCell>& cells(const unsigned _area) const { return m_cells[_area]; }
		uint64_t writesTotal(const unsigned _area) const { return m_writesTotal[_area]; }
		uint64_t writesAbove(const unsigned _area) const { return m_writesAbove[_area]; }
		uint64_t writesP() const { return m_writesP; }

	private:
		bool                   m_armed = false;
		std::vector<WriteCell> m_cells[2];
		uint64_t               m_writesTotal[2] = {0, 0};
		uint64_t               m_writesAbove[2] = {0, 0};
		uint64_t               m_writesP = 0;
	};

	/* ------------------------------------------------------------------ DMA
	 *
	 * Every reading this instrument has ever taken of the transmit path went
	 * through getDSR(4). Channel 4 is one of six, and the firmware arms at
	 * least two of them from the same descriptor ring: DSR4 from x:$46 and
	 * DSR5 from y:$46. Channel 5 has never been read at runtime, so "no DSP
	 * writes $1e10" was a statement about a register nobody had looked at.
	 *
	 * This dumps all six channels of all eight DSPs. It reads only, so it may
	 * sit in any arm; and it decodes the DCR rather than printing it raw,
	 * because the source space is the field that decides whether the second
	 * transmit bus sources from X or from Y, and a raw hex DCR invites the
	 * reader to decode it by hand and get it wrong.
	 *
	 * The bit positions are DmaChannel::DcrBits, restated here rather than
	 * reached for, because DcrBits is an enum in a submodule this run pins and
	 * does not modify. */
	const char* memAreaName(const dsp56k::EMemArea _a)
	{
		switch(_a)
		{
		case dsp56k::MemArea_X: return "X";
		case dsp56k::MemArea_Y: return "Y";
		case dsp56k::MemArea_P: return "P";
		default:                return "?";
		}
	}

	// The DCR request-source field, translated the way the 56311 translates it
	// -- hardware 21 is ESAI_1 receive and hardware 22 is ESAI_1 transmit. The
	// numbers below 21 need no translation.
	std::string dmaRequestSourceName(const uint32_t _raw)
	{
		switch(_raw)
		{
		case 0x00: return "IRQA";
		case 0x01: return "IRQB";
		case 0x02: return "IRQC";
		case 0x03: return "IRQD";
		case 0x04: return "DMA0done";
		case 0x05: return "DMA1done";
		case 0x06: return "DMA2done";
		case 0x07: return "DMA3done";
		case 0x08: return "DMA4done";
		case 0x09: return "DMA5done";
		case 0x0b: return "EsaiRX";
		case 0x0c: return "EsaiTX";
		case 0x10: return "HostRX";
		case 0x11: return "HostTX";
		case 0x12: return "Timer0";
		case 0x13: return "Timer1";
		case 0x14: return "Timer2";
		case 0x15: return "Esai1RX";
		case 0x16: return "Esai1TX";
		default:   return "raw" + std::to_string(_raw);
		}
	}

	void reportDma(const char* const _label, dsp56k::Peripherals56311& _p, const unsigned _dsp)
	{
		dsp56k::Dma& dma = _p.getDMA();

		for(dsp56k::TWord c = 0; c < 6u; ++c)
		{
			const dsp56k::TWord dcr = dma.getDCR(c);

			const uint32_t srcSpace = dcr & 3u;
			const uint32_t dstSpace = (dcr >> 2) & 3u;
			const uint32_t addrMode = (dcr >> 4) & 0x3fu;
			const uint32_t reqSrc   = (dcr >> 11) & 0x1fu;
			const uint32_t xferMode = (dcr >> 19) & 7u;
			const bool     enabled  = (dcr & (1u << 23)) != 0;
			const bool     cont     = (dcr & (1u << 16)) != 0;

			std::cout << "DMA " << _label << " dsp " << _dsp << " ch " << c
			          << " DCR=" << hex32(dcr)
			          << " en=" << (enabled ? 1 : 0)
			          << " src=" << memAreaName(dsp56k::DmaChannel::convertMemArea(srcSpace))
			          << ':' << hex32(dma.getDSR(c))
			          << " dst=" << memAreaName(dsp56k::DmaChannel::convertMemArea(dstSpace))
			          << ':' << hex32(dma.getDDR(c))
			          << " DCO=" << dma.getDCO(c)
			          << " req=" << dmaRequestSourceName(reqSrc)
			          << " mode=" << xferMode
			          << " am=" << addrMode
			          << " cont=" << (cont ? 1 : 0)
			          << std::endl;
		}
	}

	/* An exact MCU program-counter coverage recorder for one phase of a run.
	 *
	 * The DSP-side snapshots answer whether a note reached a DSP. They cannot
	 * answer where in the MC68k firmware it stopped, because nothing here saw
	 * the MCU between two instructions. `g2::McuRunner` is that point -- the
	 * scheduler asks the installed runner for the quantum's MCU cycles, and a
	 * runner that spends them one instruction at a time can read the PC after
	 * each one. It is a coverage SET, not a trace: only which addresses were
	 * reached, and how often.
	 *
	 * Read as a DIFFERENCE against a quanta-matched arm that posted no MIDI.
	 * An address the note arm reaches and the idle arm does not is firmware the
	 * note caused to run, and the deepest such address is where the note got
	 * to. A single arm's coverage says nothing: the firmware's idle loop
	 * reaches thousands of addresses every phase. */
	class McuPcCoverage final : public g2::McuRunner
	{
	public:
		McuPcCoverage(g2::Board& _board, const uint32_t _base, const uint32_t _size)
			: m_board(_board), m_base(_base), m_counts(_size / 2u, 0u) {}

		uint32_t runMcu(const uint32_t _want) noexcept override
		{
			uint32_t spent = 0;

			while(spent < _want)
			{
				const uint32_t n = m_board.runMcu(1);
				if(n == 0)
					break;
				spent += n;
				++m_instructions;

				const uint32_t pc = m_board.mcuReg(17);
				if(pc >= m_base && pc < m_base + uint32_t(m_counts.size()) * 2u)
				{
					uint32_t& c = m_counts[(pc - m_base) >> 1];
					if(c != 0xffffffffu)
						++c;
				}
				else
				{
					++m_outside;
				}
			}

			return spent;
		}

		uint64_t instructions() const { return m_instructions; }
		uint64_t outside() const { return m_outside; }

		uint64_t distinct() const
		{
			uint64_t n = 0;
			for(const uint32_t c : m_counts)
				if(c != 0)
					++n;
			return n;
		}

		bool write(const std::string& _path) const
		{
			std::ofstream f(_path, std::ios::binary);
			if(!f)
				return false;
			for(size_t i = 0; i < m_counts.size(); ++i)
				if(m_counts[i] != 0)
					f << hex32(m_base + uint32_t(i) * 2u) << " " << m_counts[i] << "\n";
			return true;
		}

	private:
		g2::Board& m_board;
		uint32_t m_base;
		std::vector<uint32_t> m_counts;
		uint64_t m_instructions = 0;
		uint64_t m_outside = 0;
	};

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status) override
		{
			_status = MCF5407_BUS_OK;
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
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

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5407_bus_status& _status) override
		{
			_status = MCF5407_BUS_OK;
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
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
			m_watchBase = _offset;
			m_watchLength = _length;
		}

		uint64_t contentWrites() const { return m_contentWrites; }
		const std::vector<uint8_t>& bytes() const { return m_bytes; }

	private:
		std::vector<uint8_t> m_bytes;
		uint32_t m_watchBase = 0;
		uint32_t m_watchLength = 0;
		uint64_t m_contentWrites = 0;
	};

	/* zz_patch_store's stretched CS4 window: base 0x14000000, size 0x1C000000,
	 * so absolute 0x20000800 -- where SRAM_20000800.bin belongs -- is inside it
	 * and SDRAM at 0x30000000 is not. Storage exists only for the SRAM bank;
	 * every offset below it reads zero, which is what the corpus records the
	 * panel/latch hole must do or the boot hangs in the vector handler. */
	class SparseCs4 final : public g2::BusTarget
	{
	public:
		static constexpr uint32_t kWindowBase = 0x14000000u;
		static constexpr uint32_t kWindowSize = 0x1C000000u;
		static constexpr uint32_t kBankBase   = 0x20000000u;
		static constexpr uint32_t kBankSize   = 0x00800000u;

		SparseCs4() : m_bank(kBankSize, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5407_bus_status& _status) override
		{
			_status = MCF5407_BUS_OK;
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
				return 0u;
			}
			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t value = 0u;
			for(uint32_t i = 0; i < count; ++i)
			{
				value <<= 8;
				const uint32_t abs = kWindowBase + _offset + i;
				if(abs >= kBankBase && abs < kBankBase + kBankSize)
					value |= m_bank[abs - kBankBase];
			}
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5407_bus_status& _status) override
		{
			_status = MCF5407_BUS_OK;
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5407_BUS_SIZE_ILLEGAL;
				return;
			}
			const uint32_t count = uint32_t(_size) / 8u;
			for(uint32_t i = 0; i < count; ++i)
			{
				const uint32_t abs = kWindowBase + _offset + i;
				const int shift = int(8u * (count - 1u - i));
				const uint8_t byte = uint8_t((_value >> shift) & 0xffu);
				if(abs >= kBankBase && abs < kBankBase + kBankSize)
				{
					m_bank[abs - kBankBase] = byte;
					++m_bankWrites;
				}
				else
				{
					++m_holeWrites;
				}
			}
		}

		bool place(const uint32_t _absolute, const std::vector<uint8_t>& _image)
		{
			if(_absolute < kBankBase || size_t(_absolute - kBankBase) + _image.size() > m_bank.size())
				return false;
			std::memcpy(m_bank.data() + (_absolute - kBankBase), _image.data(), _image.size());
			return true;
		}

		uint64_t bankWrites() const { return m_bankWrites; }
		uint64_t holeWrites() const { return m_holeWrites; }

	private:
		std::vector<uint8_t> m_bank;
		uint64_t m_bankWrites = 0;
		uint64_t m_holeWrites = 0;
	};

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	g2::BoardConfig makeConfig(const bool _stretchCs4)
	{
		g2::BoardConfig config;
		config.memory.cs0   = {g_cs0Base,       g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,   g_cs1Size};
		config.memory.cs2   = {g_cs2Base,       g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base,   g_cs3Size};
		config.memory.cs4   = _stretchCs4
			? g2::Window{SparseCs4::kWindowBase, SparseCs4::kWindowSize}
			: g2::Window{g_cs4Base, g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,   g_cs5Size};
		config.memory.mbar  = {g_mbarBase,      g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};
		return config;
	}

	/* t1_patch_running's derivation of the tail's hardware port. The chain
	 * adapter's position and the hardware port are not the same number; entry i
	 * of the firmware's table at 0x30116970 holds the CS1 address of the port at
	 * chain position i, and A3..A10 are eight active-low one-cold selects. */
	unsigned portOfChainPosition(g2::Board& _board, const unsigned _wanted, const unsigned _count)
	{
		constexpr uint32_t g_portTableBase = 0x30116970u;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5407_bus_status status = MCF5407_BUS_OK;
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

	// Every slot of every frame the walk pulled.
	struct AudioReading
	{
		uint64_t framesRequested = 0;
		uint64_t framesReturned  = 0;
		uint64_t nonZeroFrames   = 0;
		int      firstNonZero    = -1;
		int32_t  firstL          = 0;
		int32_t  firstR          = 0;
		int32_t  minSample       = 0;
		int32_t  maxSample       = 0;
		uint64_t nonZeroSlots[g2::Frame::kSlots] = {0,0,0,0,0,0,0,0};
		uint64_t distinctBuckets = 0;   // how many of 64 magnitude buckets were seen
	};

	void observe(AudioReading& _r, const g2::Frame& _f, const unsigned _q, uint64_t* _buckets)
	{
		bool any = false;

		for(unsigned s = 0; s < g2::Frame::kSlots; ++s)
		{
			const int32_t v = _f.slot[s];

			if(v != 0)
			{
				any = true;
				++_r.nonZeroSlots[s];
			}

			if(_r.framesReturned == 1 && s == 0)
			{
				_r.minSample = v;
				_r.maxSample = v;
			}
			else
			{
				_r.minSample = std::min(_r.minSample, v);
				_r.maxSample = std::max(_r.maxSample, v);
			}

			// A magnitude histogram over 64 buckets. Audio that is one repeated
			// value and audio that moves are different findings and a min/max
			// pair alone does not separate them.
			uint32_t mag = uint32_t(v < 0 ? -int64_t(v) : int64_t(v));
			unsigned bucket = 0;
			while(mag != 0 && bucket < 63)
			{
				mag >>= 1;
				++bucket;
			}
			_buckets[bucket] += 1;
		}

		if(!any)
			return;

		++_r.nonZeroFrames;

		if(_r.firstNonZero < 0)
		{
			_r.firstNonZero = int(_q);
			_r.firstL = _f.slot[0];
			_r.firstR = _f.slot[1];
		}
	}

	void reportAudio(const char* _label, const AudioReading& _r, const uint64_t* _buckets)
	{
		std::cout << _label << ": framesRequested=" << _r.framesRequested
		          << " framesReturned=" << _r.framesReturned
		          << " nonZeroFrames=" << _r.nonZeroFrames
		          << " firstNonZeroQuantum=" << _r.firstNonZero
		          << " range=[" << _r.minSample << ", " << _r.maxSample << "]"
		          << std::endl;

		std::cout << _label << ": firstNonZero slot0=" << _r.firstL
		          << " slot1=" << _r.firstR << std::endl;

		std::cout << _label << ": nonZero per slot =";
		for(unsigned s = 0; s < g2::Frame::kSlots; ++s)
			std::cout << " [" << s << "]=" << _r.nonZeroSlots[s];
		std::cout << std::endl;

		std::cout << _label << ": magnitude buckets (2^n, non-empty) =";
		for(unsigned b = 0; b < 64; ++b)
			if(_buckets[b] != 0)
				std::cout << " " << b << ":" << _buckets[b];
		std::cout << std::endl;
	}

	/* The MCU-to-DSP direction, counted exactly rather than sampled.
	 *
	 * `g2::Hdi08Adapter` is the single funnel for every CS1 cycle the MCU makes
	 * to any of the eight HDI08 host ports, so a delta taken across a phase is
	 * every access that phase made -- and a zero is the MCU having made none,
	 * not a sampler having missed one.
	 *
	 * The phases are printed separately because the interesting quantity is a
	 * DIFFERENCE. The upload phase drives thousands of words into every port and
	 * is this instrument's own known positive for the counter: if the boot and
	 * window rows are empty, nothing in the note row means anything.
	 *
	 * Register names are the DSP56300 host-side layout `mc68k::Hdi08` models:
	 * ICR, CVR, ISR, IVR at +0..+3 and TXH/TXM/TXL at +5..+7. TXL is what
	 * completes a 24-bit word, so the word count is the TXL column. */
	void reportHdi08(const char* _label,
		const g2::Hdi08Adapter::AccessCounts& _from,
		const g2::Hdi08Adapter::AccessCounts& _to)
	{
		static const char* const regName[8] =
			{"ICR", "CVR", "ISR", "IVR", "r4", "TXH", "TXM", "TXL"};

		for(int p = 0; p < g2::g_hdi08PortCount; ++p)
		{
			uint64_t writeTotal = 0, readTotal = 0;
			for(int r = 0; r < 8; ++r)
			{
				writeTotal += _to.writes[p][r] - _from.writes[p][r];
				readTotal  += _to.reads[p][r]  - _from.reads[p][r];
			}

			const uint64_t words = _to.words[p] - _from.words[p];
			const uint64_t cmds  = _to.hostCommands[p] - _from.hostCommands[p];

			std::cout << "HDI08 " << _label << " port " << p
			          << " writes=" << writeTotal
			          << " reads=" << readTotal
			          << " words=" << words
			          << " hostCommands=" << cmds
			          << " w[";
			for(int r = 0; r < 8; ++r)
				std::cout << (r ? "," : "") << regName[r] << ":"
				          << (_to.writes[p][r] - _from.writes[p][r]);
			std::cout << "] r[";
			for(int r = 0; r < 8; ++r)
				std::cout << (r ? "," : "") << regName[r] << ":"
				          << (_to.reads[p][r] - _from.reads[p][r]);
			std::cout << "] vec[";

			bool anyVector = false;
			for(unsigned i = 0; i < 128; ++i)
			{
				const uint64_t n = _to.vectorCounts[p][i] - _from.vectorCounts[p][i];
				if(n == 0)
					continue;
				std::cout << (anyVector ? "," : "") << "0x" << std::hex << (i * 2) << std::dec
				          << ":" << n;
				anyVector = true;
			}

			std::cout << "]" << std::endl;
		}
	}
}

int main()
{
	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why);
		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		const char* const modeEnv = std::getenv("G2_AUDIO_MODE");
		std::string mode = modeEnv ? modeEnv : "framed";

		const bool withSram = std::getenv("G2_AUDIO_NOSRAM") == nullptr;

		// The quanta the machine runs after the patch is handed over. The
		// findings record the event loop not running until past 180,000, so a
		// window that closes before that reports a machine that was never given
		// the chance to run the path.
		uint32_t windowQuanta = 400000u;
		if(const char* const q = std::getenv("G2_AUDIO_QUANTA"))
			windowQuanta = uint32_t(std::strtoul(q, nullptr, 10));

		uint32_t walkQuanta = 8192u;
		if(const char* const w = std::getenv("G2_AUDIO_WALK"))
			walkQuanta = uint32_t(std::strtoul(w, nullptr, 10));

		// Optional: a MIDI note-on delivered on the emulated UART receive path
		// before the walk. A synthesiser with no note held is legitimately
		// silent, so a run without this cannot separate "no audio path" from
		// "nothing was asked to sound".
		const bool sendNote = std::getenv("G2_AUDIO_NOTE") != nullptr;
		// The note arm's own control: run the note phase's quanta WITHOUT posting
		// any MIDI. Without it a zero note-phase delta is unreadable, because the
		// no-note arm runs no quanta at all -- so its zero says only that time
		// did not pass, not that the note did nothing.
		const bool noteIdle = std::getenv("G2_AUDIO_NOTEIDLE") != nullptr;
		uint32_t noteQuanta = 40000u;
		if(const char* const n = std::getenv("G2_AUDIO_NOTEQUANTA"))
			noteQuanta = uint32_t(std::strtoul(n, nullptr, 10));

		// Optional: push t1_egress's impulse into the codec source at walk
		// quantum 0. It measures the pass-through path, not the patch.
		const bool pushImpulse = std::getenv("G2_AUDIO_IMPULSE") != nullptr;

		double deadlineSeconds = 3600.0;
		if(const char* const d = std::getenv("G2_AUDIO_DEADLINE"))
			deadlineSeconds = std::strtod(d, nullptr);

		std::cout << "mode=" << mode << " sram=" << (withSram ? 1 : 0)
		          << " windowQuanta=" << windowQuanta
		          << " walkQuanta=" << walkQuanta
		          << " note=" << (sendNote ? 1 : 0)
		          << " impulse=" << (pushImpulse ? 1 : 0)
		          << " deadline=" << deadlineSeconds << "s" << std::endl;

		// Every switch this file reads, echoed whether it is set or not.
		//
		// A switch that is read but never reported is the shape that already
		// cost this project a run: an environment name is a string, a mistyped
		// one is simply absent, and the run then measures the default while
		// looking exactly like the run that was asked for. Reporting an unset
		// switch as "-" makes the intended name visible when the name that was
		// actually exported was something else.
		{
			static const char* const g_switches[] =
			{
				"G2_AUDIO_MODE", "G2_AUDIO_PATCH", "G2_AUDIO_NAME",
				"G2_AUDIO_QUANTA", "G2_AUDIO_WALK", "G2_AUDIO_DEADLINE",
				"G2_AUDIO_NOTE", "G2_AUDIO_NOTEIDLE", "G2_AUDIO_NOTEQUANTA",
				"G2_AUDIO_IMPULSE", "G2_AUDIO_NOSRAM", "G2_AUDIO_PCBUCKET",
				"G2_AUDIO_MIDIBYTES", "G2_AUDIO_MSGHEX", "G2_AUDIO_FORCEKBD",
				"G2_AUDIO_MCUTRACE", "G2_AUDIO_MCUTRACEWIN", "G2_AUDIO_HDI08CAP",
				"G2_AUDIO_PDUMP", "G2_AUDIO_POISON", "G2_AUDIO_POISONWATCH",
				"G2_AUDIO_WINSAMPLE", "G2_AUDIO_YWIN", "G2_AUDIO_SCRATCH",
				"G2_AUDIO_SCRATCHADDR", "G2_AUDIO_WRITETRACE", "G2_AUDIO_TRACEHI",
				"G2_LOG_ESAI_UNDERRUN"
			};

			// The resolved directory, not the variable that suggested it.
			// NMG2_ARTIFACTS is the single most consequential input this
			// harness takes -- it decides which firmware images are loaded --
			// and a run that does not name it cannot be told apart from a run
			// against a different or a stale corpus. This project has already
			// drawn a wrong conclusion from a stale checkout once.
			std::cout << "ARTIFACTS: resolved='" << directory << "'"
			          << " NMG2_ARTIFACTS='"
			          << (std::getenv("NMG2_ARTIFACTS") ? std::getenv("NMG2_ARTIFACTS") : "-")
			          << "'" << std::endl;

			std::cout << "SWITCHES:";
			for(const char* const name : g_switches)
			{
				const char* const value = std::getenv(name);
				// Quoted: a value may hold a space -- a patch name does --
				// and one token per switch is what makes the line greppable.
				std::cout << ' ' << name << "='" << (value ? value : "-") << '\'';
			}
			std::cout << std::endl;
		}

		const std::vector<uint8_t> code  = readFile(directory + "/CODE_30000400.bin");

		// The path and the entry name are separate on purpose. They used to be one
		// variable, and loading a patch from outside corpus/pch2 therefore sent its
		// whole relative path as the entry name. The firmware silently discards a
		// patch whose entry name exceeds 16 characters -- no error, no DSP work, a
		// result indistinguishable from a patch it considered and declined. Three
		// fixture patches were measured as inert for that reason and reported as a
		// finding before the cause was found.
		const char* loadResultName = nullptr;
		std::string patchPath = "BackTo72 demo";
		if(const char* const p = std::getenv("G2_AUDIO_PATCH"))
			patchPath = p;

		// Default the transmitted name to the file's own stem, so a path never
		// reaches the wire; G2_AUDIO_NAME overrides it for tests about the name.
		std::string patchName = patchPath;
		if(const auto slash = patchName.find_last_of('/'); slash != std::string::npos)
			patchName = patchName.substr(slash + 1);
		if(const char* const n = std::getenv("G2_AUDIO_NAME"))
			patchName = n;

		constexpr std::size_t g_maxEntryNameChars = 16;
		if(patchName.size() > g_maxEntryNameChars)
			std::cout << "WARNING entry name is " << patchName.size()
				<< " characters, over the " << g_maxEntryNameChars
				<< " this emulator's loader accepts; it will REFUSE to originate"
				<< std::endl;

		const std::vector<uint8_t> patch = readFile(directory + "/corpus/pch2/" + patchPath + ".pch2");

		if(code.empty() || (mode != "none" && patch.empty()))
		{
			std::cout << "FAIL artifacts missing under " << directory << std::endl;
			return false;
		}

		std::cout << "patch=\"" << patchPath << "\" name=\"" << patchName << "\" bytes=" << patch.size() << std::endl;

		Logging::setLogFunc(&countLog);
		baseLib::logging::setLogFunc(&countLog);

		g2::Board board(makeConfig(withSram));
		Ram ram(g_sdramSize);
		SparseCs4 cs4;

		if(withSram)
		{
			const std::vector<uint8_t> sram = readFile(directory + "/SRAM_20000800.bin");
			if(sram.empty() || !cs4.place(0x20000800u, sram))
			{
				std::cout << "FAIL SRAM_20000800.bin missing or does not fit the bank" << std::endl;
				return false;
			}
			std::cout << "sram image bytes = " << sram.size() << std::endl;
			board.memory().attach(g2::Region::Cs4, &cs4);
		}

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit SDRAM" << std::endl;
			return false;
		}

		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);
			for(uint32_t e = 0; e < g_vectorTableEntries; ++e)
				for(uint32_t b = 0; b < 4u; ++b)
					table[e * 4u + b] = uint8_t((g_vectorHandler >> ((3u - b) * 8u)) & 0xffu);
			if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit SDRAM" << std::endl;
				return false;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL VBR refused" << std::endl;
			return false;
		}

		g2::SerialExecutor executor;
		g2::Status schedulerStatus{};
		const g2::Scheduler::Config config;
		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		const unsigned dspCount = board.dspSet().dspCount();

		std::cout << "dspCount=" << dspCount
		          << " hopFrames=" << config.hopFrames
		          << " lookaheadFrames=" << config.lookaheadFrames << std::endl;

		const auto start = std::chrono::steady_clock::now();
		const auto seconds = [&]() {
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		};
		const auto expired = [&]() { return seconds() > deadlineSeconds; };

		// ------------------------------------------------------------- the boot
		bool booted = false;
		bool programsLanded = false;
		uint32_t bootQuanta = 0;
		uint32_t settle = 0;

		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			bootQuanta = i + 1;
			scheduler->runFrames(1);

			if(board.mcuHalted() || expired())
				break;
			if(ram.contentWrites() == 0)
				continue;
			if(++settle < g_bannerSettleQuanta)
				continue;

			booted = true;

			unsigned landed = 0;
			for(unsigned d = 0; d < dspCount; ++d)
			{
				const bool* const flag = board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}
			if(landed == dspCount)
			{
				programsLanded = true;
				break;
			}
		}

		std::cout << "boot: quanta=" << bootQuanta << " booted=" << (booted ? 1 : 0)
		          << " programsLanded=" << (programsLanded ? 1 : 0)
		          << " halted=" << (board.mcuHalted() ? 1 : 0)
		          << " chainAttached=" << (scheduler->chainAttached() ? 1 : 0)
		          << " pc=" << hex32(board.mcuReg(g_regPc))
		          << " t=" << seconds() << "s" << std::endl;

		/* The MCU-to-DSP host-port accounting, phase by phase. The counters live
		 * on the adapter and start at zero, so the boot row is a delta against a
		 * default-constructed set. */
		const g2::Hdi08Adapter::AccessCounts hdiZero{};
		const g2::Hdi08Adapter::AccessCounts hdiAfterBoot = board.hdi08().accessCounts();
		reportHdi08("boot", hdiZero, hdiAfterBoot);

		// -------------------------------------------------------- the delivery
		std::vector<uint8_t> delivered;

		// ONE client, sized for the largest thing any arm originates. Two
		// attached clients would put a second endpoint in a hub that holds
		// three, and a delivery that failed for that reason would look like
		// a delivery the firmware ignored.
		//
		/* THE CLIENT OUTLIVES THE DELIVERY BLOCK, and that is load-bearing.
		 * Board::pumpTransport takes at most ONE frame out of the hub per
		 * quantum and holds it until the firmware has drained every packet of
		 * it, so a second originated frame necessarily waits in the hub's
		 * queue for many quanta. Destroying the client at the end of the
		 * delivery block detached its endpoint and took that queued frame with
		 * it -- measured: with the client block-scoped, a second message
		 * reported sent=1 and the long window's MCU coverage was BYTE-IDENTICAL
		 * to the no-message arm, 46,092 addresses either way, with the message
		 * worker at 0x3004C10C present in both. The frame never reached the
		 * device at all, and every downstream zero was about that and not about
		 * the message. */
		std::vector<uint8_t> scratch(g2::g_maxPatchLoadMessageBytes + 4);
		std::optional<g2::InternalClient> clientStorage;
		clientStorage.emplace(board.transport(), scratch.size(), 4);
		g2::InternalClient& client = *clientStorage;

		{

			if(mode == "objects")
			{
				const g2::Pch2LoadResult r = g2::pch2Load(patch.data(), patch.size(), client);
				loadResultName = g2::pch2LoadResultName(r);
				std::cout << "pch2Load = " << loadResultName << std::endl;
				delivered = patch;
			}
			else if(mode == "framed")
			{
				const g2::Pch2LoadResult r = g2::pch2LoadFramed(patch.data(), patch.size(),
					patchName.c_str(), 0, client, scratch.data(), scratch.size());
				loadResultName = g2::pch2LoadResultName(r);
				std::cout << "pch2LoadFramed = " << loadResultName << std::endl;

				g2::Pch2LoadResult cr = g2::Pch2LoadResult::Loaded;
				std::vector<uint8_t> msg(g2::g_maxPatchLoadMessageBytes + 4);
				const std::size_t n = g2::pch2ComposePatchLoad(patch.data(), patch.size(),
					patchName.c_str(), 0, msg.data(), msg.size(), cr);
				if(cr == g2::Pch2LoadResult::Loaded)
					delivered.assign(msg.begin(), msg.begin() + n);
				std::cout << "composed message bytes = " << delivered.size() << std::endl;
			}
			else
			{
				std::cout << "no delivery (the negative control arm)" << std::endl;
			}

			/* G2_AUDIO_MSGHEX originates ONE further message whose BODY is
			 * given verbatim as hex. The instrument writes the frame around it
			 * -- the 2-byte big-endian total that includes the prefix, and the
			 * 2-byte big-endian CRC-16/CCITT-XMODEM over the body -- so the
			 * caller states only the bytes the firmware's message worker
			 * reads. Nothing else about the body is interpreted here: a wrong
			 * body must be REFUSED by the firmware, not corrected by the
			 * instrument. Several messages may be given, separated by '/', and
			 * they are sent in the order written. */
			if(const char* const h = std::getenv("G2_AUDIO_MSGHEX"))
			{
				std::vector<std::vector<uint8_t>> bodies(1);
				for(const char* p = h; *p != '\0'; )
				{
					if(*p == ' ' || *p == ',') { ++p; continue; }
					if(*p == '/') { bodies.emplace_back(); ++p; continue; }
					char pair[3] = {p[0], p[1], '\0'};
					char* end = nullptr;
					const unsigned long v = std::strtoul(pair, &end, 16);
					if(end != pair + 2)
					{
						std::cout << "MSGHEX-BAD-HEX at offset " << (p - h) << std::endl;
						bodies.clear();
						break;
					}
					bodies.back().push_back(uint8_t(v));
					p += 2;
				}

				for(const std::vector<uint8_t>& body : bodies)
				{
					if(body.empty())
						continue;

					const std::size_t total = body.size() + 4;
					std::vector<uint8_t> frame(total);
					frame[0] = uint8_t(total >> 8);
					frame[1] = uint8_t(total & 0xFFu);
					std::memcpy(frame.data() + 2, body.data(), body.size());
					g2::crc16Store(frame.data() + 2 + body.size(),
						g2::crc16(frame.data() + 2, body.size()));

					const bool ok = client.send(g2::ProtocolFrame{ frame.data(), frame.size() });

					std::cout << "MSGHEX bodyBytes=" << body.size()
					          << " frameBytes=" << frame.size()
					          << " sent=" << (ok ? 1 : 0) << " body=";
					for(const uint8_t b : body)
						std::cout << " " << std::hex << unsigned(b) << std::dec;
					std::cout << std::endl;
				}
			}

			board.pumpTransport();
			scheduler->runFrames(1);
		}

		// FNV-1a over the low 0x8000 words of each area, plus the instruction
		// counter. Only equality, inequality and the non-zero counts are read.
		const auto takeSnap = [&]() -> std::vector<DspSnap>
		{
			std::vector<DspSnap> out(dspCount);
			const dsp56k::EMemArea areas[3] = {dsp56k::MemArea_X, dsp56k::MemArea_Y, dsp56k::MemArea_P};

			for(unsigned d = 0; d < dspCount; ++d)
			{
				out[d].instr = board.dspSet().dsp(d).getInstructionCounter();
				dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

				for(unsigned a = 0; a < 3; ++a)
				{
					uint64_t hash = 1469598103934665603ull, nz = 0;
					for(dsp56k::TWord w = 0; w < 0x8000u; ++w)
					{
						const dsp56k::TWord v = memory.get(areas[a], w);
						if(v != 0)
							++nz;
						hash = (hash ^ uint64_t(v)) * 1099511628211ull;
					}
					out[d].nz[a]   = nz;
					out[d].hash[a] = hash;
				}
			}
			return out;
		};

		// A: patch handed to the transport, firmware has not yet uploaded it.
		const std::vector<DspSnap> snapA = takeSnap();

		// ------------------------------------------------------- the long window
		//
		/* G2_AUDIO_MCUTRACEWIN records MCU coverage across the LONG WINDOW, not
		 * the note phase. A message originated before the window and never
		 * executed by the firmware, and a message executed and refused, are the
		 * same silence at every other observation point this instrument has.
		 * The trace separates them: the message worker's own addresses either
		 * appear in the coverage set or they do not. */
		const char* const winTracePath = std::getenv("G2_AUDIO_MCUTRACEWIN");
		std::unique_ptr<McuPcCoverage> winTrace;
		if(winTracePath != nullptr)
		{
			winTrace = std::make_unique<McuPcCoverage>(board, g2::g_sdramBase, g_sdramSize);
			scheduler->setMcuRunner(winTrace.get());
		}

		uint32_t ran = 0;

		for(uint32_t i = 0; i < windowQuanta; ++i)
		{
			scheduler->runFrames(1);
			ran = i + 1;

			if((ran % 50000u) == 0)
			{
				std::cout << "  progress q=" << ran << " pc=" << hex32(board.mcuReg(g_regPc))
				          << " chainAttached=" << (scheduler->chainAttached() ? 1 : 0)
				          << " t=" << seconds() << "s" << std::endl;
				std::cout.flush();
			}

			if((i & 0x3ffu) == 0 && expired())
				break;
			if(board.mcuHalted())
				break;
		}

		std::cout << "window: ranQuanta=" << ran
		          << " totalQuanta=" << (uint64_t(bootQuanta) + 1u + ran)
		          << " pc=" << hex32(board.mcuReg(g_regPc))
		          << " halted=" << (board.mcuHalted() ? 1 : 0)
		          << " faulted=" << (scheduler->faulted() ? 1 : 0)
		          << " chainAttached=" << (scheduler->chainAttached() ? 1 : 0)
		          << " frameIndex=" << scheduler->frameIndex()
		          << " t=" << seconds() << "s" << std::endl;

		const g2::Hdi08Adapter::AccessCounts hdiAfterWindow = board.hdi08().accessCounts();
		reportHdi08("window", hdiAfterBoot, hdiAfterWindow);

		if(winTrace)
		{
			scheduler->setMcuRunner(nullptr);
			const bool ok = winTrace->write(winTracePath);
			std::cout << "MCUTRACEWIN path=" << winTracePath
			          << " written=" << (ok ? 1 : 0)
			          << " mcuInstructions=" << winTrace->instructions()
			          << " distinctAddresses=" << winTrace->distinct()
			          << " outsideSdram=" << winTrace->outside()
			          << std::endl;
		}

		// B: after the upload window. A-to-B is the KNOWN POSITIVE for this
		// instrument -- the firmware uploads the patch during this window, so P
		// memory must move here. If it does not, nothing below means anything.
		const std::vector<DspSnap> snapB = takeSnap();

		// Was the patch stored? Kept small: one needle from the middle of what
		// went on the wire, so a silent run can say whether the patch was there
		// at all.
		{
			const std::vector<uint8_t>& mem = ram.bytes();
			const void* found = nullptr;
			if(delivered.size() > 424)
				found = ::memmem(mem.data(), mem.size(), delivered.data() + 400, 24);
			std::cout << "patch chain in SDRAM: "
			          << (found ? hex32(g2::g_sdramBase + uint32_t(static_cast<const uint8_t*>(found) - mem.data()))
			                    : std::string(delivered.empty() ? "(nothing delivered)" : "absent"))
			          << std::endl;

			const void* const byName = ::memmem(mem.data(), mem.size(), patchName.data(), patchName.size());
			std::cout << "patch name in SDRAM: "
			          << (byName ? hex32(g2::g_sdramBase + uint32_t(static_cast<const uint8_t*>(byName) - mem.data()))
			                     : std::string("absent")) << std::endl;
		}

		if(withSram)
			std::cout << "cs4: bankWrites=" << cs4.bankWrites() << " holeWrites=" << cs4.holeWrites() << std::endl;

		// Quanta actually consumed by the note phase. The note arm runs MORE of
		// them than the idle arm does: every posted byte is followed by a bounded
		// wait for the firmware to drain it, and those waits run the machine. A
		// raw instruction-count difference between the two arms is therefore not
		// the note's cost -- it is the note's cost plus that extra time, and at
		// twelve bytes the extra time alone is the right size to explain it.
		uint64_t notePhaseQuanta = 0;

		/* The four per-slot keyboard-assignment records the firmware's note-on
		 * router walks, read out of SDRAM before the note phase.
		 *
		 * `FUN_30025758` at 0x30025758 loops slots 0..3 and, for a slot whose
		 * channel matches, calls the acceptor at 0x300513fe with
		 * `*(uint32*)(*(uint32*)(0x302a7c74 + slot*4) + 0x47c)`. That acceptor
		 * refuses the note unless the bytes at +0x101 and +0x102 of the record
		 * are both non-zero, and -- when +0x103 is non-zero -- unless the note
		 * lies in [+0x104, +0x105]. Reading those bytes turns a disassembly
		 * into a measurement of the state this instrument actually drives the
		 * machine into.
		 *
		 * G2_AUDIO_FORCEKBD writes 1 into +0x102 of every slot's record. It is
		 * a PERTURBING probe: it proves the gate is what stops the note, and it
		 * says nothing about whether the firmware would ever set that byte by
		 * itself. */
		{
			const auto rd32 = [&](const uint32_t _addr) -> uint32_t
			{
				const std::vector<uint8_t>& m = ram.bytes();
				const uint32_t o = _addr - g2::g_sdramBase;
				if(size_t(o) + 4 > m.size())
					return 0;
				return uint32_t(m[o]) << 24 | uint32_t(m[o+1]) << 16 | uint32_t(m[o+2]) << 8 | m[o+3];
			};
			const auto rd8 = [&](const uint32_t _addr) -> unsigned
			{
				const std::vector<uint8_t>& m = ram.bytes();
				const uint32_t o = _addr - g2::g_sdramBase;
				return size_t(o) < m.size() ? m[o] : 0u;
			};

			const bool forceKbd = std::getenv("G2_AUDIO_FORCEKBD") != nullptr;

			for(unsigned slot = 0; slot < 4; ++slot)
			{
				const uint32_t slotPtr = rd32(0x302a7c74u + slot * 4u);
				const uint32_t rec = slotPtr != 0 ? rd32(slotPtr + 0x47cu) : 0u;

				std::cout << "kbd slot " << slot
				          << " slotPtr=" << hex32(slotPtr)
				          << " record=" << hex32(rec);

				if(rec >= g2::g_sdramBase && rec < g2::g_sdramBase + g_sdramSize)
				{
					std::cout << " +101=" << rd8(rec + 0x101)
					          << " +102=" << rd8(rec + 0x102)
					          << " +103=" << rd8(rec + 0x103)
					          << " +104=" << rd8(rec + 0x104)
					          << " +105=" << rd8(rec + 0x105)
					          << " +106=" << rd8(rec + 0x106)
					          << " +107=" << rd8(rec + 0x107);

					if(forceKbd)
					{
						mcf5407_bus_status st = MCF5407_BUS_OK;
						ram.write(rec + 0x102u - g2::g_sdramBase, 8, 1u, st);
						std::cout << " FORCED+102=" << rd8(rec + 0x102);
					}
				}

				std::cout << std::endl;
			}

			std::cout << "kbd channelByte=" << rd8(0x30115cc8u) << std::endl;

			/* The performance-settings object the message layer parses INTO,
			 * read one level above the keyboard record. `FUN_3002473e` reads
			 * its per-slot parameter bytes at settings + 43 + slot*25, applies
			 * index 0 to slotPtr+0x489 and index 1 to the keyboard record's
			 * +0x102. Printing them separates "the message never parsed" from
			 * "it parsed and the apply did not run". */
			{
				const uint32_t settings = rd32(0x302a7c70u);
				std::cout << "perfSettings=" << hex32(settings);
				if(settings >= g2::g_sdramBase && settings < g2::g_sdramBase + g_sdramSize)
				{
					for(unsigned slot = 0; slot < 4; ++slot)
					{
						const uint32_t p = settings + 43u + slot * 25u;
						std::cout << " s" << slot << "[" << rd8(p) << "," << rd8(p + 1)
						          << "," << rd8(p + 2) << "]";
					}
				}
				std::cout << std::endl;
			}
		}

		/* G2_AUDIO_HDI08CAP arms a bounded per-port capture of the words and
		 * host commands the MCU issues DURING THE NOTE PHASE. The counts say
		 * how much moved; this says what. */
		std::size_t hdiCapLimit = 0;
		if(const char* const c = std::getenv("G2_AUDIO_HDI08CAP"))
			hdiCapLimit = std::size_t(std::strtoul(c, nullptr, 0));
		if(hdiCapLimit)
			board.hdi08().armWordCapture(hdiCapLimit);

		// The MCU coverage recorder, armed for the note phase ONLY. Arming it
		// for the boot and upload windows would multiply their cost by the
		// per-instruction dispatch and measure a phase no arm varies.
		const char* const mcuTracePath = std::getenv("G2_AUDIO_MCUTRACE");
		std::unique_ptr<McuPcCoverage> mcuTrace;
		if(mcuTracePath != nullptr)
		{
			mcuTrace = std::make_unique<McuPcCoverage>(board, g2::g_sdramBase, g_sdramSize);
			scheduler->setMcuRunner(mcuTrace.get());
		}

		// ------------------------------------------------------------- the note
		//
		// Uart0::receive DROPS the byte and returns void when the receiver is
		// not enabled, so a call that delivered nothing and a call that
		// delivered are the same statement at the call site. Every byte is
		// therefore posted one at a time and its fate read off USR, which is
		// public:
		//
		//   bit 0 RxRDY -- set while the receiver FIFO holds a character.
		//
		// Posted and RxRDY still clear   the byte was DROPPED (receiver
		//                                disabled, or the FIFO refused it).
		// Posted and RxRDY set           the byte is IN the FIFO.
		// RxRDY clears afterwards        the FIRMWARE read it out. That is
		//                                ingestion, and nothing else is.
		if(sendNote)
		{
			g2::Uart0& uart = board.uart0();

			std::cout << "midi: before any byte, usr=0x" << std::hex << unsigned(uart.usr())
			          << std::dec << " interruptAsserted=" << (uart.interruptAsserted() ? 1 : 0)
			          << " uivr=0x" << std::hex << unsigned(uart.uivr()) << std::dec << std::endl;

			// The message, and then a second one on a different channel: an
			// arm that reaches only channel 1 and an arm that reaches none are
			// different findings.
			std::vector<uint8_t> message = {
				0x90u, 0x3Cu, 0x64u,   // note on,  channel 1, C4, velocity 100
				0x91u, 0x40u, 0x64u,   // note on,  channel 2, E4
				0x92u, 0x43u, 0x64u,   // note on,  channel 3, G4
				0x93u, 0x3Cu, 0x64u    // note on,  channel 4, C4
			};

			/* G2_AUDIO_MIDIBYTES replaces the message with an arbitrary hex
			 * string. The default above cannot separate "the firmware ran code
			 * because a MIDI BYTE arrived" from "because a NOTE-ON arrived",
			 * and those are different findings: the first is a UART wake-up,
			 * the second is note handling. A byte stream that is not a note-on
			 * is the control that separates them. */
			if(const char* const b = std::getenv("G2_AUDIO_MIDIBYTES"))
			{
				std::vector<uint8_t> custom;
				for(const char* p = b; *p != '\0'; )
				{
					if(*p == ' ' || *p == ',') { ++p; continue; }
					char* end = nullptr;
					char pair[3] = {p[0], p[1], '\0'};
					const unsigned long v = std::strtoul(pair, &end, 16);
					if(end != pair + 2)
						break;
					custom.push_back(uint8_t(v));
					p += 2;
				}
				if(!custom.empty())
					message = custom;
			}

			std::cout << "midi: bytes =";
			for(const uint8_t b : message)
				std::cout << " " << std::hex << unsigned(b) << std::dec;
			std::cout << " (" << message.size() << ")" << std::endl;

			unsigned posted = 0, accepted = 0, consumed = 0, dropped = 0;

			for(const uint8_t byte : message)
			{
				++posted;
				uart.receive(byte);

				if((uart.usr() & 0x01u) == 0u)
				{
					++dropped;
					continue;
				}

				++accepted;

				// Give the firmware room to take it out. Bounded, so a byte
				// nothing ever reads ends the wait rather than the run.
				bool taken = false;
				for(uint32_t i = 0; i < 20000u && !taken; ++i)
				{
					scheduler->runFrames(1);
				++notePhaseQuanta;
					if((uart.usr() & 0x01u) == 0u)
						taken = true;
					if(board.mcuHalted())
						break;
				}

				if(taken)
					++consumed;
			}

			std::cout << "midi: posted=" << posted << " accepted=" << accepted
			          << " dropped=" << dropped << " consumedByFirmware=" << consumed
			          << " usrAfter=0x" << std::hex << unsigned(uart.usr()) << std::dec
			          << std::endl;

			// The MIDI instrument's own KNOWN NEGATIVE: with the receiver in
			// whatever state the firmware left it, a byte posted while the FIFO
			// is already full must not be accepted. If every post is accepted no
			// matter what, the acceptance reading means nothing.
			{
				unsigned acceptedInBurst = 0;
				for(unsigned i = 0; i < 8; ++i)
				{
					uart.receive(0xF8u);           // timing clock, a real-time byte
					if((uart.usr() & 0x02u) != 0u) // FFULL
						break;
					++acceptedInBurst;
				}
				std::cout << "midi known negative: burst posted 8 without running the machine, "
				          << "acceptedBeforeFull=" << acceptedInBurst
				          << " usr=0x" << std::hex << unsigned(uart.usr()) << std::dec << std::endl;
			}

			for(uint32_t i = 0; i < noteQuanta; ++i)
			{
				scheduler->runFrames(1);
				++notePhaseQuanta;
				if((i & 0x3ffu) == 0 && expired())
					break;
				if(board.mcuHalted())
					break;
			}

			std::cout << "note: ran " << noteQuanta << " further quanta, pc="
			          << hex32(board.mcuReg(g_regPc)) << " t=" << seconds() << "s" << std::endl;
		}
		else if(noteIdle)
		{
			for(uint32_t i = 0; i < noteQuanta; ++i)
			{
				scheduler->runFrames(1);
				++notePhaseQuanta;
				if((i & 0x3ffu) == 0 && expired())
					break;
				if(board.mcuHalted())
					break;
			}

			std::cout << "noteIdle: ran " << noteQuanta
			          << " quanta with NO midi posted, pc="
			          << hex32(board.mcuReg(g_regPc)) << " t=" << seconds() << "s" << std::endl;
		}

		if(mcuTrace)
		{
			scheduler->setMcuRunner(nullptr);
			const bool ok = mcuTrace->write(mcuTracePath);
			std::cout << "MCUTRACE path=" << mcuTracePath
			          << " written=" << (ok ? 1 : 0)
			          << " mcuInstructions=" << mcuTrace->instructions()
			          << " distinctAddresses=" << mcuTrace->distinct()
			          << " outsideSdram=" << mcuTrace->outside()
			          << " notePhaseQuanta=" << notePhaseQuanta
			          << std::endl;
		}

		const g2::Hdi08Adapter::AccessCounts hdiAfterNote = board.hdi08().accessCounts();
		reportHdi08("note", hdiAfterWindow, hdiAfterNote);

		/* The DSP-side receive ring, read at the end of the note phase.
		 *
		 * The host-port counts above are the MCU pushing words at a DSP. They do
		 * not say the DSP took them. `dsp56k::HDI08` holds pushed words in an
		 * 8,192-entry ring the DSP drains with its own RX reads, and the bridge
		 * defers what will not fit -- so a ring that is filling, or full, is a
		 * DSP that is not reading, and every parameter after that point is
		 * queued rather than applied. A near-empty ring is the DSP keeping up. */
		for(unsigned d = 0; d < dspCount; ++d)
		{
			const dsp56k::HDI08& h = board.dspSet().peripherals(d).getHDI08();
			std::cout << "HDI08RX dsp " << d
			          << " pending=" << h.rxData().size()
			          << " capacity=" << h.rxData().capacity()
			          << std::endl;
		}

		if(hdiCapLimit)
		{
			board.hdi08().disarmWordCapture();
			for(int p = 0; p < g2::g_hdi08PortCount; ++p)
			{
				const std::vector<g2::Hdi08Adapter::CapturedEntry>& e =
					board.hdi08().capturedEntries(p);
				std::cout << "HDI08CAP port " << p << " entries=" << e.size() << " :";
				for(const g2::Hdi08Adapter::CapturedEntry& c : e)
					std::cout << " " << (c.isCommand ? "c" : "w")
					          << std::hex << c.value << std::dec;
				std::cout << std::endl;
			}
		}

		// C: after the note phase. B-to-C is the question this run exists to
		// answer, and it is asked with the same instrument that produced the
		// A-to-B positive above.
		const std::vector<DspSnap> snapC = takeSnap();

		{
			const char* const names[3] = {"X", "Y", "P"};

			std::cout << "DSPSNAP mode=" << mode
			          << " note=" << (sendNote ? 1 : 0)
			          << " noteIdle=" << (noteIdle ? 1 : 0)
			          << " noteQuantaRequested=" << ((sendNote || noteIdle) ? noteQuanta : 0u)
			          << " notePhaseQuantaActual=" << notePhaseQuanta
			          << std::endl;

			for(unsigned d = 0; d < dspCount; ++d)
			{
				std::cout << "  dsp " << d
				          << " uploadInstr=" << (snapB[d].instr - snapA[d].instr)
				          << " noteInstr=" << (snapC[d].instr - snapB[d].instr)
				          << " uploadChanged=";

				for(unsigned a = 0; a < 3; ++a)
					std::cout << names[a] << (snapA[d].hash[a] != snapB[d].hash[a] ? "1" : "0");

				std::cout << " noteChanged=";
				for(unsigned a = 0; a < 3; ++a)
					std::cout << names[a] << (snapB[d].hash[a] != snapC[d].hash[a] ? "1" : "0");

				std::cout << " nzUpload=";
				for(unsigned a = 0; a < 3; ++a)
					std::cout << names[a] << (int64_t(snapB[d].nz[a]) - int64_t(snapA[d].nz[a])) << ",";

				std::cout << " nzNote=";
				for(unsigned a = 0; a < 3; ++a)
					std::cout << names[a] << (int64_t(snapC[d].nz[a]) - int64_t(snapB[d].nz[a])) << ",";

				/* The ABSOLUTE digest at C, not only the within-arm change.
				 * `noteChanged` compares B to C inside ONE run and so reports
				 * time passing as well as the note; the digest itself is
				 * comparable ACROSS runs, so an accept arm and a quanta-matched
				 * idle arm can be held against each other. Equal digests at C
				 * say the note left no trace in DSP memory; different ones say
				 * it did. */
				std::cout << "  dsp " << d << " digestC";
				for(unsigned a = 0; a < 3; ++a)
					std::cout << " " << names[a] << "=" << std::hex << snapC[d].hash[a] << std::dec
					          << "/nz" << snapC[d].nz[a];
				std::cout << " instrC=" << snapC[d].instr << std::endl;

				std::cout << std::endl;
			}
		}

		// The DMA arming, read before the walk opens. In the `none` arm this is
		// what resident firmware alone arms; in a loaded arm it is that plus
		// whatever the uploaded ISR has done to it by now. Both arms print the
		// same eight-by-six table, so a difference is a difference and not a
		// missing row.
		for(unsigned d = 0; d < dspCount; ++d)
			reportDma("prewalk", board.dspSet().peripherals(d), d);

		// ------------------------------------------------- the play transition
		scheduler->beginPlayPhase();

		{
			std::vector<g2::Frame> primed(config.lookaheadFrames);
			const size_t pulled = scheduler->pull(primed.data(), primed.size());
			std::cout << "play: primedPulled=" << pulled << " of " << config.lookaheadFrames << std::endl;
		}

		// The DSP instruction counters as the walk opens. A DSP that executes
		// nothing during the walk and a DSP that executes and emits zeros are
		// different findings, and the sink alone cannot separate them.
		std::vector<uint64_t> beforeWalk(dspCount, 0);
		for(unsigned d = 0; d < dspCount; ++d)
			beforeWalk[d] = board.dspSet().dsp(d).getInstructionCounter();

		// A program-counter histogram for every DSP, sampled once per walk
		// quantum. It is what answers whether the code the patch wrote into a
		// DSP is EXECUTED: an address range that holds new instructions and is
		// never visited is a different finding from one that runs and emits
		// zeros. Sampled and not traced -- one sample per quantum is a
		// visit-frequency estimate, not a coverage proof, and it is read only
		// as "this region was reached" against a control that reached another.
		// Bucket width for the PC histogram. 256 words is coarse enough to hide
		// whether a DSP runs code a patch just uploaded: a patch adds on the order
		// of 1,600 non-zero program words, which spans several buckets, so a DSP
		// executing in one bucket only is running almost none of it -- but at 256
		// words that inference rests on arithmetic rather than on the histogram.
		// G2_AUDIO_PCBUCKET makes it measurable directly.
		uint32_t pcBucketWords = 256u;
		if(const char* const b = std::getenv("G2_AUDIO_PCBUCKET"))
		{
			const unsigned long v = std::strtoul(b, nullptr, 0);
			if(v >= 1 && (v & (v - 1)) == 0 && v <= 0x10000)
				pcBucketWords = static_cast<uint32_t>(v);
			else
				std::cout << "WARNING G2_AUDIO_PCBUCKET=" << b
					<< " is not a power of two in [1,65536]; keeping 256" << std::endl;
		}
		const uint32_t pcBucketMask = ~(pcBucketWords - 1u);
		std::cout << "pcBucketWords=" << pcBucketWords << std::endl;

		std::vector<std::map<uint32_t, uint64_t>> pcHistogram(dspCount);

		// -------------------------------------------- the poisoned audio window
		//
		// Reading the transmit window as zero after the walk cannot tell "nobody
		// wrote here" from "somebody wrote zeros here", and those are different
		// findings: the second says the synthesis runs and computes silence, the
		// first says it never reaches the buffer at all.
		//
		// G2_AUDIO_POISON fills the whole window with a sentinel BEFORE the walk.
		// Any word still holding the sentinel afterwards was not written by
		// anyone -- DSP or DMA. A word that changed was written, whatever it was
		// written with.
		//
		// This deliberately perturbs the machine: the transmit DMA sources from
		// this window, so the poison is carried to the codec. That is a second
		// reading and not a defect -- poison arriving at the sink shows the
		// buffer-to-codec half of the path carrying data that the DSP did not
		// produce, which is the known positive for the stage the walk measures.
		// Because it perturbs, it is off by default and never runs in an arm
		// whose sink reading is being used for anything else.
		constexpr dsp56k::TWord g_poisonWord = 0x0ACE55u;
		constexpr dsp56k::TWord g_poisonLo   = 0x1C00u;
		constexpr dsp56k::TWord g_poisonHi   = 0x2000u;

		// G2_AUDIO_POISONWATCH restores the sentinel after every quantum instead
		// of only before the walk, so a write that a later write hides is still
		// counted. It implies the poison fill.
		const bool poisonWatch = std::getenv("G2_AUDIO_POISONWATCH") != nullptr;
		const bool poison      = poisonWatch || std::getenv("G2_AUDIO_POISON") != nullptr;

		const bool winSample = std::getenv("G2_AUDIO_WINSAMPLE") != nullptr;

		/* G2_AUDIO_YWIN extends every window probe -- the fill, the read-only
		 * sampler and the per-quantum watch -- to the SAME address range in Y
		 * space, counted separately.
		 *
		 * It exists because firmware arms `DSR5 <- y:$46` and the descriptor
		 * ring's y half holds $1c10/$1d10/$1e10/$1f10, and every probe this
		 * instrument has ever run reads X. The end-of-walk read does report a
		 * `nonZeroY` and it has always been 0 -- but the poison fills X only,
		 * so that zero has never had a known positive beside it and is not yet
		 * a measurement of anything. With YWIN on, the poison arm supplies one.
		 *
		 * Y is also the space the firmware's own clear routine does NOT touch:
		 * int_0000f4 stores through `x:(r2)+`. So a Y zero is less blind than
		 * an X zero, which cuts the other way and makes the missing positive
		 * the only thing standing between it and a real negative result. */
		const bool yWin = std::getenv("G2_AUDIO_YWIN") != nullptr;

		/* G2_AUDIO_SCRATCH samples, once per quantum and writing nothing, the
		 * LOW pages of X and Y -- the pages the voice payload's synthesis
		 * actually addresses, `r3 = r4 = #$50`.
		 *
		 * Everything this instrument has ever counted lives at $1c00..$1fff,
		 * which is where the transmit DMA sources from. If a voice DSP writes
		 * no computed word THERE and also writes none in its own scratch, the
		 * machine is computing silence at the source and no routing question
		 * can matter. If it writes non-zero scratch and no non-zero window, the
		 * routing question is the whole of it. The two readings separate those,
		 * and nothing in the corpus separates them today.
		 *
		 * The range is deliberately wider than $50: the payload's own base is
		 * $50, but its stride is unknown and a range that stops short would
		 * report an absence that is really a bound. */
		const bool scratchSample = std::getenv("G2_AUDIO_SCRATCH") != nullptr;
		constexpr dsp56k::TWord g_scratchLo = 0x0000u;
		constexpr dsp56k::TWord g_scratchHi = 0x1C00u;

		std::vector<uint64_t> scratchQuantaNonZeroX(dspCount, 0);
		std::vector<uint64_t> scratchQuantaNonZeroY(dspCount, 0);
		std::vector<unsigned> scratchMaxX(dspCount, 0);
		std::vector<unsigned> scratchMaxY(dspCount, 0);
		std::vector<dsp56k::TWord> scratchFirstAddrY(dspCount, 0);
		std::vector<dsp56k::TWord> scratchFirstValY(dspCount, 0);

		/* A non-zero WORD COUNT is not evidence of computation. A patch's
		 * uploaded coefficients are non-zero and sit still, and they would
		 * report the same count in all 8,192 quanta as a running oscillator
		 * would. What separates them is CHANGE between one quantum and the
		 * next, so the sampler keeps the previous quantum's contents and counts
		 * words that differ.
		 *
		 * quantaChangedY counts quanta in which at least one Y word moved;
		 * maxChangedY is the largest number that moved in any one quantum. Its
		 * own known negative is the control arm, where the payload is absent. */
		std::vector<std::vector<dsp56k::TWord>> scratchPrevX(dspCount);
		std::vector<std::vector<dsp56k::TWord>> scratchPrevY(dspCount);
		std::vector<uint64_t> scratchQuantaChangedX(dspCount, 0);
		std::vector<uint64_t> scratchQuantaChangedY(dspCount, 0);
		std::vector<unsigned> scratchMaxChangedX(dspCount, 0);
		std::vector<unsigned> scratchMaxChangedY(dspCount, 0);

		/* G2_AUDIO_SCRATCHADDR records WHICH words move, not how many. It is the
		 * same loop, the same range and the same comparison that produce
		 * maxChangedX/maxChangedY above, so the addresses it names are exactly
		 * the words those counters count -- there is no second instrument to
		 * reconcile. Per address: the number of quanta in which it differed from
		 * the previous quantum, and the last value seen.
		 *
		 * The question it answers: are the 4-7 words that move with a patch the
		 * SAME addresses that move with no patch at all. If they are, the
		 * payload's 34.8 million instructions leave no trace in memory. */
		const bool scratchAddr = std::getenv("G2_AUDIO_SCRATCHADDR") != nullptr;
		std::vector<std::vector<uint32_t>> scratchChangeCountX(dspCount);
		std::vector<std::vector<uint32_t>> scratchChangeCountY(dspCount);

		std::vector<uint64_t> winSampleQuantaNonZeroY(dspCount, 0);
		std::vector<unsigned> winSampleMaxWordsY(dspCount, 0);
		std::vector<int>      winSampleFirstQuantumY(dspCount, -1);
		std::vector<uint64_t> watchZeroWritesY(dspCount, 0);
		std::vector<uint64_t> watchOtherWritesY(dspCount, 0);
		std::vector<int>      watchFirstOtherQuantumY(dspCount, -1);
		std::vector<dsp56k::TWord> watchFirstOtherAddrY(dspCount, 0);
		std::vector<dsp56k::TWord> watchFirstOtherValY(dspCount, 0);

		std::vector<uint64_t> winSampleQuantaNonZero(dspCount, 0);
		std::vector<uint64_t> winSampleWordTotal(dspCount, 0);
		std::vector<unsigned> winSampleMaxWords(dspCount, 0);
		std::vector<int>      winSampleFirstQuantum(dspCount, -1);

		std::vector<uint64_t>          watchZeroWrites(dspCount, 0);
		std::vector<uint64_t>          watchOtherWrites(dspCount, 0);
		std::vector<int>               watchFirstOtherQuantum(dspCount, -1);
		std::vector<dsp56k::TWord>     watchFirstOtherAddr(dspCount, 0);
		std::vector<dsp56k::TWord>     watchFirstOtherVal(dspCount, 0);
		std::vector<std::set<uint32_t>> watchTouched(dspCount);

		if(poison)
		{
			for(unsigned d = 0; d < dspCount; ++d)
			{
				dsp56k::Memory& memory = board.dspSet().dsp(d).memory();
				for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
				{
					memory.set(dsp56k::MemArea_X, w, g_poisonWord);
					if(yWin)
						memory.set(dsp56k::MemArea_Y, w, g_poisonWord);
				}
			}
			std::cout << "poison: filled X:" << hex32(g_poisonLo) << ".." << hex32(g_poisonHi)
			          << (yWin ? " and Y: the same range" : "")
			          << " with " << hex32(g_poisonWord) << " on " << dspCount << " dsps"
			          << " watch=" << (poisonWatch ? 1 : 0) << std::endl;
		}

		/* G2_AUDIO_WRITETRACE turns on the exact per-address write counter. Two
		 * existing dsp56k mechanisms carry it and neither is modified here:
		 * JitConfig::memoryWritesCallCpp routes every JIT-generated DSP memory
		 * write through Memory::dspWrite instead of inlining it, and
		 * DSP56300_DEBUGGER=1 makes dspWrite call the attached debugger's
		 * onMemoryWrite. The build must be configured -DDSP56300_DEBUGGER=ON or
		 * the hook is compiled out and this arm silently measures nothing --
		 * which is why the report prints the total write count first: a zero
		 * total is the instrument saying it is blind, not the machine saying it
		 * is quiet.
		 *
		 * Blocks compiled before the config change do not carry the callback, so
		 * every block is destroyed after the change and recompiled on next use.
		 *
		 * This costs speed and nothing else: the writes performed are identical,
		 * only the code path that performs them differs. */
		const bool writeTrace = std::getenv("G2_AUDIO_WRITETRACE") != nullptr;
		if(const char* const th = std::getenv("G2_AUDIO_TRACEHI"))
			g_traceHi = dsp56k::TWord(std::strtoul(th, nullptr, 0));
		std::vector<std::unique_ptr<WriteCollector>> collectors;

		if(writeTrace)
		{
#if DSP56300_DEBUGGER
			const int debuggerBuilt = 1;
#else
			const int debuggerBuilt = 0;
#endif
			for(unsigned d = 0; d < dspCount; ++d)
			{
				dsp56k::DSP& dsp = board.dspSet().dsp(d);
				dsp56k::JitConfig cfg = dsp.getJit().getConfig();
				cfg.memoryWritesCallCpp = true;
				dsp.getJit().setConfig(cfg);
				dsp.getJit().destroyAllBlocks();

				collectors.emplace_back(new WriteCollector(dsp));
				dsp.setDebugger(collectors.back().get());
			}
			std::cout << "writetrace: armed on " << dspCount << " dsps"
			          << " range X/Y " << hex32(0) << ".." << hex32(g_traceHi)
			          << " DSP56300_DEBUGGER=" << debuggerBuilt
			          << std::endl;
		}

		// ---------------------------------------------------------- the walk
		AudioReading walkRead;
		uint64_t walkBuckets[64] = {0};

		{
			g2::Frame impulse{};
			impulse.slot[0] = g_impulseLeft;
			impulse.slot[1] = g_impulseRight;

			const g2::Frame silence{};

			for(auto& c : collectors)
				c->arm(true);

			for(unsigned q = 0; q < walkQuanta; ++q)
			{
				const g2::Frame& in = (pushImpulse && q == 0) ? impulse : silence;

				(void) scheduler->push(&in, 1);
				scheduler->runFrames(1);

				for(unsigned d = 0; d < dspCount; ++d)
					pcHistogram[d][board.dspSet().dsp(d).getPC().toWord() & pcBucketMask] += 1;

				// The READ-ONLY control for the poison watch. It writes nothing,
				// so it cannot perturb the machine the way re-poisoning might,
				// and it answers the same question one step less sharply: how
				// many words of the window are non-zero at the end of each
				// quantum. A sample the synthesis stores and the firmware then
				// clears is visible here as a non-zero count in some quanta and
				// zero in others -- which the end-of-walk read cannot see at all.
				if(winSample)
				{
					for(unsigned d = 0; d < dspCount; ++d)
					{
						dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

						unsigned nz = 0, nzY = 0;
						for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
						{
							if(memory.get(dsp56k::MemArea_X, w) != 0)
								++nz;
							if(yWin && memory.get(dsp56k::MemArea_Y, w) != 0)
								++nzY;
						}

						if(nzY != 0)
						{
							++winSampleQuantaNonZeroY[d];
							if(nzY > winSampleMaxWordsY[d])
								winSampleMaxWordsY[d] = nzY;
							if(winSampleFirstQuantumY[d] < 0)
								winSampleFirstQuantumY[d] = int(q);
						}

						if(nz == 0)
							continue;

						++winSampleQuantaNonZero[d];
						winSampleWordTotal[d] += nz;
						if(nz > winSampleMaxWords[d])
							winSampleMaxWords[d] = nz;
						if(winSampleFirstQuantum[d] < 0)
							winSampleFirstQuantum[d] = int(q);
					}
				}

				if(scratchSample)
				{
					for(unsigned d = 0; d < dspCount; ++d)
					{
						dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

						const size_t span = g_scratchHi - g_scratchLo;
						const bool   hadPrev = !scratchPrevX[d].empty();
						if(!hadPrev)
						{
							scratchPrevX[d].assign(span, 0);
							scratchPrevY[d].assign(span, 0);
							if(scratchAddr)
							{
								scratchChangeCountX[d].assign(span, 0);
								scratchChangeCountY[d].assign(span, 0);
							}
						}

						unsigned nzX = 0, nzY = 0, chX = 0, chY = 0;
						dsp56k::TWord firstAddr = 0, firstVal = 0;

						for(dsp56k::TWord w = g_scratchLo; w < g_scratchHi; ++w)
						{
							const size_t        i = w - g_scratchLo;
							const dsp56k::TWord x = memory.get(dsp56k::MemArea_X, w);
							const dsp56k::TWord y = memory.get(dsp56k::MemArea_Y, w);

							if(x != 0)
								++nzX;
							if(hadPrev && x != scratchPrevX[d][i])
							{
								++chX;
								if(scratchAddr)
									++scratchChangeCountX[d][i];
							}
							if(hadPrev && y != scratchPrevY[d][i])
							{
								++chY;
								if(scratchAddr)
									++scratchChangeCountY[d][i];
							}

							scratchPrevX[d][i] = x;
							scratchPrevY[d][i] = y;

							if(y == 0)
								continue;
							if(nzY == 0)
							{
								firstAddr = w;
								firstVal  = y;
							}
							++nzY;
						}

						if(chX != 0)
						{
							++scratchQuantaChangedX[d];
							if(chX > scratchMaxChangedX[d])
								scratchMaxChangedX[d] = chX;
						}
						if(chY != 0)
						{
							++scratchQuantaChangedY[d];
							if(chY > scratchMaxChangedY[d])
								scratchMaxChangedY[d] = chY;
						}

						if(nzX != 0)
						{
							++scratchQuantaNonZeroX[d];
							if(nzX > scratchMaxX[d])
								scratchMaxX[d] = nzX;
						}
						if(nzY != 0)
						{
							++scratchQuantaNonZeroY[d];
							if(nzY > scratchMaxY[d])
							{
								scratchMaxY[d]       = nzY;
								scratchFirstAddrY[d] = firstAddr;
								scratchFirstValY[d]  = firstVal;
							}
						}
					}
				}

				// Re-poison every quantum. Reading the window only at the end
				// cannot see a word that was written and then overwritten, and
				// the firmware clears 32 words of each of the four buffers from
				// its idle loop -- so "written with a sample, then cleared" and
				// "only ever cleared" both end as zero and the end-of-walk read
				// calls them the same thing. They are not the same thing: the
				// first says the synthesis reaches the buffer and something
				// wipes it, the second says it never arrives.
				//
				// Restoring the sentinel after every quantum makes each quantum
				// its own experiment, so no write can hide behind a later one.
				if(poisonWatch)
				{
					for(unsigned d = 0; d < dspCount; ++d)
					{
						dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

						for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
						{
							const dsp56k::TWord v = memory.get(dsp56k::MemArea_X, w);
							if(v == g_poisonWord)
								continue;

							if(v == 0)
								++watchZeroWrites[d];
							else
							{
								++watchOtherWrites[d];
								if(watchFirstOtherQuantum[d] < 0)
								{
									watchFirstOtherQuantum[d] = int(q);
									watchFirstOtherAddr[d]    = w;
									watchFirstOtherVal[d]     = v;
								}
							}
							watchTouched[d].insert(w);
							memory.set(dsp56k::MemArea_X, w, g_poisonWord);
						}

						if(!yWin)
							continue;

						for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
						{
							const dsp56k::TWord v = memory.get(dsp56k::MemArea_Y, w);
							if(v == g_poisonWord)
								continue;

							if(v == 0)
								++watchZeroWritesY[d];
							else
							{
								++watchOtherWritesY[d];
								if(watchFirstOtherQuantumY[d] < 0)
								{
									watchFirstOtherQuantumY[d] = int(q);
									watchFirstOtherAddrY[d]    = w;
									watchFirstOtherValY[d]     = v;
								}
							}
							memory.set(dsp56k::MemArea_Y, w, g_poisonWord);
						}
					}
				}

				g2::Frame out{};
				++walkRead.framesRequested;
				const size_t got = scheduler->pull(&out, 1);
				if(got == 0)
					continue;
				++walkRead.framesReturned;

				observe(walkRead, out, q, walkBuckets);
			}

			for(auto& c : collectors)
				c->arm(false);
		}

		reportAudio("WALK", walkRead, walkBuckets);

		if(writeTrace)
		{
			for(unsigned d = 0; d < dspCount; ++d)
			{
				const WriteCollector& c = *collectors[d];

				for(int area = 0; area < 2; ++area)
				{
					const std::vector<WriteCell>& cells = c.cells(unsigned(area));

					uint64_t touched = 0, moving = 0, writesInRange = 0;
					for(const WriteCell& cell : cells)
					{
						if(!cell.seen)
							continue;
						++touched;
						writesInRange += cell.writes;
						if(cell.changes != 0)
							++moving;
					}

					std::cout << "  dsp " << d << " writetrace " << (area == 0 ? "X" : "Y")
					          << " totalWrites=" << c.writesTotal(unsigned(area))
					          << " inRange=" << writesInRange
					          << " above" << hex32(g_traceHi) << "=" << c.writesAbove(unsigned(area))
					          << " addrsWritten=" << touched
					          << " addrsWithAChangedValue=" << moving
					          << std::endl;

					// The addresses that carry a VALUE THAT MOVES, most first.
					// An address written every frame with the same word is a
					// store that fires and computes a constant; an address whose
					// value changes is the only thing that can become audio.
					std::vector<std::pair<uint64_t, dsp56k::TWord>> mv;
					for(size_t i = 0; i < cells.size(); ++i)
						if(cells[i].changes != 0)
							mv.emplace_back(cells[i].changes, dsp56k::TWord(i));
					std::sort(mv.begin(), mv.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

					std::cout << "    dsp " << d << " writetrace " << (area == 0 ? "X" : "Y") << " moving:";
					const size_t showMv = mv.size() < 32 ? mv.size() : 32;
					for(size_t i = 0; i < showMv; ++i)
					{
						const WriteCell& cell = cells[mv[i].second];
						std::cout << ' ' << hex32(mv[i].second) << "=n" << cell.writes
						          << "/c" << cell.changes
						          << "/[" << hex32(cell.minVal) << ".." << hex32(cell.maxVal) << ']';
					}
					if(showMv < mv.size())
						std::cout << " ...";
					std::cout << std::endl;

					// And the addresses written with a CONSTANT. These separate
					// "the store never fires" from "the store fires and stores
					// the same word every time", which no value-classifying
					// probe in this corpus has ever been able to tell apart.
					std::vector<std::pair<uint64_t, dsp56k::TWord>> ct;
					for(size_t i = 0; i < cells.size(); ++i)
						if(cells[i].seen && cells[i].changes == 0)
							ct.emplace_back(cells[i].writes, dsp56k::TWord(i));
					std::sort(ct.begin(), ct.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

					std::cout << "    dsp " << d << " writetrace " << (area == 0 ? "X" : "Y") << " constant:";
					const size_t showCt = ct.size() < 32 ? ct.size() : 32;
					for(size_t i = 0; i < showCt; ++i)
						std::cout << ' ' << hex32(ct[i].second) << "=n" << ct[i].first
						          << '/' << hex32(cells[ct[i].second].last);
					if(showCt < ct.size())
						std::cout << " ...";
					std::cout << std::endl;
				}

				std::cout << "  dsp " << d << " writetrace P writes=" << c.writesP() << std::endl;
			}

			for(unsigned d = 0; d < dspCount; ++d)
				board.dspSet().dsp(d).setDebugger(nullptr);
		}

		for(unsigned d = 0; d < dspCount; ++d)
			reportDma("postwalk", board.dspSet().peripherals(d), d);

		// The second transmit bus, beside the first. Channel 5's source is only
		// interesting if ESAI_1 is transmitting at all, and `enabledTx=0` on the
		// second bus would make every reading of channel 5 a reading of a
		// register nothing consumes.
		for(unsigned d = 0; d < dspCount; ++d)
		{
			dsp56k::Peripherals56311& p = board.dspSet().peripherals(d);
			std::cout << "  dsp " << d << " esai1"
			          << " enabledTx=" << p.getEsai1().hasEnabledTransmitters()
			          << " enabledRx=" << p.getEsai1().hasEnabledReceivers()
			          << " txWords=" << (p.getEsai1().getTxWordCount() + 1u)
			          << " rxWords=" << (p.getEsai1().getRxWordCount() + 1u)
			          << " secondBusUnderrun=" << scheduler->secondBusUnderrunFrames(d)
			          << std::endl;
		}

		{
			const g2::Hdi08Adapter::AccessCounts hdiAfterWalk = board.hdi08().accessCounts();
			reportHdi08("walk", hdiAfterNote, hdiAfterWalk);
		}

		// ------------------------------- where in the path does the zero start?
		//
		// The walk reports what the codec SINK carried. These read the stage
		// before it: the X-memory window each DSP's transmit DMA is currently
		// sourcing from. A zero there and a zero at the sink are one finding;
		// a non-zero there and a zero at the sink are a different one.
		for(unsigned d = 0; d < dspCount; ++d)
		{
			dsp56k::Peripherals56311& p = board.dspSet().peripherals(d);
			dsp56k::Esai&             esai = p.getEsai();

			const dsp56k::TWord source     = p.getDMA().getDSR(g_dmaTxChannel);
			const dsp56k::TWord frameWords = esai.getTxWordCount() + 1u;
			const dsp56k::TWord base       = frameWords != 0 ? source - (source % frameWords) : source;

			dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

			unsigned nonZero = 0;
			dsp56k::TWord first = 0;
			const dsp56k::TWord span = frameWords * 4u;

			for(dsp56k::TWord i = 0; i < span; ++i)
			{
				const dsp56k::TWord w = memory.get(dsp56k::MemArea_X, base + i);
				if(w == 0)
					continue;
				if(nonZero == 0)
					first = w;
				++nonZero;
			}

			std::cout << "  dsp " << d
			          << " instructionsInWalk="
			          << (board.dspSet().dsp(d).getInstructionCounter() - beforeWalk[d])
			          << " enabledTx=" << esai.hasEnabledTransmitters()
			          << " txWords=" << frameWords
			          << " dmaSource=" << source
			          << " txBufferNonZeroWords=" << nonZero << " of " << span
			          << " firstNonZero=" << first
			          << " cycleDebt=" << scheduler->cycleDebt(d + 1u)
			          << " longDispatch=" << scheduler->longDispatchQuanta(d + 1u)
			          << std::endl;
		}

		// One machine-readable line carrying every DSP's walk instruction count,
		// so a caller never has to grep eight separate lines and subtract against
		// baselines typed into a shell script. That is not hypothetical tidiness:
		// analysing this instrument by hand produced repeated wrong conclusions,
		// every one of them from reading a subset of the eight and generalising.
		// The counts are absolute; engagement is a comparison against a no-patch
		// run of the same build, which the caller must take itself.
		// A refusal is already reported by name where the load happens. That line
		// sits near the top of a log that runs to six figures, and every analysis
		// of this instrument grepped for counts and never read it -- so the same
		// verdict is repeated here, beside the numbers a reader actually reads.
		// A zero next to a refusal is not a measurement of anything.
		if(loadResultName != nullptr && std::string(loadResultName) != "PCH2-LOADED")
			std::cout << "*** DELIVERY REFUSED: " << loadResultName
			          << " -- nothing below measures the machine's response to a patch"
		          << std::endl;

		std::cout << "WALKCOUNTS";
		for(uint32_t d = 0; d < dspCount; ++d)
			std::cout << ' ' << (board.dspSet().dsp(d).getInstructionCounter() - beforeWalk[d]);
		std::cout << std::endl;

		// The audio buffer neighbourhood. The transmit DMA sources from around
		// X:0x1C00..0x2000 on this machine (the DSR values above are inside it),
		// and the 32-word window read earlier was empty. This widens the reading
		// to the whole neighbourhood so that "the DMA window is empty" can be
		// separated from "the DSP writes no audio anywhere near it".
		for(unsigned d = 0; d < dspCount; ++d)
		{
			dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

			unsigned nonZeroX = 0, nonZeroY = 0;
			dsp56k::TWord firstAddr = 0, firstVal = 0;

			for(dsp56k::TWord w = 0x1C00u; w < 0x2000u; ++w)
			{
				const dsp56k::TWord x = memory.get(dsp56k::MemArea_X, w);
				const dsp56k::TWord y = memory.get(dsp56k::MemArea_Y, w);

				if(x != 0)
				{
					if(nonZeroX == 0)
					{
						firstAddr = w;
						firstVal  = x;
					}
					++nonZeroX;
				}
				if(y != 0)
					++nonZeroY;
			}

			std::cout << "  dsp " << d << " audio neighbourhood X:0x1C00..0x2000"
			          << " nonZeroX=" << nonZeroX << "/1024"
			          << " nonZeroY=" << nonZeroY << "/1024"
			          << " firstX=" << hex32(firstAddr) << ":" << firstVal
			          << std::endl;
		}

		// The poison readback. survivors == the whole window means nothing wrote
		// a single word of it during the walk; survivors < the window names
		// exactly how much was written, with or without zeros.
		if(poison)
		{
			for(unsigned d = 0; d < dspCount; ++d)
			{
				dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

				unsigned survivors = 0, zeroed = 0, other = 0;
				dsp56k::TWord firstChangedAddr = 0, firstChangedVal = 0;

				for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
				{
					const dsp56k::TWord v = memory.get(dsp56k::MemArea_X, w);
					if(v == g_poisonWord)
					{
						++survivors;
						continue;
					}
					if(other == 0 && zeroed == 0)
					{
						firstChangedAddr = w;
						firstChangedVal  = v;
					}
					if(v == 0)
						++zeroed;
					else
						++other;
				}

				std::cout << "  dsp " << d << " poison readback"
				          << " survivors=" << survivors << "/1024"
				          << " overwrittenWithZero=" << zeroed
				          << " overwrittenWithOther=" << other
				          << " firstChanged=" << hex32(firstChangedAddr) << ":" << firstChangedVal
				          << std::endl;
			}
		}

		if(winSample)
		{
			for(unsigned d = 0; d < dspCount; ++d)
				std::cout << "  dsp " << d << " window sample (read-only)"
				          << " quantaWithNonZero=" << winSampleQuantaNonZero[d] << "/" << walkQuanta
				          << " maxNonZeroWords=" << winSampleMaxWords[d]
				          << " totalWordSamples=" << winSampleWordTotal[d]
				          << " firstQuantum=" << winSampleFirstQuantum[d]
				          << std::endl;

			if(yWin)
			{
				for(unsigned d = 0; d < dspCount; ++d)
					std::cout << "  dsp " << d << " window sample Y (read-only)"
					          << " quantaWithNonZeroY=" << winSampleQuantaNonZeroY[d] << "/" << walkQuanta
					          << " maxNonZeroWordsY=" << winSampleMaxWordsY[d]
					          << " firstQuantumY=" << winSampleFirstQuantumY[d]
					          << std::endl;
			}
		}

		if(scratchSample)
		{
			for(unsigned d = 0; d < dspCount; ++d)
				std::cout << "  dsp " << d << " scratch sample (read-only) "
				          << hex32(g_scratchLo) << ".." << hex32(g_scratchHi)
				          << " quantaWithNonZeroX=" << scratchQuantaNonZeroX[d] << "/" << walkQuanta
				          << " maxWordsX=" << scratchMaxX[d]
				          << " quantaWithNonZeroY=" << scratchQuantaNonZeroY[d] << "/" << walkQuanta
				          << " maxWordsY=" << scratchMaxY[d]
				          << " firstY=" << hex32(scratchFirstAddrY[d]) << ":" << scratchFirstValY[d]
				          << " quantaChangedX=" << scratchQuantaChangedX[d] << "/" << walkQuanta
				          << " maxChangedX=" << scratchMaxChangedX[d]
				          << " quantaChangedY=" << scratchQuantaChangedY[d] << "/" << walkQuanta
				          << " maxChangedY=" << scratchMaxChangedY[d]
				          << std::endl;

			if(scratchAddr)
			{
				for(unsigned d = 0; d < dspCount; ++d)
				{
					for(int area = 0; area < 2; ++area)
					{
						const std::vector<uint32_t>& cc = area == 0 ? scratchChangeCountX[d] : scratchChangeCountY[d];
						if(cc.empty())
							continue;

						std::vector<std::pair<uint32_t, dsp56k::TWord>> hits;
						for(size_t i = 0; i < cc.size(); ++i)
							if(cc[i] != 0)
								hits.emplace_back(cc[i], dsp56k::TWord(g_scratchLo + i));

						std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

						std::cout << "  dsp " << d << " scratch changed addrs " << (area == 0 ? "X" : "Y")
						          << " distinct=" << hits.size();
						const size_t show = hits.size() < 48 ? hits.size() : 48;
						for(size_t i = 0; i < show; ++i)
							std::cout << ' ' << hex32(hits[i].second) << '=' << hits[i].first;
						if(show < hits.size())
							std::cout << " ...";
						std::cout << std::endl;
					}
				}
			}
		}

		// The per-quantum tally. zeroWrites counts every quantum in which a word
		// held zero instead of the sentinel -- the firmware's buffer clear is
		// expected to dominate it. otherWrites is the one that matters: a single
		// non-zero, non-sentinel word anywhere in the window is a sample the
		// synthesis stored, and its absence over the whole walk is the finding.
		if(poisonWatch)
		{
			for(unsigned d = 0; d < dspCount; ++d)
			{
				std::vector<uint32_t> touched(watchTouched[d].begin(), watchTouched[d].end());

				std::cout << "  dsp " << d << " poison watch"
				          << " zeroWrites=" << watchZeroWrites[d]
				          << " otherWrites=" << watchOtherWrites[d]
				          << " distinctWordsTouched=" << touched.size() << "/1024";

				if(!touched.empty())
					std::cout << " touchedRange=" << hex32(touched.front())
					          << ".." << hex32(touched.back());

				std::cout << " firstOtherQuantum=" << watchFirstOtherQuantum[d]
				          << " firstOther=" << hex32(watchFirstOtherAddr[d])
				          << ":" << watchFirstOtherVal[d]
				          << std::endl;
			}

			if(yWin)
			{
				for(unsigned d = 0; d < dspCount; ++d)
					std::cout << "  dsp " << d << " poison watch Y"
					          << " zeroWritesY=" << watchZeroWritesY[d]
					          << " otherWritesY=" << watchOtherWritesY[d]
					          << " firstOtherQuantumY=" << watchFirstOtherQuantumY[d]
					          << " firstOtherY=" << hex32(watchFirstOtherAddrY[d])
					          << ":" << watchFirstOtherValY[d]
					          << std::endl;
			}
		}

		// Did the patch reach the DSPs at all? A digest of each DSP's three
		// memory areas. The digests of the delivered arm and the control arm are
		// compared BY THE READER, across two runs: a digest that is identical in
		// both says the patch changed no DSP memory, which is a different
		// finding from a patch that changed DSP memory and still made no sound.
		//
		// FNV-1a over the low 0x8000 words of each area. It is a fingerprint and
		// not a measurement of content; only equality and inequality are read.
		for(unsigned d = 0; d < dspCount; ++d)
		{
			dsp56k::Memory& memory = board.dspSet().dsp(d).memory();

			const dsp56k::EMemArea areas[3] = {dsp56k::MemArea_X, dsp56k::MemArea_Y, dsp56k::MemArea_P};
			const char* const names[3] = {"X", "Y", "P"};

			std::cout << "  dsp " << d << " memory digest";

			for(unsigned a = 0; a < 3; ++a)
			{
				uint64_t hash = 1469598103934665603ull;
				uint64_t nonZero = 0;

				for(dsp56k::TWord w = 0; w < 0x8000u; ++w)
				{
					const dsp56k::TWord v = memory.get(areas[a], w);
					if(v != 0)
						++nonZero;
					hash = (hash ^ uint64_t(v)) * 1099511628211ull;
				}

				char buf[40];
				std::snprintf(buf, sizeof(buf), " %s=%016llx/nz=%llu", names[a],
					(unsigned long long) hash, (unsigned long long) nonZero);
				std::cout << buf;
			}

			std::cout << std::endl;
		}

		// ------------------------------------- P memory dump and disassembly
		//
		// The digest above says only whether DSP memory MOVED. This says what
		// it moved TO. G2_AUDIO_PDUMP names a directory; for every DSP it
		// writes the raw program words and the disassembly of the same range,
		// so that a delivered arm and a no-patch control can be differenced
		// word by word offline rather than eyeballed through a hash.
		//
		// The raw dump is written unconditionally over the range, zeros
		// included, so that the two arms' files are line-aligned and `diff`
		// alone names every changed word. The disassembly skips NOPs, because
		// an all-zero page disassembles to thousands of identical lines that
		// bury the code.
		if(const char* const pdumpDir = std::getenv("G2_AUDIO_PDUMP"))
		{
			dsp56k::TWord lo = 0x0000u, hi = 0x8000u;
			if(const char* const v = std::getenv("G2_AUDIO_PDUMP_LO"))
				lo = static_cast<dsp56k::TWord>(std::strtoul(v, nullptr, 0));
			if(const char* const v = std::getenv("G2_AUDIO_PDUMP_HI"))
				hi = static_cast<dsp56k::TWord>(std::strtoul(v, nullptr, 0));

			std::cout << "pdump dir=" << pdumpDir
			          << " range=" << hex32(lo) << ".." << hex32(hi) << std::endl;

			for(unsigned d = 0; d < dspCount; ++d)
			{
				dsp56k::DSP&    dsp    = board.dspSet().dsp(d);
				dsp56k::Memory& memory = dsp.memory();

				std::vector<uint32_t> words;
				words.reserve(hi > lo ? hi - lo : 0u);

				unsigned nonZeroP = 0;
				for(dsp56k::TWord w = lo; w < hi; ++w)
				{
					const dsp56k::TWord v = memory.get(dsp56k::MemArea_P, w);
					if(v != 0)
						++nonZeroP;
					words.push_back(static_cast<uint32_t>(v));
				}

				char nameRaw[512], nameAsm[512];
				std::snprintf(nameRaw, sizeof(nameRaw), "%s/dsp%u.pmem.txt", pdumpDir, d);
				std::snprintf(nameAsm, sizeof(nameAsm), "%s/dsp%u.disasm.txt", pdumpDir, d);

				{
					std::ofstream raw(nameRaw);
					for(size_t i = 0; i < words.size(); ++i)
					{
						char line[32];
						std::snprintf(line, sizeof(line), "%06x %06x\n",
							static_cast<unsigned>(lo + i), words[i]);
						raw << line;
					}
				}

				std::string text;
				dsp56k::Disassembler disasm(dsp.opcodes());
				const bool ok = disasm.disassembleMemoryBlock(text, words, lo, true, true, true);

				{
					std::ofstream out(nameAsm);
					out << text;
				}

				std::cout << "  dsp " << d << " pdump nonZeroP=" << nonZeroP
				          << " words=" << words.size()
				          << " disasmOk=" << (ok ? 1 : 0)
				          << " disasmBytes=" << text.size()
				          << std::endl;
			}
		}

		for(unsigned d = 0; d < dspCount; ++d)
		{
			std::vector<std::pair<uint64_t, uint32_t>> ranked;
			for(const auto& one : pcHistogram[d])
				ranked.emplace_back(one.second, one.first);
			std::sort(ranked.begin(), ranked.end(), std::greater<>());

			std::cout << "  dsp " << d << " pc regions (" << pcBucketWords << "-word, top 6 of " << ranked.size() << "):";
			for(size_t i = 0; i < ranked.size() && i < 6; ++i)
				std::cout << " " << hex32(ranked[i].second) << "=" << ranked[i].first;
			std::cout << std::endl;
		}

		std::cout << "  mcu cycleDebt=" << scheduler->cycleDebt(0)
		          << " longDispatch=" << scheduler->longDispatchQuanta(0) << std::endl;

		std::cout << "chain health: starved=" << scheduler->starvedFrames()
		          << " overflow=" << scheduler->overflowFrames()
		          << " dropped=" << scheduler->droppedFrames()
		          << " underflow=" << scheduler->underflowFrames()
		          << " chainAttached=" << (scheduler->chainAttached() ? 1 : 0)
		          << std::endl;

		for(unsigned p = 0; p < dspCount; ++p)
			std::cout << "  position " << p
			          << " underrunFrames=" << scheduler->underrunFrames(p)
			          << " secondBusUnderrunFrames=" << scheduler->secondBusUnderrunFrames(p)
			          << " phaseErrorFrames=" << scheduler->phaseErrorFrames(p) << std::endl;

		// ---------------------------- the audio instrument's KNOWN POSITIVE
		//
		// Run AFTER the walk on the same machine, so it cannot move the
		// measurement it qualifies. It places a sentinel at the tail position's
		// transmit source and reads it back out of the codec sink, through the
		// same pull and the same comparator the walk uses.
		{
			const unsigned tailPort = portOfChainPosition(board, dspCount - 1u, dspCount);
			const bool found = tailPort < dspCount;

			std::cout << "known positive: tailPortFound=" << (found ? 1 : 0)
			          << " tailPort=" << (found ? tailPort : 0u) << std::endl;

			int      arrival = -1;
			int32_t  gotL = 0, gotR = 0;
			bool     exact = false;
			unsigned tried = 0;

			if(found)
			{
				dsp56k::Peripherals56311& p = board.dspSet().peripherals(tailPort);
				dsp56k::Esai&             tailEsai = p.getEsai();

				const g2::Frame silence{};

				for(unsigned q = 0; q < g_sinkControlQuanta && arrival < 0; ++q)
				{
					++tried;

					const dsp56k::TWord enabled = tailEsai.hasEnabledTransmitters();

					for(uint32_t reg = 0; reg < g_esaiTransmitters; ++reg)
						if(enabled & (1u << reg))
							tailEsai.writeTX(reg, g_sinkControlWord);

					{
						const dsp56k::TWord source     = p.getDMA().getDSR(g_dmaTxChannel);
						const dsp56k::TWord frameWords = tailEsai.getTxWordCount() + 1u;
						const dsp56k::TWord base       = source - (source % frameWords);

						dsp56k::Memory& tailMemory = board.dspSet().dsp(tailPort).memory();

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

					arrival = int(q);
					gotL = out.slot[0];
					gotR = out.slot[1];
					exact = out.slot[0] == g_sinkControlExpected && out.slot[1] == g_sinkControlExpected;
				}
			}

			std::cout << "known positive: quantaTried=" << tried
			          << " arrival=" << arrival
			          << " exact=" << (exact ? 1 : 0)
			          << " slot0=" << gotL << " slot1=" << gotR
			          << " expected=" << g_sinkControlExpected << std::endl;
		}

		std::cout << "log lines seen = " << g_logLines.load() << std::endl;
		std::cout << "elapsed = " << seconds() << "s" << std::endl;

		std::cout << g2::test::g_verdictNotVerified
		          << " diagnostic instrument; no assertion is made here" << std::endl;

		return true;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;
	return g2::test::gatedExitCode(counters);
}
