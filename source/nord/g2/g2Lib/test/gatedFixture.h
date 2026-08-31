#pragma once

// Task REPO-7, the skip discipline. Design section 18.5, plan section 5.2
// rules 2 and 3.
//
// This is the mechanism EVERY firmware-gated test in this repository uses. A
// gated test that cannot run must skip WITH A REASON and must never pass
// silently.
//
// Header-only on purpose: REPO-7's Files: line names this header and no
// translation unit, and a gated test needs the fixture at compile time.
//
// The fixture is written against ArtifactResolver and NEVER against getenv, so
// that a later fetch implementation of design section 4.2 changes no test.
// Design section 18.5 step 1 states that requirement directly.

#include "../artifactResolver.h"

#include <cstddef>
#include <ostream>
#include <string>

namespace g2
{
	namespace test
	{
		// Section 18.5's skip line is section 4.2's message with this prefix.
		// It is built here by CONCATENATION and is not spelled out a second
		// time: a message with two texts is a message with two meanings, and
		// the one an implementer copies is the one that drifts.
		constexpr const char* g_skippedPrefix = "SKIPPED: ";

		// The three verdict tokens. They are UPPERCASE and the count labels
		// below are lowercase, which is what lets "the summary never prints
		// PASS" be a case-sensitive test that `passed=0` cannot satisfy by
		// accident.
		constexpr const char* g_verdictPass = "PASS";
		constexpr const char* g_verdictFail = "FAIL";
		constexpr const char* g_verdictNotVerified = "NOT VERIFIED";

		// The four counts design section 18.5 step 3 names.
		//
		// `run` counts gated tests that EXECUTED THEIR BODY. A skipped test is
		// not a run test, which is what makes "a run that executed zero gated
		// tests" the condition `run == 0`.
		struct GatedCounters
		{
			std::size_t run = 0;
			std::size_t passed = 0;
			std::size_t failed = 0;
			std::size_t skipped = 0;
		};

		// Runs _body only when the resolver yields an artifact directory.
		//
		// When it does not, writes the skip line to _out and counts one skip.
		// _body returns true when the gated test passed.
		//
		// Returns true when the body ran, which is NOT the same as the body
		// passing: a caller that conflated the two would report a failed gated
		// test as a skip.
		template<typename Body>
		bool runGated(ArtifactResolver& _resolver, std::ostream& _out, GatedCounters& _counters, Body&& _body)
		{
			std::string why;
			const std::string directory = _resolver.resolve(why);

			if(directory.empty())
			{
				_out << g_skippedPrefix << why << '\n';
				++_counters.skipped;
				return false;
			}

			++_counters.run;

			if(_body())
				++_counters.passed;
			else
				++_counters.failed;

			return true;
		}

		// The exit code a skipped run carries to ctest.
		//
		// ctest reads a process exit status and never a summary line, so a
		// verdict that lives only in the text is a verdict ctest cannot act on.
		// Exiting 0 on a skip is scored Passed, which is the same answer ctest
		// gives a run that executed every gated body and verified the firmware.
		//
		// tests_int.cmake reads this number out of this header and hands it to
		// SKIP_RETURN_CODE, and fails the configure when it cannot find it. The
		// two spellings therefore cannot drift apart in silence.
		constexpr int g_gatedSkipExitCode = 77;

		// A run that executed no gated body is neither a pass nor a failure, and
		// `run == 0` is tested FIRST for the same reason summaryLine tests it
		// first: an artifact-less machine must not be failed, and must not be
		// reported as having verified anything either.
		inline int gatedExitCode(const GatedCounters& _counters)
		{
			if(_counters.run == 0)
				return g_gatedSkipExitCode;

			return _counters.failed > 0 ? 1 : 0;
		}

		// The summary line design section 18.5 step 3 requires, carrying the
		// four counts, and step 4's rule:
		//
		//   "A run that executed zero gated tests is reported as NOT VERIFIED,
		//    not as PASS."
		//
		// A green tick on a run that verified nothing is the failure the whole
		// discipline exists to prevent, so `run == 0` is tested FIRST and the
		// PASS branch is unreachable from it.
		inline std::string summaryLine(const GatedCounters& _counters)
		{
			const char* verdict = g_verdictNotVerified;

			if(_counters.run > 0)
				verdict = _counters.failed > 0 ? g_verdictFail : g_verdictPass;

			return "gated tests: run=" + std::to_string(_counters.run) +
				" passed=" + std::to_string(_counters.passed) +
				" failed=" + std::to_string(_counters.failed) +
				" skipped=" + std::to_string(_counters.skipped) +
				" -- " + verdict;
		}
	}
}
