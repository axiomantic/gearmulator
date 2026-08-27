// Task INT-2. Tier T1: this test needs the Clavia firmware artifacts and SKIPS
// with a reason when NMG2_ARTIFACTS does not resolve.
//
// Plan section 16 (INT-2), section 6 milestone M5, design sections 18.3, 12.3,
// 13.10.5 and 14.4.
//
// WHAT THIS TEST IS. INT-2's Check: line requires that "all seven counters are
// zero across the whole golden run": every underrunFrames(position), every
// secondBusUnderrunFrames(position), every phaseErrorFrames(position),
// starvedFrames(), overflowFrames(), droppedFrames() and underflowFrames().
// "A non-zero count in any of them is a failure, not a warning."
//
// ZERO IS THE WEAKEST ASSERTION THERE IS, AND THIS FILE REFUSES TO MAKE IT
// ALONE. A machine that never ran satisfies every one of those seven equalities.
// So EVERY zero asserted here is paired with a KNOWN POSITIVE: a companion case
// that drives THAT counter, through THAT accessor, above zero. The known
// positives run FIRST and their failure is this file's failure, because a seven
// zeros reported by seven counters that cannot move is the green mirage plan
// section 24.6 rows W3-95, W3-396 and W3-397 record three instances of.
//
// WHERE EACH KNOWN POSITIVE COMES FROM, AND WHY IT IS THE ONE IT IS:
//
//   underrunFrames(p)            A FIRMWARE-FREE Scheduler. Every run gate is
//   secondBusUnderrunFrames(p)   shut, so no transmit callback ever fires and
//                                the priming run of beginPlayPhase is L real
//                                underruns at every position. Read back through
//                                the SAME Scheduler accessors the golden run
//                                asserts zero on.
//
//   phaseErrorFrames(p)          A ChainAdapter driven DIRECTLY, with a real
//                                emulated Esai so the written-flag condition is
//                                real:
//                                one position's audio transmit wrapper fired
//                                TWICE inside one quantum. The Scheduler cannot
//                                be made to ask for a second transmit, which is
//                                the whole point of the counter; the Scheduler's
//                                own accessor is this adapter reading minus a
//                                baseline it takes at beginPlayPhase, and that
//                                subtraction is stated here as the limit of this
//                                particular known positive rather than claimed
//                                away.
//
//   underflowFrames              A pull for more than the CodecSink holds.
//   overflowFrames               A push for more than the CodecSource can take.
//   starvedFrames                A quantum run against an empty CodecSource.
//   droppedFrames                Play quanta run past the CodecSink's capacity
//                                with nothing draining it.
//                                All four are driven ON THE BOOTED MACHINE, in
//                                the hand-off run, after every assertion that
//                                run makes.
//
// THE HAND-OFF ASSERTION INT-2 ALSO NAMES: immediately after the boot and
// before the first host block, the CodecSource holds 0 frames, the CodecSink
// holds exactly L, and all seven counters are zero. Design section 13.10.5
// declares no queue-depth accessor and this file adds none; push() returns what
// the source ACCEPTED and pull() returns what the sink SUPPLIED, so a request of
// capacity + 1 on each measures the free space and the depth exactly. That is
// t0_begin_play_phase's own technique and it is used here for the same reason.
//
// TWO BOOTS, AND THE REASON IS THAT THE PROBES MUTATE. The hand-off depth probes
// empty the sink and fill the source, so they cannot precede the golden run and
// the golden run cannot precede them -- beginPlayPhase may not be called twice.
// Each run therefore boots its own machine.
//
// EVERY VERDICT IS AN OBSERVABLE AND NOT AN assert(). A release build deletes
// assert(). Nothing below calls assert().
//
// THE MACHINE PLACEMENT IS INT-1's, AND IT IS COPIED RATHER THAN SHARED, for
// the reason t1_kernel_load.cpp states: plan section 1.3 rule 1 keeps a
// harness's own configuration at its own site, and this task's Files: line
// names no header it could share one through.

#include "gatedFixture.h"

