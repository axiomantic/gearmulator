// The artifact resolver. Tier T0: this test runs with NMG2_ARTIFACTS unset and
// needs no firmware artifact.
//
// The three properties it holds the resolver to:
//
//   1. With NMG2_ARTIFACTS unset or empty, resolve() returns an empty string
//      and writes message 1 word for word.
//   2. With NMG2_ARTIFACTS set to a directory that is not there, or that names
//      a file rather than a directory, resolve() returns an empty string and
//      writes message 2, echoing the variable's value unchanged.
//   3. With NMG2_ARTIFACTS set to a directory that is there, resolve() returns
//      the directory and clears `_why`. When the caller passes a name and the
//      file is not in the directory, resolve() returns an empty string and
//      writes message 3, echoing both the name and the variable's value.
//
// resolve() never throws.
//
// The message literals below are written out in full on purpose. The Python
// half, nmg2_tools/artifacts.py in axiomantic/nmg2-tools, carries the same
// literals and asserts them the same way. Comparing against a full literal in
// each language is what makes "word for word in both languages" a falsifiable
// claim; deriving them from the header under test would assert only that the
// module equals itself.

#include "../artifactResolver.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

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

	// The THREE messages, spelled out. Nothing in this file derives them from
	// the header under test: a test that reads its expectation from the code it
	// tests asserts only that the code equals itself.
	//
	// Message 1 is g_artifactUnavailableMessage by definition; spelled out
	// here the same way.
	const std::string g_message1 =
		"firmware artifact not available (NMG2_ARTIFACTS unset)";

	void setArtifactsVariable(const char* _value)
	{
#ifdef _WIN32
		if(_value)
			_putenv_s("NMG2_ARTIFACTS", _value);
		else
			_putenv_s("NMG2_ARTIFACTS", "");
#else
		if(_value)
			setenv("NMG2_ARTIFACTS", _value, 1);
		else
			unsetenv("NMG2_ARTIFACTS");
#endif
	}

	// Writes _path/_name and returns the joined path. The test fixture for
	// message 3 needs an EXISTING directory that does not hold the artifact,
	// which is the case where message 3 fires, and one that does hold it, which
	// is the success case for the name parameter.
	std::string joinPath(const std::string& _path, const std::string& _name)
	{
		return _path + "/" + _name;
	}
}

