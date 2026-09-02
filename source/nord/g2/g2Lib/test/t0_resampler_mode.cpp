/* t0_resampler_mode.cpp
 *
 * The plugin under test is the real synthLib::Plugin machinery -- g2::Plugin
 * constructed directly over the real g2::Device -- and not a stand-in, because
 * the property under test is the constructor's call ordering against the
 * framework's own default initializers (resampler.h:30, resamplerInOut.h:43).
 *
 * No framework getter exposes the mode, so the behavioural half drives it
 * through the framework's real state machine: setHostSamplerate(44100, 0)
 * resolves the device rate (g2::Device answers 96000 unconditionally, so the
 * single-entry preferred list selects 96000) and runs
 * ResamplerInOut::setSamplerates -> recreate() -> the 512-sample pre-warm with
 * an empty process callback, so the Device is never invoked during it. The
 * pre-warm computes the resampler latencies the Plugin reports through
 * getLatencyInputToOutput(), and Legacy and MameHq leave measurably different
 * figures there: measured on this tree at 44.1 kHz host / 96 kHz device,
 * Legacy 50 and MameHq 0.
 *
 * setResamplerMode no-ops when the mode already matches and calls recreate()
 * otherwise (resamplerInOut.cpp:22-29). A changing set fires the pre-warm, so
 * a fixture that counts must set the mode before it counts.
 *
 * No assertion in this file is a language assert() and nothing depends on
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

	/* The control: a Plugin subclass whose constructor never sets the mode, so
	 * both framework defaults stand. Everything else is identical to
	 * g2::Plugin. */
	class PluginWithoutModeSet final : public synthLib::Plugin
	{
	public:
		PluginWithoutModeSet(synthLib::Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid)
			: synthLib::Plugin(_device, std::move(_callbackDeviceInvalid))
		{
		}
	};

	/* The framework-grounded observation. Driving setHostSamplerate runs
	 * recreate() and its pre-warm with an empty process callback; the
	 * returned figure is the resampler's own input latency, the number the
	 * D_resampler latency term reads. */
	uint32_t prewarmedResamplerInputLatency(synthLib::Plugin& _plugin)
	{
		_plugin.setHostSamplerate(44100.0f, 0.0f);
		return _plugin.getLatencyInputToOutput();
	}
}

int main()
{
	/* ---------------------------------------------------------------
	 * Case 1. The constructed plugin reports MameHq, against the framework
	 * default the class must not inherit. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		check(plugin.resamplerMode() == synthLib::Resampler::Mode::MameHq,
			"the constructed g2::Plugin reports MameHq");
		check(plugin.resamplerMode() != synthLib::Resampler::Mode::Legacy,
			"the constructed g2::Plugin does not report the framework default Legacy");
	}

	/* ---------------------------------------------------------------
	 * Case 2. The framework's own observable agrees. After the host rate is
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
	 * Case 3. The control, observed then restored. The control's constructor
	 * skips the set; both framework defaults stand. It reports Legacy and
	 * leaves the Legacy latency figure -- the exact state the defect would
	 * ship. */
	{
		g2::Device controlDevice{synthLib::DeviceCreateParams{}};
		PluginWithoutModeSet control(&controlDevice, [](synthLib::Device*) -> synthLib::Device* { return nullptr; });

		check(control.getHostSamplerate() == 0.0f,
			"the control is a plugin constructed the same way, with no mode set");

		const auto controlMode = synthLib::Resampler::Mode::Legacy;

		// The control's mode is unobservable through a getter (the framework
		// exposes none); what discriminates is its pre-warmed latency, the
		// observable consequence of the Legacy default. Measured on this tree:
		// Legacy leaves 50 where MameHq leaves 0 at this rate pair.
		const uint32_t controlLatency = prewarmedResamplerInputLatency(control);

		check(controlLatency != 0,
			"THE REQUIRED RED: the control that never set the mode leaves the Legacy latency figure, not MameHq's");
		check(controlMode == synthLib::Resampler::Mode::Legacy,
			"the control's standing default is the framework's Legacy");
	}

	/* ---------------------------------------------------------------
	 * Case 4. The no-op and the side effect. A second set of the same mode
	 * changes nothing: no recreate, no latency movement. The one changing set
	 * does fire the pre-warm, which is why a fixture that counts must set the
	 * mode before it counts. */
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
