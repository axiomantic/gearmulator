// Task BRD-10. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.2, BRD-10. Design sections 7.7 and 17 row 7.7.
//
// WHAT THIS TEST IS FOR. Design section 7.7 gives four requirements for the
// case where the plugin loads in the host and finds no OS update file:
//
//   1. The plugin loads. It does not crash.
//   2. It produces silence, and it reports its state clearly.
//   3. It shows a message that names the exact file it needs, gives the version
//      it expects, and says where to get it.
//   4. It does not retry silently and it does not fail quietly.
//
// THE FOURTH IS THE ONE A TEST USUALLY MISSES. "Does not retry silently" is a
// statement about how many times the surface asks, so case group 5 counts the
// calls a COUNTING RESOLVER receives. A surface that looped inside itself would
// answer the same and be caught by nothing else in this file.
//
// THE TEST OWNS THE ENVIRONMENT VARIABLE RATHER THAN ASSUMING IT. BRD-10's
// Check: line says "with NMG2_ARTIFACTS unset", and a developer who has it set
// would otherwise drive the opposite case and see a green run. Every case sets
// or clears the variable itself.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so an assert() is removed and a check built on
// one could never fire. Every case reports through the counters below and the
// process exit status.

#include "firmwareState.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	void checkEqual(const std::string& _actual, const std::string& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << "\n     expected <" << _expected
			<< ">\n     got      <" << _actual << ">" << std::endl;
		++g_failures;
	}

	void setArtifacts(const char* _value)
	{
#ifdef _WIN32
		_putenv_s("NMG2_ARTIFACTS", _value ? _value : "");
#else
		if(_value)
			setenv("NMG2_ARTIFACTS", _value, 1);
		else
			unsetenv("NMG2_ARTIFACTS");
#endif
	}

	// Counts what it is asked. Design section 4.2 makes ArtifactResolver the
	// seam, and REPO-5's own header says a fixture owns the resolver, so this is
	// the resolver a fixture owns rather than a mock of one.
	class CountingResolver final : public g2::ArtifactResolver
	{
	public:
		explicit CountingResolver(std::string _directory) : m_directory(std::move(_directory)) {}

		std::string resolve(std::string& _why, const char* /*_name*/) override
		{
			++calls;
			if(m_directory.empty())
			{
				_why = g2::g_artifactUnavailableMessage;
				return {};
			}
			_why.clear();
			return m_directory;
		}

		int calls = 0;

	private:
		std::string m_directory;
	};

	// THE EXPECTED MESSAGE IS WRITTEN OUT IN FULL ON PURPOSE. Design section 7.7
	// item 3 asks for a message that names several things, and a test that built
	// the expected text with the same pieces the header uses would pass for a
	// message that named none of them. Task REPO-5's own test carries the same
	// reasoning for the same reason.
	const char* g_expectedMessage =
		"firmware artifact not available (NMG2_ARTIFACTS unset). "
		"This plugin needs Clavia's own Nord Modular G2 OS update file: "
		"\"Nord Modular G2 OS v1.62 Update.dmg\" on macOS, or "
		"\"Nord Modular G2 v1.62 Setup.exe\" on Windows. "
		"Both carry OS version 1.62 and both hold the same firmware. "
		"Get the update from Clavia's own support pages for the Nord Modular G2, "
		"then set NMG2_ARTIFACTS to the directory that holds the file. "
		"This plugin is silent until then, and it does not look again on its own.";
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 0. WITH NMG2_ARTIFACTS UNSET THE SURFACE REPORTS ABSENT.
	//
	// This is BRD-10's own Check: line, driven through the resolver design
	// section 4.2 declares and not through getenv directly.
	{
		setArtifacts(nullptr);

		g2::EnvArtifactResolver resolver;
		const g2::FirmwareStatus status = g2::resolveFirmwareState(resolver);

		check(status.state == g2::FirmwareState::Absent,
			"an unset NMG2_ARTIFACTS reports the firmware absent");
		check(!status.producesAudio,
			"an absent firmware produces no audio");
		checkEqual(status.report, "firmware: absent",
			"an absent firmware reports its state in one clear line");
		checkEqual(status.message, g_expectedMessage,
			"an absent firmware names the file, the version and where to get it");
	}

	// -----------------------------------------------------------------------
	// Case group 1. THE MESSAGE CARRIES EACH OF THE THINGS SECTION 7.7
	// ITEM 3 NAMES.
	//
	// Case group 0 compares the whole text, which is the stronger assertion.
	// These four restate the requirement itself, so that a later edit which
	// dropped one of the three fails against the sentence that asked for it
	// rather than against a diff.
	//
	// THE TEXT COMES FROM THE SURFACE AND NOT FROM THE LITERAL ABOVE. A case
	// group that searched its own expected string would report the same result
	// for every possible implementation, including one that produced no message
	// at all. That is a check that cannot fail, and it is not one this file
	// carries.
	{
		setArtifacts(nullptr);

		g2::EnvArtifactResolver resolver;
		const std::string message = g2::resolveFirmwareState(resolver).message;

		check(message.find("Nord Modular G2 OS v1.62 Update.dmg") != std::string::npos
			&& message.find("Nord Modular G2 v1.62 Setup.exe") != std::string::npos,
			"item 3: the message names the exact file it needs, on both platforms");
		check(message.find("1.62") != std::string::npos,
			"item 3: the message gives the version it expects");
		check(message.find("Clavia's own support pages") != std::string::npos,
			"item 3: the message says where to get it");
		check(message.find("does not look again on its own") != std::string::npos,
			"item 4: the message says the plugin does not retry silently");
	}

	// -----------------------------------------------------------------------
	// Case group 2. NMG2_ARTIFACTS NAMING A DIRECTORY THAT IS THERE REPORTS
	// PRESENT.
	//
	// Without this case the state could be hardwired to Absent and every other
	// case in this file would still pass.
	{
		setArtifacts(".");

		g2::EnvArtifactResolver resolver;
		const g2::FirmwareStatus status = g2::resolveFirmwareState(resolver);

		check(status.state == g2::FirmwareState::Present,
			"a directory that is there reports the firmware present");
		check(status.producesAudio,
			"a present firmware produces audio");
		checkEqual(status.report, "firmware: present",
			"a present firmware reports its state in one clear line");
		checkEqual(status.message, "",
			"a present firmware shows no absence message");
		checkEqual(status.directory, ".",
			"a present firmware carries the directory the resolver returned");

		setArtifacts(nullptr);
	}

	// -----------------------------------------------------------------------
	// Case group 3. NMG2_ARTIFACTS NAMING A DIRECTORY THAT IS NOT THERE GETS A
	// DISTINCT MESSAGE FROM UNSET.
	//
	// Design section 4.2 names THREE failure messages -- one for unset, one
	// for a path that is not a directory, and one for a directory that lacks
	// the named artifact. The unset case and the missing-directory case are
	// DIFFERENT inputs and get DIFFERENT messages so that an operator with a
	// wrong path sees the path they typed rather than a message that sends
	// them to look at the wrong file.
	{
		const std::string missingDir = "/this/directory/does/not/exist/nmg2";
		setArtifacts(missingDir.c_str());

		g2::EnvArtifactResolver resolver;
		const g2::FirmwareStatus status = g2::resolveFirmwareState(resolver);

		check(status.state == g2::FirmwareState::Absent,
			"a directory that is not there reports the firmware absent");
		// The status.message opens with the resolver's message (message 2
		// here) and then carries the requirement-3 text. Both messages start
		// with "firmware artifact not available (" and that is the only
		// overlap.
		check(status.message.find(
			"firmware artifact not available (NMG2_ARTIFACTS names no directory: "
			+ missingDir + ")") == 0,
			"a directory that is not there opens with message 2 word for word");

		setArtifacts(nullptr);
	}

	// -----------------------------------------------------------------------
	// Case group 4. IT DOES NOT FAIL QUIETLY.
	//
	// Neither state may leave the caller with nothing to show. The absent state
	// carries both a report and a message; the present state carries a report.
	{
		CountingResolver absent("");
		CountingResolver present("/tmp");

		const g2::FirmwareStatus whenAbsent = g2::resolveFirmwareState(absent);
		const g2::FirmwareStatus whenPresent = g2::resolveFirmwareState(present);

		check(!whenAbsent.report.empty() && !whenAbsent.message.empty(),
			"the absent state carries both a report and a message");
		check(!whenPresent.report.empty(),
			"the present state carries a report");
		check(whenAbsent.report != whenPresent.report,
			"the two states do not report the same line");
	}

	// -----------------------------------------------------------------------
	// Case group 5. IT DOES NOT RETRY IN SILENCE.
	//
	// The surface asks the resolver ONCE for each call it is given, and never
	// more. A surface that looped inside itself -- waiting, polling, or trying
	// a second location -- would report the same state and would be caught by
	// nothing else in this file.
	//
	// A caller that wants to look again calls again, which is what makes the
	// second look the caller's decision and therefore visible to the user.
	{
		CountingResolver resolver("");

		const g2::FirmwareStatus first = g2::resolveFirmwareState(resolver);
		check(resolver.calls == 1,
			"one call to the surface is one call to the resolver, and no retry");

		const g2::FirmwareStatus second = g2::resolveFirmwareState(resolver);
		check(resolver.calls == 2,
			"a second call to the surface is a second call to the resolver");

		checkEqual(second.report, first.report,
			"a second look reports the same state as the first");
		checkEqual(second.message, first.message,
			"a second look shows the same message as the first");
	}

	// -----------------------------------------------------------------------
	// Case group 6. IT PRODUCES SILENCE.
	//
	// Design section 7.7 item 2. The buffer is filled with a pattern that is not
	// silence FIRST, so that a fill which wrote nothing at all fails here.
	{
		std::vector<int32_t> samples(64, 0x0055AA33);

		g2::fillSilence(samples.data(), samples.size());

		bool everySampleIsSilent = true;
		for(const int32_t sample : samples)
		{
			if(sample != 0)
				everySampleIsSilent = false;
		}

		check(everySampleIsSilent, "fillSilence writes Q23 silence to every sample of the buffer");
		check(g2::g_silentSample == 0, "Q23 silence is the value zero");
	}

	// -----------------------------------------------------------------------
	// Case group 7. A COUNT OF ZERO WRITES NOTHING.
	//
	// The bytes on either side of the range are left alone, so a fill that ran
	// off its own end is caught rather than tolerated.
	{
		std::vector<int32_t> samples(4, 0x0055AA33);

		g2::fillSilence(samples.data() + 1, 0);

		check(samples[0] == 0x0055AA33 && samples[1] == 0x0055AA33
			&& samples[2] == 0x0055AA33 && samples[3] == 0x0055AA33,
			"a count of zero leaves every sample of the buffer alone");
	}

	// -----------------------------------------------------------------------
	// Case group 8. A FILL WRITES ONLY THE RANGE IT WAS GIVEN.
	{
		std::vector<int32_t> samples(6, 0x0055AA33);

		g2::fillSilence(samples.data() + 2, 2);

		check(samples[0] == 0x0055AA33 && samples[1] == 0x0055AA33
			&& samples[2] == 0 && samples[3] == 0
			&& samples[4] == 0x0055AA33 && samples[5] == 0x0055AA33,
			"a fill of two samples in the middle leaves the four around it alone");
	}

	if(g_failures)
	{
		std::cout << "t0_no_firmware: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_no_firmware: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
