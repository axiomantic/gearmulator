// The M6 remainder: a REAL `.pch2` delivered to BOOTED, RUNNING firmware.
// Tier T1: it needs the Clavia-derived artifacts and SKIPS with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// WHAT WAS MISSING, AND IT WAS NOT A COMPONENT. Every piece already worked and
// no file connected them. t0_usb_ingress_byte proves a patch byte reaches the
// device register file the firmware reads -- on a Board WITH NO FIRMWARE IN IT.
// t1_boot and t1_egress boot the firmware -- and load NO PATCH. The two sets of
// files were DISJOINT: `pch2Load` and `InternalClient` appear nowhere in
// g2TestConsole/main.cpp, which is where the machine is booted, while `Board`
// appears there 29 times. So `--impulse`'s `arrival=-1` is a statement about an
// UNPATCHED machine and says nothing about a patched one. This file is the
// join.
//
// THE TWO QUESTIONS IT MAKES ANSWERABLE.
//
//   1. Does MCU routine 0x30032A82 fire on a real patch load? The workspace
//      FINDINGS record it as the project's oldest untested inference -- "that
//      0x30032A82 ALSO fires on a real patch load is inferred FROM THE CALL
//      CHAIN ALONE. No patch has ever been delivered to this firmware in the
//      emulator." Nothing in the emulator names that address (a search for it
//      under source/nord/g2 returns 0, against 23 for the `0x3003` prefix), so
//      the only way to reach it is to let the firmware run the path.
//
//   2. Audio. `arrival=-1` on an unpatched machine is not a claim about a
//      patched one.
//
// THE INSTRUMENT, AND WHY IT NEEDS NO PRODUCTION CHANGE. The MCF5307 core
// fetches every instruction word through the bus read callback -- `cpu.nim`'s
// `ctx.readFn(ctx.user, ctx.pc, 2, addr status)` -- and Board::onRead routes
// that to the MemoryMap, which routes it to the BusTarget attached at
// Region::Sdram. That target is THIS FILE'S Ram. So a counter on 16-bit reads
// at one SDRAM offset is an INSTRUCTION-FETCH COUNTER for that address, built
// entirely inside the test.
//
// THE INSTRUMENT'S CONTROLS, BOTH FROM THE SAME POPULATION. A zero from a
// counter that never fires is not a measurement.
//
//   known positive   the address the MACHINE ITSELF is sitting at when the
//                    window opens, read off Board::mcuReg(17) and installed as
//                    a probe at that instant. It is not chosen by this file,
//                    and it is fetched through the identical counter.
//   known negative   an address inside the vector TABLE. Vectors are read as
//                    32-bit longwords and never fetched as instruction words,
//                    so the same counter must read 0 there.
//
// AND THE CONTROL THAT MAKES THE ANSWER AN ANSWER: the whole run happens TWICE
// on the same code path, once WITHOUT a patch and once WITH one. A probe count
// that is non-zero in both runs says the routine fires anyway; non-zero only in
// the patched run is the patch load reaching it.
//
// WHY THE MACHINE PLACEMENT IS COPIED RATHER THAN SHARED. Plan section 1.3
// rule 1, the reason t1_egress and t1_kernel_load each carry their own: a
// harness's configuration lives at its own site.
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

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// INT-1's filter, with INT-1's limit: the underruns are REAL and expected in
	// the boot regime, because nothing drains the ESAIs until the codec queues
	// arrive. This hides the REPETITION and nothing else. Set
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

	// ------------------------------------------------- INT-1's machine placement

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
	// t0_usb_ingress_byte's instrument, unchanged and for its reasons: the
	// part's own peek command (0xD2) issued at the CS3 command port and read
	// back at the CS3 data port, with the peek target selected by the
	// endpoint-configuration command (0x20 + endpoint). It reads the head byte
	// of the OUT buffer the given endpoint delivers into, and answers the
	// model's benign 0x00 when that buffer holds nothing.
	//
	// WHY IT IS HERE AND NOT ONLY THERE. t0_usb_ingress_byte runs it on a Board
	// with NO FIRMWARE IN IT. On a BOOTED machine the same reading answers a
	// different question: whether the firmware ever took the packet out.
	constexpr uint32_t g_dataPort    = g2::g_cs3Base + 0x00u;
	constexpr uint32_t g_commandPort = g2::g_cs3Base + 0x10u;

	constexpr int g_byteWidth = 1;

	constexpr uint8_t g_endpointConfigBase = 0x20u;
	constexpr uint8_t g_peekCommand        = 0xD2u;

	// The synthetic object the known positive delivers. Its type byte is not
	// 0x00, so a reading of it cannot be confused with the benign answer; it is
	// also NOT 0x21, which is the type byte of the first object in every file
	// of the corpus, so a reading of it cannot be confused with the PATCH's
	// either. Its whole framed length is 3 + 15 = 18 bytes, well inside the
	// 64-byte capacity the model gives endpoint 2.
	constexpr uint8_t g_probeObjectType   = 0x4Au;
	constexpr size_t  g_probeObjectLength = 15u;

	uint8_t peekHeadByte(g2::Board& _board, const int _endpoint)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;

		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_endpointConfigBase) + uint32_t(_endpoint), &status);
		g2::Board::onWrite(&_board, g_commandPort, g_byteWidth,
			uint32_t(g_peekCommand), &status);

		const uint32_t value = g2::Board::onRead(&_board, g_dataPort, g_byteWidth, &status);

		return uint8_t(value & 0xffu);
	}

	// The largest FRAMED object in a `.pch2`, counting its 3-byte header, and
	// how many objects it holds. Both are COMPUTED from the file this run
	// loaded and neither is written here as a literal, so a different patch
	// reports its own figures.
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
	// THE ADDRESSES ARE THE FINDINGS' OWN and none of them is this file's
	// invention. 0x30032A82 is the load-bearing unknown; 0x3001D85C and
	// 0x3001DAD8 are the two routines the findings name as its callers, and
	// they are probed too because "the caller never ran" and "the caller ran
	// and did not reach it" are different findings about the machine.
	// 0x30032254 is the three-piece assembly 0x30032A82 reaches.
	constexpr uint32_t g_probeTarget   = 0x30032A82u;
	constexpr uint32_t g_probeCallerA  = 0x3001D85Cu;
	constexpr uint32_t g_probeCallerB  = 0x3001DAD8u;
	constexpr uint32_t g_probeAssembly = 0x30032254u;

	// THE KNOWN NEGATIVE. An address inside the vector TABLE this file writes.
	// Vectors are read as 32-bit longwords, never fetched as instruction words,
	// so the 16-bit counter must read 0 there. It is offset 4 rather than 0 so
	// that it is not the reset vector either.
	constexpr uint32_t g_probeNegative = g_vectorTableBase + 4u;

	// How many quanta the machine runs after the patch is handed over. The
	// findings name a 4000-tick deferred rebuild timer as the scheduler of the
	// routine under test, so a window shorter than that could report a routine
	// that had not been given the chance to run. This is more than ten times it.
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

		// Zeroes every counter, so that a window's counts are the WINDOW's and
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

				// A CONTENT WRITE IS ONE THAT IS NOT THE DISPLAY CLEAR, which
				// writes 0x20 and only 0x20. Plan section 24.6 row W3-397
				// records what counting 0x20 as content cost: a blank screen
				// exiting 0.
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
	// t1_egress's two values, unchanged and for its reasons: they differ from
	// each other so a chain that carried slot 0 into both slots fails rather
	// than passes, and neither is a power of two.
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	constexpr unsigned g_overrunQuanta = 1024u;

	// t0_usb_ingress_byte's in-process container, built here for the same
	// reason it is built there: a one-object `.pch2` whose single object is
	// small enough for the endpoint the model gives it. It carries no Clavia
	// byte -- every byte of it is this process's own.
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
	};

	// Boots one machine, optionally hands it `_patch`, opens the observation
	// window and then walks the codec. Returns false only when the machine could
	// not be placed at all; a machine that ran and moved nothing returns true
	// with a result that says so, because "the machine is silent" is a
	// MEASUREMENT and must reach the assertions rather than a bail-out.
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
		// THE CLIENT IS ATTACHED TO THE BOARD'S OWN HUB and to nothing else, so
		// what leaves it is drained by Board::pumpTransport at the next quantum
		// boundary and handed to the device with isp1181_rx. There is no second
		// path: this is the same `pch2Load` the plugin calls.
		{
			// The two inbox sizes bound the DEVICE-TO-PLUGIN direction only; the
			// bound on what may be ORIGINATED is the hub's and the hub reports
			// it. 4096 clears the largest object in the corpus, measured at
			// 2492 bytes across all 73 files.
			g2::InternalClient client(board.transport(), 4096, 4);

			if(_deliver)
			{
				_r.patchOffered = true;
				_r.patchBytes   = _patch.size();
				_r.loadResult   = g2::pch2Load(_patch.data(), _patch.size(), client);
				_r.loadReturned = true;
			}

			// ONE QUANTUM, so Board::pumpTransport drains what pch2Load put in
			// the hub and hands each frame to the device with isp1181_rx. Then
			// the CS3 reading, which is the only observable that says whether
			// any byte of the patch is IN THE DEVICE.
			scheduler->runFrames(1);
			_r.peekAfterHandover = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// ------------------------------------------------ the window opens
			//
			// THE KNOWN POSITIVE IS READ OFF THE MACHINE HERE, not chosen above:
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

			// THE SECOND READING. If the first was non-zero and this one is
			// 0x00, the firmware TOOK the packet out during the window; if both
			// carry the same byte, it never did.
			_r.peekAfterWindow = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);

			// ------------------------------------------- the wire's known positive
			//
			// A ONE-OBJECT CONTAINER THIS PROCESS BUILT, small enough for the
			// buffer the model gives the endpoint, delivered through THE SAME
			// client, THE SAME hub, THE SAME pump and read back through THE SAME
			// two bus writes and one bus read. It is what makes a 0x00 above a
			// measurement of the PATCH rather than of a dead wire on a booted
			// machine.
			{
				const std::vector<uint8_t> probeFile = buildProbeContainer();

				_r.probeLoaded =
					g2::pch2Load(probeFile.data(), probeFile.size(), client) == g2::Pch2LoadResult::Loaded;

				scheduler->runFrames(1);

				_r.peekProbeObject = peekHeadByte(board, g2::BoardConfig{}.usbProtocolEndpoint);
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
		// THE UNPATCHED RUN IS FIRST and it runs the identical code path. It is
		// what makes any probe count in the patched run a statement about the
		// PATCH rather than about the firmware's ordinary idle.
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
		check(control.programsLanded, "control: every DSP position took its program");
		check(patched.programsLanded, "patched: every DSP position took its program");
		check(!patched.halted,  "patched: the core is not halted when the window closes");
		check(!patched.faulted, "patched: the board reports no fault when the window closes");

		// ------------------------------------------------------------- THE ROW
		//
		// A REAL `.pch2` IS ACCEPTED BY THE BOARD'S OWN TRANSPORT HUB. Every
		// other measurement in this file is downstream of it: a load that was
		// REFUSED originated some frames and not others, and a probe count taken
		// after a partial hand-over is a count about neither machine.
		check(patched.loadResult == g2::Pch2LoadResult::Loaded,
			std::string("a real 18-object `.pch2` loads through the BOARD'S OWN hub into the running"
			            " machine; pch2Load answered ") +
			g2::pch2LoadResultName(patched.loadResult));

		// ------------------------------------------------- the wire, on a BOOTED machine
		//
		// THE KNOWN POSITIVE FOR EVERY 0x00 REPORTED ABOVE. Same board, same
		// client, same hub, same pump, same three bus calls -- only the object
		// is different, and the difference is its SIZE.
		check(patched.probeLoaded, "the one-object probe container loads through the same hub");
		check(control.probeLoaded, "the one-object probe container loads on the control run too");

		// THE KNOWN POSITIVE IS TAKEN ON THE CONTROL RUN AND ONLY THERE, and
		// that is a property of the instrument rather than a weakening of it.
		// The peek reads the HEAD of the endpoint's OUT FIFO. On the control run
		// the FIFO is empty when the probe object arrives, so the head IS the
		// probe object. On the patched run the patch's own first packet is still
		// sitting at that head -- the model gives endpoint 2 two buffers and the
		// firmware drained neither -- so the probe object queues BEHIND it and
		// the head is still the patch's byte.
		//
		// An earlier form of this file asserted the probe's type byte on BOTH
		// runs and went red on the patched one. THE ASSERTION WAS WRONG, NOT THE
		// MACHINE: it assumed a buffer of one. It is recorded here rather than
		// silently deleted, because the next reader will otherwise re-derive it.
		check(control.peekProbeObject == g_probeObjectType,
			std::string("KNOWN POSITIVE, on a BOOTED machine: a small object delivered through the"
			            " same client, hub, pump and bus calls is readable at the CS3 data port as"
			            " its own type byte ") + hex32(g_probeObjectType) + "; read " +
			hex32(control.peekProbeObject));

		// AND THE CONTROL THAT MAKES THAT KNOWN POSITIVE MEAN SOMETHING: on the
		// run where NO patch was offered, the same reading before the probe
		// object is the benign 0x00.
		check(control.peekAfterHandover == 0x00u,
			"CONTROL: with no patch offered, the endpoint buffer is empty at the same instant");

		// THE SENTENCE THIS FILE EXISTS TO HOLD. A byte of a REAL `.pch2` is in
		// the device register file of a machine that has really booted. The
		// expected value is COMPUTED from the file this run read and is not
		// written here as a literal, so the case cannot pass against a device
		// that answers a fixed byte that happens to match.
		check(patched.peekAfterHandover == firstType,
			std::string("a byte of a REAL `.pch2` is readable at the CS3 data port of a BOOTED"
			            " machine as the patch's own first object type ") + hex32(firstType) +
			"; read " + hex32(patched.peekAfterHandover));

		// -------------------------------------------------------- the verdict lines
		//
		// REPORTED AND NOT ASSERTED, deliberately. Each names a state of TODAY's
		// machine, and an assertion on it would go RED on the day the machine
		// improves -- which is the wrong direction for a gate to fire in.
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
