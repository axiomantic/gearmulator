/* t1_boot_on_restore.cpp -- the boot order, cold and on restore. Tier T1: this
 * test boots the real Clavia firmware and skips with a reason when
 * NMG2_ARTIFACTS does not resolve.
 *
 * The six boot steps are an order, and the order is the deliverable:
 *
 *   1 Scheduler::create   2 Scheduler::reset   3 Scheduler::stateLoad (restore
 *   only)   4 many runFrames   5 beginPlayPhase   6 the release store of true
 *   into m_ready
 *
 * Four properties are checked, and each is checked through a different
 * observable so that no two can pass for the same reason:
 *
 *   A. The order itself, through Device::IBootObserver, the boot thread's twin
 *      of the ISchedulerDriver seam left for the audio thread. A cold boot
 *      emits five steps and not StateLoad; a restore emits six with StateLoad
 *      strictly before RunFrames.
 *
 *   B. Reset does not prime. Step 2 leaves both codec queues empty; priming the
 *      sink with L frames is step 5's job. The probe is a pull() at the moment
 *      the Reset notification arrives, read through the declared surface --
 *      pull() returns the frames actually taken.
 *
 *   C. The boot codec regime touches neither queue. A Scheduler is born in the
 *      boot regime and beginPlayPhase is the only exit from it, so step 4's
 *      many quanta must leave all four codec counters at zero: starved,
 *      overflow, dropped and underflow. Were the play regime running during the
 *      boot, the sink would fill, push would refuse, and the boot would stall
 *      part-way through with no callback that could ever drain it.
 *
 *   D. The restore path loads before it runs. Step 3 precedes step 4, which is
 *      what makes a restored machine run from the restored state rather than
 *      over it.
 *
 * The two controls are known positives from the same population, and they run
 * before the boot so that a reader meets the proof that the predicates
 * discriminate before meeting the numbers they produced:
 *
 *   Control 1 -- a reordered sequence. The same predicate that accepts the
 *   observed order is handed the order with two steps swapped, and must refuse
 *   it.
 *
 *   Control 2 -- a primed sink. The not-priming predicate is handed a Scheduler
 *   that has been primed -- by beginPlayPhase, which is the one thing in the
 *   system that primes -- and must report it as primed. The control reaches the
 *   primed state through the real code path and not by writing a flag, so it is
 *   a known positive drawn from the population the check runs on.
 *
 * Why the controls need no firmware. Both construct their own Scheduler over a
 * bare Board. They are therefore ungated and they run even on a machine with no
 * artifacts, which is what keeps a skipped run from also losing the evidence
 * that the predicates work.
 */

#include "gatedFixture.h"

#include "../board.h"
#include "../executor.h"
#include "../frame.h"
#include "../scheduler.h"
#include "../status.h"

#include "../../g2JucePlugin/g2Device.h"