#include "../board.h"
#include "../chainAdapter.h"
#include "../executor.h"
#include "../frame.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

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

	void checkEqual(const uint64_t _observed, const uint64_t _expected, const std::string& _what)
	{
		check(_observed == _expected,
			_what + " (observed " + std::to_string(_observed) +
			", expected " + std::to_string(_expected) + ")");
	}

	// ------------------------------------------------ the ESAI underrun log filter
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

	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;

	constexpr uint32_t g_bootQuantumBound   = 500000u;
	constexpr uint32_t g_bannerSettleQuanta = 20000u;

	// THE GOLDEN RUN'S LENGTH. It is a test parameter and not a derived
	// expectation, and it is named here so that the report can say what "the
	// whole golden run" covered: 4096 quanta is about 42.7 ms of 96 kHz audio,
	// which is long enough that a per-quantum defect appearing once in a
	// thousand frames has to show.
	constexpr unsigned g_goldenQuanta = 4096u;

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
				const uint8_t byte = uint8_t((_value >> shift) & 0xffu);

				// A CONTENT WRITE IS ONE THAT IS NOT THE DISPLAY CLEAR: the
				// clear writes 0x20 and only 0x20. Plan section 24.6 row W3-397
				// records what counting 0x20 as content cost.
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

	// ----------------------------------------------------- the seven, as a record
	//
	// ONE READING OF ALL SEVEN AT ONE INSTANT. The three per-position counters
	// are reduced to their MAXIMUM over the positions and the position that
	// carried it is kept, so a report names WHICH position moved rather than
	// that one did.
	struct Seven
	{
		uint64_t underrun            = 0;
		unsigned underrunAt          = 0;
		uint64_t secondBusUnderrun   = 0;
		unsigned secondBusUnderrunAt = 0;
		uint64_t phaseError          = 0;
		unsigned phaseErrorAt        = 0;
		uint64_t starved             = 0;
		uint64_t overflow            = 0;
		uint64_t dropped             = 0;
		uint64_t underflow           = 0;

		bool allZero() const
		{
			return underrun == 0 && secondBusUnderrun == 0 && phaseError == 0
				&& starved == 0 && overflow == 0 && dropped == 0 && underflow == 0;
		}
	};

	Seven readSeven(const g2::Scheduler& _s, const unsigned _dspCount)
	{
		Seven seven;

		for(unsigned p = 0; p < _dspCount; ++p)
		{
			const uint64_t u = _s.underrunFrames(p);
			if(u > seven.underrun)
			{
				seven.underrun   = u;
				seven.underrunAt = p;
			}

			const uint64_t s = _s.secondBusUnderrunFrames(p);
			if(s > seven.secondBusUnderrun)
			{
				seven.secondBusUnderrun   = s;
				seven.secondBusUnderrunAt = p;
			}

			const uint64_t e = _s.phaseErrorFrames(p);
			if(e > seven.phaseError)
			{
				seven.phaseError   = e;
				seven.phaseErrorAt = p;
			}
		}

		seven.starved   = _s.starvedFrames();
		seven.overflow  = _s.overflowFrames();
		seven.dropped   = _s.droppedFrames();
		seven.underflow = _s.underflowFrames();

		return seven;
	}

	void reportSeven(const Seven& _s, const std::string& _when)
	{
		std::cout << "seven[" << _when << "]:"
		          << " underrun=" << _s.underrun << "@" << _s.underrunAt
		          << " secondBusUnderrun=" << _s.secondBusUnderrun << "@" << _s.secondBusUnderrunAt
		          << " phaseError=" << _s.phaseError << "@" << _s.phaseErrorAt
		          << " starved=" << _s.starved
		          << " overflow=" << _s.overflow
		          << " dropped=" << _s.dropped
		          << " underflow=" << _s.underflow << std::endl;
	}

	void checkSevenZero(const Seven& _s, const std::string& _when)
	{
		checkEqual(_s.underrun, 0u, _when + ": underrunFrames is zero at every position, worst at position " + std::to_string(_s.underrunAt));
		checkEqual(_s.secondBusUnderrun, 0u, _when + ": secondBusUnderrunFrames is zero at every position, worst at position " + std::to_string(_s.secondBusUnderrunAt));
		checkEqual(_s.phaseError, 0u, _when + ": phaseErrorFrames is zero at every position, worst at position " + std::to_string(_s.phaseErrorAt));
		checkEqual(_s.starved, 0u, _when + ": starvedFrames is zero");
		checkEqual(_s.overflow, 0u, _when + ": overflowFrames is zero");
		checkEqual(_s.dropped, 0u, _when + ": droppedFrames is zero");
		checkEqual(_s.underflow, 0u, _when + ": underflowFrames is zero");
	}

	// ------------------------------------------------------------- the boot, once
	//
	// Owns the machine for the caller's lambda. Returns false only when the
	// machine could not be placed; a machine that ran and moved nothing returns
	// true, because that is a MEASUREMENT the assertions must see.
	struct Machine
	{
		g2::Board                            board{makeConfig()};
		Ram                                  ram{g_sdramSize};
		g2::SerialExecutor                   executor;
		g2::Scheduler::Config                config;
		std::unique_ptr<g2::Scheduler>       scheduler;

		unsigned dspCount   = 0;
		uint32_t bootQuanta = 0;
		bool     booted     = false;
		bool     landed     = false;
		bool     halted     = false;
		bool     faulted    = false;
	};

	bool placeAndBoot(const std::string& _directory, Machine& _m)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		if(!_m.ram.place(g_entryPc - g2::g_sdramBase, code))
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

			if(!_m.ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit the configured SDRAM window" << std::endl;
				return false;
			}
		}

		_m.board.memory().attach(g2::Region::Sdram, &_m.ram);
		_m.ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);

		_m.board.resetMcu(g_entrySp, g_entryPc);

		if(!_m.board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::Status schedulerStatus{};
		_m.scheduler = g2::Scheduler::create(_m.config, _m.executor, _m.board, schedulerStatus);

		if(!_m.scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		_m.dspCount = _m.board.dspSet().dspCount();

		uint32_t settle = 0;

		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			_m.bootQuanta = i + 1;

			_m.scheduler->runFrames(1);

			if(_m.board.mcuHalted())
				break;

			if(_m.ram.contentWrites() == 0)
				continue;

			if(++settle < g_bannerSettleQuanta)
				continue;

			_m.booted = true;

			unsigned landed = 0;
			for(unsigned d = 0; d < _m.dspCount; ++d)
			{
				const bool* const flag = _m.board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}

			if(landed == _m.dspCount)
			{
				_m.landed = true;
				break;
			}
		}

		_m.halted  = _m.board.mcuHalted();
		_m.faulted = _m.board.faulted();

		std::cout << "machine: bootQuanta=" << _m.bootQuanta
		          << " dspCount=" << _m.dspCount
		          << " booted=" << (_m.booted ? 1 : 0)
		          << " programsLanded=" << (_m.landed ? 1 : 0)
		          << " halted=" << (_m.halted ? 1 : 0)
		          << " faulted=" << (_m.faulted ? 1 : 0) << std::endl;

		return true;
	}

	// ------------------------------------------- the known positives, cases 1..3
	//
	// One position's two real Esai objects. t0_chain_counters' fixture, copied
	// for the reason the machine placement is: this task's Files: line names no
	// header to share one through.
	dsp56k::DefaultMemoryValidator g_memoryValidator;

	struct PositionEsai
	{
		dsp56k::PeripheralsNop periphX;
		dsp56k::PeripheralsNop periphY;
		dsp56k::Esai           audioEsai;
		dsp56k::Esai           secondEsai;

		PositionEsai()
			: audioEsai(periphX, dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{
		}
	};

	// KNOWN POSITIVE for phaseErrorFrames(position), through the ChainAdapter
	// accessor the Scheduler's own reading is computed from.
	void knownPositivePhaseError()
	{
		constexpr unsigned kPositions = 2u;
		constexpr unsigned kHop       = 1u;
		constexpr unsigned kDivider   = 4u;

		g2::ChainAdapter adapter(kPositions, kHop, g2::ChainTopology::Ring, kDivider);

		PositionEsai pos[kPositions];

		for(unsigned p = 0; p < kPositions; ++p)
			adapter.attachEsai(p, pos[p].audioEsai, pos[p].secondEsai);

		const g2::EsaiWriteTxCallback tx = adapter.audioTxCallback(0u);

		// NO UNDERRUN OUTSTANDING on this Esai, so the first delivery sets the
		// written flag. Without a set flag the second delivery could not be
		// told from the first, and this known positive would be the green
		// mirage it exists to refute. The status register is written here for
		// the same reason it always was -- to pin the peripheral's starting
		// state -- but the flag's source is Esai::txUnderrunInFrame(), not
		// M_TUE, so this Esai having transmitted nothing is what makes the
		// reading clear.
		pos[0].audioEsai.writestatusRegister(0u);

		dsp56k::Audio::TxFrame frame;
		frame.resize(8);

		uint64_t frameIndex = 0;

		tx(frameIndex, frame);

		checkEqual(adapter.phaseErrorFrames(0u), 0u,
			"KNOWN POSITIVE (phaseErrorFrames): the one delivery the scheduler asks for is not counted");

		tx(frameIndex, frame);

		checkEqual(adapter.phaseErrorFrames(0u), 1u,
			"KNOWN POSITIVE (phaseErrorFrames): a SECOND audio transmit inside one quantum raises "
			"phaseErrorFrames at that position");
		checkEqual(adapter.phaseErrorFrames(1u), 0u,
			"KNOWN POSITIVE (phaseErrorFrames): the neighbouring position is untouched, so the counter "
			"is per position and not shared");
	}

	// KNOWN POSITIVE for underrunFrames(position) and
	// secondBusUnderrunFrames(position), through the SCHEDULER accessors the
	// golden run asserts zero on.
	//
	// A FIRMWARE-FREE Board: every run gate is shut, no transmit callback ever
	// fires, and each of beginPlayPhase's L priming quanta is a real audio-bus
	// underrun at every position. t0_begin_play_phase pins the same reading.
	void knownPositiveUnderruns()
	{
		constexpr unsigned kLookahead    = 4u;
		constexpr unsigned kMaxHostBlock = 3u;

		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.lookaheadFrames    = kLookahead;
		config.maxHostBlockFrames = kMaxHostBlock;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		if(!scheduler)
		{
			check(false, "KNOWN POSITIVE (underruns): a firmware-free Scheduler could be created; g2::Status = "
				+ std::to_string(uint32_t(status)));
			return;
		}

		// Enough boot quanta that the priming run's second-bus window is
		// reached whatever the divider is; the divider itself decides which
		// quanta are windows, so the count is DERIVED from it and not typed.
		scheduler->runFrames(config.secondBusFrameDivider * kLookahead);

		scheduler->beginPlayPhase();

		const unsigned dspCount = board.dspSet().dspCount();

		const Seven seven = readSeven(*scheduler, dspCount);
		reportSeven(seven, "known-positive firmware-free");

		check(dspCount > 0, "KNOWN POSITIVE (underruns): the firmware-free board reports at least one DSP position");

		check(seven.underrun > 0,
			"KNOWN POSITIVE (underrunFrames): a firmware-free machine's priming run raises underrunFrames "
			"above zero, read through Scheduler::underrunFrames");

		check(seven.secondBusUnderrun > 0,
			"KNOWN POSITIVE (secondBusUnderrunFrames): the same priming run raises secondBusUnderrunFrames "
			"above zero, read through Scheduler::secondBusUnderrunFrames");

		// EVERY position, not merely the worst one: a counter that moved at one
		// position and nowhere else would satisfy the maximum above while
		// leaving seven of the eight readings unproven.
		unsigned movedUnderrun = 0;
		unsigned movedSecond   = 0;

		for(unsigned p = 0; p < dspCount; ++p)
		{
			if(scheduler->underrunFrames(p) > 0)
				++movedUnderrun;
			if(scheduler->secondBusUnderrunFrames(p) > 0)
				++movedSecond;
		}

		checkEqual(movedUnderrun, dspCount,
			"KNOWN POSITIVE (underrunFrames): EVERY position's counter moved, not just one");
		checkEqual(movedSecond, dspCount,
			"KNOWN POSITIVE (secondBusUnderrunFrames): EVERY position's counter moved, not just one");
	}
}

