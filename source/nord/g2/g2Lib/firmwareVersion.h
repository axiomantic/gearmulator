// The firmware version and the mismatch policy: what happens when a project
// was authored against one OS version and is restored on a machine that runs
// another. Warn and require explicit confirmation; never reinterpret silently.
//
//   | Versions match      | Load normally.                                    |
//   | Versions differ     | Load the machine. Do not load the patch data.     |
//   |                     | Show a message that names both versions. Offer to |
//   |                     | load anyway.                                      |
//   | No firmware present | firmwareState.h answers.                          |
//
// Patch data portability across OS versions is unproved. A silent
// reinterpretation of a bit-packed structure produces a wrong sound with no
// warning, which is the worst of the three outcomes: the machine does not stop,
// the user is not told, and the fault reaches the ear rather than the log.
//
// The decision is a value and the caller acts on it. Nothing here reads plugin
// state, opens a file or shows a window. The no-firmware row carries no message
// here, because two texts for one state are two texts that can drift.

#pragma once

#include "firmwareExtract.h"

#include <cstdint>
#include <string>

namespace g2
{
	enum class FirmwareVersionOutcome
	{
		LoadNormally,     // the versions match
		MachineOnly,      // the versions differ
		NoFirmware,
	};

	/* What the caller does, and what it shows.
	 *
	 * `message` is empty except on a mismatch. The no-firmware state gets its
	 * message from firmwareState.h. */
	struct FirmwareVersionDecision
	{
		FirmwareVersionOutcome outcome = FirmwareVersionOutcome::NoFirmware;
		bool loadMachine = false;
		bool loadPatchData = false;
		bool offerToLoadAnyway = false;
		std::string message;
	};

	/* The decision, from the version word the machine's firmware carries and the
	 * version word the patch was saved with. Both are raw 16-bit words, not text.
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

		// The message names both release numbers, and which is which: a message
		// naming one version alone would leave the user unable to tell whether
		// the patch or the machine is the older, which is the fact the answer
		// to the offer turns on.
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
