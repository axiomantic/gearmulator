// Task BRD-23. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 12, BRD-23. Design sections 20.3 and 5.2. Plan section 7.4.2.
//
// WHAT THIS TEST IS FOR. Task BRD-0 puts the mcf5307::mcf5307 link behind
// option(G2_LINK_MCF5307 ... OFF), because task CPU-1 exports that target one
// wave later than BRD-0 runs. BRD-23 turns the option ON. This test is the
// evidence that the option is LIVE and not decorative.
//
// THE TEST LINKS g2Lib AND NOTHING ELSE. It never names mcf5307::mcf5307 on
// its own link line. Every mcf5307 name it uses -- the header and the symbol
// -- has to arrive through g2Lib's own PUBLIC link, which is the one line
// BRD-23 turns on. Link the test against the core directly and the test would
// pass with that line deleted, which is the exact defect it exists to catch.
//
// THE TWO FAILURE MODES ARE DIFFERENT STEPS AND THIS IS MEASURED, NOT ARGUED.
// Plan section 7.7 measurement 4 carries the transcript:
//
//   * With G2_LINK_MCF5307 OFF the FetchContent declaration of the root
//     CMakeLists.txt is never populated, so mcf5307.h is absent from the tree
//     and this translation unit stops at the COMPILE step with
//     `fatal error: 'mcf5307.h' file not found`.
//   * With the header present and the symbol unresolved the LINK step stops
//     with `Undefined symbols ... "_mcf5307_runtime_init"`.
//
// Both fail, so the negative case fires either way. They are named apart
// because a reader who expects the second and observes the first reads a real
// failure as the wrong failure, and the obvious repair for that misreading --
// putting the header on the include path with the option OFF -- is the one
// change that would make the negative case stop testing anything.
//
// BRD-23's Check: line asks the test to call mcf5307_runtime_init() and take
// the address of mcf5307_exec. Both halves are satisfied below. An earlier pin
// exported only mcf5307_runtime_init, so taking the second address would have
// failed to link and the deviation was written down here; the pin this build
// carries defines both, so the case is written rather than deferred. To
// confirm against libmcf5307.a built from the pinned commit:
//
//   $ nm -g libmcf5307.a | grep mcf5307_
//
// NO STUB is supplied for either symbol. A stub would make the link succeed
// against a definition that is not the core, which is the one outcome this
// test exists to refuse.

#include <mcf5307.h>

#include <cstddef>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	// The totals below are COUNTED here rather than written as a literal in
	// the report. A literal goes stale the moment a case is added, and says so
	// in no way at all.
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
}

// WHAT THIS TEST CAN AND CANNOT CATCH, WRITTEN DOWN SO THAT NOBODY READS MORE
// INTO A GREEN RUN THAN IT CARRIES.
//
// It CATCHES: a g2Lib that does not link the core. That failure lands at the
// COMPILE step when the header is absent and at the LINK step when the header
// is present and the symbol is not, and the cases below are what force
// the linker to resolve the symbol at all.
//
// It CATCHES: a runtime entry point that does not return -- an aborting Nim
// runtime, or an idempotence latch that recursed.
//
// It CATCHES: an initialisation latch that stalled. mcf5307_runtime_init
// answers 1 when the runtime is usable and 0 when the latch was abandoned, and
// the status is checked below rather than dropped. A 0 makes mcf5307_create
// and isp1181_create return null, so every later call in the track answers out
// of a runtime that was never initialised; this is the first place that shows.
//
// It DOES NOT CATCH a wrong ANSWER from the core. Nothing below executes a
// program: mcf5307_exec has its address taken but is never called, because
// calling it needs a context and a program and would be a behavioural
// assertion. Every behavioural assertion about the core belongs to the cpu
// track's own conformance tests, in the cpu track's own repository. The cases
// here are the ones a link defect can turn red, rather than a longer list
// padded with assertions no defect could.
//
// It DOES NOT CATCH a stalled initialisation latch. mcf5307_runtime_init
// answers a TRUTH VALUE and not a POSIX code -- 1 when the runtime is usable,
// 0 when the latch was abandoned, which is terminal -- and the cases below
// take the status only to make the pointer type match the header. Checking
// the status is the job of a test that has something to do when it is 0.

int main()
{
	// Case 1. The symbol resolved to a real address.
	//
	// THE POINTER IS volatile AND EVERY CALL GOES THROUGH IT. A direct call
	// gives the compiler an inline no-op it may erase, and an erased call
	// proves nothing about the link. An indirect call through a volatile
	// pointer forces the address to be materialised and forces the linker to
	// resolve the symbol, so this case is red -- at the link step, naming
	// _mcf5307_runtime_init -- for a g2Lib that does not carry the core.
	int (*volatile runtimeInit)() = &mcf5307_runtime_init;

	check(runtimeInit != nullptr,
		"mcf5307_runtime_init resolved to a non-null address through g2Lib");

	// Case 1b. The execution entry point resolved too, and through the same
	// link line.
	//
	// This is the second half of BRD-23's Check: line. The pointer is volatile
	// for the reason above: the address must be materialised, so that a g2Lib
	// which does not carry the core is red at the link step naming
	// _mcf5307_exec. It is the symbol Board::runMcu forwards to, so this case
	// covers the one mcf5307 entry point g2Lib itself calls.
	//
	// The address is taken and NOT called. Calling it needs a context and a
	// program, and what it returned would be a statement about the core rather
	// than about the link.
	uint32_t (*volatile exec)(mcf5307_ctx*, uint32_t) = &mcf5307_exec;

	check(exec != nullptr,
		"mcf5307_exec resolved to a non-null address through g2Lib");

	// Case 2. The runtime entry point runs, and it runs repeatedly.
	//
	// It is the procedure design section 5.4 rule 2 requires a C++ caller to
	// use in place of ever naming NimMain, and src/mcf5307.nim states that it
	// is idempotent behind a latch that is set AFTER the call. A latch set
	// before the call would be written back by module initialisation and
	// every later call would run the initialiser again.
	//
	// The counter is volatile, so the compiler cannot fold the comparison,
	// and it is incremented AFTER each call. It reaches three only if all
	// three calls returned. A runtime built with --panics:on ends the process
	// on a defect rather than returning a wrong value, so a core that aborted
	// on the second call leaves this at one and never reaches the report.
	volatile int returnedCalls = 0;
	volatile int initialisedCalls = 0;

	initialisedCalls += runtimeInit();
	++returnedCalls;
	initialisedCalls += runtimeInit();
	++returnedCalls;
	initialisedCalls += runtimeInit();
	++returnedCalls;

	check(returnedCalls == 3,
		"mcf5307_runtime_init returned from all three of three calls");

	// Case 3. The runtime reports itself usable, and keeps reporting it.
	//
	// The status is a truth value and not a POSIX error code: 1 is usable, 0
	// is a latch that reached its deadline and was abandoned. The abandoned
	// state is terminal, so a stall on any of the three calls leaves this sum
	// below three. Summing rather than checking the last call is what keeps a
	// latch that answered 1 and then 0 from passing.
	check(initialisedCalls == 3,
		"mcf5307_runtime_init reported the runtime usable on all three calls");

	if(g_failures)
	{
		std::cout << "t0_mcf5307_link: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_mcf5307_link: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
