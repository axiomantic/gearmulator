// Task BRD-11. The firmware version and the mismatch policy.
//
// Plan section 13.2, BRD-11. Design sections 7.3 step 6 and 15.8.
//
// WHAT THIS FILE IS. Design section 15.8 answers what happens when a project
// was authored against one OS version and is restored on a machine that runs
// another. Its decision is: WARN AND REQUIRE EXPLICIT CONFIRMATION, NEVER
// REINTERPRET SILENTLY.
//
//   | Versions match      | Load normally.                                    |
//   | Versions differ     | Load the machine. Do not load the patch data.     |
//   |                     | Show a message that names both versions. Offer to |
//   |                     | load anyway.                                      |
//   | No firmware present | Design section 7.7.                               |
//
// The reason is that patch data portability across OS versions is UNPROVED. A
// silent reinterpretation of a bit-packed structure produces a wrong sound with
// no warning, which is the worst of the three outcomes: the machine does not
// stop, the user is not told, and the fault reaches the ear rather than the
// log.
//
// THIS TASK DECLARES THE POLICY AND WIRES NOTHING. Task PLG-5 wires it into
// g2State.cpp, because plan section 7.4.2 gives g2JucePlugin/ to the plugin
// track. Nothing here reads plugin state, opens a file or shows a window: the
// decision is a value, and the caller acts on it.
//
// THE THIRD ROW ANSWERS NOTHING HERE. Design section 7.7 owns the no-firmware
// state and task BRD-10 declares its surface, so this file reports NoFirmware
// and carries no message for it. Two texts for one state are two texts that can
// drift, and the one an implementer copies is the one that goes stale.
//
// HEADER-ONLY ON PURPOSE: BRD-11's Files: line names this header and no
// translation unit.

#pragma once

#include "firmwareExtract.h"

#include <cstdint>
#include <string>

namespace g2
{
	// The three rows of the design section 15.8 table.
	enum class FirmwareVersionOutcome
	{
		LoadNormally,     // the versions match
		MachineOnly,      // the versions differ
		NoFirmware,       // design section 7.7 answers
	};

	/* What the caller does, and what it shows.
	 *
	 * `message` is EMPTY except on a mismatch. An empty message is not a
	 * silence: the two states that carry none are the two the caller already
	 * knows what to do about, and the no-firmware state gets its message from
	 * design section 7.7. */
	struct FirmwareVersionDecision
	{
		FirmwareVersionOutcome outcome = FirmwareVersionOutcome::NoFirmware;
		bool loadMachine = false;
		bool loadPatchData = false;
		bool offerToLoadAnyway = false;
		std::string message;
	};

	/* The decision, from the version word the machine's firmware carries and the
	 * version word the patch was saved with. Both are the RAW 16-BIT WORDS that
	 * design section 7.3 step 6 records, and not text.
	 *
	 * `_firmwarePresent` false answers NoFirmware whatever the two words hold,
	 * because a machine with no firmware has no version to compare against. */
	inline FirmwareVersionDecision decideFirmwareVersion(const bool _firmwarePresent,
		const uint16_t _machineVersion, const uint16_t _patchVersion)
	{
		FirmwareVersionDecision decision;

		if(!_firmwarePresent)
		{
			decision.outcome = FirmwareVersionOutcome::NoFirmware;
			return decision;
		}

		decision.loadMachine = true;

		if(_machineVersion == _patchVersion)
		{
			decision.outcome = FirmwareVersionOutcome::LoadNormally;
			decision.loadPatchData = true;
			return decision;
		}

		// THE MESSAGE NAMES BOTH RELEASE NUMBERS, and it names which is which.
		// A message that named one version alone would leave the user unable to
		// tell whether the patch or the machine is the older of the two, and
		// that is the fact the answer to the offer turns on.
		decision.outcome = FirmwareVersionOutcome::MachineOnly;
		decision.loadPatchData = false;
		decision.offerToLoadAnyway = true;
		decision.message = "This patch was saved with OS " + versionText(_patchVersion)
			+ " and this machine runs OS " + versionText(_machineVersion)
			+ ". The machine is loaded and the patch data is not, because patch data "
			+ "portability across OS versions is unproved. Load the patch data anyway?";
		return decision;
	}
}
