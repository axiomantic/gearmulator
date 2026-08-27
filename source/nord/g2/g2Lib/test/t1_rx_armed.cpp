// `--impulse` DOES NOT ENTER THE PLAY PHASE UNTIL THE AUDIO PATH IS ARMED.
//
// Tier T1 and gated: the child boots the real firmware out of NMG2_ARTIFACTS,
// so this test skips with the section 18.5 skip line when the artifacts are
// absent. It resolves through ArtifactResolver and never through getenv.
//
// ---------------------------------------------------------------------------
// WHAT THIS TEST HOLDS, AND WHY THE PREDICATE IT HOLDS IS NOT THE OBVIOUS ONE
//
// `--impulse` used to leave its boot drive the moment every DSP position
// reported `programLanded`. That predicate is about PROGRAM LOADING and says
// nothing about audio, and the two are separated by more than a factor of five
// in this firmware: programLanded goes true at boot iteration 44,515, and the
// ESAI RECEIVE DMA request is not armed on any position until 231,296. At the
// moment of the boot-time DMA configuration the rx-arming code is not even
// resident -- P:$000250-$000270 disassembles as all zeros -- because it arrives
// in a later-loaded DSP program.
//
// So the old exit handed `beginPlayPhase()` a machine whose receive path was
// still dead, and the command's `OUTCOME=STOPPED` was a statement about
// TRANSPORT NOT YET EXISTING rather than about routing. The emulator was right
// and the instrument stopped too early.
//
// THE PREDICATE THIS TEST HOLDS IS THEREFORE AN OBSERVATION OF THE AUDIO PATH:
// Dma::hasTrigger(EsaiReceiveData) true on EVERY position. That is the DMA
// request registration itself, not a proxy for it.
//
// WHAT WOULD MAKE THIS PREDICATE WRONG, stated here because a predicate whose
// failure mode is unstated is a predicate nobody can re-check:
//
//   1. hasTrigger is STICKY only because finishTransfer clears DE without
//      calling removeTriggerTarget. If dsp56300 ever unregisters on completion,
//      the predicate becomes a race: it would go false between transfers and
//      the drive could run to its bound on a healthy machine.
//   2. setDCR UNREGISTERS the trigger target on any reconfiguration. A kernel
//      that arms the channel and then rewrites DCR would show a window in which
//      hasTrigger is false. The predicate reads it every iteration, so it
//      latches the first true and does not depend on the value persisting --
//      but a run that observed only such a window would be arming that has
//      since been withdrawn.
//   3. hasTrigger reports REGISTRATION, not TRAFFIC. A channel registered
//      against a source that never asserts would satisfy it forever. That is
//      why the DDR2 assertion below is separate: it reads the destination
//      pointer the DMA actually advanced.
//
// Point 1 is what makes this a bounded drive rather than a spin. The predicate
// is checked against `g_iterations` exactly as the old one was, and a run that
// reaches the bound without arming reports DID-NOT-RUN and exits 2 -- it does
// NOT report STOPPED, because a machine whose receive path never came up did
// not measure the chain at all.
//
// Everything asserted here is a process EXIT STATUS or bytes on a standard
// stream. No assert() carries a predicate: the default build is Release with
// NDEBUG and would delete it.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "gatedFixture.h"

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

	struct CommandResult
	{
		int         exitCode = -1;
		bool        ran      = false;
		std::string output;
	};

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

	std::vector<std::string> splitLines(const std::string& _output)
	{
		std::vector<std::string> lines;
		std::string              current;

		for(const char c : _output)
		{
			if(c == '\n')
			{
				if(!current.empty() && current.back() == '\r')
					current.pop_back();
				lines.push_back(current);
				current.clear();
				continue;
			}
			current += c;
		}

		if(!current.empty())
			lines.push_back(current);

		return lines;
	}

	// The outcome is a WHOLE LINE and not a substring anywhere in the output.
	// A substring search would be satisfied by the usage text or by a longer
	// line that merely contains this one.
	bool carriesLine(const std::vector<std::string>& _lines, const std::string& _line)
	{
		for(const std::string& l : _lines)
			if(l == _line)
				return true;
		return false;
	}

	// The value of one `key=value` token on one line, or the empty string.
	// The match is on ` key=` so that `DE2everSet=` can never be read as
	// `everSet=` and `rxRequestArmed=` never as `RequestArmed=`.
	std::string tokenValue(const std::string& _line, const std::string& _key)
	{
		const std::string needle = " " + _key + "=";
		const size_t      at     = _line.find(needle);

		if(at == std::string::npos)
			return std::string();

		const size_t from = at + needle.size();
		const size_t to   = _line.find(' ', from);

		return _line.substr(from, to == std::string::npos ? std::string::npos : to - from);
	}

	// The `DDR2=[low,high]` token of one ESAI line, split into its two ends.
	// The receive channel's DESTINATION pointer advances on each transfer, so
	// `low != high` is the DMA having moved and not merely having registered.
	bool ddr2Moved(const std::string& _line)
	{
		const std::string value = tokenValue(_line, "DDR2");

		const size_t comma = value.find(',');
		if(comma == std::string::npos)
			return false;

		const std::string low  = value.substr(1, comma - 1);
		const std::string high = value.substr(comma + 1, value.size() - comma - 2);

		return !low.empty() && !high.empty() && low != high;
	}

	// ONE ENTRY PER DSP POSITION. The population is `rx-probe: esai port N`
	// lines, of which the command prints exactly one per position -- so the
	// count below is a count of POSITIONS and not of lines that happen to
	// mention a port.
	struct EsaiLine
	{
		std::string rxRequestArmed;
		std::string txRequestArmed;
		std::string de2everSet;
		bool        ddr2Moved = false;
	};

	std::vector<EsaiLine> esaiLines(const std::vector<std::string>& _lines)
	{
		std::vector<EsaiLine> found;

		const std::string prefix = "rx-probe: esai port ";

		for(const std::string& l : _lines)
		{
			if(l.compare(0, prefix.size(), prefix) != 0)
				continue;

			EsaiLine e;
			e.rxRequestArmed = tokenValue(l, "rxRequestArmed");
			e.txRequestArmed = tokenValue(l, "txRequestArmed");
			e.de2everSet     = tokenValue(l, "DE2everSet");
			e.ddr2Moved      = ddr2Moved(l);
			found.push_back(e);
		}

		return found;
	}

	bool body()
	{
		const int before = g_failures;

		const CommandResult run = runConsole({"--impulse", "--rx-probe"});

		const std::vector<std::string> lines = splitLines(run.output);

		check(run.ran, "--impulse --rx-probe: the child ran to completion and its exit status is readable");

		// THE CAPTURE'S OWN KNOWN POSITIVE. Every assertion below is a claim
		// about a line in this capture, and all of them would be red together
		// on an empty capture without saying why. This line the command has
		// printed since INT-2 proves the pipe carried the child's output and
		// that the line splitter works.
		check(carriesLine(lines, "impulse: dspCount=8 hopFrames=1 lookaheadFrames=1 D_chain=7 D_codec=0"),
			"the capture is real: the command's first figure line is found whole");

		// ---------------- the predicate the command reports for itself
		//
		// The command names WHY it left the boot drive, and the successful
		// answer is one fixed line. A drive that reached its bound prints a
		// different word and a smaller count, so this cannot be satisfied by a
		// run that gave up.
		check(carriesLine(lines, "impulse: bootExit=rx-armed rxArmedPorts=8/8"),
			"--impulse leaves the boot drive because the ESAI receive DMA is armed on all eight positions, "
			"and it says so in one whole line");

		// ---------------- the audio path itself, read by a separate instrument
		//
		// The line above is the command's own claim about its own predicate. It
		// is worth exactly nothing on its own: a predicate that printed
		// `rx-armed` unconditionally would satisfy it. THIS is the independent
		// reading -- the probe's own accumulation of Dma::hasTrigger over the
		// play phase, which is a different structure sampled at a different
		// time.
		const std::vector<EsaiLine> esai = esaiLines(lines);

		check(esai.size() == 8,
			"the probe reported one ESAI line per DSP position; expected 8, got " + std::to_string(esai.size()));

		unsigned rxArmed  = 0;
		unsigned txArmed  = 0;
		unsigned de2Set   = 0;

		for(const EsaiLine& e : esai)
		{
			if(e.rxRequestArmed == "1") ++rxArmed;
			if(e.txRequestArmed == "1") ++txArmed;
			if(e.de2everSet     == "1") ++de2Set;
		}

		// THE KNOWN POSITIVE FOR THIS PARSER, AND IT IS NOT DECORATION. The
		// TRANSMIT channel was already armed under the old predicate, so a
		// parser that could never extract a 1 -- a wrong key, a wrong
		// delimiter, an off-by-one substr -- is red HERE while the receive
		// claim below would be red for a reason that has nothing to do with
		// the machine. The two are read by the same function from the same
		// lines, which is the whole point.
		check(txArmed == 8,
			"KNOWN POSITIVE: the parser reads txRequestArmed=1 on all eight positions, so it can extract a 1 "
			"from these lines; got " + std::to_string(txArmed) + "/8");

		check(rxArmed == 8,
			"the ESAI receive DMA request is armed on all eight positions when the play phase begins; got "
			+ std::to_string(rxArmed) + "/8");

		check(de2Set == 8,
			"the receive channel's DE bit was observed set on all eight positions; got "
			+ std::to_string(de2Set) + "/8");

		// ---------------- the channel MOVED, and did not merely register
		//
		// hasTrigger reports REGISTRATION and not TRAFFIC, so a channel
		// registered against a source that never asserts would satisfy every
		// assertion above. The receive channel's DESTINATION pointer is the
		// traffic: DDR2 advances once per transferred word, so `min != max`
		// over the walk is transfers having happened. Under the old predicate
		// the probe reported DDR2=[$001C00,$001C00] on seven positions and
		// [$001C04,$001C04] on the head -- an interval of zero width, on every
		// one of them.
		//
		// THE BUFFER CONTENTS ARE NOT ASSERTED, DELIBERATELY. The probe also
		// reports which positions had a non-zero word land in the receive
		// buffer, and that count is NOT stable: five runs of the same binary on
		// the same artifacts gave 2, 4, 4, 1 and 1 of eight, with different
		// positions and different values each time. DDR2's interval was
		// identical in all five. A gate built on the varying figure would be a
		// flake; the varying figure is a finding about the emulator's
		// determinism and belongs in a report, not in a check.
		unsigned rxChannelsMoved = 0;
		for(const EsaiLine& e : esai)
			if(e.ddr2Moved)
				++rxChannelsMoved;

		check(rxChannelsMoved == 8,
			"the receive DMA's destination pointer advanced on all eight positions during the walk, so the armed "
			"channel carried transfers and not just a registration; got "
			+ std::to_string(rxChannelsMoved) + "/8");

		// The command's exit status is NOT asserted to be 0. An unpatched Nord
		// Modular is silent by design, so STOPPED (exit 1) is the answer this
		// desk reaches and this test makes no audio claim. What it refuses is
		// DID-NOT-RUN, which is what the command now returns when the drive
		// reaches its bound without arming -- and which the old predicate could
		// never produce because it never looked.
		check(run.exitCode != 2,
			"--impulse did not report DID-NOT-RUN: the receive path came up within the drive bound; exit status was "
			+ std::to_string(run.exitCode));

		return g_failures == before;
	}
}

int main()
{
	g2::EnvArtifactResolver  resolver;
	g2::test::GatedCounters  counters;

	try
	{
		g2::test::runGated(resolver, std::cout, counters, body);
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL t1_rx_armed threw std::exception: " << _e.what() << std::endl;
		return 1;
	}
	catch(...)
	{
		std::cout << "FAIL t1_rx_armed threw a non-std exception" << std::endl;
		return 1;
	}

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