#include "dsp56kBase/logging.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;

		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}

		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	// ------------------------------------------------ the ESAI underrun log filter
	//
	// dsp56kEmu's Esai::writeSlotToFrame calls LOG() once per transmit slot whose
	// data was never written, and this boot turns very many Scheduler frames. The
	// filter hides the repetition of a real and expected condition and nothing
	// else; the count is reported rather than discarded, so "the log was
	// silenced" stays a statement about volume and not about evidence. Set
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

	// ------------------------------------------------ the configuration
	//
	// L is deliberately greater than one. The not-priming check asks whether the
	// sink holds L frames after step 2, and at L = 1 -- the Config default, which
	// scheduler.h states is the smallest legal value and not the shipped one -- a
	// primed sink and an empty one differ by a single frame.
	constexpr unsigned g_lookaheadFrames    = 4;
	constexpr unsigned g_maxHostBlockFrames = 64;

	g2::Scheduler::Config makeConfig()
	{
		g2::Scheduler::Config config;

		config.lookaheadFrames    = g_lookaheadFrames;
		config.maxHostBlockFrames = g_maxHostBlockFrames;

		return config;
	}

	// The boot budget for the one boot that is meant to reach the running
	// machine. The real boot is measured to need roughly 425,000 frames to reach
	// the patch browser, so this is a ceiling above that and not a prediction of
	// where the boot ends: the boot leaves on Scheduler::chainAttached(), the
	// machine's own signal.
	constexpr uint64_t g_fullBootBudget = 500000;

	// The budget for the boots whose subject is the order of the steps and not
	// the booted machine. The order is complete after step 6 whatever step 4 ran,
	// so these do not pay for a full boot.
	constexpr uint64_t g_orderBootBudget = 4096;

	// ------------------------------------------------ the display, for evidence
	//
	// The display buffer base is 0x302A0DB8 and a line is sixteen cells. The
	// clear byte is 0x20, and it is named because a "the firmware composed
	// something" predicate must refuse to be satisfied by it.
	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;
	constexpr uint8_t  g_clearByte   = 0x20u;
	constexpr int      g_byte        = 1;

	// ------------------------------------------------ the recorder
	//
	// It records the order and, at each step, the two things that make the order
	// meaningful: whether the machine was published yet, and what the Scheduler
	// looked like. Nothing here drives the boot.
	struct StepRecord
	{
		g2::Device::BootStep step;
		bool                 ready;          // Device::isValid() at that moment
		uint64_t             frameIndex;
		uint64_t             starved;
		uint64_t             overflow;
		uint64_t             dropped;
		uint64_t             underflow;
		size_t               primedFramesSeen;  // probe result; only Reset uses it
		std::string          faultDetail;
	};

	const char* stepName(const g2::Device::BootStep _step)
	{
		switch(_step)
		{
		case g2::Device::BootStep::Create:         return "1 create";
		case g2::Device::BootStep::Reset:          return "2 reset";
		case g2::Device::BootStep::StateLoad:      return "3 stateLoad";
		case g2::Device::BootStep::RunFrames:      return "4 runFrames";
		case g2::Device::BootStep::BeginPlayPhase: return "5 beginPlayPhase";
		case g2::Device::BootStep::Publish:        return "6 publish";
		}
		return "?";
	}

	std::string spell(const std::vector<StepRecord>& _records)
	{
		std::string out;

		for(const auto& r : _records)
		{
			if(!out.empty())
				out += " -> ";
			out += stepName(r.step);
		}

		return out;
	}

	/* The predicate is one function so that control 1 can hand it a wrong order.
	 * A predicate written inline at the assertion site could not be shown to
	 * refuse anything. */
	bool isExpectedOrder(const std::vector<StepRecord>& _records, const std::vector<g2::Device::BootStep>& _expected)
	{
		if(_records.size() != _expected.size())
			return false;

		for(size_t i = 0; i < _records.size(); ++i)
		{
			if(_records[i].step != _expected[i])
				return false;
		}

		return true;
	}

	/* The device the test drives. It exists to reach two protected members --
	 * the Board, for the display evidence, and processAudio -- and it adds no
	 * behaviour of its own. */
	class TestDevice final : public g2::Device
	{
	public:
		explicit TestDevice(const synthLib::DeviceCreateParams& _params) : Device(_params) {}

		g2::Board* boardForEvidence() const noexcept { return board(); }
	};

	/* The observer. `probeReset` decides whether the Reset notification also
	 * pulls: the pull is the not-priming probe and it perturbs the four codec
	 * counters, so the boot that answers property C runs with it OFF and a
	 * separate, cheap boot runs with it ON. Two questions, two boots, and
	 * neither answer can be produced by the other's probe. */
	class Recorder final : public g2::Device::IBootObserver
	{
	public:
		Recorder(const g2::Device& _device, const bool _probeReset)
			: m_device(_device), m_probeReset(_probeReset)
		{
		}

		void onBootStep(const g2::Device::BootStep _step, g2::Scheduler* const _scheduler) noexcept override
		{
			StepRecord record{};

			record.step  = _step;
			record.ready = m_device.isValid();

			if(_scheduler)
			{
				record.frameIndex = _scheduler->frameIndex();
				record.starved    = _scheduler->starvedFrames();
				record.overflow   = _scheduler->overflowFrames();
				record.dropped    = _scheduler->droppedFrames();
				record.underflow  = _scheduler->underflowFrames();

				if(m_probeReset && _step == g2::Device::BootStep::Reset)
					record.primedFramesSeen = probePrimedFrames(*_scheduler);

				/* Which context faulted, and why, recorded at the step that
				 * could have caused it. faulted() is the disjunction and a
				 * disjunction names nothing; the index is 0 for the MCU and
				 * 1..dspCount for the DSPs, ascending, and an index above
				 * dspCount reads back false rather than running off the
				 * end. */
				for(unsigned c = 0; c < 16; ++c)
				{
					if(!_scheduler->contextFaulted(c))
						continue;

					if(!record.faultDetail.empty())
						record.faultDetail += ", ";

					record.faultDetail += "context " + std::to_string(c) + " fault " +
						std::to_string(uint32_t(_scheduler->contextFault(c)));
				}
			}

			m_records.push_back(record);
		}

		const std::vector<StepRecord>& records() const { return m_records; }

	private:
		/* How many frames the sink hands back, read through the declared
		 * surface: pull() returns the frames actually taken, and the part it
		 * could not supply reads as silence. An empty sink answers 0. */
		static size_t probePrimedFrames(g2::Scheduler& _scheduler)
		{
			g2::Frame frames[g_lookaheadFrames]{};
			return _scheduler.pull(frames, g_lookaheadFrames);
		}

		const g2::Device&       m_device;
		const bool              m_probeReset;
		std::vector<StepRecord> m_records;
	};

	// ------------------------------------------------ the controls
	//
	// Neither needs firmware. Both construct their own Board and Scheduler, so
	// they run on a machine with no artifacts at all.
	void runControls()
	{
		std::cout << "-- controls (ungated: they need no firmware)" << std::endl;

		/* Control 1 -- a reordered sequence must be refused. The expected order
		 * is the cold boot's five steps; the input swaps steps 4 and 5, which is
		 * the boot/play merge expressed as an order rather than as a
		 * regime. */
		{
			const std::vector<g2::Device::BootStep> expected =
			{
				g2::Device::BootStep::Create,
				g2::Device::BootStep::Reset,
				g2::Device::BootStep::RunFrames,
				g2::Device::BootStep::BeginPlayPhase,
				g2::Device::BootStep::Publish
			};

			std::vector<StepRecord> right;
			for(const auto step : expected)
			{
				StepRecord r{};
				r.step = step;
				right.push_back(r);
			}

			std::vector<StepRecord> swapped = right;
			std::swap(swapped[2], swapped[3]);

			check(isExpectedOrder(right, expected),
				"CONTROL the order predicate ACCEPTS the boot step order BootStep declares");

			check(!isExpectedOrder(swapped, expected),
				"CONTROL the order predicate REFUSES the same sequence with steps 4 and 5 swapped, "
				"so a boot that ran beginPlayPhase before its boot quanta could not pass");
		}

		/* Control 2 -- a primed sink must read as primed. The Scheduler reaches
		 * the primed state through beginPlayPhase, which is the one thing in the
		 * system that primes, so this is a known positive drawn from the
		 * population the check runs on and not a flag written by the test. */
		{
			g2::Board          board;
			g2::SerialExecutor executor;
			g2::Status         status{};

			const std::unique_ptr<g2::Scheduler> scheduler =
				g2::Scheduler::create(makeConfig(), executor, board, status);

			if(!scheduler)
			{
				check(false, std::string("CONTROL Scheduler::create yielded an object; g2::Status = ") +
					std::to_string(uint32_t(status)));
				return;
			}

			scheduler->reset();

			g2::Frame afterReset[g_lookaheadFrames]{};
			const size_t takenAfterReset = scheduler->pull(afterReset, g_lookaheadFrames);

			check(takenAfterReset == 0,
				"CONTROL a reset Scheduler hands back 0 frames, which is the value the "
				"not-priming check reads as NOT PRIMED (saw " + std::to_string(takenAfterReset) + ")");

			scheduler->beginPlayPhase();

			g2::Frame afterPrime[g_lookaheadFrames]{};
			const size_t takenAfterPrime = scheduler->pull(afterPrime, g_lookaheadFrames);

			check(takenAfterPrime == g_lookaheadFrames,
				"CONTROL a PRIMED Scheduler hands back L = " + std::to_string(g_lookaheadFrames) +
				" frames, so the not-priming check discriminates a primed sink from an empty one "
				"(saw " + std::to_string(takenAfterPrime) + ")");
		}
	}

	// ------------------------------------------------ the display evidence
	std::string readDisplayLine(g2::Board& _board, const uint32_t _line)
	{
		std::string out;
		const uint32_t base = g_displayBase + _line * g_lineWidth;

		for(uint32_t col = 0; col < g_lineWidth; ++col)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t byte = g2::Board::onRead(&_board, base + col, g_byte, &status);

			out += char(byte & 0xffu);
		}

		return out;
	}

	/* The two bytes a "the firmware composed something" predicate must refuse to
	 * be satisfied by. 0x20 is what the firmware's own display clear writes.
	 * 0x00 is what a cell that was never written holds, because the store starts
	 * zeroed -- so counting it as content makes a machine that reached nothing
	 * report a full screen of it. Measured: with 0x00 counted, this check passed
	 * on a boot that faulted inside its first 64 quanta. */
	uint32_t countContentCells(const std::string& _line)
	{
		uint32_t n = 0;

		for(const char c : _line)
		{
			if(uint8_t(c) != g_clearByte && uint8_t(c) != 0x00u)
				++n;
		}

		return n;
	}
}

