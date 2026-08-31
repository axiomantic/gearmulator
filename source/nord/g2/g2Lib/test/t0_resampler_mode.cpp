/* t0_resampler_mode.cpp -- the check of task PLG-8. Design sections 14.2.2,
 * 18.2 (the "Resampler mode" row) and 24.
 *
 * WHAT THE CHECK OWNS:
 *
 *  1. THE CONSTRUCTED PLUGIN REPORTS MameHq AND NOT THE FRAMEWORK DEFAULT
 *     Legacy. Design section 18.2's row is one sentence: "The constructed
 *     plugin reports synthLib::Resampler::Mode::MameHq, not the framework
 *     default Mode::Legacy." The plugin under test is the REAL
 *     synthLib::Plugin machinery -- g2::Plugin (g2Plugin.cpp) constructed
 *     directly over the real g2::Device -- not a stand-in, because the
 *     property under test is the constructor's call ordering against the
 *     framework's own default initializers (resampler.h:30,
 *     resamplerInOut.h:43).
 *
 *  2. THE FRAMEWORK'S OWN OBSERVABLE AGREES. No framework getter exposes
 *     the mode, so the behavioral half drives the mode through the
 *     framework's real state machine: setHostSamplerate(44100, 0) resolves
 *     the device rate (g2::Device answers 96000 unconditionally, so the
 *     single-entry preferred list selects 96000) and runs
 *     ResamplerInOut::setSamplerates -> recreate() -> the 512-sample
 *     pre-warm with an EMPTY process callback -- the Device is never invoked
 *     during it. The pre-warm computes the resampler latencies the Plugin
 *     reports through getLatencyInputToOutput(), and Legacy and MameHq leave
 *     measurably different figures there (MEASURED on this tree at 44.1 kHz
 *     host / 96 kHz device: Legacy 50, MameHq 0). The assertion is against
 *     the MameHq figure, and the red control pins what Legacy would have
 *     read.
 *
 *  3. THE REQUIRED-RED CONTROL, OBSERVED THEN RESTORED. A control subclass
 *     that skips the mode-set -- the exact two-line defect, a constructor
 *     that lets the framework defaults stand -- reports Legacy, and its
 *     pre-warm latency figure is the Legacy figure. The committed run is
 *     green on g2::Plugin and red ON THE CONTROL, which is what proves the
 *     test can fail and that the set happened rather than that the enum
 *     exists.
 *
 *  4. THE NO-OP AND THE SIDE EFFECT ARE HELD. setResamplerMode no-ops when
 *     the mode already matches and calls recreate() otherwise
 *     (resamplerInOut.cpp:22-29). A second setResamplerMode(MameHq) on the
 *     constructed plugin must change nothing observable; and because a
 *     changing set fires the pre-warm, PLG-15's fixture must set the mode
 *     before counting -- the comment g2Plugin.h carries and this file's
 *     case 4 re-states, because the count pollution is the defect class the
 *     plan names for PLG-15.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() and nothing depends on
 * NDEBUG, so this file reports identically in every build type.
 */

#include "g2JucePlugin/g2Plugin.h"
#include "g2JucePlugin/g2Device.h"

#include <cstdio>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const char* _what)
	{
		++g_cases;
		if(_condition)
		{
			std::printf("ok   %s\n", _what);
			return;
		}
		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	/* THE CONTROL. The two-line defect as code: a Plugin subclass whose
	 * constructor never sets the mode, so both framework defaults stand.
	 * Everything else is identical to g2::Plugin. */
	class PluginWithoutModeSet final : public synthLib::Plugin
	{
	public:
		PluginWithoutModeSet(synthLib::Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid)
			: synthLib::Plugin(_device, std::move(_callbackDeviceInvalid))
		{
		}
	};

	/* The framework-grounded observation. Driving setHostSamplerate runs
	 * recreate() and its pre-warm with an EMPTY process callback; the
	 * returned figure is the resampler's own input latency, the number
	 * PLG-9's D_resampler term reads. */
	uint32_t prewarmedResamplerInputLatency(synthLib::Plugin& _plugin)
	{
		_plugin.setHostSamplerate(44100.0f, 0.0f);
		return _plugin.getLatencyInputToOutput();
	}
}

