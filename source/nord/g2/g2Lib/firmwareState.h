// Task BRD-10. The no-firmware path.
//
// Plan section 13.2, BRD-10. Design sections 7.7 and 17 row 7.7.
//
// WHAT THIS FILE IS. Design section 7.7 answers what the plugin does when it
// loads in the host and finds no OS update file, in four requirements:
//
//   1. The plugin loads. It does not crash.
//   2. It produces silence, and it reports its state clearly.
//   3. It shows a message that names the exact file it needs, gives the version
//      it expects, and says where to get it.
//   4. It does not retry silently and it does not fail quietly.
//
// The mq, virus and xt targets set the user-flow precedent, and this follows it.
//
// THIS FILE DECLARES THE SURFACE AND WIRES NOTHING. The wiring into
// g2Device.cpp belongs to the plugin track, because plan section 7.4.2 gives
// g2JucePlugin/ to that track. Nothing here opens a window, holds plugin state
// or names a JUCE type.
//
// REQUIREMENT 4 IS A PROPERTY OF THE SHAPE AND NOT OF A COMMENT. This surface
// asks the resolver EXACTLY ONCE for each call it is given. It does not wait, it
// does not poll and it does not try a second location. A caller that wants to
// look again calls again, which is what makes the second look the caller's
// decision, and therefore visible to the user. A surface that retried inside
// itself would report the same state and nothing could tell the two apart.
//
// IT IS WRITTEN AGAINST ArtifactResolver AND NEVER AGAINST getenv, so that a
// later fetch implementation of design section 4.2 changes nothing here. Design
// section 18.5 step 1 states that requirement directly and task REPO-7's
// fixture takes the same seam.
//
// HEADER-ONLY ON PURPOSE: BRD-10's Files: line names this header and no
// translation unit.
//

#pragma once

#include "artifactResolver.h"
#include "firmwareExtract.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace g2
{
	// The two states design section 7.7 distinguishes. A directory that is not
	// there is reported ABSENT just like a variable that is not set -- both
	// cases mean the plugin cannot find the firmware -- but they reach this
	// state through DIFFERENT messages. Design section 4.2 names three failure
	// messages; this enum does not encode that detail, only the up/down
	// outcome the plugin reports to the host.
	enum class FirmwareState
	{
		Absent,
		Present,
	};

	// The version word the project expects, which design section 7.3 step 5
	// records. 0x00A2 is 162, which is release 1.62; design section 7.2 names
	// the two update files that carry it.
	constexpr uint16_t g_expectedFirmwareVersion = 0x00A2u;

	// Q23 SILENCE. AGENTS.md section 2.3 puts 1.0 at 0x800000, so silence is
	// zero, and the storage is a SIGNED INTEGER. A float here would put a
	// floating-point value inside the Q23 boundary, which design section 13.10
	// forbids.
	constexpr int32_t g_silentSample = 0;

	static_assert(std::is_integral<decltype(g_silentSample)>::value,
		"Q23 silence is integer. No floating-point type may cross this boundary.");
	static_assert(std::is_signed<decltype(g_silentSample)>::value,
		"Q23 uses bit 23 as the sign bit, so the storage is signed.");

	/* Writes Q23 silence over `_count` samples.
	 *
	 * This is the ONE statement of "silence" the project carries, so that the
	 * plugin's no-firmware path and any other silent path cannot disagree about
	 * what silence is. A count of 0 writes nothing. */
	inline void fillSilence(int32_t* _samples, const size_t _count)
	{
		for(size_t i = 0; i < _count; ++i)
			_samples[i] = g_silentSample;
	}

	/* What the plugin found, what it may do, and what it shows.
	 *
	 * `report` is ONE SHORT LINE and it is never empty. It is requirement 2's
	 * "reports its state clearly", and it is what a log or a status field
	 * carries.
	 *
	 * `message` is requirement 3's text and it is empty when the firmware is
	 * present. It is what the user reads.
	 *
	 * `directory` is the artifact directory, and it is empty when the firmware
	 * is absent. */
	struct FirmwareStatus
	{
		FirmwareState state = FirmwareState::Absent;
		bool producesAudio = false;
		std::string report;
		std::string message;
		std::string directory;
	};

	/* The message requirement 3 asks for.
	 *
	 * IT OPENS WITH THE RESOLVER'S OWN REASON, BY CONCATENATION. Design section
	 * 4.2 fixes that wording and task REPO-5 owns it; spelling it out a second
	 * time here would give one state two texts, and the one an implementer
	 * copies is the one that goes stale. Task REPO-7's fixture builds its skip
	 * line the same way for the same reason.
	 *
	 * The rest names what the requirement asks for: the exact file,
	 * on both platforms design section 7.2 accepts; the version those files
	 * carry; and where the user gets them. IT ALSO SAYS THAT THE PLUGIN WILL NOT
	 * LOOK AGAIN, which is requirement 4 made visible to the user rather than
	 * left as a property of the code. */
	inline std::string firmwareAbsentMessage(const std::string& _why)
	{
		const std::string version = versionText(g_expectedFirmwareVersion);

		return _why + ". "
			+ "This plugin needs Clavia's own Nord Modular G2 OS update file: "
			+ "\"Nord Modular G2 OS v" + version + " Update.dmg\" on macOS, or "
			+ "\"Nord Modular G2 v" + version + " Setup.exe\" on Windows. "
			+ "Both carry OS version " + version + " and both hold the same firmware. "
			+ "Get the update from Clavia's own support pages for the Nord Modular G2, "
			+ "then set NMG2_ARTIFACTS to the directory that holds the file. "
			+ "This plugin is silent until then, and it does not look again on its own.";
	}

	/* Asks the resolver ONCE and reports what it found.
	 *
	 * Never throws: this obeys the no-exceptions rule of design sections 5.3
	 * and 13.10, which is requirement 1 -- the plugin loads and does not crash
	 * -- expressed as a property of this call rather than as a hope. */
	inline FirmwareStatus resolveFirmwareState(ArtifactResolver& _resolver)
	{
		FirmwareStatus status;

		std::string why;
		const std::string directory = _resolver.resolve(why);

		if(directory.empty())
		{
			status.state = FirmwareState::Absent;
			status.producesAudio = false;
			status.report = "firmware: absent";
			status.message = firmwareAbsentMessage(why);
			return status;
		}

		status.state = FirmwareState::Present;
		status.producesAudio = true;
		status.report = "firmware: present";
		status.directory = directory;
		return status;
	}
}
