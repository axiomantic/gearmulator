// t0_artifacts_manifest.cpp -- artifacts.sha256, the fork's own manifest.
//
// This repository's default branch carries artifacts.sha256: the SHA-256 of
// every firmware file the private artifact repository must supply, and no
// payload of its own. The manifest is tooling for operating this fork, so the
// test that validates it belongs here beside it and not inside a submission
// draft.
//
// It was split out of t0_manifest_parses on 2026-09-10. That test validated
// this manifest AND golden.timebase, and the 2026-09-07 tooling extraction
// moved both manifests here while leaving the test on the band branches, where
// it then failed on all nine because neither file was there. golden.timebase
// mirrors macros in g2/timebase.h, so it is product data and went back to the
// bands with that half of the test; this half stayed with the file it reads.
//
// Every assertion below is carried over unchanged.

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


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
		std::cout << "t0_artifacts_manifest: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_artifacts_manifest: all checks passed" << std::endl;
	return 0;
}