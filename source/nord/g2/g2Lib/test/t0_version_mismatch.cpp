// Task BRD-11. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.2, BRD-11. Design sections 7.3 step 6 and 15.8.
//
// WHAT THIS TEST IS FOR. Design section 15.8 answers understanding section
// 12.3 question 14 -- what happens when a project was authored against one OS
// version and is restored on a machine that runs another -- with three rows and
// one rule: WARN AND REQUIRE EXPLICIT CONFIRMATION, NEVER REINTERPRET SILENTLY.
// This test drives all three rows and asserts the rule directly.
//
// THE WORST OF THE THREE OUTCOMES IS THE SILENT ONE. A silent reinterpretation
// of a bit-packed structure produces a wrong sound with no warning, and neither
// the plugin nor the user is told. Case group 4 is the exhaustive sweep that
// says no version pair can reach it.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so an assert() is removed and a check built on
// one could never fire. Every case reports through the counters below and the
// process exit status.
//
// THE EXPECTED MESSAGES ARE WRITTEN OUT IN FULL ON PURPOSE. Design section 15.8
// requires a message that NAMES BOTH VERSIONS, and a test that built the
// expected text with the same call the header uses would pass for any text at
// all, including a text that named neither version. Task REPO-5's own test
// carries the same reasoning for the same reason.

#include "firmwareVersion.h"

