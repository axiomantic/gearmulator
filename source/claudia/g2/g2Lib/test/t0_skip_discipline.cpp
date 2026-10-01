// Tier T0: this test runs with NMG2_ARTIFACTS unset and needs no firmware
// artifact of any kind.
//
// ---------------------------------------------------------------------------
// Why this test builds its own gated subjects
//
// The acceptance is: every gated test the build carries prints
// `SKIPPED: firmware artifact not available (NMG2_ARTIFACTS unset)`.
//
// The clause quantifies over the gated tests the build carries. When no test
// the build carries is gated, that set is EMPTY.
//
// A clause quantified over an EMPTY SET is vacuously true. Asserted as written,
// it would pass without exercising one line of the skip discipline.
//
// This test therefore CONSTRUCTS the gated tests it asserts over, through
// gatedFixture.h, which is the mechanism every later gated test will use. The
// subject set is non-empty by construction and the assertions are falsifiable.
// The vacuity is reported; it is not silently passed.
// ---------------------------------------------------------------------------

#include "gatedFixture.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#	define G2_POPEN  _popen
#	define G2_PCLOSE _pclose
#else
#	include <sys/wait.h>
#	define G2_POPEN  popen
#	define G2_PCLOSE pclose
#endif

#ifndef G2_GATED_EXECUTABLE
#	error "G2_GATED_EXECUTABLE must name a gated executable; without it the exit-code half of the discipline is unasserted"
#endif

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

	// Both literals are spelled out in full. Nothing here derives an expectation
	// from the header under test.
	const std::string g_expectedSkipLine = "SKIPPED: firmware artifact not available (NMG2_ARTIFACTS unset)";
	const std::string g_notVerified = "NOT VERIFIED";
	const std::string g_pass = "PASS";

	void setArtifactsVariable(const char* _value)
	{
#ifdef _WIN32
		if(_value)
			_putenv_s("NMG2_ARTIFACTS", _value);
		else
			_putenv_s("NMG2_ARTIFACTS", "");
#else
		if(_value)
			setenv("NMG2_ARTIFACTS", _value, 1);
		else
			unsetenv("NMG2_ARTIFACTS");
#endif
	}

	size_t countOccurrences(const std::string& _haystack, const std::string& _needle)
	{
		size_t count = 0;
		for(size_t pos = _haystack.find(_needle); pos != std::string::npos; pos = _haystack.find(_needle, pos + _needle.size()))
			++count;
		return count;
	}

	bool contains(const std::string& _haystack, const std::string& _needle)
	{
		return _haystack.find(_needle) != std::string::npos;
	}

	// The exit code is read EXPLICITLY. A child that never ran must report
	// "unproven" and never collect the tick meant for a child that exited 77.
	struct CommandResult
	{
		int         exitCode = -1;
		bool        ran      = false;
		std::string output;
	};

	std::string shellQuote(const std::string& _text)
	{
#if defined(_WIN32)
		return "\"" + _text + "\"";
#else
		std::string quoted = "'";
		for(const char c : _text)
		{
			if(c == '\'')
				quoted += "'\\''";
			else
				quoted += c;
		}
		quoted += "'";
		return quoted;
#endif
	}

	// The child inherits this process's environment, which is what lets the
	// setArtifactsVariable calls above decide what the gated executable sees.
	CommandResult runCommand(const std::string& _executable)
	{
		const std::string command = shellQuote(_executable) + " 2>&1";

		CommandResult result;

		FILE* const pipe = G2_POPEN(command.c_str(), "r");
		if(pipe == nullptr)
			return result;

		char buffer[4096];
		while(fgets(buffer, sizeof(buffer), pipe) != nullptr)
			result.output += buffer;

		const int status = G2_PCLOSE(pipe);

#if defined(_WIN32)
		result.ran      = status != -1;
		result.exitCode = status;
#else
		result.ran      = WIFEXITED(status) != 0;
		result.exitCode = result.ran ? WEXITSTATUS(status) : -1;
#endif

		return result;
	}
}

