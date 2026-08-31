/* The boot thread owns create, reset, stateLoad, the boot runFrames calls and
 * beginPlayPhase. The audio thread owns push, runFrames, pull, queueMidi and
 * the accessors. Every runFrames below is called either by the thread that
 * already owns the object or after the owner was cleared.
 *
 * "The recorded identity equals the caller" is satisfied by an implementation
 * that records nothing at all if the comparison happens to be between two
 * default-constructed ids, and by an implementation that records once and
 * never again if both phases run on one thread. So every identity assertion is
 * preceded by an assertion that the recorded id is not the
 * default-constructed one, and the audio phase runs on a genuinely different
 * thread and asserts that the recorded id moved before it asserts what it
 * moved to.
 *
 * The MCU rate is 0/1, so the budget is zero at every quantum and
 * Board::runMcu is never invoked. This fixture's Board has no mapped memory,
 * so a rate that ran the core would fault it.
 *
 * No case here is a language assert() and no case catches an exception, so
 * this file reports identically under NDEBUG and without it.
 */

#include "board.h"
#include "executor.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const char* const _what)
	{
		++g_cases;

		if(_condition)
			return;

		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	/* beginPlayPhase leaves L frames in the sink and each later play quantum
	 * adds one; the capacity is L + B, so the play quanta below stay inside it
	 * and no runFrames returns early on a refused push. */
	constexpr unsigned kLookahead    = 1;
	constexpr unsigned kMaxHostBlock = 8;

	constexpr uint64_t kBootQuanta      = 5;
	constexpr uint64_t kMoreBootQuanta  = 4;
	constexpr uint64_t kAudioQuanta     = 2;
	constexpr uint64_t kMoreAudioQuanta = 3;
}

int main()
{
	std::printf("t0_thread_map: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		/* In an interpreter build no Scheduler can be created at all, so the
		 * refusal is the only claim this file may make. */
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.backend = g2::Backend::Jit;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		check(status == g2::Status::BadBackend, "an interpreter build reports BadBackend");

		std::printf("t0_thread_map: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	/* The board is declared first, so it outlives the Scheduler that borrows
	 * it. This thread is the boot thread for the whole of the file. */
	g2::Board          board;
	g2::SerialExecutor executor;

	g2::Scheduler::Config config;
	config.lookaheadFrames    = kLookahead;
	config.maxHostBlockFrames = kMaxHostBlock;
	config.mcuRate            = { 0, 1 };

	g2::Status status{};

	const std::unique_ptr<g2::Scheduler> scheduler =
		g2::Scheduler::create(config, executor, board, status);

	check(status == g2::Status::Ok, "the Config is accepted");

	if(!scheduler)
	{
		check(false, "the Config yields a Scheduler");
		std::printf("t0_thread_map: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	g2::Scheduler& s = *scheduler;

	const std::thread::id bootThread = std::this_thread::get_id();

	/* create() runs on the boot thread and does not claim ownership: the
	 * record is made by the first runFrames. */
	check(s.owningThread() == std::thread::id{},
		"a fresh Scheduler has recorded no owning thread");

	/* The boot thread's own id must not be the default id, or the assertion
	 * above and the one below would be the same assertion written twice. */
	check(bootThread != std::thread::id{},
		"KNOWN POSITIVE: this thread's id is not the default-constructed id");

	s.runFrames(kBootQuanta);

	check(s.owningThread() != std::thread::id{},
		"the boot runFrames recorded SOME owning thread");
	check(s.owningThread() == bootThread,
		"the recorded owning thread is the boot thread that called runFrames");

	s.runFrames(kMoreBootQuanta);

	check(s.owningThread() == bootThread,
		"further boot runFrames calls from the same thread leave the record unchanged");

	/* beginPlayPhase is the boot thread's last Scheduler action, and it clears
	 * the record so that the audio thread can claim it. The assertion above is
	 * the known positive for this one: the record was observed non-default
	 * immediately before this call, so "it is default now" is a statement
	 * about this call and not about a field nothing ever wrote. */
	s.beginPlayPhase();

	check(s.owningThread() == std::thread::id{},
		"beginPlayPhase cleared the recorded owning thread");

	/* The audio phase runs on a genuinely different thread: that is what
	 * separates "records the caller" from "records the first thread it ever
	 * saw". beginPlayPhase cleared the owner, so the runFrames below claims it
	 * rather than violating the map.
	 *
	 * Both audio-phase calls run on one live thread, and that is forced rather
	 * than tidy. A joined std::thread's id may be reused by the next thread
	 * the implementation starts -- it is measured to be reused on this
	 * platform -- so a second, sequentially-started thread is not reliably a
	 * different thread, and a case built on that assumption reports the
	 * platform's id-recycling rather than the Scheduler's behaviour. */
	std::thread::id audioThread{};
	std::thread::id recordAfterFirstCall{};
	std::thread::id recordAfterSecondCall{};

	{
		std::thread audio([&]
		{
			audioThread = std::this_thread::get_id();

			s.runFrames(kAudioQuanta);
			recordAfterFirstCall = s.owningThread();

			s.runFrames(kMoreAudioQuanta);
			recordAfterSecondCall = s.owningThread();
		});

		audio.join();
	}

	check(audioThread != std::thread::id{},
		"KNOWN POSITIVE: the audio thread ran and reported its own id");
	check(audioThread != bootThread,
		"KNOWN POSITIVE: the audio thread is a DIFFERENT thread from the boot thread");

	check(recordAfterFirstCall != std::thread::id{},
		"the audio phase's first runFrames recorded SOME owning thread");
	check(recordAfterFirstCall != bootThread,
		"OWNERSHIP MOVED: the record is no longer the boot thread's");
	check(recordAfterFirstCall == audioThread,
		"the recorded owning thread is the AUDIO thread that called runFrames");

	check(recordAfterSecondCall == audioThread,
		"ownership moved exactly once: a second call from the owning thread re-records nothing");

	/* This is the one case that separates the accessor answering the record
	 * from it answering the caller: it is read here, from the boot thread,
	 * after the audio thread has ended. An owningThread() implemented as
	 * `std::this_thread::get_id()` would satisfy every case above -- each of
	 * them reads the accessor from the thread that just called runFrames --
	 * and would answer the boot thread's id here. */
	check(s.owningThread() == audioThread,
		"owningThread() read from the BOOT thread still answers the AUDIO thread's id");

	/* reset() is the boot thread's, and it returns the object to its fresh
	 * state so that the whole two-phase cycle can begin again. */
	s.reset();

	check(s.owningThread() == std::thread::id{},
		"reset() cleared the recorded owning thread");

	s.runFrames(1);

	check(s.owningThread() == bootThread,
		"after reset() the next runFrames re-establishes the owner on the calling thread");

	if(g_failures != 0)
	{
		std::printf("t0_thread_map: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_thread_map: all %d cases passed\n", g_cases);
	return 0;
}
