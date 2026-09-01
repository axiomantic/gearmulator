// Tier T0: this test reads two committed text files of hashes and integers. It
// needs no firmware artifact and no NMG2_ARTIFACTS.
//
// The two manifests:
//
//   artifacts.sha256   the SHA-256 of every required firmware file, and no
//                      payload. The gate checks the hashes before it runs T1 or
//                      T2. A hash mismatch is a hard failure with the file name,
//                      never a skip.
//
//   golden.timebase    the values the golden set was recorded under. Integers
//                      only, so the file is public. A change to any of the
//                      in-boundary values invalidates the whole golden set.
//
// Every value the committed golden.timebase records is compared against the
// macro that defines that value, because a manifest whose shape is checked and
// whose values are not carries the authority of a checked artifact while
// describing a state that has moved.
//
// Every negative case asserts a named failure that identifies the offending
// symbol, and the short and long manifests are built by removing and adding a
// named symbol rather than by padding a line count: a parse that merely counted
// lines would accept five wrong lines.

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "g2/timebase.h"

#ifndef G2_REPOSITORY_ROOT
#error "G2_REPOSITORY_ROOT must be defined by tests_repo.cmake. Without it this test would look for the manifests in the working directory, find nothing, and there would be no way to tell that from a manifest that is genuinely absent."
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

	// ------------------------------------------------------------------
	// golden.timebase
	//
	// The required symbols, in the order the design lists them. This vector is
	// the required set: a symbol missing from the file is named, and a symbol in
	// the file that is not here is named.

	const std::vector<std::string> g_requiredTimebaseSymbols =
	{
		"G2_DSP_CYCLES_PER_FRAME_NUM",
		"G2_DSP_CYCLES_PER_FRAME_DEN",
		"G2_MCU_CORE_CLOCK_HZ",
		"G2_CHAIN_HOP_FRAMES",
		"G2_SECOND_BUS_FRAME_DIVIDER"
	};

	struct TimebaseEntry
	{
		std::string symbol;
		unsigned long long value = 0;
	};

	// The same symbols as they are defined, for the comparison against the same
	// symbols as they are recorded.
	//
	// One token becomes both columns, and that is the whole point: the number is
	// never written here, so this table cannot become a third copy of it that
	// drifts from the header in its turn. A macro that is renamed or deleted
	// stops being an identifier and the compile fails, rather than a row
	// quietly ceasing to match anything.

	struct TimebaseConstant
	{
		std::string symbol;
		unsigned long long value = 0;
	};

#define G2_MANIFEST_CONSTANT(_symbol) TimebaseConstant{ #_symbol, static_cast<unsigned long long>(_symbol) }

	const std::vector<TimebaseConstant> g_definedTimebaseConstants =
	{
		G2_MANIFEST_CONSTANT(G2_DSP_CYCLES_PER_FRAME_NUM),
		G2_MANIFEST_CONSTANT(G2_DSP_CYCLES_PER_FRAME_DEN),
		G2_MANIFEST_CONSTANT(G2_MCU_CORE_CLOCK_HZ),
		G2_MANIFEST_CONSTANT(G2_CHAIN_HOP_FRAMES),
		G2_MANIFEST_CONSTANT(G2_SECOND_BUS_FRAME_DIVIDER)
	};