int main()
{
	// Before anything runs, so that no emitter escapes the filter.
	installLogFilter();

	// The controls run first and are ungated: a skipped run must not also lose
	// the evidence that the predicates discriminate.
	runControls();

	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		const int failuresBefore = g_failures;

		const synthLib::DeviceCreateParams params;

		// ---------------------------------------------------------------
		// Case group 1 -- the cold boot. It runs the full budget and is the
		// one boot meant to reach a running machine, so properties A and C
		// are both read off it and neither probe perturbs the other.
		std::cout << "-- case group 1: the cold boot" << std::endl;

		TestDevice device(params);
		Recorder   recorder(device, false);

		device.installBootObserver(&recorder);

		g2::Device::BootRequest request;
		request.config      = makeConfig();
		request.frameBudget = g_fullBootBudget;

		const g2::Device::BootResult result = device.boot(request);

		std::cout << "boot: booted=" << (result.booted ? "true" : "false")
		          << " framesRun=" << result.framesRun
		          << " chainAttached=" << (result.chainAttached ? "true" : "false")
		          << " faulted=" << (result.faulted ? "true" : "false")
		          << " status=" << uint32_t(result.status)
		          << " why=\"" << result.why << '"' << std::endl;
		std::cout << "boot: observed " << spell(recorder.records()) << std::endl;

		check(result.booted, "the boot completed every step BootStep declares: " + result.why);

		const std::vector<g2::Device::BootStep> coldOrder =
		{
			g2::Device::BootStep::Create,
			g2::Device::BootStep::Reset,
			g2::Device::BootStep::RunFrames,
			g2::Device::BootStep::BeginPlayPhase,
			g2::Device::BootStep::Publish
		};

		// Property A.
		check(isExpectedOrder(recorder.records(), coldOrder),
			"the cold boot ran create -> reset -> runFrames -> beginPlayPhase -> publish, IN THAT "
			"ORDER, and emitted no stateLoad: " + spell(recorder.records()));

		// Property A, second half: the publication is last and nothing before
		// it published. A machine visible to the audio thread before step 5 is
		// a machine handed over unprimed.
		if(!recorder.records().empty())
		{
			bool readyBeforePublish = false;

			for(size_t i = 0; i + 1 < recorder.records().size(); ++i)
			{
				if(recorder.records()[i].ready)
					readyBeforePublish = true;
			}

			check(!readyBeforePublish,
				"isValid() answered FALSE at every step before the publication, so the audio "
				"thread could not touch the Scheduler during the boot window");

			check(recorder.records().back().ready,
				"isValid() answered TRUE at the publication, which is the release store that "
				"hands the whole booted machine to the audio thread");
		}

		// Property C. The codec counters after step 4.
		for(const auto& r : recorder.records())
		{
			if(r.step != g2::Device::BootStep::RunFrames)
				continue;

			check(r.starved == 0 && r.overflow == 0 && r.dropped == 0 && r.underflow == 0,
				"the boot quanta ran the BOOT codec regime: neither codec queue was touched "
				"(starved=" + std::to_string(r.starved) +
				" overflow=" + std::to_string(r.overflow) +
				" dropped=" + std::to_string(r.dropped) +
				" underflow=" + std::to_string(r.underflow) + ")");

			if(!r.faultDetail.empty())
				std::cout << "boot: " << r.faultDetail << std::endl;

			check(r.frameIndex == result.framesRun,
				"every boot quantum ran: the virtual clock stands at the frames the boot drove, "
				"so the boot did not stop part-way on a full sink (frameIndex=" +
				std::to_string(r.frameIndex) + " framesRun=" + std::to_string(result.framesRun) + ")");
		}

		// The milestone evidence. Reported whether or not it is green, because a
		// boot that did not reach the running machine must say so in the same
		// place a boot that did says so.
		check(!result.faulted, "no context faulted during the boot");

		check(result.chainAttached,
			"the machine BOOTED: Scheduler::chainAttached() is true, which turns only on the "
			"quantum after the firmware's own port table lands -- the DSP programs are down and "
			"the chain is wired (framesRun=" + std::to_string(result.framesRun) + ")");

		if(g2::Board* board = device.boardForEvidence())
		{
			const std::string line0 = readDisplayLine(*board, 0);
			const std::string line1 = readDisplayLine(*board, 1);

			std::cout << "boot: display line 0 = \"" << line0 << '"' << std::endl;
			std::cout << "boot: display line 1 = \"" << line1 << '"' << std::endl;

			/* Reported and not asserted, and the reason is a measurement.
			 * This boot leaves on chainAttached() -- the DSP programs down
			 * and the chain wired -- which is measured to arrive at roughly
			 * 17,000 frames. The banner arrives far later: roughly 425,000
			 * frames to the patch browser. Asserting display content here
			 * would therefore assert something this boot's own exit condition
			 * does not claim. The cells are printed so the reader can see
			 * what the machine had composed by the hand-off. */
			std::cout << "boot: display content cells at the hand-off = "
			          << (countContentCells(line0) + countContentCells(line1))
			          << " (the banner is roughly 425,000 frames out; t1_boot owns it)" << std::endl;
		}
		else
		{
			check(false, "the Device holds a Board after boot()");
		}

		// ---------------------------------------------------------------
		// Case group 2 -- reset does not prime. A separate, cheap boot,
		// because its probe perturbs the counters case group 1 reads.
		std::cout << "-- case group 2: step 2 does not prime" << std::endl;

		TestDevice probeDevice(params);
		Recorder   probeRecorder(probeDevice, true);

		probeDevice.installBootObserver(&probeRecorder);

		g2::Device::BootRequest probeRequest;
		probeRequest.config      = makeConfig();
		probeRequest.frameBudget = g_orderBootBudget;

		const g2::Device::BootResult probeResult = probeDevice.boot(probeRequest);

		check(probeResult.booted, "the probing boot completed every step: " + probeResult.why);

		bool sawReset = false;

		for(const auto& r : probeRecorder.records())
		{
			if(r.step != g2::Device::BootStep::Reset)
				continue;

			sawReset = true;

			// Property B.
			check(r.primedFramesSeen == 0,
				"Scheduler::reset left BOTH CODEC QUEUES EMPTY: it does not prime, and priming "
				"the sink with L = " + std::to_string(g_lookaheadFrames) + " frames is step 5's "
				"job (the sink handed back " + std::to_string(r.primedFramesSeen) + " frames)");

			check(r.frameIndex == 0,
				"Scheduler::reset left the virtual clock at frame 0");
		}

		check(sawReset, "the boot emitted a Reset notification, so the probe above ran at all");

		// ---------------------------------------------------------------
		// Case group 3 -- the restore path. Step 3 runs, and it runs before
		// step 4. The snapshot is the flat block Scheduler::stateSave writes,
		// which getState refreshes inside the hand-off window.
		std::cout << "-- case group 3: the restore path" << std::endl;

#if SYNTHLIB_DEMO_MODE == 0
		std::vector<uint8_t> hostState;
		device.getState(hostState, synthLib::StateTypeGlobal);

		check(!device.machineSnapshot().empty(),
			"getState captured a machine snapshot from the booted Scheduler, which is what a "
			"restore has to load (bytes=" + std::to_string(device.machineSnapshot().size()) + ")");

		TestDevice restoreDevice(params);
		Recorder   restoreRecorder(restoreDevice, false);

		restoreDevice.installBootObserver(&restoreRecorder);

		g2::Device::BootRequest restoreRequest;
		restoreRequest.config          = makeConfig();
		restoreRequest.frameBudget     = g_orderBootBudget;
		restoreRequest.machineSnapshot = &device.machineSnapshot();

		const g2::Device::BootResult restoreResult = restoreDevice.boot(restoreRequest);

		std::cout << "restore: booted=" << (restoreResult.booted ? "true" : "false")
		          << " stateLoaded=" << (restoreResult.stateLoaded ? "true" : "false")
		          << " status=" << uint32_t(restoreResult.status)
		          << " why=\"" << restoreResult.why << '"' << std::endl;
		std::cout << "restore: observed " << spell(restoreRecorder.records()) << std::endl;

		check(restoreResult.booted && restoreResult.stateLoaded,
			"the restoring boot ran step 3 and completed");

		const std::vector<g2::Device::BootStep> restoreOrder =
		{
			g2::Device::BootStep::Create,
			g2::Device::BootStep::Reset,
			g2::Device::BootStep::StateLoad,
			g2::Device::BootStep::RunFrames,
			g2::Device::BootStep::BeginPlayPhase,
			g2::Device::BootStep::Publish
		};

		// Property D.
		check(isExpectedOrder(restoreRecorder.records(), restoreOrder),
			"the restoring boot ran stateLoad AFTER reset and BEFORE the boot quanta, so the "
			"machine runs FROM the restored state and not over it: " + spell(restoreRecorder.records()));
#else
		std::cout << "note SYNTHLIB_DEMO_MODE hides getState, so case group 3 has no snapshot "
		             "source in this build" << std::endl;
#endif

		reportSuppressedLogLines();

		std::cout << "t1_boot_on_restore: " << g_failures << " failure(s) in " << g_cases
		          << " case(s)" << std::endl;

		return g_failures == failuresBefore;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	// A skipped run still reports the controls, which are ungated. Their
	// failures are real failures and must not be lost to the skip code.
	if(counters.run == 0 && g_failures > 0)
		return 1;

	return g2::test::gatedExitCode(counters);
}
