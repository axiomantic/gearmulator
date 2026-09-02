/* Drives `g2TestConsole --boot --dump-dsp-dma`.
 *
 * It drives the binary and not a library call. The check -- the expected
 * DDR2/DCO2/DCO4 triple for each chain position, the firmware's own
 * position-to-port table, and the conjunction that turns the rows into one
 * verdict -- lives in g2TestConsole/main.cpp. A test that re-implemented it
 * would assert its own copy and would stay green while the shipped one
 * rotted. So the operator-facing command is what runs, and its stdout is what
 * is read.
 *
 * The command prints a PASS row for a position whose registers match and a
 * FAIL row for one that does not, and it prints one verdict line under the
 * rows. Both are asserted here: asserting only `dsp-dma=PASS` would pass over
 * a run with no rows at all, and asserting only the rows would pass over a
 * verdict computed as something other than their conjunction.
 *
 * It is tier T1 and gated because it boots the firmware: the registers it
 * reads are the ones the emulated kernel programmed, which is the only state
 * the check has any meaning against. A machine without the artifacts prints
 * the skip line and reports NOT VERIFIED.
 */

#include "gatedFixture.h"

/* popen and pclose are spelled with a leading underscore on Windows and the
 * null device has a different name there. Both are answered here. */
#ifdef _WIN32
#	define G2_POPEN  _popen
#	define G2_PCLOSE _pclose
#	define G2_DEVNULL "nul"
#else
#	define G2_POPEN  popen
#	define G2_PCLOSE pclose
#	define G2_DEVNULL "/dev/null"
#endif

#include <cstdio>
#include <cstdlib>
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
			std::cout << "ok   " << _what << '\n';
			return;
		}
		std::cout << "FAIL " << _what << '\n';
		++g_failures;
	}

	/* Runs the command and captures its stdout. Returns false when the pipe
	 * could not be opened at all, which is a broken harness and not a failed
	 * check -- the two are reported differently below. */
	bool capture(const std::string& _command, std::string& _output, int& _exitStatus)
	{
		std::FILE* const pipe = G2_POPEN(_command.c_str(), "r");

		if(pipe == nullptr)
			return false;

		std::string out;
		char        buffer[4096];

		while(std::fgets(buffer, sizeof buffer, pipe) != nullptr)
			out += buffer;

		const int status = G2_PCLOSE(pipe);

		_output     = out;
		_exitStatus = status;
		return true;
	}

	// The number of `  position N ... PASS` rows, and separately the number of
	// rows whose verdict is FAIL. Counted from the text and never estimated:
	// the row count is what makes the verdict non-vacuous.
	void countRows(const std::string& _output, unsigned& _rows, unsigned& _failRows)
	{
		std::istringstream lines(_output);
		std::string        line;

		_rows     = 0;
		_failRows = 0;

		while(std::getline(lines, line))
		{
			if(line.rfind("  position ", 0) != 0)
				continue;

			++_rows;

			if(line.size() >= 4 && line.compare(line.size() - 4, 4, "FAIL") == 0)
				++_failRows;
		}
	}

	bool body()
	{
		/* The binary under test, given by the build. A path composed here
		 * would name a layout rather than a target and would go stale with
		 * the first generator change. */
		const std::string command =
			std::string("\"") + G2_TEST_CONSOLE_BINARY + "\" --boot --dump-dsp-dma 2>" G2_DEVNULL;

		std::string output;
		int         status = -1;

		if(!capture(command, output, status))
		{
			std::cout << "FAIL could not run " << G2_TEST_CONSOLE_BINARY << '\n';
			++g_failures;
			return false;
		}

		check(status == 0,
			"--boot --dump-dsp-dma exits 0 on a machine that booted and matched");

		// The positions line, which names the population the rows come from.
		const auto positions = output.find("dsp-dma positions=8");
		check(positions != std::string::npos,
			"the check reports eight DSP positions, which is the set the board holds");

		unsigned rows     = 0;
		unsigned failRows = 0;
		countRows(output, rows, failRows);

		// Cardinality first. A verdict read out of a run with no rows behind it
		// says nothing, so the row count is asserted before the rows are.
		check(rows == 8,
			"one row for each of the eight positions, so the verdict is not read "
			"from an empty set");
		check(failRows == 0,
			"no position reports FAIL");

		check(output.find("dsp-dma=PASS") != std::string::npos,
			"the verdict line reports PASS");

		// The verdict is the conjunction of the rows, so a FAIL row and a PASS
		// verdict cannot coexist. Asserted as its own clause because it is the
		// property the two clauses above only imply when they are both read.
		check(!(failRows > 0 && output.find("dsp-dma=PASS") != std::string::npos),
			"a FAIL row and a PASS verdict never coexist");

		return g_failures == 0;
	}
}

int main()
{
	g2::EnvArtifactResolver       resolver;
	g2::test::GatedCounters       counters;

	g2::test::runGated(resolver, std::cout, counters, body);

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
