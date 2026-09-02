// The no-firmware path: what the plugin does when it loads in the host and
// finds no OS update file.
//
// The surface asks the resolver exactly once for each call it is given. It does
// not wait, it does not poll and it does not try a second location. A caller
// that wants to look again calls again, which is what makes the second look the
// caller's decision, and therefore visible to the user. A surface that retried
// inside itself would report the same state and nothing could tell the two
// apart.
//
// It is written against ArtifactResolver and never against getenv, so that a
// later fetch implementation changes nothing here.

#pragma once

#include "artifactResolver.h"
#include "firmwareExtract.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace g2
{
	// A directory that is not there is reported Absent just like a variable
	// that is not set -- both mean the plugin cannot find the firmware -- but
	// they reach this state through different messages. This enum carries only
	// the up/down outcome the plugin reports to the host.
	enum class FirmwareState
	{
		Absent,
		Present,
	};

	// The version word the project expects. 0x00A2 is 162, which is release
	// 1.62.
	constexpr uint16_t g_expectedFirmwareVersion = 0x00A2u;

	// Q23 silence. 1.0 is 0x800000, so silence is zero, and the storage is a
	// signed integer: no floating-point value may cross the Q23 boundary.
	constexpr int32_t g_silentSample = 0;

	static_assert(std::is_integral<decltype(g_silentSample)>::value,
		"Q23 silence is integer. No floating-point type may cross this boundary.");
	static_assert(std::is_signed<decltype(g_silentSample)>::value,
		"Q23 uses bit 23 as the sign bit, so the storage is signed.");

	/* Writes Q23 silence over `_count` samples.
	 *
	 * This is the one statement of "silence" the project carries, so that the
	 * plugin's no-firmware path and any other silent path cannot disagree about
	 * what silence is. */
	inline void fillSilence(int32_t* _samples, const size_t _count)
	{
		for(size_t i = 0; i < _count; ++i)
			_samples[i] = g_silentSample;
	}

	/* What the plugin found, what it may do, and what it shows.
	 *
	 * `report` is one short line and is never empty; it is what a log or a
	 * status field carries. `message` is what the user reads and is empty when
	 * the firmware is present. `directory` is empty when it is absent. */
	struct FirmwareStatus
	{
		FirmwareState state = FirmwareState::Absent;
		bool producesAudio = false;
		std::string report;
		std::string message;
		std::string directory;
	};

	/* It opens with the resolver's own reason, by concatenation. Spelling that
	 * wording out a second time here would give one state two texts, and the one
	 * an implementer copies is the one that goes stale.
	 *
	 * The rest names the exact file on both platforms, the version those files
	 * carry, and where the user gets them. It also says that the plugin will not
	 * look again, so that is visible to the user and not only a property of the
	 * code. */
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

	/* Asks the resolver once and reports what it found. Never throws, so the
	 * plugin loads and does not crash. */
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
