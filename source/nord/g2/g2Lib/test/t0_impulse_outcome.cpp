// `--impulse` reports an outcome word, and the outcomes do not look alike.
//
// Tier T0: the child this test spawns runs with NMG2_ARTIFACTS unset, so it
// boots no firmware and needs no artifact.
//
// `--impulse` is a transport probe. It answers "does the path carry data", and
// it never answers "does the machine make music" -- the default state of a Nord
// Modular is to not play sound, and sound comes from loading patches, so a
// silent chain on an unpatched machine is the emulator agreeing with the
// hardware.
//
// Three different answers otherwise wear the same shape:
//
//   - the machine never reached the play phase,
//   - the chain reached the play phase and carried nothing,
//   - the observer never saw a single frame and therefore could not have
//     reported an arrival whatever the chain did.
//
// The third is the dangerous one: an unobserved impulse and a blind observer
// print the same `arrival=-1`. So this test requires the command to name which
// of the outcomes it reached, in one word, on one line.
//
// Everything asserted here is a process exit status or bytes on a standard
// stream. No assert() carries a predicate: the default build is Release with
// NDEBUG and would delete it.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "g2TestConsole/impulseOutcome.h"

#if defined(_WIN32)
#	define G2_POPEN  _popen
#	define G2_PCLOSE _pclose
#else
#	include <sys/wait.h>
#	define G2_POPEN  popen
#	define G2_PCLOSE pclose
#endif

#ifndef G2_TEST_CONSOLE_EXECUTABLE
#	error "G2_TEST_CONSOLE_EXECUTABLE must name the g2TestConsole binary; without it nothing here reads an exit status"
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

	CommandResult runConsole(const std::vector<std::string>& _args)
	{
		std::string command = shellQuote(G2_TEST_CONSOLE_EXECUTABLE);

		for(const std::string& arg : _args)
			command += " " + shellQuote(arg);

		command += " 2>&1";

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

	// The outcome is a whole line and not a substring anywhere in the output.
	// A substring search would be satisfied by the usage text, by a comment
	// echoed back, or by a longer word that merely contains this one.
	bool carriesLine(const std::string& _output, const std::string& _line)
	{
		std::string current;

		for(const char c : _output)
		{
			if(c == '\n')
			{
				if(!current.empty() && current.back() == '\r')
					current.pop_back();

				if(current == _line)
					return true;

				current.clear();
				continue;
			}
			current += c;
		}

		return current == _line;
	}

	void unsetArtifacts()
	{
#ifdef _WIN32
		_putenv_s("NMG2_ARTIFACTS", "");
#else
		unsetenv("NMG2_ARTIFACTS");
#endif
	}
}