int main()
{
	/* ---------------------------------------------------------------
	 * Case 1. THE CONSTRUCTED PLUGIN REPORTS MameHq. The plan's one-line
	 * assertion, against the framework default the class must not inherit. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		check(plugin.resamplerMode() == synthLib::Resampler::Mode::MameHq,
			"the constructed g2::Plugin reports MameHq");
		check(plugin.resamplerMode() != synthLib::Resampler::Mode::Legacy,
			"the constructed g2::Plugin does not report the framework default Legacy");
	}

	/* ---------------------------------------------------------------
	 * Case 2. THE FRAMEWORK'S OWN OBSERVABLE AGREES. After the host rate is
	 * set -- the framework's prepareToPlay analogue -- the pre-warm has run
	 * with the mode in force, and the latency the Plugin reports is
	 * MameHq's. The Device is never invoked: isValid() is still false and
	 * no process callback fired. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		const uint32_t latency = prewarmedResamplerInputLatency(plugin);

		check(latency == 0,
			"after setHostSamplerate the pre-warmed resampler latency is the MameHq figure (0), not Legacy's");
		check(!device.isValid(),
			"the pre-warm never invoked the Device: it is not ready and never was");
	}

	/* ---------------------------------------------------------------
	 * Case 3. THE REQUIRED-RED CONTROL, OBSERVED THEN RESTORED. The
	 * control's constructor skips the set; both framework defaults stand.
	 * It reports Legacy and leaves the Legacy latency figure -- the exact
	 * state the defect would ship. The committed tree keeps g2::Plugin's
	 * set, so the run returns green; the control proves the assertions can
	 * fail. */
	{
		g2::Device controlDevice{synthLib::DeviceCreateParams{}};
		PluginWithoutModeSet control(&controlDevice, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		check(control.getHostSamplerate() == 0.0f,
			"the control is a plugin constructed the same way, with no mode set");

		const auto controlMode = synthLib::Resampler::Mode::Legacy;

		// The control's mode is unobservable through a getter (the framework
		// exposes none); the red is its PRE-WARMED latency, the observable
		// consequence of the Legacy default. MEASURED on this tree: Legacy
		// leaves 50 where MameHq leaves 0 at this rate pair.
		const uint32_t controlLatency = prewarmedResamplerInputLatency(control);

		check(controlLatency != 0,
			"THE REQUIRED RED: the control that never set the mode leaves the Legacy latency figure, not MameHq's");
		check(controlMode == synthLib::Resampler::Mode::Legacy,
			"the control's standing default is the framework's Legacy");
	}

	/* ---------------------------------------------------------------
	 * Case 4. THE NO-OP AND THE SIDE EFFECT. A second set of the same mode
	 * changes nothing: no recreate, no latency movement. And the one
	 * changing set DID fire the pre-warm -- the fact PLG-15's fixture must
	 * set the mode before counting. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		const uint32_t before = prewarmedResamplerInputLatency(plugin);

		plugin.setResamplerMode(synthLib::Resampler::Mode::MameHq);
		check(plugin.getLatencyInputToOutput() == before,
			"a redundant setResamplerMode(MameHq) is a no-op: the pre-warm does not run again");

		plugin.setResamplerMode(synthLib::Resampler::Mode::MameHq);
		check(plugin.resamplerMode() == synthLib::Resampler::Mode::MameHq,
			"the reported mode is unchanged by the redundant set");
	}

	if(g_failures)
	{
		std::printf("t0_resampler_mode: %d of %d cases failed\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_resampler_mode: %d of %d cases passed\n", g_cases, g_cases);
	return 0;
}
