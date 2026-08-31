/* t1_boot_on_restore.cpp -- task PLG-12's check. Tier T1: this test boots the
 * real Clavia firmware and SKIPS with a reason when NMG2_ARTIFACTS does not
 * resolve (REPO-7's gated fixture, design section 18.5).
 *
 * Design section 15.6, design section 13.10 rule 3, plan block PLG-12.
 *
 * WHAT THIS TEST HOLDS. Design section 15.6's six steps are an ORDER, and the
 * order is the deliverable:
 *
 *   1 Scheduler::create   2 Scheduler::reset   3 Scheduler::stateLoad (restore
 *   only)   4 many runFrames   5 beginPlayPhase   6 the release store of true
 *   into m_ready
 *
 * Four properties are checked, and each is checked through a DIFFERENT
 * observable so that no two can pass for the same reason:
 *
 *   A. THE ORDER ITSELF, through Device::IBootObserver -- the boot thread's
 *      twin of the ISchedulerDriver seam PLG-4 left for the audio thread. A
 *      cold boot emits five steps and NOT StateLoad; a restore emits six with
 *      StateLoad strictly before RunFrames.
 *
 *   B. RESET DOES NOT PRIME. Step 2 leaves both codec queues EMPTY; priming the
 *      sink with L frames is step 5's job. The probe is a pull() at the moment
 *      the Reset notification arrives, read through the declared surface --
 *      pull() returns the frames actually taken -- which is the technique
 *      t0_codec_regimes uses for the same question. A reset that primed would
 *      hand back L frames there.
 *
 *   C. THE BOOT CODEC REGIME TOUCHES NEITHER QUEUE. A Scheduler is born in the
 *      boot regime and beginPlayPhase is the only exit from it, so step 4's
 *      many quanta must leave all four codec counters at zero: starved,
 *      overflow, dropped and underflow. Were the play regime running during the
 *      boot, the sink would fill, push would refuse, and the boot would stall
 *      part-way through with no callback that could ever drain it.
 *
 *   D. THE RESTORE PATH LOADS BEFORE IT RUNS. Step 3 precedes step 4, which is
 *      what makes a restored machine run FROM the restored state rather than
 *      over it.
 *
 * THE TWO CONTROLS ARE KNOWN POSITIVES FROM THE SAME POPULATION, and they run
 * BEFORE the boot so that a reader meets the proof that the predicates
 * discriminate before meeting the numbers they produced. PLG-12's block names
 * no required-RED mutation, so these two are planted here:
 *
 *   CONTROL 1 -- A REORDERED SEQUENCE. The same predicate that accepts the
 *   observed order is handed the order with two steps swapped, and must refuse
 *   it. Without this, a predicate that accepted any sequence would pass A and D
 *   silently.
 *
 *   CONTROL 2 -- A PRIMED SINK. The not-priming predicate is handed a Scheduler
 *   that HAS been primed -- by beginPlayPhase, which is the one thing in the
 *   system that primes -- and must report it as primed. The control reaches the
 *   primed state through the REAL code path and not by writing a flag, so it is
 *   a known positive drawn from the population the check runs on.
 *
 * WHY THE CONTROLS NEED NO FIRMWARE. Both construct their own Scheduler over a
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
	// PORTED FROM t1_boot.cpp UNCHANGED IN BEHAVIOUR, and it is here for the
	// reason it is there: dsp56kEmu's Esai::writeSlotToFrame calls LOG() once per
	// transmit slot whose data was never written, and this boot turns hundreds of
	// thousands of Scheduler frames. The filter hides the REPETITION of a REAL and
	// EXPECTED condition and nothing else; the count is reported rather than
	// discarded, so "the log was silenced" stays a statement about volume and not
	// about evidence. Set G2_LOG_ESAI_UNDERRUN to install no filter at all.
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
	// L IS DELIBERATELY GREATER THAN ONE. The not-priming check asks whether the
	// sink holds L frames after step 2, and at L = 1 -- the Config default, which
	// scheduler.h states is the smallest legal value and not the shipped one -- a
	// primed sink and an empty one differ by a single frame. Four makes the
	// discriminator four frames wide and gives the CONTROL a number to report.
	constexpr unsigned g_lookaheadFrames    = 4;
	constexpr unsigned g_maxHostBlockFrames = 64;

	g2::Scheduler::Config makeConfig()
	{
		g2::Scheduler::Config config;

		config.lookaheadFrames    = g_lookaheadFrames;
		config.maxHostBlockFrames = g_maxHostBlockFrames;

		return config;
	}

	// The boot budget for the ONE boot that is meant to reach the running
	// machine. t1_boot and g2TestConsole both measure that the real boot needs
	// roughly 425,000 frames to reach the patch browser, so this is a ceiling
	// above that and NOT a prediction of where the boot ends: the boot leaves on
	// Scheduler::chainAttached(), the machine's own signal.
	constexpr uint64_t g_fullBootBudget = 500000;

	// The budget for the boots whose subject is the ORDER of the steps and not
	// the booted machine. The order is complete after step 6 whatever step 4 ran,
	// so these do not pay for a full boot.
	constexpr uint64_t g_orderBootBudget = 4096;

	// ------------------------------------------------ the display, for evidence
	//
	// Plan section 6.6.4 clause 1, as t1_boot.cpp records it: the display buffer
	// base is 0x302A0DB8 and a line is sixteen cells. The clear byte is 0x20, and
	// it is named because it is the ONE value a "the firmware composed something"
	// predicate must refuse to be satisfied by.
	constexpr uint32_t g_displayBase = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth   = 16u;
	constexpr uint8_t  g_clearByte   = 0x20u;
	constexpr int      g_byte        = 1;

	// ------------------------------------------------ the recorder
	//
	// It records the ORDER and, at each step, the two things that make the order
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

	/* THE PREDICATE, AND IT IS ONE FUNCTION SO THAT CONTROL 1 CAN HAND IT A
	 * WRONG ORDER. A predicate written inline at the assertion site could not be
	 * shown to refuse anything. */
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

	/* THE DEVICE THE TEST DRIVES. It exists to reach two protected members --
	 * the Board, for the display evidence, and processAudio, which nothing here
	 * calls but which the subclass shape makes available to a later case -- and
	 * it adds no behaviour of its own. */
	class TestDevice final : public g2::Device
	{
	public:
		explicit TestDevice(const synthLib::DeviceCreateParams& _params) : Device(_params) {}

		g2::Board* boardForEvidence() const noexcept { return board(); }
	};

	/* THE OBSERVER. `probeReset` decides whether the Reset notification also
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

				/* WHICH CONTEXT FAULTED, AND WHY, RECORDED AT THE STEP THAT
				 * COULD HAVE CAUSED IT. faulted() is the disjunction and a
				 * disjunction names nothing; design section 13.5's index is 0
				 * for the MCU and 1..dspCount for the DSPs, ascending, and an
				 * index above dspCount reads back false rather than running
				 * off the end. */
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
		/* HOW MANY FRAMES THE SINK HANDS BACK, read through the declared
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

		/* CONTROL 1 -- A REORDERED SEQUENCE MUST BE REFUSED. The expected order
		 * is the cold boot's five steps; the input swaps steps 4 and 5, which is
		 * exactly the merge design section 15.6 warns about, expressed as an
		 * order rather than as a regime. */
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
				"CONTROL the order predicate ACCEPTS the sequence design section 15.6 states");

			check(!isExpectedOrder(swapped, expected),
				"CONTROL the order predicate REFUSES the same sequence with steps 4 and 5 swapped, "
				"so a boot that ran beginPlayPhase before its boot quanta could not pass");
		}

		/* CONTROL 2 -- A PRIMED SINK MUST READ AS PRIMED. The Scheduler reaches
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

	/* THE TWO BYTES A "THE FIRMWARE COMPOSED SOMETHING" PREDICATE MUST REFUSE TO
	 * BE SATISFIED BY, and the second is the one an earlier revision of this file
	 * missed. 0x20 is what the firmware's own display CLEAR writes, and t1_boot
	 * records it. 0x00 is what a cell that was NEVER WRITTEN holds, because the
	 * store starts zeroed -- so counting it as content makes a machine that
	 * reached nothing report a full screen of it. MEASURED: with 0x00 counted,
	 * this check passed on a boot that faulted inside its first 64 quanta. */
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
	// BEFORE ANYTHING RUNS, so that no emitter escapes the filter.
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
		// CASE GROUP 1 -- THE COLD BOOT. It runs the full budget and is the
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

		check(result.booted, "the boot completed every step of design section 15.6: " + result.why);

		const std::vector<g2::Device::BootStep> coldOrder =
		{
			g2::Device::BootStep::Create,
			g2::Device::BootStep::Reset,
			g2::Device::BootStep::RunFrames,
			g2::Device::BootStep::BeginPlayPhase,
			g2::Device::BootStep::Publish
		};

		// PROPERTY A.
		check(isExpectedOrder(recorder.records(), coldOrder),
			"the cold boot ran create -> reset -> runFrames -> beginPlayPhase -> publish, IN THAT "
			"ORDER, and emitted no stateLoad: " + spell(recorder.records()));

		// PROPERTY A, second half: the publication is LAST and nothing before
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

		// PROPERTY C. The four codec counters after step 4.
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

		// THE MILESTONE EVIDENCE. Reported whether or not it is green, because a
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

			/* REPORTED AND NOT ASSERTED, and the reason is a measurement.
			 * This boot leaves on chainAttached(), which is the state design
			 * section 15.6 step 4 describes -- the DSP programs down and the
			 * chain wired -- and it arrives at roughly 17,000 frames. The
			 * BANNER arrives far later: t1_boot and g2TestConsole both measure
			 * roughly 425,000 frames to the patch browser. Asserting display
			 * content here would therefore assert something this boot's own
			 * exit condition does not claim, and the honest way to get it
			 * green would be to run 25x the quanta for evidence t1_boot
			 * already owns. The cells are PRINTED so the reader can see what
			 * the machine had composed by the hand-off. */
			std::cout << "boot: display content cells at the hand-off = "
			          << (countContentCells(line0) + countContentCells(line1))
			          << " (the banner is roughly 425,000 frames out; t1_boot owns it)" << std::endl;
		}
		else
		{
			check(false, "the Device holds a Board after boot()");
		}

		// ---------------------------------------------------------------
		// CASE GROUP 2 -- RESET DOES NOT PRIME. A separate, cheap boot,
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

			// PROPERTY B.
			check(r.primedFramesSeen == 0,
				"Scheduler::reset left BOTH CODEC QUEUES EMPTY: it does not prime, and priming "
				"the sink with L = " + std::to_string(g_lookaheadFrames) + " frames is step 5's "
				"job (the sink handed back " + std::to_string(r.primedFramesSeen) + " frames)");

			check(r.frameIndex == 0,
				"Scheduler::reset left the virtual clock at frame 0");
		}

		check(sawReset, "the boot emitted a Reset notification, so the probe above ran at all");

		// ---------------------------------------------------------------
		// CASE GROUP 3 -- THE RESTORE PATH. Step 3 runs, and it runs BEFORE
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

		/* THE KNOWN GAP, REPORTED AND NOT ASSERTED GREEN. Scheduler::stateSave
		 * writes the codec regime into its own limb of the state block, so a
		 * snapshot taken through getState -- necessarily a PLAY-regime snapshot
		 * -- puts step 4 into the play regime. PLG-12 reports the condition and
		 * cannot repair it: the repair is on the Scheduler side and
		 * scheduler.{h,cpp} is not on this task's Files: line. Asserting this
		 * false would be asserting a defect fixed. */
		if(restoreResult.regimeRestoredFromSnapshot)
		{
			std::cout << "note THE RESTORE PATH DOES NOT RUN THE BOOT CODEC REGIME: "
			          << restoreResult.why << std::endl;
		}

		const std::vector<g2::Device::BootStep> restoreOrder =
		{
			g2::Device::BootStep::Create,
			g2::Device::BootStep::Reset,
			g2::Device::BootStep::StateLoad,
			g2::Device::BootStep::RunFrames,
			g2::Device::BootStep::BeginPlayPhase,
			g2::Device::BootStep::Publish
		};

		// PROPERTY D.
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