#include <cstdint>
#include <iostream>
#include <string>

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

	const char* outcomeName(const g2::FirmwareVersionOutcome _outcome)
	{
		switch(_outcome)
		{
		case g2::FirmwareVersionOutcome::LoadNormally:   return "LoadNormally";
		case g2::FirmwareVersionOutcome::MachineOnly:    return "MachineOnly";
		case g2::FirmwareVersionOutcome::NoFirmware:     return "NoFirmware";
		}
		return "unknown";
	}

	// THE WHOLE DECISION AS ONE STRING, so that every case asserts every field.
	// A case that compared one field would pass for a decision that carried the
	// right outcome and the wrong message, which is the only part of the
	// decision the user ever sees.
	std::string render(const g2::FirmwareVersionDecision& _decision)
	{
		std::string text = outcomeName(_decision.outcome);
		text += " machine=";
		text += _decision.loadMachine ? "yes" : "no";
		text += " patch=";
		text += _decision.loadPatchData ? "yes" : "no";
		text += " offer=";
		text += _decision.offerToLoadAnyway ? "yes" : "no";
		text += " message=<" + _decision.message + ">";
		return text;
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 0. THE VERSION WORD READS AS A RELEASE NUMBER.
	//
	// Design section 7.3 step 6 records the version word, and the word is a
	// plain integer that the release splits at the hundreds. 0x00A2 is 162,
	// which is release 1.62.
	checkEqual(g2::versionText(0x00A2u), "1.62", "0x00A2 reads as release 1.62");
	checkEqual(g2::versionText(100u), "1.00", "100 reads as release 1.00");
	checkEqual(g2::versionText(99u), "0.99", "99 reads as release 0.99");
	checkEqual(g2::versionText(0u), "0.00", "0 reads as release 0.00");
	checkEqual(g2::versionText(1000u), "10.00", "1000 reads as release 10.00");

	// -----------------------------------------------------------------------
	// Case group 1. THE VERSIONS MATCH, SO THE PATCH LOADS NORMALLY.
	{
		const g2::FirmwareVersionDecision decision =
			g2::decideFirmwareVersion(true, 0x00A2u, 0x00A2u);

		checkEqual(render(decision),
			"LoadNormally machine=yes patch=yes offer=no message=<>",
			"matching versions load the machine and the patch data, with no message");
	}

	// -----------------------------------------------------------------------
	// Case group 2. THE VERSIONS DIFFER.
	//
	// Design section 15.8: load the machine, do NOT load the patch data, show a
	// message that names BOTH versions, and offer to load anyway.
	{
		const g2::FirmwareVersionDecision decision =
			g2::decideFirmwareVersion(true, 0x00A2u, 0x00A0u);

		checkEqual(render(decision),
			"MachineOnly machine=yes patch=no offer=yes message=<"
			"This patch was saved with OS 1.60 and this machine runs OS 1.62. "
			"The machine is loaded and the patch data is not, because patch data "
			"portability across OS versions is unproved. Load the patch data anyway?>",
			"a patch older than the machine loads the machine alone and names both versions");
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE PATCH IS NEWER THAN THE MACHINE.
	//
	// The same row of the table, and the two names swap slots. A message built
	// from one version and a fixed word for the other would pass case group 2
	// and fail here.
	{
		const g2::FirmwareVersionDecision decision =
			g2::decideFirmwareVersion(true, 0x0096u, 0x00A2u);

		checkEqual(render(decision),
			"MachineOnly machine=yes patch=no offer=yes message=<"
			"This patch was saved with OS 1.62 and this machine runs OS 1.50. "
			"The machine is loaded and the patch data is not, because patch data "
			"portability across OS versions is unproved. Load the patch data anyway?>",
			"a patch newer than the machine loads the machine alone and names both versions");
	}

	// -----------------------------------------------------------------------
	// Case group 4. NO FIRMWARE IS PRESENT.
	//
	// Design section 15.8's third row hands this case to section 7.7, which is
	// task BRD-10's. THIS POLICY ANSWERS NOTHING HERE ON ITS OWN: it reports
	// NoFirmware, loads nothing, offers nothing and carries no message, so that
	// two texts for one state cannot drift apart. The two version arguments are
	// deliberately different, and they change nothing.
	{
		const g2::FirmwareVersionDecision decision =
			g2::decideFirmwareVersion(false, 0x00A2u, 0x0096u);

		checkEqual(render(decision),
			"NoFirmware machine=no patch=no offer=no message=<>",
			"no firmware loads nothing and hands the message to design section 7.7");

		const g2::FirmwareVersionDecision same =
			g2::decideFirmwareVersion(false, 0x0000u, 0x0000u);

		checkEqual(render(same), render(decision),
			"no firmware answers the same way whatever the two version words hold");
	}

	// -----------------------------------------------------------------------
	// Case group 5. THE EXHAUSTIVE SWEEP: NEVER REINTERPRET SILENTLY.
	//
	// Every pair of version words in 0 to 399 against 0 to 399, which is
	// 160,000 pairs. The rule is asserted directly rather than by example:
	//
	//   * The patch data loads if and only if the two words are EQUAL.
	//   * A pair that differs ALWAYS carries a message, ALWAYS offers to load
	//     anyway, and NEVER reports LoadNormally.
	//   * A pair that differs names BOTH release numbers in its message.
	//
	// The third of those is what a message built from one version alone fails,
	// and the second is the silent reinterpretation the design calls the worst
	// of the three outcomes.
	{
		bool patchFollowsEquality = true;
		bool everyDifferenceWarns = true;
		bool everyDifferenceNamesBoth = true;
		bool everyPairLoadsTheMachine = true;
		int differingPairs = 0;

		for(uint16_t machine = 0; machine < 400u; ++machine)
		{
			for(uint16_t patch = 0; patch < 400u; ++patch)
			{
				const g2::FirmwareVersionDecision decision =
					g2::decideFirmwareVersion(true, machine, patch);

				if(decision.loadPatchData != (machine == patch))
					patchFollowsEquality = false;

				if(!decision.loadMachine)
					everyPairLoadsTheMachine = false;

				if(machine == patch)
					continue;

				++differingPairs;

				if(decision.outcome != g2::FirmwareVersionOutcome::MachineOnly
					|| decision.message.empty()
					|| !decision.offerToLoadAnyway)
				{
					everyDifferenceWarns = false;
				}

				const std::string machineText = g2::versionText(machine);
				const std::string patchText = g2::versionText(patch);

				if(decision.message.find(machineText) == std::string::npos
					|| decision.message.find(patchText) == std::string::npos)
				{
					everyDifferenceNamesBoth = false;
				}
			}
		}

		check(differingPairs == 400 * 400 - 400,
			"the sweep drove 159,600 differing pairs and 400 matching ones");
		check(patchFollowsEquality,
			"the patch data loads if and only if the two version words are equal");
		check(everyPairLoadsTheMachine,
			"the machine loads for every pair, matching or not");
		check(everyDifferenceWarns,
			"every differing pair warns, offers to load anyway and never reports LoadNormally");
		check(everyDifferenceNamesBoth,
			"every differing pair names both release numbers in its message");
	}

	if(g_failures)
	{
		std::cout << "t0_version_mismatch: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_version_mismatch: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