#undef G2_MANIFEST_CONSTANT

	// Parses golden.timebase. Returns the entries, and appends one named
	// failure per defect to _failures. An empty _failures means the manifest is
	// well formed.
	std::vector<TimebaseEntry> parseTimebase(const std::string& _text, std::vector<std::string>& _failures)
	{
		std::vector<TimebaseEntry> entries;

		std::istringstream stream(_text);
		std::string line;
		size_t lineNumber = 0;

		while(std::getline(stream, line))
		{
			++lineNumber;

			// A trailing carriage return would otherwise become part of the
			// value and the file would parse differently on two platforms.
			if(!line.empty() && line.back() == '\r')
				line.pop_back();

			if(line.empty())
				continue;

			std::istringstream lineStream(line);
			std::string symbol;
			std::string valueText;
			std::string surplus;

			if(!(lineStream >> symbol >> valueText))
			{
				_failures.push_back("MANIFEST-TIMEBASE-MALFORMED-LINE: line " + std::to_string(lineNumber) + ": " + line);
				continue;
			}

			if(lineStream >> surplus)
			{
				_failures.push_back("MANIFEST-TIMEBASE-SURPLUS-FIELD: " + symbol + ": " + surplus);
				continue;
			}

			// Integers only, no Clavia data, so the file is public. A value
			// that is not an integer is the
			// route by which something that is not an integer could enter a
			// file this project publishes on the strength of that sentence.
			const bool allDigits = !valueText.empty() &&
				std::all_of(valueText.begin(), valueText.end(), [](const unsigned char _c) { return std::isdigit(_c) != 0; });

			if(!allDigits)
			{
				_failures.push_back("MANIFEST-TIMEBASE-NOT-AN-INTEGER: " + symbol + ": " + valueText);
				continue;
			}

			const bool known = std::find(g_requiredTimebaseSymbols.begin(), g_requiredTimebaseSymbols.end(), symbol) != g_requiredTimebaseSymbols.end();

			if(!known)
			{
				_failures.push_back("MANIFEST-TIMEBASE-UNKNOWN-SYMBOL: " + symbol);
				continue;
			}

			const bool duplicate = std::any_of(entries.begin(), entries.end(), [&](const TimebaseEntry& _e) { return _e.symbol == symbol; });

			if(duplicate)
			{
				_failures.push_back("MANIFEST-TIMEBASE-DUPLICATE-SYMBOL: " + symbol);
				continue;
			}

			TimebaseEntry entry;
			entry.symbol = symbol;
			entry.value = std::stoull(valueText);
			entries.push_back(entry);
		}

		for(const std::string& required : g_requiredTimebaseSymbols)
		{
			const bool present = std::any_of(entries.begin(), entries.end(), [&](const TimebaseEntry& _e) { return _e.symbol == required; });

			if(!present)
				_failures.push_back("MANIFEST-TIMEBASE-MISSING-SYMBOL: " + required);
		}

		return entries;
	}

	// ------------------------------------------------------------------
	// artifacts.sha256

	const std::vector<std::string> g_requiredArtifactNames =
	{
		"BOOT_128_Loader.bin",
		"NMG2_128_OS.bin",
		"CODE_30000400.bin",
		"SRAM_20000800.bin"
	};

	struct ArtifactEntry
	{
		std::string hash;
		std::string name;
	};

	bool isLowercaseSha256(const std::string& _hash)
	{
		if(_hash.size() != 64)
			return false;

		return std::all_of(_hash.begin(), _hash.end(), [](const unsigned char _c)
		{
			return (_c >= '0' && _c <= '9') || (_c >= 'a' && _c <= 'f');
		});
	}

	std::vector<ArtifactEntry> parseArtifacts(const std::string& _text, std::vector<std::string>& _failures)
	{
		std::vector<ArtifactEntry> entries;

		std::istringstream stream(_text);
		std::string line;
		size_t lineNumber = 0;

		while(std::getline(stream, line))
		{
			++lineNumber;

			if(!line.empty() && line.back() == '\r')
				line.pop_back();

			if(line.empty())
				continue;

			std::istringstream lineStream(line);
			std::string hash;
			std::string name;
			std::string surplus;

			if(!(lineStream >> hash >> name))
			{
				_failures.push_back("MANIFEST-ARTIFACT-MALFORMED-LINE: line " + std::to_string(lineNumber) + ": " + line);
				continue;
			}

			if(lineStream >> surplus)
			{
				_failures.push_back("MANIFEST-ARTIFACT-SURPLUS-FIELD: " + name + ": " + surplus);
				continue;
			}

			if(!isLowercaseSha256(hash))
			{
				_failures.push_back("MANIFEST-ARTIFACT-NOT-A-SHA256: " + name + ": " + hash);
				continue;
			}

			const bool known = std::find(g_requiredArtifactNames.begin(), g_requiredArtifactNames.end(), name) != g_requiredArtifactNames.end();

			if(!known)
			{
				_failures.push_back("MANIFEST-ARTIFACT-UNKNOWN-FILE: " + name);
				continue;
			}

			const bool duplicate = std::any_of(entries.begin(), entries.end(), [&](const ArtifactEntry& _e) { return _e.name == name; });

			if(duplicate)
			{
				_failures.push_back("MANIFEST-ARTIFACT-DUPLICATE-FILE: " + name);
				continue;
			}

			ArtifactEntry entry;
			entry.hash = hash;
			entry.name = name;
			entries.push_back(entry);
		}

		for(const std::string& required : g_requiredArtifactNames)
		{
			const bool present = std::any_of(entries.begin(), entries.end(), [&](const ArtifactEntry& _e) { return _e.name == required; });

			if(!present)
				_failures.push_back("MANIFEST-ARTIFACT-MISSING-FILE: " + required);
		}

		return entries;
	}

	// ------------------------------------------------------------------

	bool readWholeFile(const std::string& _path, std::string& _text)
	{
		std::ifstream file(_path, std::ios::binary);

		if(!file)
			return false;

		std::ostringstream buffer;
		buffer << file.rdbuf();
		_text = buffer.str();
		return true;
	}

	std::string joined(const std::vector<std::string>& _lines)
	{
		std::string out;
		for(const std::string& line : _lines)
			out += "\n       " + line;
		return out;
	}

	const std::string g_repositoryRoot = G2_REPOSITORY_ROOT;
}

