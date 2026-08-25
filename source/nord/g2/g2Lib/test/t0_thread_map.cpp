/* t0_thread_map.cpp -- the check of SCH-21 step 5 (formerly SCH-28's own
 * Check: line). Design sections 13.10.5 and 18.2.
 *
 * WHAT THE STEP REQUIRES, AND WHERE EACH CLAUSE IS ASSERTED:
 *
 *   "The boot thread owns create, reset, stateLoad, the boot runFrames calls
 *    and beginPlayPhase. The audio thread owns push, runFrames, pull,
 *    queueMidi and the accessors."
 *       -- docs/threading.md carries the full map. NOTHING IN THIS FILE READS
 *          THAT DOCUMENT. A test that greps prose asserts the prose, passes
 *          while the code is wrong, and goes red when someone edits a
 *          sentence. What is asserted here is the BEHAVIOUR the map describes.
 *   "Ownership moves exactly once."
 *       -- cases 3 and 6: a second call from the SAME thread leaves the
 *          recorded identity byte-for-byte unchanged, in each of the two
 *          phases.
 *   "The scheduler records the owning thread identity and exposes it, and the
 *    registered test asserts the recorded identity against the caller for each
 *    of the two phases, in a release build as well as a debug build."
 *       -- cases 2 and 5, and the whole file is free of assert().
 *   "A debug build additionally asserts on a call from another thread; the
 *    assertion is not the check's predicate."
 *       -- scheduler.cpp carries that assertion. NO CASE HERE DRIVES IT AND NO
 *          CASE HERE READS IT. Every runFrames below is called either by the
 *          thread that already owns the object or after the owner was cleared,
 *          which is exactly what the map permits.
 *
 * THE ANTI-MIRAGE GUARDS, NAMED. "The recorded identity equals the caller" is
 * satisfied by an implementation that records nothing at all if the comparison
 * happens to be between two default-constructed ids, and by an implementation
 * that records once and never again if both phases run on one thread. So:
 *
 *   -- every identity assertion is preceded by an assertion that the recorded
 *      id is NOT the default-constructed one, and
 *   -- THE AUDIO PHASE RUNS ON A GENUINELY DIFFERENT THREAD, and case 5
 *      asserts that the recorded id MOVED -- that it differs from the boot
 *      thread's -- before it asserts what it moved to.
 *
 * WITHOUT THAT SECOND GUARD THE WHOLE FILE WOULD PASS AGAINST A Scheduler THAT
 * RECORDED THE FIRST THREAD IT EVER SAW AND NEVER LOOKED AGAIN.
 *
 * THE MCU RATE IS 0/1, so the section 13.4.6 block's budget is zero at every
 * quantum and Board::runMcu is never invoked. This fixture's Board has no
 * mapped memory, so a rate that ran the core would fault it -- which is
 * t0_scheduler_faults' subject and not this file's.
 *
 * NO CASE HERE IS A LANGUAGE assert() AND NO CASE CATCHES AN EXCEPTION, so this
 * file reports identically under NDEBUG and without it.
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

	/* L AND B ARE CHOSEN SO THAT THE PLAY-REGIME CASES CANNOT FILL THE SINK.
	 * beginPlayPhase leaves L frames in it and each later play quantum adds
	 * one; the capacity is L + B, so the eight play quanta below stay well
	 * inside it and no runFrames returns early on a refused push. */
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
		/* In an interpreter build no Scheduler can be created at all
		 * (section 11.4.3), so the refusal is the only claim this file may
		 * make. */
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

	/* THE BOARD IS DECLARED FIRST, so it outlives the Scheduler that borrows
	 * it. THIS THREAD IS THE BOOT THREAD for the whole of the file. */
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

	/* -----------------------------------------------------------------
	 * CASE 1. A FRESH Scheduler RECORDS NO OWNER. create() runs on the boot
	 * thread and does not claim ownership: the map gives create to the boot
	 * thread and the RECORD to the first runFrames.
	 */
	check(s.owningThread() == std::thread::id{},
		"a fresh Scheduler has recorded no owning thread");

	/* THE GUARD THAT MAKES THE ABOVE MEAN SOMETHING. The boot thread's own id
	 * must not BE the default id, or case 1 and case 2 would be the same
	 * assertion written twice. */
	check(bootThread != std::thread::id{},
		"KNOWN POSITIVE: this thread's id is not the default-constructed id");

	/* -----------------------------------------------------------------
	 * CASE 2. THE BOOT PHASE. The first runFrames on the boot thread records
	 * that thread, and the accessor reports it in a release build as well as a
	 * debug build.
	 */
	s.runFrames(kBootQuanta);

	check(s.owningThread() != std::thread::id{},
		"the boot runFrames recorded SOME owning thread");
	check(s.owningThread() == bootThread,
		"the recorded owning thread is the boot thread that called runFrames");

	/* -----------------------------------------------------------------
	 * CASE 3. OWNERSHIP MOVES EXACTLY ONCE within the boot phase: further
	 * calls from the same thread re-record nothing.
	 */
	s.runFrames(kMoreBootQuanta);

	check(s.owningThread() == bootThread,
		"further boot runFrames calls from the same thread leave the record unchanged");

	/* -----------------------------------------------------------------
	 * CASE 4. beginPlayPhase IS THE BOOT THREAD'S LAST Scheduler ACTION, and
	 * its step 5 clears the record so that the audio thread can claim it.
	 *
	 * The assertion above is the known positive for this one: the record was
	 * observed NON-default immediately before this call, so "it is default
	 * now" is a statement about this call and not about a field nothing ever
	 * wrote.
	 */
	s.beginPlayPhase();

	check(s.owningThread() == std::thread::id{},
		"beginPlayPhase cleared the recorded owning thread");

	/* -----------------------------------------------------------------
	 * CASE 5. THE AUDIO PHASE, ON A GENUINELY DIFFERENT THREAD. This is the
	 * half no single-threaded fixture can check: it is what separates "records
	 * the caller" from "records the first thread it ever saw".
	 *
	 * The runFrames below is legal under the map and does not drive the debug
	 * assertion: beginPlayPhase cleared the owner, so this call CLAIMS it
	 * rather than violating it.
	 *
	 * CASE 6 IS INSIDE THE SAME THREAD BODY, AND THAT IS FORCED RATHER THAN
	 * TIDY. A joined std::thread's id MAY BE REUSED by the next thread the
	 * implementation starts -- it is measured to be reused on this platform --
	 * so a second, sequentially-started thread is not reliably a different
	 * thread and a case built on that assumption reports the platform's
	 * id-recycling rather than the Scheduler's behaviour. Both audio-phase
	 * calls therefore run on ONE live thread, which is also what the map
	 * describes: one audio thread for the whole play phase.
	 */
	std::thread::id audioThread{};
	std::thread::id recordAfterFirstCall{};
	std::thread::id recordAfterSecondCall{};

	{
		std::thread audio([&]
		{
			audioThread = std::this_thread::get_id();

			s.runFrames(kAudioQuanta);
			recordAfterFirstCall = s.owningThread();

			/* CASE 6. OWNERSHIP MOVES EXACTLY ONCE: a second call from the
			 * thread that already owns the object re-records nothing. */
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

	/* THE ACCESSOR ANSWERS THE RECORD AND NOT THE CALLER, AND THIS IS THE ONE
	 * CASE THAT SEPARATES THE TWO. It is read HERE, from the boot thread,
	 * after the audio thread has ended. An owningThread() implemented as
	 * `std::this_thread::get_id()` would satisfy every case above -- each of
	 * them reads the accessor from the thread that just called runFrames -- and
	 * would answer the BOOT thread's id here. */
	check(s.owningThread() == audioThread,
		"owningThread() read from the BOOT thread still answers the AUDIO thread's id");

	/* -----------------------------------------------------------------
	 * CASE 7. reset() IS THE BOOT THREAD'S, and it returns the object to the
	 * state case 1 observed -- so the whole two-phase cycle can begin again.
	 */
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