int main()
{
	try
	{
		g2::EnvArtifactResolver resolver;

		// ---------------- case 1: the variable is unset (message 1)

		setArtifactsVariable(nullptr);

		std::string whyUnset = "this string must be overwritten";
		const std::string resultUnset = resolver.resolve(whyUnset);

		check(resultUnset.empty(), "unset: resolve() returns an empty result");
		check(whyUnset == g_message1,
			"unset: resolve() writes message 1 word for word");

		// ---------------- case 2: the variable names a directory that is not
		// there (message 2). The path is under a name nobody owns and is never
		// created. The Python half uses the same path so the two test files
		// name the same input.
		{
			const std::string missingDir = "/nmg2/no/such/directory/REPO-5";
			setArtifactsVariable(missingDir.c_str());

			std::string whyMissing = "this string must be overwritten";
			const std::string resultMissing = resolver.resolve(whyMissing);

			check(resultMissing.empty(),
				"missing directory: resolve() returns an empty result");
			const std::string expectedMessage2 =
				"firmware artifact not available (NMG2_ARTIFACTS names no directory: "
				+ missingDir + ")";
			check(whyMissing == expectedMessage2,
				"missing directory: resolve() writes message 2 word for word");

			// Message 1 and message 2 are DISTINCT on purpose. A resolver that
			// collapsed the two would send an operator with a wrong path to
			// the message for an unset variable, which is the message for a
			// different problem.
			check(whyMissing != whyUnset,
				"missing directory: message 2 is distinct from message 1");
			check(resultMissing == resultUnset,
				"missing directory: the empty result is the same shape as the unset result");
		}

		// ---------------- case 3: an empty variable is an unset variable
		// (message 1)
		//
		// Windows has no way to remove a variable through _putenv_s other than
		// assigning it the empty string, so the empty value must behave as the
		// unset value or this test means different things on two platforms.

		setArtifactsVariable("");

		std::string whyEmpty = "this string must be overwritten";
		const std::string resultEmpty = resolver.resolve(whyEmpty);

		check(resultEmpty.empty(), "empty value: resolve() returns an empty result");
		check(whyEmpty == g_message1,
			"empty value: resolve() writes message 1 word for word");

		// ---------------- case 4: the variable names a file, not a directory
		// (message 2)
		//
		// "names anything that is not an existing directory" includes a path
		// that exists as a file. The Python half carries the same case in
		// test_a_path_that_is_a_file_and_not_a_directory_returns_message_two.
		{
			std::ofstream notADir("t0_artifact_resolver_temp.txt");
			notADir << "not a directory\n";
			notADir.close();

			const std::string filePath = "t0_artifact_resolver_temp.txt";
			setArtifactsVariable(filePath.c_str());

			std::string whyFile = "this string must be overwritten";
			const std::string resultFile = resolver.resolve(whyFile);

			check(resultFile.empty(),
				"path that is a file: resolve() returns an empty result");
			const std::string expectedMessage2File =
				"firmware artifact not available (NMG2_ARTIFACTS names no directory: "
				+ filePath + ")";
			check(whyFile == expectedMessage2File,
				"path that is a file: resolve() writes message 2 word for word");

			std::remove(filePath.c_str());
		}

		// ---------------- case 5: the variable names a real directory and the
		// caller passes a name whose file is not in it (message 3)
		//
		// The Python half covers this in
		// test_directory_without_named_artifact_returns_message_three. The
		// message echoes the name and the variable's value.
		{
			const std::string presentDir = ".";
			setArtifactsVariable(presentDir.c_str());

			const char* const absentName = "REPO-5-not-here.bin";
			std::string whyMissingFile = "this string must be overwritten";
			const std::string resultMissingFile = resolver.resolve(whyMissingFile, absentName);

			check(resultMissingFile.empty(),
				"directory without named artifact: resolve() returns an empty result");
			const std::string expectedMessage3 =
				"firmware artifact not available ("
				+ std::string(absentName)
				+ " not found under NMG2_ARTIFACTS: "
				+ presentDir + ")";
			check(whyMissingFile == expectedMessage3,
				"directory without named artifact: resolve() writes message 3 word for word");

			// Message 3 is distinct from message 1 and message 2.
			check(whyMissingFile != g_message1,
				"directory without named artifact: message 3 is distinct from message 1");
		}

		// ---------------- case 6: the variable names a real directory, the
		// caller passes a name, and the file is in it (success)
		//
		// The negative case for message 3. Without it, the resolver could
		// always fire message 3 and every message-3 assertion above would hold.
		{
			std::ofstream presentFile("REPO-5-present.bin");
			presentFile << "not a real artifact\n";
			presentFile.close();

			std::string whyFoundFile = "this string must be cleared";
			const std::string resultFoundFile = resolver.resolve(whyFoundFile, "REPO-5-present.bin");

			check(!resultFoundFile.empty(),
				"directory with named artifact: resolve() returns a non-empty result");
			check(resultFoundFile == ".",
				"directory with named artifact: the resolved directory is the one the variable named");
			check(whyFoundFile.empty(),
				"directory with named artifact: a successful resolve writes no reason");

			std::remove("REPO-5-present.bin");
		}

		// ---------------- case 7: a real directory and no name (success)
		//
		// The negative case for messages 2 and 3. Without it, the resolver could
		// always fail and every empty-result assertion above would hold.
		// This is also the call shape firmwareState.h and gatedFixture.h use
		// today.
		{
			setArtifactsVariable(".");

			std::string whyPresent = "this string must be cleared";
			const std::string resultPresent = resolver.resolve(whyPresent);

			check(!resultPresent.empty(),
				"present directory, no name: resolve() returns a non-empty result");
			check(resultPresent == ".",
				"present directory, no name: the resolved directory is the one the variable named");
			check(whyPresent.empty(),
				"present directory, no name: a successful resolve writes no reason");
		}

		// ---------------- case 8: a real directory, a name, and the file is
		// there with no NMG2_ARTIFACTS set at all is a contradiction this test
		// does not run. The directory for case 6 was created in case 6 itself.

		setArtifactsVariable(nullptr);
	}
	catch(const std::exception& _e)
	{
		std::cout << "FAIL resolve() threw std::exception: " << _e.what() << std::endl;
		++g_failures;
	}
	catch(...)
	{
		std::cout << "FAIL resolve() threw a non-std exception" << std::endl;
		++g_failures;
	}

	if(g_failures)
	{
		std::cout << "t0_artifact_resolver: " << g_failures << " failure(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_artifact_resolver: all checks passed" << std::endl;
	return 0;
}
