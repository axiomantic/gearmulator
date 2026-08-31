/* t0_channel_counts.cpp -- the check of task PLG-2. Design sections 14.6,
 * 14.7 and 17 rows 7.32/7.33.
 *
 * WHAT THE CHECK OWNS: getChannelCountIn() returns 2 and getChannelCountOut()
 * returns 2, and both return their FINAL values the instant the Device
 * constructor returns -- before the firmware is loaded, before the boot, and
 * whether or not any artifact was found.
 *
 * THE LOAD-BEARING CLAUSE IS THE NO-FIRMWARE PATH. The Plugin queries the
 * counts exactly once, in its constructor's member-initializer list
 * (plugin.cpp:15, the only call site anywhere in synthLib), and
 * ResamplerInOut stores both as const members that nothing re-reads. A count
 * that becomes final only after the firmware loads or the boot completes is
 * a count the host has already read as something else.
 *
 * THE TWO CASE GROUPS, ONE PER RESOLVER OUTCOME:
 *
 *  1. NMG2_ARTIFACTS cleared -- the no-firmware path of design section 7.7.
 *     The device is constructed with the variable unset, the counts are read
 *     the instant the constructor returns, and the FirmwareStatus the
 *     constructor resolved is asserted ABSENT so the group cannot pass by
 *     accident with artifacts in view.
 *
 *  2. NMG2_ARTIFACTS set to an existing directory -- the Present path. The
 *     directory needs no artifact file in it: the constructor resolves
 *     firmware STATE (BRD-10) and does not boot, and with no artifact name
 *     asked for, an existing directory is enough for the resolver to succeed.
 *     The status is asserted PRESENT so the group proves it walked the other
 *     outcome. The counts are read and must still be 2 and 2.
 *
 * THE ENVIRONMENT IS OWNED, NOT ASSUMED, exactly as t0_no_firmware states:
 * ctest runs a test with whatever the invoking shell carried, so a count that
 * depended on the shell's variable would be untestable. Every case sets or
 * clears the variable itself. Paths are plain strings: std::filesystem is
 * unavailable at this deployment target, and the directory this group names
 * is one the platform guarantees.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() and nothing here depends
 * on NDEBUG, so this file reports identically in every build type.
 */

#include "g2JucePlugin/g2Device.h"

#include <cstdint>
#include <cstdlib>
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

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	/* The same helper t0_no_firmware.cpp carries, for the same reason: the
	 * test owns the variable rather than inheriting it from the invoking
	 * shell. */
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

	/* An existing directory with no artifact file in it. The resolver is asked
	 * for no name, so a present directory alone resolves -- and it is the
	 * constructor's resolveFirmwareState call, not this test, that asks. */
	constexpr const char* kPresentDirectory = "/tmp";
}

int main()
{
	/* ------------- Case group 1. THE NO-FIRMWARE PATH, the clause the plan
	 * marks load-bearing: the constructor returns with no artifact found, and
	 * the counts are already final. */
	{
		setArtifacts(nullptr);

		const synthLib::DeviceCreateParams params;
		g2::Device device(params);

		// The control that makes this group mean what it says: the device was
		// really constructed on the Absent outcome, not on whatever the
		// invoking shell carried.
		check(device.firmwareStatus().state == g2::FirmwareState::Absent,
			"the first device really resolved the no-firmware path (the variable was cleared, the status is Absent)");

		// The counts are NOT const virtuals (design row 7.32, device.h:78-79),
		// so the ask is on a non-const device -- the same shape the Plugin's
		// member-initializer list makes.
		checkEqual(uint32_t(device.getChannelCountIn()), uint32_t(2),
			"getChannelCountIn() returns 2 the instant the constructor returns with no firmware artifact");
		checkEqual(uint32_t(device.getChannelCountOut()), uint32_t(2),
			"getChannelCountOut() returns 2 the instant the constructor returns with no firmware artifact");

		// Repeated asks stay 2: the Plugin's ResamplerInOut stores the pair as
		// const members, so a count that answered differently on a later call
		// would never be read by anyone.
		checkEqual(uint32_t(device.getChannelCountIn()), uint32_t(2),
			"getChannelCountIn() answers 2 again on a second call");
		checkEqual(uint32_t(device.getChannelCountOut()), uint32_t(2),
			"getChannelCountOut() answers 2 again on a second call");
	}

	/* ------------- Case group 2. THE PRESENT PATH. The variable names an
	 * existing directory; the constructor resolves firmware STATE (BRD-10)
	 * and does not boot, so no artifact file is needed. The counts must be
	 * final here too. */
	{
		setArtifacts(kPresentDirectory);

		const synthLib::DeviceCreateParams params;
		g2::Device device(params);

		check(device.firmwareStatus().state == g2::FirmwareState::Present,
			"the second device really resolved the firmware-present path (the status is Present)");

		checkEqual(uint32_t(device.getChannelCountIn()), uint32_t(2),
			"getChannelCountIn() returns 2 the instant the constructor returns with the firmware present");
		checkEqual(uint32_t(device.getChannelCountOut()), uint32_t(2),
			"getChannelCountOut() returns 2 the instant the constructor returns with the firmware present");

		setArtifacts(nullptr);
	}

	if(g_failures)
	{
		std::cout << "t0_channel_counts: " << g_failures << " of " << g_cases
			<< " cases FAILED" << std::endl;
		return 1;
	}
	std::cout << "t0_channel_counts: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
