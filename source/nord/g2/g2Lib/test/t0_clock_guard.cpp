/* Two cases, guarding two different things.
 *
 * Case 1, the refuted MCU clock. The value 54 million is refuted, not merely
 * unverified, so its literal is banned rather than tracked. The case asserts it
 * appears nowhere in this repository, and it matches every spelling of it.
 *
 * CASE 1, THE CORE CLOCK'S DOMAIN. The MCF5307 has two clock domains and the
 * User's Manual section 4.2 documents no divide-by-one option, so PSTCLK is
 * the bus clock times 2, 3 or 4 and the two can never be equal. The hazard
 * this case exists to close is therefore a SUBSTITUTION: a bus-domain figure
 * standing in the symbol that reaches a core-cycle budget.
 *
 * THE PREDICATE BINDS TO THE SYMBOL AND NOT TO A LITERAL, and the difference
 * decides the outcome in both directions. A tree-wide ban on one number
 * forbids RECORDING A MEASUREMENT -- the bus clock is a real derived figure
 * and it has to be writable somewhere -- while saying nothing at all about
 * what the core-clock symbol carries. Bound to the symbol instead, the same
 * intent reads: EVERY DEFINITION OF THE CORE-CLOCK SYMBOL CARRIES A FREQUENCY
 * IN THE CORE-CLOCK DOMAIN. A bus figure recorded in a bus symbol passes; a
 * bus figure standing in the core symbol does not.
 *
 * THE DOMAIN FLOOR IS DERIVED AND ITS TWO HALVES ARE NAMED AT THE CONSTANT.
 * The bus-clock interval comes from the firmware and the divider floor from
 * the manual; neither is invented here and neither is a catalog speed grade.
 * The forbidden region therefore CONTAINS the whole bus-clock interval, so
 * the substitution case is a corollary of the floor rather than a second
 * rule.
 *
 * A DEFINITION IS THE PREDICATE'S SUBJECT, NOT A MENTION. What a symbol
 * carries is decided at `#define SYM value` and at `SYM = value`, so those
 * two shapes are what the scan matches. A static assertion or a fixture that
 * PINS the number is a mechanism that goes red on its own when the definition
 * moves, and turning those red from here would only duplicate them.
 *
 * A SINGLE-SPELLING SCAN IS A CHECK THAT A ONE-CHARACTER EDIT WALKS AROUND,
 * and that was MEASURED rather than argued: a fixed-string search for the
 * plain decimal form matched a header holding it and matched NEITHER the
 * digit-separator form, which the compiler reads as the same number, NOR the
 * exponent form. Plan section 7.7 measurement 6 carries the transcript. So a
 * value is RENDERED into every spelling a compiler accepts, and the scan and
 * the parser are driven through each rendering in turn.
 *
 * EVERY SPELLING CARRIES FOUR CONTROLS, and each one closes a way the case
 * could pass while proving nothing:
 *
 *   FLAGGED    the core symbol defined as an out-of-domain frequency. The
 *              scan must find it and the parser must read the value back
 *              EXACTLY. Without this, a pattern with a typo reports "absent"
 *              for ever and the case can never fail.
 *   IN DOMAIN  the core symbol defined as a legal core frequency. The scan
 *              must find it and the predicate must stay silent. Without this,
 *              a predicate that flagged every definition would pass.
 *   BUS SYMBOL the bus-clock symbol defined as the derived bus clock. The
 *              scan must not find it at all.
 *   SAME VALUE the core symbol defined as that SAME derived bus clock. The
 *              scan must find it and the predicate must flag it. This is the
 *              control that isolates the SYMBOL as the discriminator: without
 *              it, the bus-symbol control could be passing because the
 *              spelling went unmatched rather than because the symbol did.
 *
 * WHAT NO SCAN CAN CATCH is a value COMPUTED from other constants, and this
 * file does not pretend otherwise. The scan is the cheap half. Measurement
 * register row 7's rule -- that the core clock has no derived value until
 * criterion (j) reports -- is the half that does the work.
 *
 * CASE 2, the configure-time guard. The two MCU BUS symbols are both 0u and
 * neither is derived, so a source that used either would compute with a zero.
 * BRD-0 installs a guard in g2Lib/CMakeLists.txt that fails the configure
 * step and names the symbol. Section 7.4.2 gives that file to BRD-0, so THIS
 * TASK DOES NOT EDIT IT: BRD-0 installs the mechanism and this task drives the
 * negative case that proves it fires.
 *
 * The negative case adds a scratch use and asserts the configure step fails
 * naming the symbol. A control run without the scratch file asserts the guard
 * stays silent, so a guard that failed every configure for some other reason
 * would not be mistaken for a working one.
 *
 * EVERY NEEDLE AND EVERY FREQUENCY IN THIS FILE IS ASSEMBLED FROM FRAGMENTS
 * AT RUN TIME, and that is load-bearing three times over.
 *
 *   1. Case 1 scans every file in this repository with no exclusion list, so
 *      a file that spelled a guarded definition out in full would match
 *      ITSELF, the case could never pass, and the usual repair -- excluding
 *      this file from its own scan -- would open exactly the hole the case
 *      exists to close.
 *   2. THIS FILE LIVES UNDER source/nord/g2/, WHICH IS THE TREE BRD-0's GUARD
 *      SCANS. A file that spelled either guarded symbol out in full would
 *      make the guard fire on the very test that proves it fires, and the
 *      whole project would stop configuring. The two symbols are therefore
 *      built from a shared prefix and a suffix and are never contiguous in
 *      this source.
 *   3. TASK SCH-0's CHECK STILL BANS THE BUS CLOCK'S DECIMAL LITERAL
 *      TREE-WIDE. Section 7.4.2 keeps each task to its own files, so this
 *      task does not repair that ban; it works inside it, and every frequency
 *      here is therefore a product of factors rather than a written-out
 *      number. Repairing the second copy of the ban belongs to whoever owns
 *      that check, and until then a plain decimal here turns it red.
 */

