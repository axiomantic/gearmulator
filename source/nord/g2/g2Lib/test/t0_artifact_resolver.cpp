// Task REPO-5. Tier T0: this test runs with NMG2_ARTIFACTS unset and needs no
// firmware artifact of any kind.
//
// Plan section 9.2, REPO-5. Design sections 4.2 and 18.5.
//
// The three properties this test holds the resolver to:
//   1. With NMG2_ARTIFACTS UNSET, resolve() returns an empty string and writes
//      the message of design section 4.2 WORD FOR WORD.
//   2. With NMG2_ARTIFACTS set to a directory that does not exist, the result
//      is THE SAME -- the same empty string and the same message. Design
//      section 4.2 states both cases and gives them one message.
//   3. resolve() NEVER throws. Design sections 5.3 and 13.10 give the
//      no-exceptions rule and section 4.2 restates it on this method.
//
// The message literal below is written out in full ON PURPOSE. The Python half
// of this task, nmg2_tools/artifacts.py in axiomantic/nmg2-tools, carries the
// same literal and tests/test_artifacts.py asserts it the same way. Comparing
// against a full literal in each language is what makes "word for word and
// identically in both languages" a falsifiable claim.

#include "../artifactResolver.h"

#include <cstdlib>
#include <exception>
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

	// The message, spelled out. Nothing in this file derives it from the header
	// under test: a test that reads its expectation from the code it tests
	// asserts only that the code equals itself.
	const std::string g_expectedMessage = "firmware artifact not available (NMG2_ARTIFACTS unset)";

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
}

int main()
{
	try
	{
		g2::EnvArtifactResolver resolver;

		// ---------------- case 1: the variable is unset

		setArtifactsVariable(nullptr);

		std::string whyUnset = "this string must be overwritten";
		const std::string resultUnset = resolver.resolve(whyUnset);

		check(resultUnset.empty(), "unset: resolve() returns an empty result");
		check(whyUnset == g_expectedMessage, "unset: resolve() writes the message word for word");

		// ---------------- case 2: the variable names a directory that is not there
		//
		// The path is under the build's own temporary space and is never
		// created. The plan requires the result to be THE SAME as case 1.

		setArtifactsVariable("/nmg2/no/such/directory/REPO-5");

		std::string whyMissing = "this string must be overwritten";
		const std::string resultMissing = resolver.resolve(whyMissing);

		check(resultMissing.empty(), "missing directory: resolve() returns an empty result");
		check(whyMissing == g_expectedMessage, "missing directory: resolve() writes the message word for word");

		// The word "same" is asserted directly and not left to the reader.
		check(resultMissing == resultUnset, "missing directory: the result is the same as the unset result");
		check(whyMissing == whyUnset, "missing directory: the message is the same as the unset message");

		// ---------------- case 3: an empty variable is an unset variable
		//
		// Windows has no way to remove a variable through _putenv_s other than
		// assigning it the empty string, so the empty value must behave as the
		// unset value or this test means different things on two platforms.

		setArtifactsVariable("");

		std::string whyEmpty = "this string must be overwritten";
		const std::string resultEmpty = resolver.resolve(whyEmpty);

		check(resultEmpty.empty(), "empty value: resolve() returns an empty result");
		check(whyEmpty == g_expectedMessage, "empty value: resolve() writes the message word for word");

		// ---------------- the negative case
		//
		// Every assertion above is that a counter is empty. Section 5.2 rule 6:
		// a counter asserted to be zero needs a companion case that drives it
		// above zero. A directory that DOES exist must resolve, or the four
		// assertions above would hold for a resolver that always fails.

		const char* const presentDirectory = ".";
		setArtifactsVariable(presentDirectory);

		std::string whyPresent = "this string must be cleared";
		const std::string resultPresent = resolver.resolve(whyPresent);

		check(!resultPresent.empty(), "negative case: an existing directory resolves to a non-empty result");
		check(resultPresent == presentDirectory, "negative case: the resolved directory is the one the variable named");
		check(whyPresent.empty(), "negative case: a successful resolve writes no reason");

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