int main()
{
	try
	{
		unsetArtifacts();

		// ---------------- the machine that never ran says so in one word
		//
		// With no artifact there is no image, so no board is placed and no play
		// phase is entered. The command must not report this as a silent chain:
		// nothing was measured at all, and the word has to say that.

		const CommandResult run = runConsole({"--impulse"});

		std::cout << run.output;

		check(run.ran, "--impulse: the child ran to completion and its exit status is readable");

		check(carriesLine(run.output, "impulse: OUTCOME=DID-NOT-RUN"),
			"--impulse with no artifact prints the whole line `impulse: OUTCOME=DID-NOT-RUN`, "
			"so a machine that never reached the play phase is not read as a chain that carried nothing");

		check(run.exitCode == 2,
			"--impulse with no artifact exits 2; got " + std::to_string(run.exitCode));

		// A known-positive control on this test's own line matcher. The three
		// assertions above are all negative-shaped -- each would pass on an
		// empty capture if `carriesLine` were broken or the pipe returned
		// nothing. This one names a line the command always prints, so a matcher
		// that can never match anything is red here before it is quietly red
		// above.
		check(carriesLine(run.output, "g2TestConsole: --impulse: failed with exit status 2"),
			"the capture is real and the line matcher matches: the refusal line INT-2 already printed is found whole");

		// ---------------- the arms a machine on this desk cannot reach
		//
		// An unpatched Nord Modular is silent by design, so the arm this
		// hardware reaches with an artifact present is STOPPED and no run on
		// this desk can exercise PROPAGATED. The classification is therefore a
		// free function over a plain record, and every arm is driven here
		// directly. The point of the sweep is the pair: `blind` and `stopped`
		// differ in exactly one field, and they must not produce one answer.

		using g2console::ImpulseObservation;
		using g2console::classify;
		using g2console::exitStatus;
		using g2console::name;

		// The record a healthy, propagating chain would produce. Every case
		// below is this one with the named field changed, so each assertion
		// names its own single cause.
		ImpulseObservation good;
		good.reachedPlayPhase = true;
		good.observerSelfTest = true;
		good.framesPulled     = 1031;
		good.arrival          = 7;
		good.arrivalExact     = true;
		good.expectedArrival  = 7;
		good.countersZero     = true;
		good.sinkControlArrival = 0;
		good.sinkControlExact   = true;

		check(std::string(name(classify(good))) == "PROPAGATED",
			"a pattern that arrived unchanged at the derived frame, beside zero counters, is PROPAGATED");

		ImpulseObservation neverRan = good;
		neverRan.reachedPlayPhase  = false;

		check(std::string(name(classify(neverRan))) == "DID-NOT-RUN",
			"a machine that never reached the play phase is DID-NOT-RUN even when every other field reads healthy");

		// `blind` and `stopped` are the same record but for framesPulled, and
		// both carry arrival=-1. A report of "no arrival" from an observer that
		// received nothing says nothing at all about the chain.
		ImpulseObservation blind = good;
		blind.framesPulled       = 0;
		blind.arrival            = -1;
		blind.arrivalExact       = false;

		ImpulseObservation stopped = good;
		stopped.framesPulled      = 1031;
		stopped.arrival           = -1;
		stopped.arrivalExact      = false;

		check(std::string(name(classify(blind))) == "INSTRUMENT-BLIND",
			"an observer the sink delivered ZERO frames to is INSTRUMENT-BLIND and is NOT reported as a chain that carried nothing");

		check(std::string(name(classify(stopped))) == "STOPPED",
			"an observer that received 1031 frames and saw the pattern in none of them is STOPPED: the chain did not carry it");

		check(std::string(name(classify(blind))) != std::string(name(classify(stopped))),
			"the two records differ in exactly one field and the two outcomes differ: an unobserved impulse and a blind observer are told apart");

		// The observer's own control. A detector that cannot detect must not be
		// allowed to report an absence, whatever the sink delivered.
		ImpulseObservation brokenObserver = stopped;
		brokenObserver.framesPulled       = 1031;
		brokenObserver.observerSelfTest   = false;

		check(std::string(name(classify(brokenObserver))) == "INSTRUMENT-BLIND",
			"a failed observer self-test is INSTRUMENT-BLIND even with frames in hand: a detector that cannot detect cannot report an absence");

		ImpulseObservation goodButBroken = good;
		goodButBroken.observerSelfTest   = false;

		check(std::string(name(classify(goodButBroken))) == "INSTRUMENT-BLIND",
			"a failed observer self-test is INSTRUMENT-BLIND even when the record claims an exact arrival");

		// THE ARRIVAL INSTRUMENT'S OWN KNOWN POSITIVE, WHICH IS NOT THE
		// COMPARATOR SELF-TEST ABOVE. observerSelfTest drives the two
		// predicates over two frames the PROGRAM built and never over a frame
		// the SINK delivered, so it holds whatever the arrival path does. The
		// three records below are `stopped` with only the control's own two
		// fields moved, and each names a different way the arrival path can be
		// dead while every other field still reads healthy.
		ImpulseObservation deadSink = stopped;
		deadSink.sinkControlArrival = -1;
		deadSink.sinkControlExact   = false;

		check(std::string(name(classify(deadSink))) == "INSTRUMENT-BLIND",
			"a sink control that never arrived is INSTRUMENT-BLIND: an arrival path that cannot report a frame it was HANDED cannot report an absence either");

		check(std::string(name(classify(deadSink))) != std::string(name(classify(stopped))),
			"deadSink and stopped differ in the control's fields alone and the two outcomes differ: a dead arrival path is told apart from a chain that carried nothing");

		ImpulseObservation corruptSink = stopped;
		corruptSink.sinkControlArrival = 0;
		corruptSink.sinkControlExact   = false;

		check(std::string(name(classify(corruptSink))) == "INSTRUMENT-BLIND",
			"a sink control that arrived CHANGED is INSTRUMENT-BLIND: a path that mangles a known value cannot be trusted to have carried an unknown one unchanged");

		ImpulseObservation claimedButUnproven = good;
		claimedButUnproven.sinkControlArrival = -1;
		claimedButUnproven.sinkControlExact   = false;

		check(std::string(name(classify(claimedButUnproven))) == "INSTRUMENT-BLIND",
			"an arrival claimed by an arrival path whose control never arrived is INSTRUMENT-BLIND, not PROPAGATED");

		ImpulseObservation late = good;
		late.arrival            = 9;

		ImpulseObservation changed = good;
		changed.arrivalExact       = false;

		ImpulseObservation noisy = good;
		noisy.countersZero       = false;

		check(std::string(name(classify(late))) == "PROPAGATED-OFF-SPEC",
			"a pattern that arrived at frame 9 against a derived expectation of 7 is PROPAGATED-OFF-SPEC and not PROPAGATED");

		check(std::string(name(classify(changed))) == "PROPAGATED-OFF-SPEC",
			"a pattern that arrived at the right frame but changed on the way is PROPAGATED-OFF-SPEC");

		check(std::string(name(classify(noisy))) == "PROPAGATED-OFF-SPEC",
			"a pattern that arrived exactly, beside a non-zero chain-health counter, is PROPAGATED-OFF-SPEC");

		// ---------------- the status a caller reads separates the answers
		//
		// A caller that reads only the exit status still learns which of the
		// three non-success answers it got. Every one of these would collapse
		// if exitStatus() returned a constant.

		check(exitStatus(classify(good)) == 0,
			"PROPAGATED exits 0; got " + std::to_string(exitStatus(classify(good))));
		check(exitStatus(classify(stopped)) == 1,
			"STOPPED exits 1 -- the answer is NO; got " + std::to_string(exitStatus(classify(stopped))));
		check(exitStatus(classify(late)) == 1,
			"PROPAGATED-OFF-SPEC exits 1; got " + std::to_string(exitStatus(classify(late))));
		check(exitStatus(classify(neverRan)) == 2,
			"DID-NOT-RUN exits 2 -- there was no run; got " + std::to_string(exitStatus(classify(neverRan))));
		check(exitStatus(classify(blind)) == 3,
			"INSTRUMENT-BLIND exits 3 -- there is no answer; got " + std::to_string(exitStatus(classify(blind))));

		// The words themselves are pairwise distinct. A name() that returned one
		// string for two arms would satisfy every equality above that happened
		// to name that string, and this is what refuses it.
		const std::vector<std::string> words = {
			name(classify(good)), name(classify(neverRan)), name(classify(blind)),
			name(classify(stopped)), name(classify(late))};

		bool allDistinct = true;
		for(size_t a = 0; a < words.size(); ++a)
			for(size_t b = a + 1; b < words.size(); ++b)
				if(words[a] == words[b])
					allDistinct = false;

		check(allDistinct,
			"the five outcome words are pairwise distinct, so no two answers wear the same word");
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL t0_impulse_outcome threw std::exception: " << _e.what() << std::endl;
		++g_failures;
	}
	catch(...)
	{
		std::cout << "FAIL t0_impulse_outcome threw a non-std exception" << std::endl;
		++g_failures;
	}

	if(g_failures)
	{
		std::cout << "t0_impulse_outcome: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_impulse_outcome: all checks passed" << std::endl;
	return 0;
}
