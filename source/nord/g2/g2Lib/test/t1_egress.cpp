// This test needs the Clavia firmware artifacts and skips with a reason when
// NMG2_ARTIFACTS does not resolve.
//
// A known pattern is injected at the codec source of a machine whose audio path
// is really up, and the frame index at which it reappears at the codec sink is
// compared against the delay the chain's own geometry predicts.
//
// "Really up" is the ESAI receive DMA request armed on every chain position, and
// not the display banner and not programLanded. The boot section below states
// why, and a run reports how far apart the three are.
//
// ---------------------------------------------------------------------------
// THE ARRIVAL ASSERTIONS AT THE FOOT OF THIS FILE DO NOT HOLD ON THIS FIRMWARE,
// and two plausible explanations for that have been measured and ruled out. They
// are recorded here so that the next reader spends the effort somewhere new.
//
//   Not the boot predicate. The exit below is the receive DMA armed on every
//   position, which is the predicate g2TestConsole's `--impulse` and
//   t1_chain_health hold and the one this file used to lack. Adopting it moved
//   nothing: a run reports the arming already complete at the quantum the event
//   loop first runs, so the gate this file already had subsumed it.
//
//   Not the walk bound. The walk is D_chain + 1024 quanta and the sustained
//   probe drives the impulse for 2048 more. Nothing arrives in either. Lengthening
//   the window is therefore not the fix, and a window lengthened until an
//   assertion passes would be measuring something other than D_chain.
//
// What a passing arrival would need is a chain that carries codec audio at all,
// and no measurement in this repository has yet observed one. t1_patch_running
// runs this same walk twice, once unpatched and once with a real `.pch2` the
// Board's own hub accepted, and reports arrival=-1 with zero non-zero frames on
// BOTH -- while its sink control passes on both, so the reporting path is sound.
// That file REPORTS the figure where this one ASSERTS it, and it names this file
// when it says arrival=-1 on an unpatched machine is not a claim about a patched
// one. t0_impulse_outcome states the reason plainly: a Nord Modular does not make
// sound until a patch routes it, so a silent chain can be the emulator agreeing
// with the hardware.
//
// So these assertions are aspirational rather than regressive: they state where
// the audio path is going, and they are the only place that states it. Deciding
// between making them hold and restating this file's scope is a decision about
// the emulator, not about this test, and it is not taken here.
// ---------------------------------------------------------------------------
//
// Where the expected delay comes from, and why it is not a literal. The audio bus
// is a Line of dspCount + 1 mailboxes. The ingress phase writes mailbox 0's read
// frame and the egress phase reads the last mailbox's write frame, so neither
// codec edge carries a delay of its own: D_codec is 0. Every DSP-to-DSP hand-off
// costs one mailbox hop of `hopFrames` quanta, and a chain of dspCount positions
// has dspCount - 1 of them. So
//
//     D_chain = (dspCount - 1) * hopFrames
//
// with dspCount read off the booted machine (Board::dspSet().dspCount()) and
// hopFrames read off the Scheduler::Config this file hands to the factory.
// Neither number is typed here.
//
// Every verdict is an observable and not an assert(): a release build deletes
// assert(), so a predicate spelled as one is a predicate the shipped build does
// not have.

#include "gatedFixture.h"
#include "rxArmed.h"

