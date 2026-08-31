// Task REPO-7. Tier T0: this test runs with NMG2_ARTIFACTS unset and needs no
// firmware artifact of any kind.
//
// Plan section 9.2, REPO-7. Design section 18.5. Plan section 5.2 rules 2 and 3.
//
// ---------------------------------------------------------------------------
// Why this test builds its own gated subjects
//
// REPO-7's check reads: "Every gated test the build carries prints
// `SKIPPED: firmware artifact not available (NMG2_ARTIFACTS unset)`."
//
// MEASURED, at the moment REPO-7 and its dependencies are complete and nothing
// later is built: the build carries THIRTY-TWO registered tests and ZERO of
// them are gated. `ctest -N` lists 31 inherited upstream tests plus REPO-5's
// `t0_artifact_resolver`, and `t0_artifact_resolver` is T0 -- it drives the
// resolver directly and needs no artifact. A grep for NMG2_ARTIFACTS across
// source/nord/ finds only REPO-5's own two files and this task's.
//
// A clause quantified over an EMPTY SET is vacuously true. Asserted as written,
// it would pass without exercising one line of the skip discipline -- the exact
// shape this task's own block already caught one clause over, where the plan
// removed the standing `-R t1_` sweep from this gate because at Wave 2b it
// "matches nothing and exits 8".
//
// This test therefore CONSTRUCTS the gated tests it asserts over, through
// gatedFixture.h, which is the mechanism every later gated test will use. The
// subject set is non-empty by construction and the assertions are falsifiable.
// The vacuity is reported as a plan defect; it is not silently passed.
// ---------------------------------------------------------------------------

#include "gatedFixture.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
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
			// satisfy a substring test and would not satisfy section 18.5.
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