int main()
{
	try
	{
		// ================= golden.timebase, the committed file

		std::string timebaseText;
		const std::string timebasePath = g_repositoryRoot + "/golden.timebase";

		if(!readWholeFile(timebasePath, timebaseText))
		{
			std::cout << "FAIL golden.timebase is not committed at " << timebasePath << std::endl;
			++g_failures;
		}
		else
		{
			std::vector<std::string> failures;
			const std::vector<TimebaseEntry> entries = parseTimebase(timebaseText, failures);

			check(failures.empty(), "golden.timebase: the committed manifest parses with no failure" + joined(failures));
			check(entries.size() == 5, "golden.timebase: the committed manifest holds exactly five values");

			// The line count is asserted on the file and not only on the parse,
			// because the parse skips blank lines.
			const size_t newlineCount = static_cast<size_t>(std::count(timebaseText.begin(), timebaseText.end(), '\n'));
			check(newlineCount == 5, "golden.timebase: the file holds exactly five lines");

			for(const std::string& required : g_requiredTimebaseSymbols)
			{
				const bool present = std::any_of(entries.begin(), entries.end(), [&](const TimebaseEntry& _e) { return _e.symbol == required; });
				check(present, "golden.timebase: carries " + required);
			}

			// Integers only, so the file is public. Asserted on the bytes and
			// not only through the parse: this is the sentence on whose
			// strength the file is published.
			const bool integersAndNamesOnly = std::all_of(timebaseText.begin(), timebaseText.end(), [](const unsigned char _c)
			{
				return (_c >= '0' && _c <= '9') || (_c >= 'A' && _c <= 'Z') || _c == '_' || _c == ' ' || _c == '\n';
			});
			check(integersAndNamesOnly, "golden.timebase: every byte is a symbol character, a digit, a space or a newline");

			// Every recorded value against the value its macro defines. Both
			// numbers are printed whichever way the comparison goes, because a
			// drift report that names only the symbol sends its reader back to
			// both files to find out which side moved.
			for(const TimebaseConstant& defined : g_definedTimebaseConstants)
			{
				const auto recorded = std::find_if(entries.begin(), entries.end(),
					[&](const TimebaseEntry& _e) { return _e.symbol == defined.symbol; });

				const std::string what = "golden.timebase: " + defined.symbol + " matches g2/timebase.h (recorded " +
					(recorded == entries.end() ? std::string("nothing") : std::to_string(recorded->value)) +
					", defined " + std::to_string(defined.value) + ")";

				check(recorded != entries.end() && recorded->value == defined.value, what);
			}

			// The comparison is closed over the required set. Without this, a
			// sixth symbol added to the manifest and to the required set, and
			// not to the table above, would be recorded and never compared --
			// which is the silence this comparison exists to end, returning
			// through the door the comparison itself left open.
			for(const std::string& required : g_requiredTimebaseSymbols)
			{
				const bool compared = std::any_of(g_definedTimebaseConstants.begin(), g_definedTimebaseConstants.end(),
					[&](const TimebaseConstant& _c) { return _c.symbol == required; });
				check(compared, "golden.timebase: " + required + " is compared against g2/timebase.h and not only parsed");
			}
		}

		// ================= artifacts.sha256, the committed file

		std::string artifactsText;
		const std::string artifactsPath = g_repositoryRoot + "/artifacts.sha256";

		if(!readWholeFile(artifactsPath, artifactsText))
		{
			std::cout << "FAIL artifacts.sha256 is not committed at " << artifactsPath << std::endl;
			++g_failures;
		}
		else
		{
			std::vector<std::string> failures;
			const std::vector<ArtifactEntry> entries = parseArtifacts(artifactsText, failures);

			check(failures.empty(), "artifacts.sha256: the committed manifest parses with no failure" + joined(failures));
			check(entries.size() == 4, "artifacts.sha256: the committed manifest holds exactly four hashes");

			// And no payload. A hard byte ceiling well under the 65,536-byte
			// fixture ceiling makes "no payload" a measured property rather
			// than an intention.
			check(artifactsText.size() < 512, "artifacts.sha256: the file is under 512 bytes, so it carries no payload");

			const bool hexAndNamesOnly = std::all_of(artifactsText.begin(), artifactsText.end(), [](const unsigned char _c)
			{
				return std::isalnum(_c) != 0 || _c == '_' || _c == '.' || _c == ' ' || _c == '\n';
			});
			check(hexAndNamesOnly, "artifacts.sha256: every byte is alphanumeric, an underscore, a dot, a space or a newline");

			// Distinct hashes. A manifest whose rows were copied from one
			// another would satisfy every shape assertion above.
			std::vector<std::string> hashes;
			for(const ArtifactEntry& entry : entries)
				hashes.push_back(entry.hash);
			std::sort(hashes.begin(), hashes.end());
			check(std::unique(hashes.begin(), hashes.end()) == hashes.end(), "artifacts.sha256: the four hashes are distinct");

			// The SHA-256 of the empty input. A placeholder hash is a forbidden
			// failure mode, and it has one well-known spelling.
			const std::string emptyInputHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
			const bool anyEmpty = std::any_of(entries.begin(), entries.end(), [&](const ArtifactEntry& _e) { return _e.hash == emptyInputHash; });
			check(!anyEmpty, "artifacts.sha256: no row carries the hash of an empty file");
		}

		// ================= the negative cases

		// ---- four lines: the required symbols minus one.
		{
			const std::string fourLines =
				"G2_DSP_CYCLES_PER_FRAME_NUM   150000000\n"
				"G2_DSP_CYCLES_PER_FRAME_DEN   96000\n"
				"G2_MCU_CORE_CLOCK_HZ          45000000\n"
				"G2_CHAIN_HOP_FRAMES           1\n";

			std::vector<std::string> failures;
			const std::vector<TimebaseEntry> entries = parseTimebase(fourLines, failures);

			std::cout << "     four-line manifest failures:" << joined(failures) << std::endl;

			check(entries.size() == 4, "four lines: four values parsed");
			check(failures.size() == 1, "four lines: exactly one failure");
			check(!failures.empty() && failures.front() == "MANIFEST-TIMEBASE-MISSING-SYMBOL: G2_SECOND_BUS_FRAME_DIVIDER",
				"four lines: the failure NAMES the missing symbol");
		}

		// ---- six lines: the required symbols plus one that is not one.
		//
		// The extra symbol is deliberately not one of the two unmeasured bus
		// symbols. The configure-time guard fails the build when any source
		// under g2Lib/ carries either name, and a negative case that broke the
		// configure step would not be a negative case.
		//
		// Measured, and worth recording: that guard is a plain substring match
		// over the file text, so it fires on a comment that names either symbol
		// exactly as it fires on a use. This paragraph is therefore written
		// without the two names in it.
		{
			const std::string sixLines =
				"G2_DSP_CYCLES_PER_FRAME_NUM   150000000\n"
				"G2_DSP_CYCLES_PER_FRAME_DEN   96000\n"
				"G2_MCU_CORE_CLOCK_HZ          45000000\n"
				"G2_CHAIN_HOP_FRAMES           1\n"
				"G2_SECOND_BUS_FRAME_DIVIDER   4\n"
				"G2_NOT_A_TIMEBASE_SYMBOL      7\n";

			std::vector<std::string> failures;
			const std::vector<TimebaseEntry> entries = parseTimebase(sixLines, failures);

			std::cout << "     six-line manifest failures:" << joined(failures) << std::endl;

			check(entries.size() == 5, "six lines: the five required values still parsed");
			check(failures.size() == 1, "six lines: exactly one failure");
			check(!failures.empty() && failures.front() == "MANIFEST-TIMEBASE-UNKNOWN-SYMBOL: G2_NOT_A_TIMEBASE_SYMBOL",
				"six lines: the failure NAMES the surplus symbol");
		}

		// ---- six lines by duplication.
		//
		// A parse that only counted would call this six lines and reject it for
		// the wrong reason. A parse that only checked the required set would
		// accept it. Neither is what the gate needs.
		{
			const std::string duplicated =
				"G2_DSP_CYCLES_PER_FRAME_NUM   150000000\n"
				"G2_DSP_CYCLES_PER_FRAME_DEN   96000\n"
				"G2_MCU_CORE_CLOCK_HZ          45000000\n"
				"G2_CHAIN_HOP_FRAMES           1\n"
				"G2_SECOND_BUS_FRAME_DIVIDER   4\n"
				"G2_CHAIN_HOP_FRAMES           2\n";

			std::vector<std::string> failures;
			parseTimebase(duplicated, failures);

			std::cout << "     duplicate-symbol manifest failures:" << joined(failures) << std::endl;

			check(failures.size() == 1, "duplicated symbol: exactly one failure");
			check(!failures.empty() && failures.front() == "MANIFEST-TIMEBASE-DUPLICATE-SYMBOL: G2_CHAIN_HOP_FRAMES",
				"duplicated symbol: the failure NAMES the repeated symbol");
		}

		// ---- five lines, one of them not an integer.
		//
		// The count is right and the symbol set is right. This is the case a
		// line-counting parse accepts, and it is the one that would let
		// something that is not an integer into a file this project publishes
		// on the strength of "integers only".
		{
			const std::string notAnInteger =
				"G2_DSP_CYCLES_PER_FRAME_NUM   150000000\n"
				"G2_DSP_CYCLES_PER_FRAME_DEN   96000\n"
				"G2_MCU_CORE_CLOCK_HZ          PENDING\n"
				"G2_CHAIN_HOP_FRAMES           1\n"
				"G2_SECOND_BUS_FRAME_DIVIDER   4\n";

			std::vector<std::string> failures;
			parseTimebase(notAnInteger, failures);

			std::cout << "     non-integer manifest failures:" << joined(failures) << std::endl;

			check(failures.size() == 2, "non-integer value: two failures, the bad value and the symbol it left missing");
			check(!failures.empty() && failures.front() == "MANIFEST-TIMEBASE-NOT-AN-INTEGER: G2_MCU_CORE_CLOCK_HZ: PENDING",
				"non-integer value: the failure NAMES the symbol and the value");
		}

		// ---- artifacts.sha256 with three hashes, and with a bad hash.
		{
			const std::string threeHashes =
				"d1b8e30804edbccae853b647e06ac20ae902fd6da05ade7b5d2090ce17c24d88  BOOT_128_Loader.bin\n"
				"b3a76b7db724d88e3f603e1f500cf873fd525d8015e35d4f985866a842751c3a  NMG2_128_OS.bin\n"
				"2fa65ac9a1ca2d96c5060baedb1bd220efb4140e606738e8e2686a3b93c35788  CODE_30000400.bin\n";

			std::vector<std::string> failures;
			parseArtifacts(threeHashes, failures);

			std::cout << "     three-hash manifest failures:" << joined(failures) << std::endl;

			check(failures.size() == 1, "three hashes: exactly one failure");
			check(!failures.empty() && failures.front() == "MANIFEST-ARTIFACT-MISSING-FILE: SRAM_20000800.bin",
				"three hashes: the failure NAMES the missing file");
		}

		{
			const std::string badHash =
				"d1b8e30804edbccae853b647e06ac20ae902fd6da05ade7b5d2090ce17c24d88  BOOT_128_Loader.bin\n"
				"b3a76b7db724d88e3f603e1f500cf873fd525d8015e35d4f985866a842751c3a  NMG2_128_OS.bin\n"
				"2fa65ac9a1ca2d96c5060baedb1bd220efb4140e606738e8e2686a3b93c35788  CODE_30000400.bin\n"
				"TBD                                                               SRAM_20000800.bin\n";

			std::vector<std::string> failures;
			parseArtifacts(badHash, failures);

			std::cout << "     placeholder-hash manifest failures:" << joined(failures) << std::endl;

			check(failures.size() == 2, "placeholder hash: two failures, the bad hash and the file it left missing");
			check(!failures.empty() && failures.front() == "MANIFEST-ARTIFACT-NOT-A-SHA256: SRAM_20000800.bin: TBD",
				"placeholder hash: the failure NAMES the file and the value that is not a hash");
		}
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL the manifest parse threw std::exception: " << _e.what() << std::endl;
		++g_failures;
	}
	catch(...)
	{
		std::cout << "FAIL the manifest parse threw a non-std exception" << std::endl;
		++g_failures;
	}

	if(g_failures)
	{
		std::cout << "t0_manifest_parses: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_manifest_parses: all checks passed" << std::endl;
	return 0;
}
