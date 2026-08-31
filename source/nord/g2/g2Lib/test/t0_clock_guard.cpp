/* t0_clock_guard.cpp -- the check of task SCH-3. Design sections 13.4.3, 23.1.
 *
 * The cases below guard different things.
 *
 * CASE 1, the refuted MCU clock. The value 54 million is REFUTED on five
 * independent grounds, not merely unverified: an unverified value may turn out
 * right and a refuted one will not, so its literal is banned rather than
 * tracked. The case asserts it appears NOWHERE in this repository, and it
 * matches EVERY SPELLING of it and not one.
 *
 * A single-spelling grep is a check that a one-character edit walks around,
 * and that was MEASURED rather than argued: a fixed-string search for the
 * plain decimal form matched a header holding it and matched NEITHER the
 * digit-separator form, which the compiler reads as the same number, NOR the
 * exponent form. Plan section 7.7 measurement 6 carries the transcript.
 *
 * So the case matches the decimal form with optional digit separators, the
 * hexadecimal form in either letter case, and the exponent forms, and the
 * spellings are ENUMERATED IN ONE TABLE so that a reader sees the whole set.
 *
 * EVERY SPELLING CARRIES A POSITIVE CONTROL. Each pattern is first run
 * against a scratch file that holds that spelling, and it must MATCH. Without
 * that half, a pattern with a typo in it reports "absent" for ever and the
 * case can never fail.
 *
 * WHAT NO GREP CAN CATCH is a value COMPUTED from other constants, and this
 * file does not pretend otherwise. The grep is the cheap half. Measurement
 * register row 7's rule -- that no shipped header carries the number -- is
 * the half that does the work.
 *
 * CASE 2, the configure-time guard. The two MCU BUS symbols are both 0u and
 * neither is derived, so a source that used either would compute with a zero.
 * BRD-0 installs a guard in g2Lib/CMakeLists.txt that fails the configure
 * step and names the symbol. Section 7.4.2 gives that file to BRD-0, so THIS
 * TASK DOES NOT EDIT IT: BRD-0 installs the mechanism and this task drives the
 * negative case that proves it fires.
 *
 * The negative case adds a scratch use and asserts the configure step fails
 * naming the symbol. A CONTROL RUN WITHOUT THE SCRATCH FILE asserts the guard
 * stays silent, so a guard that failed every configure for some other reason
 * would not be mistaken for a working one.
 *
 * EVERY NEEDLE IN THIS FILE IS ASSEMBLED FROM FRAGMENTS AT RUN TIME, and that
 * is load-bearing twice over.
 *
 *   1. Case 1 scans every file in this repository with no exclusion list, so
 *      a file that spelled the refuted value out in full would match ITSELF,
 *      the case could never pass, and the usual repair -- excluding this file
 *      from its own scan -- would open exactly the hole the case exists to
 *      close.
 *   2. THIS FILE LIVES UNDER source/nord/g2/, WHICH IS THE TREE BRD-0's GUARD
 *      SCANS. A file that spelled either guarded symbol out in full would
 *      make the guard fire on the very test that proves it fires, and the
 *      whole project would stop configuring. The two symbols are therefore
 *      built from a shared prefix and a suffix and are never contiguous in
 *      this source.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

namespace
{
	int failures = 0;

	void fail(const std::string& what)
	{
		printf("FAIL %s\n", what.c_str());
		++failures;
	}

	void check(const bool condition, const std::string& what)
	{
		if(!condition)
			fail(what);
	}

	/* ---------------- the two guarded symbol names, never contiguous here */

	std::string busSymbol(const std::string& suffix)
	{
		return std::string("G2_MCU_") + "BUS_" + suffix;
	}

	/* ---------------- running a child process and reading everything it says
	 *
	 * The exit code is read EXPLICITLY and never through a truthiness test. A
	 * scan that could not run at all must report "unproven", never "passed".
	 */
	struct CommandResult
	{
		int         exitCode = -1;
		bool        ran      = false;
		std::string output;
	};

	std::string shellQuote(const std::string& text)
	{
#if defined(_WIN32)
		return "\"" + text + "\"";
#else
		std::string quoted = "'";
		for(const char c : text)
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

	CommandResult runCommand(const std::string& workingDirectory,
		const std::vector<std::string>& argv)
	{
		std::string command = "cd " + shellQuote(workingDirectory) + " && ";

		for(size_t i = 0; i < argv.size(); ++i)
		{
			if(i != 0)
				command += ' ';
			command += shellQuote(argv[i]);
		}

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

	bool contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	void writeTextFile(const std::string& path, const std::string& text)
	{
		std::ofstream stream(path.c_str(), std::ios::binary | std::ios::trunc);
		stream << text;
	}

	bool fileExists(const std::string& path)
	{
		std::ifstream stream(path.c_str(), std::ios::binary);
		return stream.good();
	}

	/* PATHS ARE PLAIN STRINGS HERE. std::filesystem is unavailable at this
	 * target's deployment version, and every path operation this check needs
	 * is a join, a basename or a directory creation. Directory work goes
	 * through `cmake -E`, which every supported CMake carries and which needs
	 * no platform branch of its own. */
	std::string join(const std::string& directory, const std::string& leaf)
	{
		return directory + "/" + leaf;
	}

	std::string baseName(const std::string& path)
	{
		const size_t cut = path.find_last_of("/\\");
		return cut == std::string::npos ? path : path.substr(cut + 1);
	}

	/* A CMake list file reads a backslash as an escape, so a path written into
	 * one carries forward slashes on every platform. */
	std::string forwardSlashes(std::string path)
	{
		for(char& c : path)
		{
			if(c == '\\')
				c = '/';
		}
		return path;
	}

	/* ---------------- what is NOT source, and how it is found
	 *
	 * THE SCAN MUST NOT READ BUILD OUTPUT, AND A DIRECTORY NAME IS NOT A
	 * RELIABLE WAY TO TELL.
	 *
	 * The scan covers this repository's tracked files plus its untracked,
	 * non-ignored ones. `.gitignore` ignores /build/, so a build tree at that
	 * one path is out of scope -- and a build tree at ANY OTHER PATH is not.
	 * A CTest log quotes the output of other tests, and task SCH-0's own check
	 * PRINTS the refuted value in full when it reports the value absent. So a
	 * second build tree, at any name but `build`, puts the literal into
	 * <tree>/Testing/Temporary/LastTest.log and this case reports it as
	 * present, on matches that are all log lines.
	 *
	 * A check that passes because of a directory name is an accident, not a
	 * check. The scope is therefore stated by a PROPERTY of the directory and
	 * not by its name: a directory that holds a CMakeCache.txt is a CMake
	 * build tree, and a build tree is output rather than source.
	 *
	 * The exclusions are REPORTED with any failure, so the scope is never
	 * invisible to whoever reads the result.
	 *
	 * TASK SCH-0's CHECK CARRIES THE SAME FRAGILITY and this task does not
	 * repair it: that check belongs to SCH-0 and section 7.4.2 keeps each task
	 * to its own files. It is reported instead. */
	std::vector<std::string> buildTreeExclusions(const std::string& repositoryRoot,
		const std::string& gitExecutable)
	{
		std::vector<std::string> exclusions;

		/* The top-level entries git can see. A CMake build tree placed deeper
		 * than one level is not covered, and saying so is better than implying
		 * a completeness this does not have. */
		const CommandResult listing = runCommand(repositoryRoot,
			{ gitExecutable, "ls-files", "--others", "--directory",
			  "--exclude-standard" });

		if(!listing.ran)
			return exclusions;

		size_t start = 0;

		while(start < listing.output.size())
		{
			size_t end = listing.output.find('\n', start);
			if(end == std::string::npos)
				end = listing.output.size();

			std::string entry = listing.output.substr(start, end - start);
			start = end + 1;

			if(entry.empty())
				continue;

			/* git reports a directory with a trailing slash. */
			if(entry.back() != '/')
				continue;

			if(!fileExists(repositoryRoot + "/" + entry + "CMakeCache.txt"))
				continue;

			exclusions.push_back(":(exclude)" + entry);
		}

		return exclusions;
	}

	/* ---------------- the spelling table
	 *
	 * One row for each spelling the refuted value can take. `pattern` is the
	 * extended regular expression the scan uses; `example` is a real string in
	 * that spelling and it is what the positive control searches for. */
	struct Spelling
	{
		std::string name;
		std::string pattern;
		std::string example;
	};

	std::vector<Spelling> buildSpellingTable()
	{
		const std::string d0 = "0";
		const std::string d3 = "3";
		const std::string d4 = "4";
		const std::string d5 = "5";
		const std::string d6 = "6";
		const std::string d7 = "7";
		const std::string d8 = "8";
		const std::string d9 = "9";

		/* An optional C++14 digit separator between any two digits. The
		 * compiler reads a separated literal as the same number, so a pattern
		 * that did not allow one is walked around by a single edit. */
		const std::string sep = "'?";

		std::vector<Spelling> table;

		/* ---- decimal: 5, 4 and six zeros, with optional separators. */
		{
			std::string pattern = d5 + sep + d4;
			std::string plain   = d5 + d4;

			for(int i = 0; i < 6; ++i)
			{
				pattern += sep + d0;
				plain   += d0;
			}

			/* The same value in the separated spelling the compiler accepts,
			 * so the control proves the optional separator really is optional
			 * in both directions. */
			std::string separated = d5 + d4;
			for(int i = 0; i < 6; ++i)
			{
				if(i % 3 == 0)
					separated += "'";
				separated += d0;
			}

			table.push_back({ "the decimal form", pattern, plain });
			table.push_back({ "the decimal form with digit separators",
				pattern, separated });
		}

		/* ---- hexadecimal, either letter case, optional leading zeros. */
		{
			const std::string digits =
				d3 + sep + d3 + sep + d7 + sep + "[fF]" + sep + d9 + sep + d8
				+ sep + d0;

			const std::string pattern = d0 + "[xX]" + d0 + "*" + digits;

			table.push_back({ "the hexadecimal form, lower-case prefix",
				pattern, d0 + "x" + d3 + d3 + d7 + "F" + d9 + d8 + d0 });
			table.push_back({ "the hexadecimal form, upper-case prefix",
				pattern, d0 + "X" + d3 + d3 + d7 + "f" + d9 + d8 + d0 });
		}

		/* ---- the exponent forms. */
		{
			table.push_back({ "the exponent form, mantissa 54",
				d5 + sep + d4 + "[eE][+]?" + d6,
				d5 + d4 + "e" + d6 });
			table.push_back({ "the exponent form, mantissa 5.4",
				d5 + "[.]" + d4 + "[eE][+]?" + d7,
				d5 + "." + d4 + "e" + d7 });
		}

		return table;
	}
}

int main(const int argc, const char* const* const argv)
{
	/* ---------------- the arguments, checked before anything runs.
	 *
	 * A check that cannot obtain its inputs REPORTS that. It does not pass
	 * vacuously. */
	static const char* const kArgumentNames[] =
	{
		"the repository root",
		"the git executable",
		"the cmake executable",
		"the g2Lib source directory",
		"the scratch header path",
		"the work directory"
	};

	static constexpr int kArgumentCount =
		static_cast<int>(sizeof(kArgumentNames) / sizeof(kArgumentNames[0]));

	if(argc != kArgumentCount + 1)
	{
		printf("FAIL t0_clock_guard: %d argument(s) supplied, %d needed. The "
			"check cannot run, and it reports that rather than passing.\n",
			argc - 1, kArgumentCount);
		return 1;
	}

	for(int i = 0; i < kArgumentCount; ++i)
	{
		const std::string value = argv[i + 1];
		const std::string notFound = "NOTFOUND";

		const bool missing = value.empty()
			|| (value.size() >= notFound.size()
				&& value.compare(value.size() - notFound.size(),
					notFound.size(), notFound) == 0);

		if(missing)
		{
			printf("FAIL t0_clock_guard: %s was not supplied (value: '%s'). "
				"The check cannot run, and it reports that rather than "
				"passing.\n", kArgumentNames[i], value.c_str());
			return 1;
		}
	}

	const std::string repositoryRoot  = argv[1];
	const std::string gitExecutable   = argv[2];
	const std::string cmakeExecutable = argv[3];
	const std::string g2LibSourceDir  = argv[4];
	const std::string scratchHeader   = argv[5];
	const std::string workDirectory   = argv[6];

	(void) runCommand(".", { cmakeExecutable, "-E", "remove_directory",
		workDirectory });
	(void) runCommand(".", { cmakeExecutable, "-E", "make_directory",
		workDirectory });

	/* A scratch header left behind by an interrupted run would fail every
	 * later configure of this tree, so it is removed before anything else. */
	std::remove(scratchHeader.c_str());

	/* ================ case 1: the refuted value, every spelling */
	{
		const std::vector<Spelling> table = buildSpellingTable();

		check(table.size() == 6,
			"the spelling table holds every spelling this case claims");

		const std::vector<std::string> exclusions =
			buildTreeExclusions(repositoryRoot, gitExecutable);

		std::string scopeReport = "the scan excluded these build trees:";
		if(exclusions.empty())
			scopeReport += " none";
		for(const std::string& exclusion : exclusions)
			scopeReport += " " + exclusion;

		printf("t0_clock_guard case 1: %s\n", scopeReport.c_str());

		/* The positive control's scratch tree. It lives in the work directory,
		 * which is inside the build tree and therefore outside the scan of the
		 * real case below. */
		const std::string controlDirectory = join(workDirectory, "spellings");
		(void) runCommand(".", { cmakeExecutable, "-E", "make_directory",
			controlDirectory });

		for(size_t i = 0; i < table.size(); ++i)
		{
			const Spelling& spelling = table[i];

			/* ---- the positive control.
			 *
			 * One file for each spelling, so a pattern that only matched some
			 * other row's example would still be caught. */
			const std::string controlLeaf =
				"spelling_" + std::to_string(i) + ".txt";
			const std::string controlFile = join(controlDirectory, controlLeaf);

			writeTextFile(controlFile, spelling.example + "\n");

			const CommandResult control = runCommand(
				controlDirectory,
				{
					gitExecutable, "grep", "--no-index", "-I",
					"--line-number", "-E", "-e", spelling.pattern,
					"--", controlLeaf
				});

			std::remove(controlFile.c_str());

			if(!control.ran)
			{
				fail("the positive control for " + spelling.name
					+ " could not run at all");
				continue;
			}

			if(control.exitCode != 0)
			{
				fail("the pattern for " + spelling.name + " does NOT match "
					"its own example, so the scan below could never fail "
					"(pattern: " + spelling.pattern + ", exit code "
					+ std::to_string(control.exitCode) + ")");
				continue;
			}

			/* ---- the real case.
			 *
			 * THE SCOPE IS STATED. `git grep` here searches this repository's
			 * own tracked files plus its untracked, non-ignored ones. It does
			 * NOT recurse into submodules and it does not read ignored paths,
			 * so the build tree is out of scope.
			 *
			 * That scope is chosen against a measurement. A literal walk of
			 * the working tree finds the decimal form in four VENDORED
			 * third-party files, where it is a coincidental substring of a
			 * checksum table and has nothing to do with any clock. A check
			 * scoped to the literal tree could therefore never pass on a
			 * clone with its submodules initialised. This scope is the widest
			 * one that still passes and it covers every file this project
			 * actually writes. */
			std::vector<std::string> scanArguments =
			{
				gitExecutable, "grep", "-I", "--untracked",
				"--line-number", "-E", "-e", spelling.pattern
			};

			if(!exclusions.empty())
			{
				scanArguments.push_back("--");
				scanArguments.push_back(".");
				for(const std::string& exclusion : exclusions)
					scanArguments.push_back(exclusion);
			}

			const CommandResult scan = runCommand(repositoryRoot, scanArguments);

			if(!scan.ran)
			{
				fail("the scan for " + spelling.name + " could not run, so "
					"the case is unproven rather than passed");
				continue;
			}

			/* git grep exits 0 when it MATCHED, 1 when it did not, and above 1
			 * on an error. A match is the failure here, so the codes are read
			 * explicitly rather than through a test that would score an error
			 * as a pass. */
			if(scan.exitCode == 0)
			{
				fail("the refuted MCU clock is present in this repository, in "
					+ spelling.name + ". Design section 13.4.3 refutes it on "
					"five independent grounds and it must never come back.\n"
					+ scopeReport + "\n" + scan.output);
				continue;
			}

			if(scan.exitCode != 1)
			{
				fail("the scan for " + spelling.name + " reported exit code "
					+ std::to_string(scan.exitCode) + ", so the case is "
					"unproven rather than passed.\n" + scan.output);
				continue;
			}

			printf("t0_clock_guard case 1: %s is absent\n",
				spelling.name.c_str());
		}
	}

	/* ================ case 2: the configure-time guard fires */
	{
		/* A scratch project whose whole content is one add_subdirectory of
		 * g2Lib. It configures the REAL g2Lib/CMakeLists.txt, which is where
		 * the guard lives, and it configures nothing else -- so the case costs
		 * about a second and it needs no build of its own. */
		const std::string projectDirectory = join(workDirectory, "project");
		(void) runCommand(".", { cmakeExecutable, "-E", "make_directory",
			projectDirectory });

		writeTextFile(join(projectDirectory, "CMakeLists.txt"),
			"cmake_minimum_required(VERSION 3.15)\n"
			"project(t0ClockGuardNegative)\n"
			"add_subdirectory(\"" + forwardSlashes(g2LibSourceDir)
			+ "\" g2Lib)\n");

		/* THE MCF5307 LINK IS TURNED OFF FOR THE SCRATCH CONFIGURE, and the
		 * reason is stated rather than left as a flag nobody can explain.
		 *
		 * G2_LINK_MCF5307 defaults ON, and g2Lib then
		 * either adds a subdirectory the ROOT CMakeLists.txt points it at or
		 * calls FetchContent_MakeAvailable(mcf5307) against details the ROOT
		 * declares. This scratch project is not that root, so the fetch has no
		 * details and the configure fails BEFORE it reaches the guard -- which
		 * makes both the control run and the negative run fail for a reason
		 * that has nothing to do with the guard.
		 *
		 * Turning the option off removes the only part of g2Lib/CMakeLists.txt
		 * that needs a root. The guard block itself is downstream of it and is
		 * unaffected by the option, so the case still drives the real guard in
		 * the real file. */
		const std::string linkOptionOff = "-DG2_LINK_MCF5307=OFF";

		const std::string clockSymbol   = busSymbol("CLOCK_HZ");
		const std::string dividerSymbol = busSymbol("DIVIDER");

		/* ---- the control run.
		 *
		 * With no scratch file the guard must stay silent and the configure
		 * must succeed. Without this half, a guard that failed every configure
		 * for an unrelated reason would look like a working one. */
		{
			const CommandResult control = runCommand(
				workDirectory,
				{
					cmakeExecutable, linkOptionOff,
					"-S", projectDirectory,
					"-B", join(workDirectory, "controlBuild")
				});

			check(control.ran, "the control configure ran");

			if(control.ran)
			{
				check(control.exitCode == 0,
					"the control configure succeeds when no source uses either "
					"symbol:\n" + control.output);

				check(!contains(control.output, "uses " + clockSymbol)
					&& !contains(control.output, "uses " + dividerSymbol),
					"the guard stays silent when no source uses either "
					"symbol:\n" + control.output);
			}
		}

		/* ---- the negative case, once for each guarded symbol.
		 *
		 * Both are 0u and neither is derived, so the guard covers both and
		 * both are driven. */
		for(const std::string& symbol : { clockSymbol, dividerSymbol })
		{
			/* THE SCRATCH USE. It is a real source file under
			 * source/nord/g2/, which is the tree the guard scans. */
			writeTextFile(scratchHeader,
				std::string(
					"/* Written by t0_clock_guard and removed by it. If this\n"
					" * file is still here, an earlier run was interrupted and\n"
					" * every configure of this tree fails until it is\n"
					" * deleted. */\n"
					"#define G2_CLOCK_GUARD_SCRATCH_USE ") + symbol + "\n");

			const CommandResult negative = runCommand(
				workDirectory,
				{
					cmakeExecutable, linkOptionOff,
					"-S", projectDirectory,
					"-B", join(workDirectory, "negativeBuild_" + symbol)
				});

			/* REMOVED BEFORE THE RESULT IS READ, so that a failing assertion
			 * cannot leave behind a file that breaks every later configure. */
			std::remove(scratchHeader.c_str());

			check(negative.ran,
				"the negative configure for " + symbol + " ran");

			if(!negative.ran)
				continue;

			check(negative.exitCode != 0,
				"the configure step FAILS when a source uses " + symbol + ":\n"
					+ negative.output);

			/* The message must name the symbol AND the file, so a configure
			 * that failed for some other reason cannot be read as a pass. The
			 * guard's explanation names both symbols, so the form matched here
			 * is the one that names the symbol that FIRED it. */
			check(contains(negative.output, "uses " + symbol),
				"the guard's message names " + symbol + " as the symbol that "
					"fired it:\n" + negative.output);

			check(contains(negative.output, baseName(scratchHeader)),
				"the guard's message names the file that uses " + symbol
					+ ":\n" + negative.output);

			printf("t0_clock_guard case 2: the guard fires on %s\n",
				symbol.c_str());
		}

		/* And the scratch file really is gone. */
		check(!fileExists(scratchHeader),
			"the scratch header is removed, so no later configure of this tree "
			"carries it");
	}

	if(failures != 0)
	{
		printf("t0_clock_guard: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_clock_guard: all cases passed\n");
	return 0;
}
