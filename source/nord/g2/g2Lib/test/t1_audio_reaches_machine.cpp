/* t1_audio_reaches_machine.cpp -- processAudio on a Device booted through
 * Device::boot() drives the machine the boot produced. Tier T1: it boots the
 * real Clavia firmware and skips with a reason when NMG2_ARTIFACTS does not
 * resolve.
 *
 * Why this test exists where the other two audio tests do not reach. The audio
 * thread reaches the Scheduler through ISchedulerDriver and through nothing
 * else, so a test that calls installDriver() to substitute a recording driver
 * measures the CALL ORDER and cannot measure the WIRING: the production driver
 * is the one object such a test has replaced. This test installs nothing. It
 * boots, and then it asks the machine whether the callback arrived.
 *
 * The observable is the Scheduler's own virtual clock. Scheduler::frameIndex()
 * counts the quanta the Scheduler has turned, so a callback that reached the
 * machine advances it by the callback's frame count and a callback that reached
 * nothing leaves it where the boot left it.
 *
 * The clock is read on the Scheduler the BOOT OBSERVER saw published, not on
 * the one the driver reports holding. The two reads answer different questions
 * and the second cannot substitute for the first: a driver that forwards
 * nowhere would still report its own pointer truthfully, so a test that read
 * only the driver's view could be satisfied by an accessor and never by a
 * turned quantum.
 *
 * Three properties:
 *
 *   A. The driver the audio thread calls through holds the very Scheduler the
 *      boot published, which is the wiring itself.
 *
 *   B. One callback advances the published machine's virtual clock by exactly
 *      its frame count, so runFrames arrived with the count the callback was
 *      given rather than with some count or none.
 *
 *   C. Repeated callbacks keep advancing it, by the sum of their counts, and
 *      the device stays valid across them -- a machine driven for real, not one
 *      touched once.
 *
 * The controls are ungated, they run before the boot, and they are the known
 * positive and the known negative for the wiring itself. The negative is the
 * shape the defect had: a SchedulerDriver over no Scheduler answers every call
 * inertly and reaches nothing, and the positive is the same class over a real
 * Scheduler. Both build their own Board, so they run on a machine with no
 * artifacts at all and a skipped run keeps the evidence that the predicate
 * discriminates.
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
#include <memory>
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

	/* The ESAI underrun log filter. dsp56kEmu logs once per transmit slot whose
	 * data was never written, and this boot turns very many frames. The count is
	 * reported rather than discarded, so "the log was silenced" stays a statement
	 * about volume. Set G2_LOG_ESAI_UNDERRUN to install no filter. */
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
	constexpr unsigned g_lookaheadFrames    = 4;
	constexpr unsigned g_maxHostBlockFrames = 64;

	// The callback block, in frames. Smaller than B so that the queues the
	// Scheduler allocated hold it, and larger than L so that a pull of one block
	// asks the sink for more than the priming.
	constexpr size_t g_blockFrames = 32;

	constexpr unsigned g_callbackRounds = 8;

	g2::Scheduler::Config makeConfig()
	{
		g2::Scheduler::Config config;

		config.lookaheadFrames    = g_lookaheadFrames;
		config.maxHostBlockFrames = g_maxHostBlockFrames;

		return config;
	}

	// The boot leaves on Scheduler::chainAttached(), the machine's own signal.
	// This is the ceiling that keeps a firmware which never gets there from
	// running for ever, not a prediction of where the boot ends.
	constexpr uint64_t g_bootBudget = 500000;

	/* The device the test drives. processAudio is protected, and a host is what
	 * normally calls it; this subclass is the seam that lets the test be the
	 * host. It adds no behaviour. */
	class TestDevice final : public g2::Device
	{
	public:
		explicit TestDevice(const synthLib::DeviceCreateParams& _params) : Device(_params) {}

		using g2::Device::installedScheduler;

		void callback(const size_t _samples)
		{
			float left[g_blockFrames]{};
			float right[g_blockFrames]{};

			synthLib::TAudioInputs  inputs{};
			synthLib::TAudioOutputs outputs{};

			inputs[0]  = left;
			inputs[1]  = right;
			outputs[0] = left;
			outputs[1] = right;

			processAudio(inputs, outputs, _samples);
		}
	};

	/* The published machine, taken from the boot thread's own notification. It is
	 * the Scheduler the boot handed to the audio thread, obtained without asking
	 * the wiring about itself. */
	class PublishedScheduler final : public g2::Device::IBootObserver
	{
	public:
		void onBootStep(const g2::Device::BootStep _step, g2::Scheduler* const _scheduler) noexcept override
		{
			if(_step == g2::Device::BootStep::Publish)
				m_published = _scheduler;
		}

		g2::Scheduler* published() const noexcept { return m_published; }

	private:
		g2::Scheduler* m_published = nullptr;
	};

	/* The predicate, written once so that the controls can hand it the two
	 * cases it must separate. A driver reaches the machine when driving it
	 * advances the machine's own virtual clock by the frames it was asked for. */
	bool driverReachesMachine(g2::Device::ISchedulerDriver& _driver, g2::Scheduler& _scheduler, const size_t _frames)
	{
		const uint64_t before = _scheduler.frameIndex();

		g2::Frame in[g_blockFrames]{};
		g2::Frame out[g_blockFrames]{};

		_driver.push(in, _frames);
		_driver.runFrames(_frames);
		_driver.pull(out, _frames);

		return _scheduler.frameIndex() == before + _frames;
	}

	void runControls()
	{
		std::cout << "-- controls (ungated: they need no firmware)" << std::endl;

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
		scheduler->beginPlayPhase();

		/* Control 1 -- the known NEGATIVE, and it is the shape the defect had: a
		 * SchedulerDriver holding no Scheduler. Every call through it is inert,
		 * so the clock of a machine standing beside it does not move, and a
		 * plugin wired this way emits silence for ever while reporting itself
		 * ready. */
		{
			g2::Device::SchedulerDriver inert{nullptr};

			check(!driverReachesMachine(inert, *scheduler, g_blockFrames),
				"CONTROL a SchedulerDriver holding NO Scheduler reaches no machine: the virtual "
				"clock did not move, so the predicate below refuses an unwired driver");

			check(inert.push(nullptr, g_blockFrames) == 0 && inert.pull(nullptr, g_blockFrames) == 0,
				"CONTROL an unwired SchedulerDriver accepts no frames and supplies none, which is "
				"the digital silence such a plugin would emit");
		}

		/* Control 2 -- the known POSITIVE from the same population. The same
		 * class, over a real Scheduler, must be accepted. */
		{
			g2::Device::SchedulerDriver wired{scheduler.get()};

			check(driverReachesMachine(wired, *scheduler, g_blockFrames),
				"CONTROL a SchedulerDriver holding a Scheduler DOES reach it: the virtual clock "
				"advanced by the frames the driver was asked for, so the predicate discriminates "
				"a wired driver from an unwired one");
		}
	}
}

