#pragma once

// The skip discipline for firmware-gated tests. A gated test that cannot run
// must skip with a reason and must never pass silently.
//
// Header-only on purpose: a gated test needs the fixture at compile time.
//
// The fixture is written against ArtifactResolver and never against getenv, so
// that a later fetch implementation changes no test.

#include "../artifactResolver.h"

#include <cstddef>
#include <ostream>
#include <string>

namespace g2
{
	namespace test
	{
		// The skip line is the resolver's message with this prefix. It is
		// built here by concatenation and is not spelled out a second time: a
		// message with two texts is a message with two meanings, and the one
		// an implementer copies is the one that drifts.
		constexpr const char* g_skippedPrefix = "SKIPPED: ";

		// The verdict tokens are uppercase and the count labels below are
		// lowercase, so a case-sensitive search for PASS in a summary line is
		// not satisfied by `passed=0`.
		constexpr const char* g_verdictPass = "PASS";
		constexpr const char* g_verdictFail = "FAIL";
		constexpr const char* g_verdictNotVerified = "NOT VERIFIED";

		// `run` counts gated tests that executed their body. A skipped test is
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
		// Returns true when the body ran, which is not the same as the body
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
		// `run == 0` is tested first for the same reason summaryLine tests it
		// first: an artifact-less machine must not be failed, and must not be
		// reported as having verified anything either.
		inline int gatedExitCode(const GatedCounters& _counters)
		{
			if(_counters.run == 0)
				return g_gatedSkipExitCode;

			return _counters.failed > 0 ? 1 : 0;
		}

		// The summary line, carrying the counts, and the rule:
		//
		//   "A run that executed zero gated tests is reported as NOT VERIFIED,
		//    not as PASS."
		//
		// A green tick on a run that verified nothing is the failure the whole
		// discipline exists to prevent, so `run == 0` is tested first and the
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
