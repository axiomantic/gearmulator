// Tier T0: this test needs no firmware artifact of any kind.
//
// The mcf5407::mcf5407 link sits behind option(G2_LINK_MCF5407). This test is
// the evidence that the option is live and not decorative.
//
// The test links g2Lib and nothing else. It never names mcf5407::mcf5407 on
// its own link line. Every mcf5407 name it uses -- the header and the symbol
// -- has to arrive through g2Lib's own PUBLIC link. Link the test against the
// core directly and the test would pass with that line deleted, which is the
// exact defect it exists to catch.
//
// The two failure modes are different steps, and this is measured, not argued:
//
//   * With G2_LINK_MCF5407 OFF the FetchContent declaration of the root
//     CMakeLists.txt is never populated, so mcf5407.h is absent from the tree
//     and this translation unit stops at the COMPILE step with
//     `fatal error: 'mcf5407.h' file not found`.
//   * With the header present and the symbol unresolved the LINK step stops
//     with `Undefined symbols ... "_mcf5407_runtime_init"`.
//
// Both fail, so the negative case fires either way. They are named apart
// because a reader who expects the second and observes the first reads a real
// failure as the wrong failure, and the obvious repair for that misreading --
// putting the header on the include path with the option OFF -- is the one
// change that would make the negative case stop testing anything.
//
// To confirm that the pinned core defines both symbols, against libmcf5407.a
// built from the pinned commit:
//
//   $ nm -g libmcf5407.a | grep mcf5407_
//
// No stub is supplied for either symbol. A stub would make the link succeed
// against a definition that is not the core, which is the one outcome this
// test exists to refuse.

#include <mcf5407.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	// The totals below are counted here rather than written as a literal in
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

int main()
{
	// Both symbols have to arrive through g2Lib's own PUBLIC link, so both
	// addresses are taken here. The pointers are volatile: a direct call gives
	// the compiler an inline no-op it may erase, and an erased call proves
	// nothing about the link, while an indirect call through a volatile pointer
	// forces the address to be materialised and forces the linker to resolve
	// the symbol.
	//
	// mcf5407_exec is resolved and never called. Calling it needs a context and
	// a program, and what it returned would be a statement about the core
	// rather than about the link.
	int (*volatile runtimeInit)() = &mcf5407_runtime_init;
	uint32_t (*volatile exec)(mcf5407_ctx*, uint32_t) = &mcf5407_exec;
	(void) exec;

	// The runtime reports itself usable, and keeps reporting it.
	//
	// mcf5407_runtime_init is the procedure a C++ caller must use in place of
	// ever naming NimMain, and src/mcf5407.nim states that it is idempotent
	// behind a latch that is set AFTER the call. A latch set before the call
	// would be written back by module initialisation and every later call would
	// run the initialiser again.
	//
	// The status is a truth value and not a POSIX error code: 1 is usable, 0 is
	// a latch that reached its deadline and was abandoned, which is terminal.
	// Summing rather than checking the last call is what keeps a latch that
	// answered 1 and then 0 from passing. The counter is volatile, so the
	// compiler cannot fold the comparison, and a runtime built with --panics:on
	// ends the process on a defect rather than returning a wrong value.
	volatile int initialisedCalls = 0;

	initialisedCalls += runtimeInit();
	initialisedCalls += runtimeInit();
	initialisedCalls += runtimeInit();

	check(initialisedCalls == 3,
		"mcf5407_runtime_init reported the runtime usable on all three calls");

	if(g_failures)
	{
		std::cout << "t0_mcf5407_link: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_mcf5407_link: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
