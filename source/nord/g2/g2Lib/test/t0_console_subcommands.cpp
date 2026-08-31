// Tier T0: every child this test spawns runs with NMG2_ARTIFACTS unset, so no
// firmware artifact is touched and no boot is performed here.
//
// The rule under test: `--help` names exactly the subcommands the binary
// implements -- no name it does not implement, and no implemented subcommand it
// does not name -- and a name that is printed but not implemented, or not
// recognised at all, exits non-zero after printing a diagnostic that names
// itself.
//
// This test holds no list of subcommands. It reads the printed list out of the
// binary and the dispatched list out of main.cpp, and asserts the two sets are
// equal. Adding a dispatch without a listing line, and deleting a listing line
// while the dispatch stays, are then both red without anything here being
// edited.
//
// Everything asserted is a process exit status or bytes on a standard stream.
// No assert() carries a predicate here: the default build is Release with
// NDEBUG and would delete it.
//
// The diagnostic is not satisfied by the usage text. The usage listing already
// prints every subcommand name, so "the output contains the name" would be
// vacuously true for any refusal that printed usage. The predicate is therefore
// a line that begins with the program's own diagnostic prefix and carries the
// name -- a listing line, which is indented, can never match it.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

#ifndef G2_TEST_CONSOLE_SOURCE
#	error "G2_TEST_CONSOLE_SOURCE must name g2TestConsole/main.cpp; without it the dispatch half of the rule is unasserted"
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

	// The program's own diagnostic prefix, spelled out here and derived from
	// nothing the binary prints.
	const std::string g_diagnosticPrefix = "g2TestConsole: ";

	// The heading the machine-readable subcommand block opens with.
	const std::string g_subcommandHeading = "subcommands:";

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

	std::vector<std::string> splitLines(const std::string& _text)
	{
		std::vector<std::string> lines;
		std::istringstream       in(_text);
		std::string              line;

		while(std::getline(in, line))
		{
			if(!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(line);
		}

		return lines;
	}

	// A listing line is exactly two spaces, then a long option, then either the
	// end of the line or whitespace. The block ends at the first line that is
	// not of that shape, which is what lets the options block sit below the
	// subcommands block in the same output.
	bool listedName(const std::string& _line, std::string& _name)
	{
		if(_line.rfind("  --", 0) != 0)
			return false;

		size_t end = 2;
		while(end < _line.size() && !isspace(static_cast<unsigned char>(_line[end])))
			++end;

		_name = _line.substr(2, end - 2);
		return _name.size() > 2;
	}

	std::set<std::string> namesPrintedByHelp(const std::string& _helpOutput)
	{
		std::set<std::string>          names;
		const std::vector<std::string> lines = splitLines(_helpOutput);

		bool inBlock = false;

		for(const std::string& line : lines)
		{
			if(!inBlock)
			{
				inBlock = line == g_subcommandHeading;
				continue;
			}

			std::string name;
			if(!listedName(line, name))
				break;

			names.insert(name);
		}

		return names;
	}

	// The dispatch table, read out of main.cpp. main() compares its first
	// argument through a variable named `command`, and every subcommand is
	// therefore spelled `command == "<name>"` exactly once. A modifier such as
	// --dump-dsp-dma is compared against a later argv entry and not against
	// `command`, so this reads subcommands and never modifiers.
	// Comments are not code, and this file is heavily commented. A comment that
	// quoted the dispatch pattern while describing it would otherwise enter the
	// dispatch set and turn this case red over prose. Both comment forms are
	// removed before the scan.
	std::string withoutComments(const std::string& _text)
	{
		std::string out;
		out.reserve(_text.size());

		for(size_t i = 0; i < _text.size();)
		{
			if(_text.compare(i, 2, "//") == 0)
			{
				while(i < _text.size() && _text[i] != '\n')
					++i;
				continue;
			}

			if(_text.compare(i, 2, "/*") == 0)
			{
				const size_t end = _text.find("*/", i + 2);
				i = end == std::string::npos ? _text.size() : end + 2;
				continue;
			}

			out += _text[i];
			++i;
		}

		return out;
	}

	std::set<std::string> namesDispatchedByMain(const std::string& _sourcePath, bool& _readFile)
	{
		std::set<std::string> names;

		std::ifstream in(_sourcePath.c_str());
		_readFile = in.good();

		if(!_readFile)
			return names;

		std::ostringstream all;
		all << in.rdbuf();
		const std::string text = withoutComments(all.str());

		const std::string needle = "command == \"";

		for(size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
		{
			const size_t start = pos + needle.size();
			const size_t end   = text.find('"', start);

			if(end == std::string::npos)
				break;

			names.insert(text.substr(start, end - start));
		}

		return names;
	}

	std::string join(const std::set<std::string>& _names)
	{
		std::string text;
		for(const std::string& name : _names)
		{
			if(!text.empty())
				text += " ";
			text += name;
		}
		return text.empty() ? "<empty>" : text;
	}

	// A diagnostic is a line that opens with the program's prefix and carries
	// the subject. The usage listing cannot satisfy this: its lines are
	// indented, so none of them opens with the prefix.
	bool diagnosticNames(const std::string& _output, const std::string& _subject)
	{
		for(const std::string& line : splitLines(_output))
		{
			if(line.rfind(g_diagnosticPrefix, 0) != 0)
				continue;

			if(line.find(_subject) != std::string::npos)
				return true;
		}

		return false;
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
		// Every child below inherits this. --boot with no artifacts refuses
		// immediately: it must then exit non-zero and name itself. Booting the
		// firmware here would be t1_boot's work done twice.
		unsetArtifacts();

		// ---------------- --help is a handled word and exits 0

		const CommandResult help = runConsole({"--help"});

		check(help.ran, "--help: the child ran to completion and its exit status is readable");
		check(help.exitCode == 0,
			"--help: exits 0; got " + std::to_string(help.exitCode));

		const std::set<std::string> printed = namesPrintedByHelp(help.output);

		std::cout << "     --help prints:      " << join(printed) << std::endl;

		// A known-positive control on the parser. An empty parse would make
		// every per-name assertion below quantify over an empty set and pass
		// vacuously, which is the shape this whole block exists to refuse.
		check(!printed.empty(),
			"--help: the output carries a '" + g_subcommandHeading + "' block with at least one name, so the sweep below is not vacuous");

		// ---------------- the listing and the dispatch table are the same set

		bool  readSource = false;
		const std::set<std::string> dispatched = namesDispatchedByMain(G2_TEST_CONSOLE_SOURCE, readSource);

		std::cout << "     main() dispatches:  " << join(dispatched) << std::endl;

		check(readSource,
			std::string("dispatch table: ") + G2_TEST_CONSOLE_SOURCE + " was opened and read");

		// The second known-positive control: an extraction that matched nothing
		// would report the dispatch set as empty and would agree with a --help
		// that printed nothing.
		check(!dispatched.empty(),
			"dispatch table: main.cpp carries at least one `command == \"--name\"` comparison, so the extraction read a real table");

		check(printed == dispatched,
			"the set --help prints and the set main() dispatches are EQUAL; printed=[" + join(printed) +
			"] dispatched=[" + join(dispatched) + "]");

		// ---------------- every name --help prints answers for itself

		for(const std::string& name : printed)
		{
			const CommandResult run = runConsole({name});

			std::cout << "     " << name << " -> exit " << run.exitCode << std::endl;

			check(run.ran, name + ": the child ran to completion and its exit status is readable");

			if(name == "--help")
			{
				// The one name whose own observable this case owns: --help's
				// documented observable is the listing, and it is compared
				// whole against the run above rather than sampled.
				check(run.exitCode == 0, "--help: a listed name that IS implemented exits 0");
				check(run.output == help.output, "--help: prints the same listing on every run");
				continue;
			}

			check(run.exitCode != 0,
				name + ": a listed name whose own observable is not available here EXITS NON-ZERO; got " +
				std::to_string(run.exitCode));

			check(diagnosticNames(run.output, name),
				name + ": the refusal prints a line opening '" + g_diagnosticPrefix +
				"' that names " + name + " itself, so an operator reads WHICH subcommand refused");
		}

		// ---------------- a name that is not implemented never exits 0
		//
		// --render and --impulse belong to other work. Neither may exist here
		// and neither may be quiet.

		for(const std::string& absent : {std::string("--render"), std::string("--impulse"), std::string("--nmg2-no-such-subcommand")})
		{
			const CommandResult run = runConsole({absent});

			std::cout << "     " << absent << " -> exit " << run.exitCode << std::endl;

			check(run.ran, absent + ": the child ran to completion and its exit status is readable");
			check(run.exitCode != 0,
				absent + ": an unimplemented or unrecognised argument NEVER exits 0; got " +
				std::to_string(run.exitCode));
			check(printed.find(absent) == printed.end(),
				absent + ": is not printed by --help, because the binary does not implement it");
			check(diagnosticNames(run.output, absent),
				absent + ": the refusal prints a line opening '" + g_diagnosticPrefix + "' that names " + absent + " itself");
		}

		// ---------------- no argument at all

		const CommandResult bare = runConsole({});

		std::cout << "     <no argument> -> exit " << bare.exitCode << std::endl;

		check(bare.ran, "no argument: the child ran to completion and its exit status is readable");
		check(bare.exitCode == 2,
			"no argument: exits 2; got " + std::to_string(bare.exitCode));
		check(bare.output == help.output,
			"no argument: prints the usage text, byte for byte the same text --help prints");
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL t0_console_subcommands threw std::exception: " << _e.what() << std::endl;
		++g_failures;
	}
	catch(...)
	{
		std::cout << "FAIL t0_console_subcommands threw a non-std exception" << std::endl;
		++g_failures;
	}

	if(g_failures)
	{
		std::cout << "t0_console_subcommands: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_console_subcommands: all checks passed" << std::endl;
	return 0;
}
