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
// -------------------------------------------------------------------------
// A DEVIATION FROM THE Check: LINE OF BRD-23, STATED HERE RATHER THAN HIDDEN.
//
// BRD-23's Check: line says the test "calls mcf5307_runtime_init() and takes
// the address of mcf5307_exec". THE SECOND HALF CANNOT BE SATISFIED at the
// commit this build pins. src/mcf5307.nim exports mcf5307_runtime_init;
// mcf5307_exec is DECLARED in include/mcf5307.h and is DEFINED nowhere in the
// library. To confirm against libmcf5307.a built from the pinned commit:
//
//   $ nm -g libmcf5307.a | grep mcf5307_
//   $ c++ probe.o libmcf5307.a -o probe
//
// So a test that took that address would fail to LINK, and BRD-23's positive
// case could not pass at all. The address is NOT taken here and NO stub is
// supplied for it: a stub would make the link succeed against a definition
// that is not the core, which is the one outcome this test exists to refuse.
// mcf5307_exec belongs in this test the day a cpu task defines it.
//
// Plan section 1.3 rule 5 is why this is written down instead of worked
// around quietly.
// -------------------------------------------------------------------------

#include <mcf5307.h>

#include <cstddef>
#include <iostream>
#include <string>

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
// It DOES NOT CATCH a wrong ANSWER from the core. Every behavioural assertion
// about the core belongs to the cpu track's own conformance tests, in the cpu
// track's own repository. Two honest cases are written here rather than a
// longer list padded with assertions that no defect could turn red.

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

	runtimeInit();
	++returnedCalls;
	runtimeInit();
	++returnedCalls;
	runtimeInit();
	++returnedCalls;

	check(returnedCalls == 3,
		"mcf5307_runtime_init returned from all three of three calls");

	if(g_failures)
	{
		std::cout << "t0_mcf5307_link: " << g_failures << " of 2 cases failed"
			<< std::endl;
		return 1;
	}

	std::cout << "t0_mcf5307_link: 2 of 2 cases passed" << std::endl;
	return 0;
}