#include "../board.h"
#include "../executor.h"
#include "../frame.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

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

	void checkEqual(const uint64_t _observed, const uint64_t _expected, const std::string& _what)
	{
		check(_observed == _expected,
			_what + " (observed " + std::to_string(_observed) +
			", expected " + std::to_string(_expected) + ")");
	}

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// The underruns are real and expected in the boot regime, because nothing
	// drains the ESAIs until the codec queues arrive. This hides the repetition
	// and nothing else. Set G2_LOG_ESAI_UNDERRUN to install no filter at all.
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

	// ------------------------------------------------------- the machine placement

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

	// The watched display cells. Reaching them is what says the firmware booted
	// far enough to be running its own code rather than sitting in reset; the
	// drive below leaves on that plus every DSP program having landed.
	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;

	// The bound the drive gives itself, in quanta. The boot needs hundreds of
	// thousands of frames.
	constexpr uint32_t g_bootQuantumBound = 500000u;

	// The settle window: the banner is composed a character at a
	// time, so leaving on the first content byte samples a machine mid-write.
	constexpr uint32_t g_bannerSettleQuanta = 20000u;

	// The firmware's event loop. Reaching it is what this file means by booted.
	//
	// The banner and its settle count are not that. They say a character other
	// than the display clear reached the watched cells and that the machine kept
	// running afterwards, which the machine does while it is still initialising:
	// it leaves a long initialisation wait later still, and the event loop does
	// not run until later than that. A predicate keyed on the banner is true on a
	// machine that cannot yet consume anything, and every measurement taken
	// behind it reads not-yet as never.
	//
	// The event loop is the condition worth keying on because it is what every
	// consumer here depends on. The address is the one the findings corpus names,
	// and the reading is a 16-bit read at it, which is the width the core fetches
	// an instruction word at. Both quanta are recorded, so a run says how far
	// apart they are rather than only which one it used.
	constexpr uint32_t g_eventLoopEntry = 0x30004674u;

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

			if(_size == 16 && m_fetchWatchSet && _offset == m_fetchWatch)
				++m_fetchesAtWatch;

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

				// A content write is one that is not the display clear. The
				// clear writes 0x20 and only 0x20, so a byte other than 0x20
				// inside the watched run is the firmware composing something.
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

		// The event loop's own fetch counter. One address, counted on the width
		// the core fetches an instruction at, so that "booted" can mean the loop
		// ran rather than that a banner appeared.
		void watchFetch(const uint32_t _offset) { m_fetchWatch = _offset; m_fetchWatchSet = true; }

		uint64_t fetchesAtWatch() const { return m_fetchesAtWatch; }

	private:
		std::vector<uint8_t> m_bytes;
		uint32_t             m_watchBase    = 0;
		uint32_t             m_watchLength  = 0;
		uint64_t             m_contentWrites = 0;
		uint32_t             m_fetchWatch    = 0;
		bool                 m_fetchWatchSet = false;
		uint64_t             m_fetchesAtWatch = 0;
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
	// Two different non-zero Q23 values, one for each codec slot. They differ
	// from each other so that a chain that carried slot 0 into both slots is a
	// failure here rather than a pass, and neither is a power of two, so a
	// value the chain shifted or masked does not land back on itself.
	constexpr int32_t g_impulseLeft  = 0x0055AA33;
	constexpr int32_t g_impulseRight = 0x00337799;

	// How far past the predicted arrival the walk keeps looking. "Arrived late"
	// and "never arrived" are different findings and a walk that stopped at the
	// prediction could not tell them apart, so the walk runs far past it: 1024
	// quanta is about 10.7 ms at 96 kHz.
	constexpr unsigned g_overrunQuanta = 1024u;

	// The sustained probe. It runs only when the impulse never arrived, and it
	// asserts nothing: its job is to separate "the chain delays by more than the
	// walk" from "the chain carries no codec audio at all", which are different
	// findings about the machine and must not be reported as one.
	constexpr unsigned g_sustainedQuanta = 2048u;

	/* ------------------------------- the arrival instrument's known positive
	 *
	 * `arrival >= 0` failing says the pattern did not appear at the sink; it
	 * does not say whether the chain declined to carry it or whether the
	 * arrival path could not have reported it either way.
	 *
	 * The control does. It places a sentinel at the tail position's transmit
	 * source and reads it back out of the codec sink through the same `pull`
	 * and the same comparator the walk uses, so a failing `arrival` assertion
	 * beside a passing control is a statement about the chain and not about the
	 * instrument. */
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
	 * is the index of the single line pulled down. On this machine position 7
	 * is port 0. */
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

	struct EgressResult
	{
		bool     placed          = false;
		bool     schedulerBuilt  = false;
		unsigned dspCount        = 0;
		unsigned hopFrames       = 0;
		unsigned lookaheadFrames = 0;
		uint32_t bootQuanta      = 0;
		bool     booted          = false;   // the event loop ran

		// The quantum at which the banner-and-settle predicate this file used to
		// boot on became true, and the event loop's fetch count at that instant.
		// The second is the known negative for the predicate that replaced it: a
		// machine that satisfied the old one had not run the event loop, so the
		// count must read 0 there. It is taken on the same run that later reads a
		// positive, so it separates "the loop had not run yet" from "the counter
		// cannot see the loop at all".
		uint32_t bannerQuanta          = 0;
		uint64_t eventLoopHitsAtBanner = 0;
		uint32_t eventLoopQuanta       = 0;
		bool     programsLanded  = false;

		// The quantum at which every DSP position first reported programLanded,
		// and the arming count read at exactly that instant.
		//
		// Recorded and not asserted. The gap this pair was added to look for is
		// the one g2TestConsole's `--impulse` documents between landing and
		// arming, and on this firmware it does not survive the event-loop gate
		// this file already had: both instants land on the same quantum with
		// every position armed. Asserting a gap would be asserting a hypothesis
		// about a machine other than the one under the test. The pair stays
		// because the next reader's first question is what the distance is, and
		// a run answers it instead of being re-instrumented.
		uint32_t programsLandedQuanta = 0;
		unsigned rxArmedAtLanded      = 0;

		// The exit predicate: the ESAI receive DMA request registered on every
		// position. rxArmedPorts is read after the drive so a run that never
		// reached the poll still reports a measured number.
		bool     rxArmed         = false;
		unsigned rxArmedPorts    = 0;
		uint32_t rxArmedQuanta   = 0;
		bool     halted          = false;
		bool     faulted         = false;

		size_t   primedPulled    = 0;       // frames the sink held at hand-off
		unsigned walkQuanta      = 0;

		// The walk. One entry for each quantum after the injection quantum.
		std::vector<g2::Frame> pulled;

		// -1 when the pattern never appeared.
		int      arrival         = -1;
		bool     arrivalExact    = false;   // both slots matched, bit for bit

		// Every frame the walk fed in was accepted by the CodecSource and every
		// quantum of the walk consumed one. Without these two the report could
		// not tell "the chain dropped it" from "it never went in".
		size_t   pushedShort     = 0;
		size_t   pulledShort     = 0;
		uint64_t starvedAfter    = 0;
		uint64_t overflowAfter   = 0;

		// The sustained probe. sustainedRan says it happened at all.
		bool     sustainedRan    = false;
		int      sustainedFirst  = -1;

		// The arrival instrument's known positive, run last so it cannot move
		// the measurement it qualifies.
		unsigned sinkControlPort      = 0;
		bool     sinkControlPortFound = false;
		unsigned sinkControlQuanta    = 0;
		int      sinkControlArrival   = -1;
		bool     sinkControlExact     = false;
		int32_t  sinkControlL         = 0;
		int32_t  sinkControlR         = 0;
	};

	// Runs the whole thing on one booted machine. Returns false only when the
	// machine could not be placed at all; a machine that ran and moved nothing
	// returns true with a result that says so, because "the chain is silent" is
	// a measurement and must reach the assertions rather than a bail-out.
	bool runEgress(const std::string& _directory, EgressResult& _result)
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

		// Installed before the core runs, so every count is the firmware's.
		ram.watchCells(g_displayBase - g2::g_sdramBase, g_lineWidth);
		ram.watchFetch(g_eventLoopEntry - g2::g_sdramBase);

		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		_result.placed = true;

		g2::SerialExecutor            executor;
		g2::Status                    schedulerStatus{};
		const g2::Scheduler::Config   config;

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		_result.schedulerBuilt  = true;
		_result.dspCount        = board.dspSet().dspCount();
		_result.hopFrames       = config.hopFrames;
		_result.lookaheadFrames = config.lookaheadFrames;

		// ---------------------------------------------------------- the boot
		//
		// The drive leaves on a property of the AUDIO PATH, and not on program
		// loading: the ESAI receive DMA request registered on every position.
		//
		// Why not programLanded, which this file used to exit on. Landing is a
		// fact about the kernel download, and the arming code is not even
		// resident when the boot-time DMA configuration runs -- it arrives in a
		// later-loaded DSP program -- so on a machine where the two come apart, a
		// drive leaving on landing hands beginPlayPhase a receive path that is
		// still dead, and the silence the walk then measures is a statement about
		// transport not yet existing rather than about the chain. That is the gap
		// g2TestConsole's `--impulse` documents and holds this same predicate
		// against.
		//
		// It is asserted here even though a run shows the event-loop gate this
		// file already had arriving no earlier: the exit should name the property
		// the measurement depends on rather than one that happens to imply it, so
		// that a firmware or a gate whose order differs is caught by the drive
		// instead of read as a silent chain.
		//
		// The event loop gate stays: it is a precondition for polling, because a
		// machine still in reset arms nothing. Landing is likewise a
		// precondition and no longer the exit.
		uint32_t settle = 0;

		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			_result.bootQuanta = i + 1;

			scheduler->runFrames(1);

			if(board.mcuHalted())
				break;

			// The old predicate, recorded rather than acted on. Its first firing
			// is the instant a machine stopped early would have been called
			// booted, and the event loop's count is read at exactly that instant.
			if(_result.bannerQuanta == 0 && ram.contentWrites() != 0 && ++settle >= g_bannerSettleQuanta)
			{
				_result.bannerQuanta          = i + 1;
				_result.eventLoopHitsAtBanner = ram.fetchesAtWatch();
			}

			if(ram.fetchesAtWatch() == 0)
				continue;

			if(!_result.booted)
				_result.eventLoopQuanta = i + 1;

			_result.booted = true;

			unsigned landed = 0;
			for(unsigned d = 0; d < _result.dspCount; ++d)
			{
				const bool* const flag = board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}

			if(landed == _result.dspCount && !_result.programsLanded)
			{
				_result.programsLanded      = true;
				_result.programsLandedQuanta = i + 1;
				_result.rxArmedAtLanded      = g2test::countRxArmed(board, _result.dspCount);
			}

			if(!_result.programsLanded)
				continue;

			if(g2test::countRxArmed(board, _result.dspCount) == _result.dspCount)
			{
				_result.rxArmed       = true;
				_result.rxArmedQuanta = i + 1;
				break;
			}
		}

		_result.rxArmedPorts = g2test::countRxArmed(board, _result.dspCount);
		_result.halted  = board.mcuHalted();
		_result.faulted = board.faulted();

		// ------------------------------------------------- the play transition
		scheduler->beginPlayPhase();

		// The primed frames are taken off the sink before the walk begins, so
		// that the walk's own index is measured from the injection quantum and
		// not from the lookahead. beginPlayPhase leaves exactly L frames there
		// and the pull is for exactly L, so a sink holding fewer would raise
		// underflowFrames and a sink holding more would shift every arrival.
		{
			std::vector<g2::Frame> primed(_result.lookaheadFrames);
			_result.primedPulled = scheduler->pull(primed.data(), primed.size());
		}

		// ------------------------------------------------------------ the walk
		//
		// One frame in and one frame out for each quantum. The pattern goes in
		// on the first quantum of the walk and silence on every one after it, so
		// the index of the pulled frame that carries the pattern is the delay in
		// frames, measured from the injection.
		const unsigned expected = (_result.dspCount > 0 ? _result.dspCount - 1u : 0u) * _result.hopFrames;
		const unsigned walk     = expected + g_overrunQuanta;

		g2::Frame impulse{};
		impulse.slot[0] = g_impulseLeft;
		impulse.slot[1] = g_impulseRight;

		const g2::Frame silence{};

		for(unsigned q = 0; q < walk; ++q)
		{
			const g2::Frame& in = (q == 0) ? impulse : silence;

			if(scheduler->push(&in, 1) != 1)
				++_result.pushedShort;

			scheduler->runFrames(1);

			g2::Frame out{};

			if(scheduler->pull(&out, 1) != 1)
				++_result.pulledShort;

			// Only the first few frames are kept for the report. The walk is
			// long and a full dump would bury the line a reader needs.
			if(_result.pulled.size() < size_t(expected) + 8u)
				_result.pulled.push_back(out);

			if(_result.arrival < 0 && (out.slot[0] != 0 || out.slot[1] != 0))
			{
				_result.arrival = int(q);
				_result.arrivalExact =
					out.slot[0] == g_impulseLeft && out.slot[1] == g_impulseRight;
			}
		}

		_result.walkQuanta    = walk;
		_result.starvedAfter  = scheduler->starvedFrames();
		_result.overflowAfter = scheduler->overflowFrames();

		// ------------------------------------------------------ the sustained probe
		if(_result.arrival < 0)
		{
			_result.sustainedRan = true;

			for(unsigned q = 0; q < g_sustainedQuanta; ++q)
			{
				(void) scheduler->push(&impulse, 1);
				scheduler->runFrames(1);

				g2::Frame out{};
				(void) scheduler->pull(&out, 1);

				if(_result.sustainedFirst < 0 && (out.slot[0] != 0 || out.slot[1] != 0))
					_result.sustainedFirst = int(q);
			}
		}

		// ---------------------------- the arrival instrument's known positive
		//
		// The links it traverses: the tail DSP's X memory, its transmit DMA,
		// the ESAI transmit register file, ESAI frame assembly, the installed
		// WriteTxCallback (ChainAdapter::audioTxCallback(N-1)), fromEsaiFrame,
		// mailbox N, ChainAdapter::advanceAll, extractCodecSink,
		// CodecSink::push, Scheduler::pull and the walk's own two predicates.
		//
		// The links it does not: no DSP core executes any part of it, and
		// positions 0..N-2, every receive callback, the mailbox hop chain and
		// injectCodecSource are all upstream of the tail. It qualifies the
		// arrival reporting path and makes no claim about the chain.
		{
			const unsigned tailPort =
				portOfChainPosition(board, _result.dspCount - 1u, _result.dspCount);

			_result.sinkControlPortFound = tailPort < _result.dspCount;
			_result.sinkControlPort      = _result.sinkControlPortFound ? tailPort : 0u;

			if(_result.sinkControlPortFound)
			{
				dsp56k::Peripherals56311& p = board.dspSet().peripherals(_result.sinkControlPort);
				dsp56k::Esai&             tailEsai = p.getEsai();

				for(unsigned q = 0; q < g_sinkControlQuanta && _result.sinkControlArrival < 0; ++q)
				{
					++_result.sinkControlQuanta;

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

						dsp56k::Memory& tailMemory =
							board.dspSet().dsp(_result.sinkControlPort).memory();

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

					_result.sinkControlArrival = int(q);
					_result.sinkControlL       = out.slot[0];
					_result.sinkControlR       = out.slot[1];
					_result.sinkControlExact   = out.slot[0] == g_sinkControlExpected
						&& out.slot[1] == g_sinkControlExpected;
				}
			}
		}

		return true;
	}

	void report(const EgressResult& _r, const unsigned _expected)
	{
		std::cout << "egress: dspCount=" << _r.dspCount
		          << " hopFrames=" << _r.hopFrames
		          << " lookaheadFrames=" << _r.lookaheadFrames
		          << " D_chain=" << _expected
		          << " D_codec=0" << std::endl;
		std::cout << "egress: bootQuanta=" << _r.bootQuanta
		          << " booted=" << (_r.booted ? 1 : 0)
		          << " programsLanded=" << (_r.programsLanded ? 1 : 0)
		          << " rxArmed=" << (_r.rxArmed ? 1 : 0)
		          << " rxArmedPorts=" << _r.rxArmedPorts << "/" << _r.dspCount
		          << " halted=" << (_r.halted ? 1 : 0)
		          << " faulted=" << (_r.faulted ? 1 : 0) << std::endl;
		std::cout << "egress: programsLandedQuanta=" << _r.programsLandedQuanta
		          << " rxArmedAtLanded=" << _r.rxArmedAtLanded << "/" << _r.dspCount
		          << " rxArmedQuanta=" << _r.rxArmedQuanta << std::endl;
		std::cout << "egress: eventLoopQuanta=" << _r.eventLoopQuanta
		          << " at 0x" << std::hex << g_eventLoopEntry << std::dec
		          << "; the banner predicate fired at " << _r.bannerQuanta
		          << " with " << _r.eventLoopHitsAtBanner
		          << " reads of the event loop by then" << std::endl;
		std::cout << "egress: primedPulled=" << _r.primedPulled
		          << " walkQuanta=" << _r.walkQuanta
		          << " arrival=" << _r.arrival
		          << " arrivalExact=" << (_r.arrivalExact ? 1 : 0) << std::endl;
		std::cout << "egress: pushedShort=" << _r.pushedShort
		          << " pulledShort=" << _r.pulledShort
		          << " starvedAfter=" << _r.starvedAfter
		          << " overflowAfter=" << _r.overflowAfter << std::endl;
		std::cout << "egress: sustainedRan=" << (_r.sustainedRan ? 1 : 0)
		          << " sustainedFirst=" << _r.sustainedFirst << std::endl;
		std::cout << "egress: sinkControl tailPosition="
		          << (_r.dspCount > 0 ? _r.dspCount - 1u : 0u)
		          << " tailPort=" << (_r.sinkControlPortFound ? int(_r.sinkControlPort) : -1)
		          << " controlQuanta=" << _r.sinkControlQuanta
		          << " sinkControlArrival=" << _r.sinkControlArrival
		          << " sinkControlExact=" << (_r.sinkControlExact ? 1 : 0)
		          << " sinkControlValue=" << _r.sinkControlL << "/" << _r.sinkControlR
		          << std::endl;

		std::cout << "egress: head of walk =";
		for(size_t i = 0; i < _r.pulled.size(); ++i)
			std::cout << " [" << i << "]=" << _r.pulled[i].slot[0]
			          << "/" << _r.pulled[i].slot[1];
		std::cout << std::endl;
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

		EgressResult result;

		if(!runEgress(directory, result))
			return false;

		// D_chain, derived. dspCount is read off the booted machine and
		// hopFrames off the Config the factory accepted; no arrival figure is
		// typed into this file.
		const unsigned expected =
			(result.dspCount > 0 ? result.dspCount - 1u : 0u) * result.hopFrames;

		report(result, expected);

		// The preconditions of the measurement, asserted before the measurement
		// so that a silent chain on a machine that never booted is reported as
		// the machine's failure and not as the chain's.
		check(result.schedulerBuilt, "the Scheduler was created");
		check(result.dspCount > 0, "the booted machine reports at least one DSP position");
		check(result.hopFrames > 0, "the Scheduler Config carries a non-zero hop");

		// ------------------------------------------- the boot predicate's floor
		//
		// The machine ran the event loop, and it had NOT run it at the instant the
		// banner predicate this file used to boot on became true. The second half
		// is the one that matters: it is this run's own early-stopped machine,
		// measured with the same counter that later reads a positive, so a zero
		// there is the loop not yet reached and not a counter that cannot see it.
		check(result.booted,
			"egress: the machine ran the event loop within the boot bound");
		check(result.bannerQuanta != 0,
			"egress: the banner predicate fired at some quantum, so the reading below was "
			"taken and is not a field that was never written");
		check(result.eventLoopHitsAtBanner == 0,
			"egress: the event loop had not run when the banner predicate fired; observed "
			+ std::to_string(result.eventLoopHitsAtBanner) + " reads at quantum "
			+ std::to_string(result.bannerQuanta));
		check(result.eventLoopQuanta > result.bannerQuanta,
			"egress: the event loop ran later than the banner predicate fired; banner at "
			+ std::to_string(result.bannerQuanta) + ", event loop at "
			+ std::to_string(result.eventLoopQuanta));
		check(result.programsLanded, "every DSP position took its program before the play phase began");

		// ------------------------------------------ the audio path's own arming
		//
		// The receive path is up at the play transition. This is the precondition
		// the walk below depends on and the one the old exit did not name, so a
		// walk that measures silence cannot be a walk against a machine with no
		// transport. The count is a positive reading of the same accessor the
		// drive polled, so it is not a zero standing on its own.
		check(result.rxArmed,
			"egress: the ESAI receive DMA request is armed on every position within the boot bound; observed "
			+ std::to_string(result.rxArmedPorts) + " of " + std::to_string(result.dspCount));
		check(!result.halted, "the core is not halted at the play transition");
		check(!result.faulted, "the board reports no fault at the play transition");

		check(result.primedPulled == size_t(result.lookaheadFrames),
			"beginPlayPhase left EXACTLY lookaheadFrames frames on the CodecSink: pulled "
			+ std::to_string(result.primedPulled) + ", lookaheadFrames is "
			+ std::to_string(result.lookaheadFrames));

		// Separate assertions, because "never arrived", "arrived at the wrong
		// frame" and "arrived changed" are different findings and one combined
		// predicate would report them as one. The push and pull checks say every
		// frame the walk fed in really entered the ingress phase; without them,
		// "the pattern never arrived" could not be told from "the pattern was
		// never injected".
		checkEqual(result.pushedShort, 0u,
			"every walk frame was accepted by the CodecSource");
		checkEqual(result.pulledShort, 0u,
			"every walk quantum supplied a frame at the CodecSink");
		checkEqual(result.starvedAfter, 0u,
			"no walk quantum ran against an empty CodecSource, so every injected frame was consumed by an ingress phase");
		checkEqual(result.overflowAfter, 0u,
			"no walk frame was refused by the CodecSource");

		// ---------------- the arrival instrument's known positive
		//
		// It runs before the arrival checks because it qualifies them. A
		// failing `arrival >= 0` beside a passing control is a statement about
		// the chain; the same failure beside a failing control is a statement
		// about nothing.
		check(result.sinkControlPortFound,
			"the firmware's port table names a port at chain position "
			+ std::to_string(result.dspCount - 1u));
		check(result.sinkControlArrival == 0,
			"the sink control arrived on the first control quantum: the tail writes mailbox N in the "
			"same quantum the egress phase reads it; observed "
			+ std::to_string(result.sinkControlArrival));
		check(result.sinkControlExact,
			"the sink control arrived unchanged in BOTH codec slots, so the arrival path can report a "
			"frame it was handed; observed " + std::to_string(result.sinkControlL) + "/"
			+ std::to_string(result.sinkControlR) + " against "
			+ std::to_string(g_sinkControlExpected));

		check(result.arrival >= 0,
			"the injected pattern reached the codec sink at all within "
			+ std::to_string(result.walkQuanta) + " quanta");

		check(result.arrival == int(expected),
			"the injected pattern arrived at EXACTLY D_chain + D_codec = "
			+ std::to_string(expected) + " frames; observed arrival frame "
			+ std::to_string(result.arrival));

		check(result.arrivalExact,
			"the frame that arrived carries the injected pattern bit for bit in both codec slots");

		return g_failures == 0;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