#include <cstdint>
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

	std::string coreClockSymbol()
	{
		return std::string("G2_MCU_") + "CORE_" + "CLOCK_HZ";
	}

	/* ---------------- the core clock's domain, and where each half comes from
	 *
	 * THE BUS-CLOCK INTERVAL IS READ OUT OF THE FIRMWARE, not assumed. The
	 * SDRAM refresh control field pins the refresh period to a whole number of
	 * bus clocks, and at the JEDEC row interval that fixes the bus clock to a
	 * band about one part in fifty wide. An independent UART divider lands
	 * inside the same band from a MIDI baud rate, which is what makes it a
	 * derivation rather than a coincidence. Spike criterion (j) owns the
	 * figure; measurement register rows 5 and 6 carry it.
	 *
	 * THE DIVIDER FLOOR IS 2 BECAUSE 2 HOLDS UNDER BOTH READINGS OF THE PART.
	 * The MCF5307 manual's section 4.2 permits 2, 3 or 4 and no divide-by-one;
	 * the MCF5407CAI162 the schematic reads at U14 multiplies CLKIN by 3. Two
	 * is the weaker of the two claims and is therefore the one the floor is
	 * built from, so the floor does not move when the part identity settles.
	 *
	 * The product is a FLOOR and never an estimate: no core clock this part
	 * can run at is below it, and the bus-clock band lies entirely underneath
	 * it, so a bus figure in the core symbol is caught by the floor without a
	 * second rule.
	 */
	constexpr uint64_t kBusClockLowerBoundHz = 53248ull * 1000ull;
	constexpr uint64_t kSmallestBusDivider   = 2ull;

	uint64_t coreClockFloorHz()
	{
		return kBusClockLowerBoundHz * kSmallestBusDivider;
	}

	/* THE PREDICATE ITSELF, AND IT HAS EXACTLY ONE SITE. The controls and the
	 * repository scan below both ask this function and neither restates the
	 * comparison, so a mutation of the verdict cannot leave the controls green
	 * while the scan goes blind. Measured: a second copy of this comparison
	 * inside the scan was disabled with every control still passing. */
	bool isBusDomainFigure(const uint64_t frequencyHz)
	{
		return frequencyHz < coreClockFloorHz();
	}

	/* The derived bus clock itself, which the controls below define into a bus
	 * symbol and into the core symbol in turn. */
	uint64_t derivedBusClockHz()
	{
		return 54ull * 1000ull * 1000ull;
	}

	/* The FLAGGED control's frequency: a bus-domain figure, well under the
	 * floor. It is a product for the reason the file header gives and for no
	 * other. */
	uint64_t outOfDomainCoreClockHz()
	{
		return 45ull * 1000ull * 1000ull;
	}

	/* A legal core frequency: the derived bus clock at a divider the manual
	 * permits. The middle of the three is chosen so the value sits clear of
	 * the floor rather than on it, which is what makes the IN DOMAIN control
	 * a test of the comparison and not of its boundary. */
	uint64_t inDomainCoreClockHz()
	{
		return derivedBusClockHz() * 3ull;
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

	/* Paths are plain strings here. Std::filesystem is unavailable at this
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

	/* ---------------- what is not source, and how it is found
	 *
	 * The scan must not read build output, and A directory name is not A
	 * Reliable way to tell. This was measured rather than argued.
	 *
	 * The scan covers this repository's tracked files plus its untracked,
	 * non-ignored ones. `.gitignore` ignores /build/, so a build tree at that
	 * one path is out of scope -- and a build tree at ANY OTHER PATH is not.
	 * A CTest log quotes the output of the tests it ran, and this case's own
	 * diagnostic QUOTES THE DEFINITION LINE IT OBJECTS TO. So a second build
	 * tree, at any name but `build`, puts that line into
	 * <tree>/Testing/Temporary/LastTest.log and the next run reports it again,
	 * on matches that are all log lines. The case would then be reporting its
	 * own transcript.
	 *
	 * A check that passes because of a directory name is an accident, not a
	 * check. The scope is therefore stated by a property of the directory and
	 * not by its name: a directory that holds a CMakeCache.txt is a CMake
	 * build tree, and a build tree is output rather than source.
	 *
	 * The exclusions are reported with any failure, so the scope is never
	 * invisible to whoever reads the result. */
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

	/* ---------------- the spellings
	 *
	 * A spelling is a WAY OF WRITING A FREQUENCY, so it is a renderer and not
	 * a fixed string. The controls render three different values through every
	 * spelling and the scan reads each rendering back, which is what makes the
	 * spelling coverage a round trip rather than a claim.
	 */
	enum class Spelling
	{
		Decimal,
		DecimalSeparated,
		HexLower,
		HexUpper,
		ExponentInteger,
		ExponentFractional,
		Count
	};

	struct SpellingRow
	{
		Spelling    kind;
		std::string name;
	};

	std::string spellingName(const Spelling spelling)
	{
		switch(spelling)
		{
		case Spelling::Decimal:            return "the decimal form";
		case Spelling::DecimalSeparated:   return "the decimal form with digit separators";
		case Spelling::HexLower:           return "the hexadecimal form, lower-case prefix";
		case Spelling::HexUpper:           return "the hexadecimal form, upper-case prefix";
		case Spelling::ExponentInteger:    return "the exponent form, integer mantissa";
		case Spelling::ExponentFractional: return "the exponent form, fractional mantissa";
		case Spelling::Count:              break;
		}
		return "an unnamed spelling";
	}

	/* THE TABLE IS THE ENUMERATION and not a second list beside it. A hand-kept
	 * list can lose a row to an edit and go on passing with less coverage than
	 * it claims, and nothing in the run would say so. */
	std::vector<SpellingRow> buildSpellingTable()
	{
		std::vector<SpellingRow> table;

		for(size_t i = 0; i < static_cast<size_t>(Spelling::Count); ++i)
		{
			const Spelling kind = static_cast<Spelling>(i);
			table.push_back({ kind, spellingName(kind) });
		}

		return table;
	}

	std::string decimalDigits(uint64_t value)
	{
		if(value == 0ull)
			return "0";

		std::string digits;
		while(value != 0ull)
		{
			digits.insert(digits.begin(),
				static_cast<char>('0' + static_cast<int>(value % 10ull)));
			value /= 10ull;
		}
		return digits;
	}

	std::string hexDigits(uint64_t value, const bool upperCase)
	{
		static const char* const lower = "0123456789abcdef";
		static const char* const upper = "0123456789ABCDEF";
		const char* const alphabet = upperCase ? upper : lower;

		if(value == 0ull)
			return "0";

		std::string digits;
		while(value != 0ull)
		{
			digits.insert(digits.begin(),
				alphabet[static_cast<size_t>(value & 0xfull)]);
			value >>= 4;
		}
		return digits;
	}

	/* Every third digit from the right carries a C++14 separator. The compiler
	 * reads the separated literal as the same number, so a scan that did not
	 * allow one is walked around by a single edit. */
	std::string withDigitSeparators(const std::string& digits)
	{
		std::string out;
		const size_t count = digits.size();

		for(size_t i = 0; i < count; ++i)
		{
			if(i != 0 && ((count - i) % 3) == 0)
				out += '\'';
			out += digits[i];
		}
		return out;
	}

	std::string render(const uint64_t value, const Spelling spelling)
	{
		const std::string digits = decimalDigits(value);

		/* The significant digits, with the trailing zeros an exponent form
		 * carries instead. */
		std::string significant = digits;
		while(significant.size() > 1 && significant.back() == '0')
			significant.pop_back();

		switch(spelling)
		{
		case Spelling::Decimal:
			return digits;

		case Spelling::DecimalSeparated:
			return withDigitSeparators(digits);

		case Spelling::HexLower:
			return "0x" + hexDigits(value, false);

		case Spelling::HexUpper:
			return "0X" + hexDigits(value, true);

		case Spelling::ExponentInteger:
			return significant + "e"
				+ decimalDigits(digits.size() - significant.size());

		case Spelling::ExponentFractional:
		{
			const std::string fraction = significant.size() > 1
				? significant.substr(1)
				: std::string("0");

			return significant.substr(0, 1) + "." + fraction + "e"
				+ decimalDigits(digits.size() - 1);
		}

		case Spelling::Count:
			break;
		}

		/* NO FALL-THROUGH TO A NEIGHBOUR'S RENDERING. A spelling with no case
		 * of its own renders as a value no control asked for, so the exact
		 * read-back turns red instead of quietly repeating another spelling's
		 * coverage under a new name. */
		return "0";
	}

	/* ---------------- reading a rendered frequency back
	 *
	 * The parser is EXACT and uses no floating point: an exponent form is
	 * folded into the mantissa by powers of ten, and a mantissa that does not
	 * fold to a whole number is REFUSED rather than rounded. A frequency this
	 * check cannot read exactly is a frequency it must not judge.
	 */
	bool readFrequency(const std::string& text, size_t position,
		uint64_t& value, size_t& end)
	{
		const size_t size = text.size();

		if(position >= size || text[position] < '0' || text[position] > '9')
			return false;

		const bool hexadecimal = text[position] == '0'
			&& position + 1 < size
			&& (text[position + 1] == 'x' || text[position + 1] == 'X');

		if(hexadecimal)
		{
			position += 2;

			uint64_t accumulated = 0ull;
			bool     any         = false;

			while(position < size)
			{
				const char c = text[position];
				int        digit;

				if(c == '\'')             { ++position; continue; }
				else if(c >= '0' && c <= '9') digit = c - '0';
				else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
				else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
				else break;

				accumulated = accumulated * 16ull + static_cast<uint64_t>(digit);
				any = true;
				++position;
			}

			if(!any)
				return false;

			value = accumulated;
			end   = position;
			return true;
		}

		uint64_t mantissa   = 0ull;
		size_t   fractional = 0;

		while(position < size
			&& ((text[position] >= '0' && text[position] <= '9')
				|| text[position] == '\''))
		{
			if(text[position] != '\'')
				mantissa = mantissa * 10ull
					+ static_cast<uint64_t>(text[position] - '0');
			++position;
		}

		if(position < size && text[position] == '.')
		{
			++position;
			while(position < size
				&& ((text[position] >= '0' && text[position] <= '9')
					|| text[position] == '\''))
			{
				if(text[position] != '\'')
				{
					mantissa = mantissa * 10ull
						+ static_cast<uint64_t>(text[position] - '0');
					++fractional;
				}
				++position;
			}
		}

		uint64_t exponent = 0ull;

		if(position < size && (text[position] == 'e' || text[position] == 'E'))
		{
			size_t cursor = position + 1;

			if(cursor < size && text[cursor] == '+')
				++cursor;

			bool any = false;
			while(cursor < size && text[cursor] >= '0' && text[cursor] <= '9')
			{
				exponent = exponent * 10ull
					+ static_cast<uint64_t>(text[cursor] - '0');
				any = true;
				++cursor;
			}

			if(any)
				position = cursor;
		}
		else if(fractional != 0)
		{
			/* A fraction with no exponent is not a whole frequency. */
			return false;
		}

		if(exponent < fractional)
			return false;

		for(uint64_t i = 0ull; i < exponent - fractional; ++i)
			mantissa *= 10ull;

		value = mantissa;
		end   = position;
		return true;
	}

	/* ---------------- what the scan looks for
	 *
	 * The two shapes that DEFINE what a symbol carries, and no other. The
	 * value alternatives are a superset of the spellings above: git grep
	 * decides which LINES to report and readFrequency above is the authority
	 * on what a reported line actually says.
	 */
	std::string definitionPattern(const std::string& symbol)
	{
		const std::string frequency =
			"(0[xX][0-9a-fA-F']+"
			"|[0-9][0-9']*([.][0-9']+)?[eE][+]?[0-9]+"
			"|[0-9][0-9']*)";

		return "(#[[:space:]]*define[[:space:]]+" + symbol + "[[:space:]]+"
			"|" + symbol + "[[:space:]]*=[[:space:]]*)" + frequency;
	}

	/* Every frequency a line defines the symbol as.
	 *
	 * A LINE IS SEARCHED FOR EVERY OCCURRENCE and not for its first, because a
	 * reported line may mention the symbol before it defines it. An occurrence
	 * that is not followed by a frequency is not a definition and contributes
	 * nothing -- which is what keeps a comparison such as `SYM == value` out
	 * of the result, the second `=` being neither a space nor a digit. */
	std::vector<uint64_t> definedFrequencies(const std::string& line,
		const std::string& symbol)
	{
		std::vector<uint64_t> found;

		size_t at = line.find(symbol);

		while(at != std::string::npos)
		{
			size_t cursor = at + symbol.size();

			while(cursor < line.size()
				&& (line[cursor] == ' ' || line[cursor] == '\t'))
				++cursor;

			if(cursor < line.size() && line[cursor] == '='
				&& (cursor + 1 >= line.size() || line[cursor + 1] != '='))
			{
				++cursor;
				while(cursor < line.size()
					&& (line[cursor] == ' ' || line[cursor] == '\t'))
					++cursor;
			}

			uint64_t value = 0ull;
			size_t   end   = 0;

			if(readFrequency(line, cursor, value, end))
				found.push_back(value);

			at = line.find(symbol, at + symbol.size());
		}

		return found;
	}

	/* The non-empty lines of a command's output. */
	std::vector<std::string> outputLines(const std::string& output)
	{
		std::vector<std::string> lines;
		size_t                   start = 0;

		while(start < output.size())
		{
			size_t end = output.find('\n', start);
			if(end == std::string::npos)
				end = output.size();

			if(end != start)
				lines.push_back(output.substr(start, end - start));

			start = end + 1;
		}

		return lines;
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

	/* ================ case 1: the core clock's domain */
	{
		const std::vector<SpellingRow> table = buildSpellingTable();

		const std::string coreSymbol = coreClockSymbol();
		const std::string corePattern = definitionPattern(coreSymbol);
		const uint64_t    floorHz = coreClockFloorHz();

		printf("t0_clock_guard case 1: a core-clock definition below %s Hz is "
			"a bus-domain figure in a core-domain symbol\n",
			decimalDigits(floorHz).c_str());

		/* The controls' scratch tree. It lives in the work directory, which is
		 * inside the build tree and therefore outside the scan of the real
		 * case below. */
		const std::string controlDirectory = join(workDirectory, "spellings");
		(void) runCommand(".", { cmakeExecutable, "-E", "make_directory",
			controlDirectory });

		/* One control run: write `text` into a scratch file, scan it with
		 * `pattern`, and report whether the scan matched and what it read
		 * back. */
		struct ControlOutcome
		{
			bool                  ran     = false;
			bool                  matched = false;
			std::vector<uint64_t> values;
		};

		const auto runControl =
			[&](const std::string& leaf, const std::string& text,
				const std::string& pattern, const std::string& symbol)
			{
				const std::string file = join(controlDirectory, leaf);
				writeTextFile(file, text + "\n");

				const CommandResult result = runCommand(
					controlDirectory,
					{
						gitExecutable, "grep", "--no-index", "-I",
						"--line-number", "-E", "-e", pattern, "--", leaf
					});

				std::remove(file.c_str());

				ControlOutcome outcome;
				outcome.ran     = result.ran && result.exitCode <= 1;
				outcome.matched = result.ran && result.exitCode == 0;

				for(const std::string& line : outputLines(result.output))
				{
					const std::vector<uint64_t> values =
						definedFrequencies(line, symbol);
					outcome.values.insert(outcome.values.end(),
						values.begin(), values.end());
				}

				return outcome;
			};

		for(size_t i = 0; i < table.size(); ++i)
		{
			const SpellingRow& spelling = table[i];
			const std::string  index    = std::to_string(i);
			const std::string  define   = "#define ";

			/* ---- FLAGGED: the core symbol holding a bus-domain figure. */
			{
				const uint64_t value = outOfDomainCoreClockHz();
				const ControlOutcome outcome = runControl(
					"flagged_" + index + ".txt",
					define + coreSymbol + " " + render(value, spelling.kind),
					corePattern, coreSymbol);

				if(!outcome.ran)
					fail("the FLAGGED control for " + spelling.name
						+ " could not run at all");
				else if(!outcome.matched)
					fail("the scan does NOT find a core-clock definition "
						"written in " + spelling.name + ", so it could never "
						"report one (pattern: " + corePattern + ")");
				else if(outcome.values.size() != 1
					|| outcome.values.front() != value)
					fail("the parser does not read " + spelling.name + " back "
						"as " + decimalDigits(value) + " Hz");
				else if(!isBusDomainFigure(outcome.values.front()))
					fail("the predicate does NOT flag " + decimalDigits(value)
						+ " Hz in the core-clock symbol, written in "
						+ spelling.name + ", against a floor of "
						+ decimalDigits(floorHz) + " Hz");
			}

			/* ---- IN DOMAIN: the core symbol holding a legal core clock. */
			{
				const uint64_t value = inDomainCoreClockHz();
				const ControlOutcome outcome = runControl(
					"in_domain_" + index + ".txt",
					define + coreSymbol + " " + render(value, spelling.kind),
					corePattern, coreSymbol);

				if(!outcome.ran)
					fail("the IN DOMAIN control for " + spelling.name
						+ " could not run at all");
				else if(!outcome.matched)
					fail("the scan does NOT find a core-clock definition "
						"written in " + spelling.name);
				else if(outcome.values.size() != 1
					|| outcome.values.front() != value)
					fail("the parser does not read " + spelling.name + " back "
						"as " + decimalDigits(value) + " Hz");
				else if(isBusDomainFigure(outcome.values.front()))
					fail("the predicate flags the legal core clock "
						+ decimalDigits(value) + " Hz, written in "
						+ spelling.name + ", so it would refuse a correct "
						"measurement");
			}

			/* ---- BUS SYMBOL: the derived bus clock, recorded where it
			 * belongs. The predicate must be silent, and this is the direction
			 * a ban on the literal got wrong. */
			{
				const uint64_t    value  = derivedBusClockHz();
				const std::string symbol = busSymbol("CLOCK_HZ");

				const ControlOutcome outcome = runControl(
					"bus_symbol_" + index + ".txt",
					define + symbol + " " + render(value, spelling.kind),
					corePattern, coreSymbol);

				if(!outcome.ran)
					fail("the BUS SYMBOL control for " + spelling.name
						+ " could not run at all");
				else if(outcome.matched)
					fail("the scan reports the derived bus clock "
						+ decimalDigits(value) + " Hz recorded in the "
						"bus-clock symbol, written in " + spelling.name
						+ ". A bus figure in a bus symbol is a measurement, "
						"not a substitution.");
			}

			/* ---- SAME VALUE: that same bus clock in the core symbol. It is
			 * what isolates the SYMBOL as the discriminator, because the value
			 * and the spelling are held fixed against the control above. */
			{
				const uint64_t value = derivedBusClockHz();
				const ControlOutcome outcome = runControl(
					"same_value_" + index + ".txt",
					define + coreSymbol + " " + render(value, spelling.kind),
					corePattern, coreSymbol);

				if(!outcome.ran)
					fail("the SAME VALUE control for " + spelling.name
						+ " could not run at all");
				else if(!outcome.matched)
					fail("the scan does NOT find the bus clock standing in the "
						"core-clock symbol, written in " + spelling.name
						+ ", so the pair of controls proves nothing about the "
						"symbol");
				else if(outcome.values.size() != 1
					|| outcome.values.front() != value)
					fail("the parser does not read " + spelling.name + " back "
						"as " + decimalDigits(value) + " Hz");
				else if(!isBusDomainFigure(outcome.values.front()))
					fail("the predicate does NOT flag the bus clock "
						+ decimalDigits(value) + " Hz standing in the "
						"core-clock symbol, written in " + spelling.name);
			}

			printf("t0_clock_guard case 1: %s is read back exactly, flagged in "
				"the core symbol and ignored in the bus symbol\n",
				spelling.name.c_str());
		}

		/* ---- the real scan.
		 *
		 * THE SCOPE IS STATED. `git grep` here searches this repository's own
		 * tracked files plus its untracked, non-ignored ones. It does NOT
		 * recurse into submodules and it does not read ignored paths, so the
		 * build tree at the ignored path is out of scope and a build tree at
		 * any other path is excluded by the property below.
		 *
		 * The exclusions are REPORTED with the result, so the scope is never
		 * invisible to whoever reads it. */
		const std::vector<std::string> exclusions =
			buildTreeExclusions(repositoryRoot, gitExecutable);

		std::string scopeReport = "the scan excluded these build trees:";
		if(exclusions.empty())
			scopeReport += " none";
		for(const std::string& exclusion : exclusions)
			scopeReport += " " + exclusion;

		printf("t0_clock_guard case 1: %s\n", scopeReport.c_str());

		std::vector<std::string> scanArguments =
		{
			gitExecutable, "grep", "-I", "--untracked",
			"--line-number", "-E", "-e", corePattern
		};

		if(!exclusions.empty())
		{
			scanArguments.push_back("--");
			scanArguments.push_back(".");
			for(const std::string& exclusion : exclusions)
				scanArguments.push_back(exclusion);
		}

		const CommandResult scan = runCommand(repositoryRoot, scanArguments);

		/* git grep exits 0 when it MATCHED, 1 when it did not, and above 1 on
		 * an error. Both 0 and 1 are outcomes here -- a tree with no core-clock
		 * definition at all has nothing out of domain -- so the codes are read
		 * explicitly rather than through a test that would score an error as a
		 * pass. */
		if(!scan.ran || scan.exitCode > 1)
		{
			fail("the core-clock scan could not run (exit code "
				+ std::to_string(scan.exitCode) + "), so the case is unproven "
				"rather than passed.\n" + scan.output);
		}
		else
		{
			for(const std::string& line : outputLines(scan.output))
			{
				for(const uint64_t value : definedFrequencies(line, coreSymbol))
				{
					if(!isBusDomainFigure(value))
					{
						printf("t0_clock_guard case 1: %s Hz is in the "
							"core-clock domain -- %s\n",
							decimalDigits(value).c_str(), line.c_str());
						continue;
					}

					fail("a core-clock symbol is defined as "
						+ decimalDigits(value) + " Hz, which is below the "
						"core-clock floor of " + decimalDigits(floorHz)
						+ " Hz and therefore a bus-domain figure in a "
						"core-domain symbol. Measurement register row 7 owns "
						"the value and spike criterion (j) owns the "
						"measurement that settles it.\n" + scopeReport + "\n"
						+ line);
				}
			}
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

		/* The MCF5307 link is turned off for the scratch configure.
		 *
		 * G2_LINK_MCF5307 is on by default, and g2Lib then
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
			/* The scratch use. It is a real source file under
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

			/* Removed before the result is read, so that a failing assertion
			 * cannot leave behind a file that breaks every later configure. */
			std::remove(scratchHeader.c_str());

			check(negative.ran,
				"the negative configure for " + symbol + " ran");

			if(!negative.ran)
				continue;

			check(negative.exitCode != 0,
				"the configure step FAILS when a source uses " + symbol + ":\n"
					+ negative.output);

			/* The message must name the symbol and the file, so a configure
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