int main()
{
	try
	{
		// ---------------- every gated test skips, and prints the exact line
		//
		// Three gated tests, not one. "every gated test the build carries"
		// is a claim about a set, and a set of one cannot distinguish "every"
		// from "the first".

		setArtifactsVariable(nullptr);

		{
			g2::EnvArtifactResolver resolver;
			g2::test::GatedCounters counters;
			std::ostringstream out;

			int bodiesThatRan = 0;

			for(int i = 0; i < 3; ++i)
				g2::test::runGated(resolver, out, counters, [&] { ++bodiesThatRan; return true; });

			const std::string text = out.str();

			check(bodiesThatRan == 0, "unset: no gated body ran");
			check(counters.skipped == 3, "unset: all three gated tests counted as skipped");
			check(counters.run == 0, "unset: the run count is zero");
			check(countOccurrences(text, g_expectedSkipLine) == 3, "unset: every gated test printed the skip line word for word");

			// The line is asserted whole, and not as a prefix plus a substring.
			// A skip line that carried extra text after the message would still
			// satisfy a substring test and would not satisfy the skip discipline.
			check(text == g_expectedSkipLine + "\n" + g_expectedSkipLine + "\n" + g_expectedSkipLine + "\n",
				"unset: the printed text is exactly three skip lines and nothing else");

			// ---------------- a run that executed zero gated tests

			const std::string summary = g2::test::summaryLine(counters);
			std::cout << "     summary(zero run): " << summary << std::endl;

			check(contains(summary, g_notVerified), "zero run: the summary prints NOT VERIFIED");
			check(!contains(summary, g_pass), "zero run: the summary never prints PASS");
			check(contains(summary, "run=0"), "zero run: the summary carries the run count");
			check(contains(summary, "passed=0"), "zero run: the summary carries the passed count");
			check(contains(summary, "failed=0"), "zero run: the summary carries the failed count");
			check(contains(summary, "skipped=3"), "zero run: the summary carries the skipped count");
		}

		// ---------------- the NEGATIVE CASE
		//
		// "A negative case asserts that a job which ran one gated test does not
		// print not VERIFIED." Without it, summaryLine could return the
		// Not VERIFIED line unconditionally and every assertion above would
		// still hold.

		{
			setArtifactsVariable(".");

			g2::EnvArtifactResolver resolver;
			g2::test::GatedCounters counters;
			std::ostringstream out;

			int bodiesThatRan = 0;

			g2::test::runGated(resolver, out, counters, [&] { ++bodiesThatRan; return true; });

			check(bodiesThatRan == 1, "negative case: the gated body ran");
			check(counters.run == 1, "negative case: the run count is one");
			check(counters.passed == 1, "negative case: the passed count is one");
			check(counters.skipped == 0, "negative case: nothing was skipped");
			check(out.str().empty(), "negative case: a gated test that ran printed no skip line");

			const std::string summary = g2::test::summaryLine(counters);
			std::cout << "     summary(one run): " << summary << std::endl;

			check(!contains(summary, g_notVerified), "negative case: a job that ran one gated test does NOT print NOT VERIFIED");
			check(contains(summary, g_pass), "negative case: a job that ran one gated test prints PASS");
			check(contains(summary, "run=1"), "negative case: the summary carries the run count");
			check(contains(summary, "skipped=0"), "negative case: the summary carries the skipped count");
		}

		// ---------------- a gated test that RAN and FAILED
		//
		// PASS must be earned and not merely be the answer to "run > 0". A
		// summary that printed PASS for a failed run would let a red gated test
		// through, which is the same green-mirage shape from the other side.

		{
			setArtifactsVariable(".");

			g2::EnvArtifactResolver resolver;
			g2::test::GatedCounters counters;
			std::ostringstream out;

			g2::test::runGated(resolver, out, counters, [&] { return false; });

			check(counters.run == 1, "failing gated test: the run count is one");
			check(counters.failed == 1, "failing gated test: the failed count is one");
			check(counters.passed == 0, "failing gated test: the passed count is zero");

			const std::string summary = g2::test::summaryLine(counters);
			std::cout << "     summary(one failure): " << summary << std::endl;

			check(!contains(summary, g_notVerified), "failing gated test: the summary does not print NOT VERIFIED");
			check(!contains(summary, g_pass), "failing gated test: the summary does not print PASS");
			check(contains(summary, "FAIL"), "failing gated test: the summary prints FAIL");
		}

		// ---------------- a directory that is not there is also a skip, and it
		// prints message 2 rather than message 1.
		//
		// The gate is written against ArtifactResolver, not against getenv, so
		// it must close for every empty resolve and not only for the unset one.

		{
			const std::string missingDir = "/nmg2/no/such/directory/REPO-7";
			setArtifactsVariable(missingDir.c_str());

			g2::EnvArtifactResolver resolver;
			g2::test::GatedCounters counters;
			std::ostringstream out;

			int bodiesThatRan = 0;
			g2::test::runGated(resolver, out, counters, [&] { ++bodiesThatRan; return true; });

			const std::string expectedMissingSkipLine =
				"SKIPPED: firmware artifact not available (NMG2_ARTIFACTS names no directory: "
				+ missingDir + ")";

			check(bodiesThatRan == 0, "missing directory: the gated body did not run");
			check(out.str() == expectedMissingSkipLine + "\n",
				"missing directory: the skip line carries message 2 word for word");
		}

		// ---------------- the three exit codes, one per verdict
		//
		// Spelled as literals. Deriving an expectation from g_gatedSkipExitCode
		// would make this pass for any value the header happened to carry, and
		// 77 is not free to change: ctest's SKIP_RETURN_CODE and the automake
		// convention it comes from both fix it.

		{
			g2::test::GatedCounters zeroRun;
			zeroRun.skipped = 3;

			g2::test::GatedCounters onePassed;
			onePassed.run = 1;
			onePassed.passed = 1;

			g2::test::GatedCounters oneFailed;
			oneFailed.run = 1;
			oneFailed.failed = 1;

			g2::test::GatedCounters ranAndSkipped;
			ranAndSkipped.run = 1;
			ranAndSkipped.passed = 1;
			ranAndSkipped.skipped = 2;

			check(g2::test::gatedExitCode(zeroRun) == 77, "exit code: a run that executed zero gated bodies is 77");
			check(g2::test::gatedExitCode(onePassed) == 0, "exit code: a run whose gated bodies all passed is 0");
			check(g2::test::gatedExitCode(oneFailed) == 1, "exit code: a run with a failed gated body is 1");

			// The discriminator is `run`, not `skipped`. Without this, an
			// implementation keyed on `skipped > 0` would satisfy all three
			// clauses above and would report a real failure as a skip.
			check(g2::test::gatedExitCode(ranAndSkipped) == 0,
				"exit code: a run that executed one gated body is 0 even though other bodies skipped");
		}

		// ---------------- the half ctest reads
		//
		// Everything above asserts the skip MESSAGE. ctest reads no message. It
		// reads a process exit status, so the status is the half that decides
		// whether a skip is scored Skipped or Passed.
		//
		// The assertion therefore binds to the REAL gated executable, in the
		// same process shape ctest launches. Asserting gatedExitCode alone
		// would prove the mapping and say nothing about whether main returns
		// it, and a main that computes its own code is the shape that turns a
		// skip back into a green tick.
		//
		// 77 is spelled out here and derived from nothing in the header, so an
		// edit to the header cannot move this expectation with it.

		{
			setArtifactsVariable(nullptr);

			const CommandResult gated = runCommand(G2_GATED_EXECUTABLE);

			std::cout << "     gated executable: " << G2_GATED_EXECUTABLE << std::endl;
			std::cout << "     gated exit code:  " << gated.exitCode << std::endl;

			check(gated.ran, "gated executable: the child ran to completion and its exit status is readable");
			check(gated.exitCode == 77,
				"gated executable: a run that executed zero gated bodies exits 77, which ctest scores "
				"Skipped rather than Passed; got " + std::to_string(gated.exitCode));

			// Ties the 77 to a SKIP. Without these, an executable that failed
			// to start, or exited 77 for an unrelated reason, would satisfy the
			// clause above.
			check(contains(gated.output, g_expectedSkipLine),
				"gated executable: the 77 came with the skip line, so it reports a skip and not some other exit");
			check(contains(gated.output, "run=0 passed=0 failed=0"),
				"gated executable: the 77 came with a summary that executed no gated body");
			check(contains(gated.output, g_notVerified) && !contains(gated.output, g_pass),
				"gated executable: the summary reports NOT VERIFIED and never PASS");
		}

		setArtifactsVariable(nullptr);
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL the skip discipline threw std::exception: " << _e.what() << std::endl;
		++g_failures;
	}
	catch(...)
	{
		std::cout << "FAIL the skip discipline threw a non-std exception" << std::endl;
		++g_failures;
	}

	if(g_failures)
	{
		std::cout << "t0_skip_discipline: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_skip_discipline: all checks passed" << std::endl;
	return 0;
}
