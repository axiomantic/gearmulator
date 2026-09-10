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
// Nothing here asserts. Every verdict is an observable.
#include "gatedFixture.h"

#include "../board.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"
#include "../transportHub.h"
#include "../uart0.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "dsp56kEmu/disasm.h"
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

		// -------------------------------------------------------- the delivery
		std::vector<uint8_t> delivered;

		{
			// ONE client, sized for the largest thing any arm originates. Two
			// attached clients would put a second endpoint in a hub that holds
			// three, and a delivery that failed for that reason would look like
			// a delivery the firmware ignored.
			std::vector<uint8_t> scratch(g2::g_maxPatchLoadMessageBytes + 4);
			g2::InternalClient client(board.transport(), scratch.size(), 4);

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

			board.pumpTransport();
			scheduler->runFrames(1);
		}

		// ------------------------------------------------------- the long window
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
			const uint8_t message[] = {
				0x90u, 0x3Cu, 0x64u,   // note on,  channel 1, C4, velocity 100
				0x91u, 0x40u, 0x64u,   // note on,  channel 2, E4
				0x92u, 0x43u, 0x64u,   // note on,  channel 3, G4
				0x93u, 0x3Cu, 0x64u    // note on,  channel 4, C4
			};

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
				if((i & 0x3ffu) == 0 && expired())
					break;
				if(board.mcuHalted())
					break;
			}

			std::cout << "note: ran " << noteQuanta << " further quanta, pc="
			          << hex32(board.mcuReg(g_regPc)) << " t=" << seconds() << "s" << std::endl;
		}

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
					memory.set(dsp56k::MemArea_X, w, g_poisonWord);
			}
			std::cout << "poison: filled X:" << hex32(g_poisonLo) << ".." << hex32(g_poisonHi)
			          << " with " << hex32(g_poisonWord) << " on " << dspCount << " dsps"
			          << " watch=" << (poisonWatch ? 1 : 0) << std::endl;
		}

		// ---------------------------------------------------------- the walk
		AudioReading walkRead;
		uint64_t walkBuckets[64] = {0};

		{
			g2::Frame impulse{};
			impulse.slot[0] = g_impulseLeft;
			impulse.slot[1] = g_impulseRight;

			const g2::Frame silence{};

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

						unsigned nz = 0;
						for(dsp56k::TWord w = g_poisonLo; w < g_poisonHi; ++w)
							if(memory.get(dsp56k::MemArea_X, w) != 0)
								++nz;

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
		}

		reportAudio("WALK", walkRead, walkBuckets);

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