int main()
{
	installLogFilter();

	// Ungated, and first: a skipped run must not also lose the evidence that the
	// predicate discriminates.
	runControls();

	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]() -> bool
	{
		const int failuresBefore = g_failures;

		const synthLib::DeviceCreateParams params;

		TestDevice         device(params);
		PublishedScheduler publication;

		device.installBootObserver(&publication);

		g2::Device::BootRequest request;
		request.config      = makeConfig();
		request.frameBudget = g_bootBudget;

		const g2::Device::BootResult result = device.boot(request);

		std::cout << "boot: booted=" << (result.booted ? "true" : "false")
		          << " framesRun=" << result.framesRun
		          << " chainAttached=" << (result.chainAttached ? "true" : "false")
		          << " faulted=" << (result.faulted ? "true" : "false")
		          << " status=" << uint32_t(result.status)
		          << " why=\"" << result.why << '"' << std::endl;

		check(result.booted, "the boot completed every step: " + result.why);
		check(!result.faulted, "no context faulted during the boot");
		check(device.isValid(), "the boot published the machine, so processAudio takes the ready branch");

		if(!result.booted)
			return false;

		g2::Scheduler* const machine = publication.published();

		check(machine != nullptr,
			"the boot's publication notification carried a Scheduler, so the clock read below "
			"has a machine to read");

		if(!machine)
			return false;

		// Property A. The wiring, asked of the driver and compared against the
		// machine the boot published.
		check(device.installedScheduler() == machine,
			"the driver the audio thread calls through holds THE SCHEDULER THE BOOT PUBLISHED, so "
			"a callback has something to reach");

		// Property B. One callback, and the clock it moved.
		const uint64_t beforeOne = machine->frameIndex();

		device.callback(g_blockFrames);

		const uint64_t afterOne = machine->frameIndex();

		std::cout << "callback: frameIndex " << beforeOne << " -> " << afterOne << std::endl;

		check(afterOne == beforeOne + g_blockFrames,
			"ONE processAudio call on a Device booted through boot() turned the machine's own "
			"virtual clock by exactly the block it was given: " + std::to_string(beforeOne) +
			" -> " + std::to_string(afterOne) + ", block " + std::to_string(g_blockFrames) +
			" frames");

		// Property C. Repeated callbacks keep driving it.
		for(unsigned round = 0; round < g_callbackRounds; ++round)
			device.callback(g_blockFrames);

		const uint64_t afterMany = machine->frameIndex();

		check(afterMany == afterOne + uint64_t(g_callbackRounds) * g_blockFrames,
			"every further callback turned the machine too: " + std::to_string(g_callbackRounds) +
			" blocks advanced the clock to " + std::to_string(afterMany));

		check(device.isValid(),
			"the machine did not fault across the callbacks, so the readiness the boot published "
			"was never withdrawn");

		reportSuppressedLogLines();

		std::cout << "t1_audio_reaches_machine: " << g_failures << " failure(s) in " << g_cases
		          << " case(s)" << std::endl;

		return g_failures == failuresBefore;
	});

	std::cout << g2::test::summaryLine(counters) << std::endl;

	// The controls are ungated. Their failures are real failures and must not be
	// lost to the skip code.
	if(counters.run == 0 && g_failures > 0)
		return 1;

	return g2::test::gatedExitCode(counters);
}