int main()
{
	installLogFilter();

	g2::EnvArtifactResolver  resolver;
	g2::test::GatedCounters  counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		std::string why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << "FAIL " << why << std::endl;
			return false;
		}

		// ---------------------------------------------------------------------
		// PART A. The known positives that need no firmware. They run FIRST, so
		// that a run whose seven zeros are reported by counters nothing can move
		// fails HERE and not on the zeros.
		std::cout << "--- part A: known positives" << std::endl;

		knownPositivePhaseError();
		knownPositiveUnderruns();

		// ---------------------------------------------------------------------
		// PART B. The hand-off, on a booted machine, and the four queue counters'
		// known positives -- which are taken AFTER every assertion this run makes,
		// because each of them mutates a queue.
		std::cout << "--- part B: the hand-off on a booted machine" << std::endl;

		{
			Machine m;

			if(!placeAndBoot(directory, m))
				return false;

			check(m.booted, "part B: the firmware composed display content, so the machine really booted");
			check(m.landed, "part B: every DSP position took its program before the play phase began");
			check(!m.halted, "part B: the core is not halted at the play transition");
			check(!m.faulted, "part B: the board reports no fault at the play transition");

			m.scheduler->beginPlayPhase();

			const Seven handOff = readSeven(*m.scheduler, m.dspCount);
			reportSeven(handOff, "hand-off");
			checkSevenZero(handOff, "hand-off");

			// THE TWO QUEUE DEPTHS, THROUGH THE DECLARED SURFACE. Capacity is
			// L + B and both numbers come off the Config this run handed the
			// factory, so neither is typed here.
			const size_t capacity = size_t(m.config.lookaheadFrames) + size_t(m.config.maxHostBlockFrames);

			{
				std::vector<g2::Frame> out(capacity + 1);
				const size_t pulled = m.scheduler->pull(out.data(), capacity + 1);

				checkEqual(pulled, m.config.lookaheadFrames,
					"part B: the CodecSink holds EXACTLY lookaheadFrames frames at the hand-off");

				checkEqual(m.scheduler->underflowFrames(), uint64_t(capacity) + 1u - m.config.lookaheadFrames,
					"KNOWN POSITIVE (underflowFrames): a pull the sink could not satisfy raises "
					"underflowFrames by the shortfall");
			}

			{
				const std::vector<g2::Frame> in(capacity + 1);
				const size_t pushed = m.scheduler->push(in.data(), capacity + 1);

				checkEqual(pushed, capacity,
					"part B: the CodecSource is EMPTY at the hand-off, so it accepts its whole capacity");

				checkEqual(m.scheduler->overflowFrames(), 1u,
					"KNOWN POSITIVE (overflowFrames): the one frame past capacity was refused and counted");
			}

			{
				// Drain the source one quantum at a time, pulling after each so
				// the sink cannot fill and steal the finding. The quantum after
				// the last queued frame is the one that starves.
				for(size_t q = 0; q < capacity; ++q)
				{
					m.scheduler->runFrames(1);
					g2::Frame out{};
					(void) m.scheduler->pull(&out, 1);
				}

				const uint64_t before = m.scheduler->starvedFrames();

				m.scheduler->runFrames(1);

				check(m.scheduler->starvedFrames() > before,
					"KNOWN POSITIVE (starvedFrames): a quantum run against an empty CodecSource raises "
					"starvedFrames");
			}

			{
				// Nothing drains the sink now, so it fills at capacity and the
				// next egress push is refused and counted.
				const uint64_t before = m.scheduler->droppedFrames();

				for(size_t q = 0; q < capacity + 2u; ++q)
					m.scheduler->runFrames(1);

				check(m.scheduler->droppedFrames() > before,
					"KNOWN POSITIVE (droppedFrames): play quanta run past the CodecSink's capacity with "
					"nothing draining it raise droppedFrames");
			}
		}

		// ---------------------------------------------------------------------
		// PART C. THE GOLDEN RUN. One frame in and one frame out for each
		// quantum, and all seven counters zero across the WHOLE of it -- checked
		// after every quantum, so the report names the FIRST quantum at which
		// any of them moved rather than only the end state.
		std::cout << "--- part C: the golden run" << std::endl;

		{
			Machine m;

			if(!placeAndBoot(directory, m))
				return false;

			check(m.booted, "part C: the firmware composed display content, so the machine really booted");
			check(m.landed, "part C: every DSP position took its program before the play phase began");

			m.scheduler->beginPlayPhase();

			const Seven handOff = readSeven(*m.scheduler, m.dspCount);
			reportSeven(handOff, "part C hand-off");

			int   firstMove  = -1;
			Seven atFirstMove;

			const g2::Frame silence{};

			for(unsigned q = 0; q < g_goldenQuanta; ++q)
			{
				(void) m.scheduler->push(&silence, 1);
				m.scheduler->runFrames(1);

				g2::Frame out{};
				(void) m.scheduler->pull(&out, 1);

				if(firstMove >= 0)
					continue;

				const Seven now = readSeven(*m.scheduler, m.dspCount);

				if(!now.allZero())
				{
					firstMove   = int(q);
					atFirstMove = now;
				}
			}

			const Seven atEnd = readSeven(*m.scheduler, m.dspCount);
			reportSeven(atEnd, "golden-run end");

			std::cout << "golden: quanta=" << g_goldenQuanta
			          << " firstNonZeroQuantum=" << firstMove << std::endl;

			if(firstMove >= 0)
			{
				reportSeven(atFirstMove, "golden-run first non-zero");
				check(false, "the golden run's seven counters stayed zero: the first non-zero reading was at "
					"quantum " + std::to_string(firstMove));
			}

			checkSevenZero(atEnd, "golden-run end");

			check(!m.scheduler->faulted(),
				"the golden run left no context faulted");
		}

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
