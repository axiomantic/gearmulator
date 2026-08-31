/* t0_latency_formula.cpp -- the latency the plugin reports to the host.
 *
 * TIER T0 AND UNGATED: no firmware artifact and no booted machine. The formula
 * reads three constants, the chain configuration and two framework getters.
 *
 *     D_total(R) = ceil((L + D_chain + D_codec) * R / G2_FRAME_RATE_HZ)
 *                  + D_resampler(R)
 *     D_chain    = (N - 1) * G2_CHAIN_HOP_FRAMES
 *
 * WHAT IT HOLDS.
 *
 * 1. The reported figure equals the formula, at each host rate of the design's
 *    table, from the NAMED constants -- g2::kLookaheadFrames, the chain delay
 *    the plugin's own configuration gives, and g2::kDelayCodecFrames -- and
 *    not from a literal written here a second time.
 *
 * 2. THE CEILING, AND IT IS THE REASON THIS TEST EXISTS. `L + D_chain +
 *    D_codec` must stay at or below 16,384 frames. Above that the framework
 *    clamps the latency it is handed and only logs it, so the plugin reports a
 *    figure SHORTER than it takes and every host silently mis-compensates. A
 *    latency the framework truncates is not a slow plugin, it is a wrong one,
 *    and nothing else in this design would see it.
 *
 *    The ceiling is asserted twice over: once as arithmetic on the shipped
 *    constants, and once behaviorally -- Scheduler::create accepts a sum AT
 *    the bound and refuses one frame above it with Status::BadLookahead. The
 *    second binding is what keeps the literal below honest: a build whose
 *    real bound moved would take the pair red rather than leave a stale number
 *    passing.
 *
 * 3. The ceil rounds UP and is not a truncation: at 44.1 kHz the sum lands on
 *    a fraction and the reported figure is the frame above it.
 *
 * 4. At a 96 kHz host rate the resampler is bypassed and D_resampler is 0.
 *
 * 5. The Device declares no internal latency of its own, which is the premise
 *    that lets D_resampler be read out of the framework's public figure.
 */

#include "g2JucePlugin/g2Plugin.h"
#include "g2JucePlugin/g2Device.h"

#include "board.h"
#include "executor.h"
#include "scheduler.h"
#include "status.h"

#include "g2/timebase.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

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

	/* The framework clamps the extra latency it is handed to this many samples
	 * and reports the clamp only through a log line -- synthLib/device.cpp's
	 * setExtraLatencySamples, whose own comment ties the figure to the DSP
	 * emulator's ring buffer. It is written here rather than derived because
	 * the framework exports no symbol for it; the behavioral pair below is
	 * what holds it to the truth. */
	constexpr uint64_t g_frameworkLatencyClamp = 16384;

	constexpr float g_hostRates[] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f };

	synthLib::Plugin::CallbackDeviceInvalid noReplacement()
	{
		return [](synthLib::Device*) -> synthLib::Device* { return nullptr; };
	}

	/* Does Scheduler::create accept a lookahead that puts the sum here? */
	bool schedulerAcceptsTotal(const uint64_t _totalFrames, g2::Status& _outStatus)
	{
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;

		const auto chainDelay = static_cast<uint64_t>(config.dspCount - 1u) * config.hopFrames;

		config.maxHostBlockFrames = 1;
		config.lookaheadFrames    = static_cast<unsigned>(_totalFrames - chainDelay);

		return g2::Scheduler::create(config, executor, board, _outStatus) != nullptr;
	}
}

int main()
{
	/* The three terms, named, and the chain delay the configuration gives. */
	uint64_t chainDelayFrames = 0;
	uint64_t totalFrames      = 0;
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, noReplacement());

		/* 5. The premise D_resampler is read on. */
		check(device.getInternalLatencyMidiToOutput() == 0 && device.getInternalLatencyInputToOutput() == 0,
			"the Device declares no internal latency, so the framework's public figure carries the resampler's alone");

		chainDelayFrames = plugin.chainDelayFrames();
		totalFrames      = static_cast<uint64_t>(g2::kLookaheadFrames) + chainDelayFrames + g2::kDelayCodecFrames;

		const g2::Scheduler::Config& config = plugin.schedulerConfig();

		check(config.lookaheadFrames == g2::kLookaheadFrames,
			"the plugin configures the Scheduler with L and does not leave the Config's smallest-legal placeholder");
		check(chainDelayFrames == static_cast<uint64_t>(config.dspCount - 1u) * G2_CHAIN_HOP_FRAMES,
			"D_chain is one hop for each link between the DSPs");

		std::printf("     L = %u, D_chain = %llu, D_codec = %u, sum = %llu\n",
			g2::kLookaheadFrames,
			static_cast<unsigned long long>(chainDelayFrames),
			g2::kDelayCodecFrames,
			static_cast<unsigned long long>(totalFrames));
	}

	/* 2. THE CEILING. */
	{
		check(totalFrames <= g_frameworkLatencyClamp,
			"L + D_chain + D_codec stays at or below the framework's 16,384-frame clamp");

		if(totalFrames > g_frameworkLatencyClamp)
		{
			std::printf("     THE REPORTED LATENCY WOULD BE SILENTLY TRUNCATED: sum = %llu, clamp = %llu\n",
				static_cast<unsigned long long>(totalFrames),
				static_cast<unsigned long long>(g_frameworkLatencyClamp));
		}
		else
		{
			std::printf("     margin: sum = %llu against clamp %llu, a factor of %.0f\n",
				static_cast<unsigned long long>(totalFrames),
				static_cast<unsigned long long>(g_frameworkLatencyClamp),
				static_cast<double>(g_frameworkLatencyClamp) / static_cast<double>(totalFrames));
		}

		g2::Status atBound{};
		g2::Status pastBound{};

		const bool acceptedAtBound   = schedulerAcceptsTotal(g_frameworkLatencyClamp, atBound);
		const bool acceptedPastBound = schedulerAcceptsTotal(g_frameworkLatencyClamp + 1u, pastBound);

		check(acceptedAtBound && atBound == g2::Status::Ok,
			"Scheduler::create accepts a sum exactly AT the clamp");
		check(!acceptedPastBound && pastBound == g2::Status::BadLookahead,
			"Scheduler::create refuses a sum ONE FRAME above the clamp with Status::BadLookahead");
	}

	/* 1, 3 and 4. The formula at each host rate, then the two rates whose
	 * behavior the formula is chosen for. ONE Device carries all of it: each
	 * prepareToPlay re-runs the pre-warm and recomputes the reported figure
	 * from scratch, and constructing a second machine would build eight DSP
	 * emulators to learn nothing. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, noReplacement());

		bool allMatch = true;

		for(const float rate : g_hostRates)
		{
			plugin.prepareToPlay(256u, rate);

			const double   exact    = static_cast<double>(totalFrames)
				* static_cast<double>(rate) / static_cast<double>(G2_FRAME_RATE_HZ);
			const uint32_t expected = static_cast<uint32_t>(std::ceil(exact)) + plugin.resamplerDelaySamples();

			if(plugin.reportedLatencySamples() != expected)
			{
				allMatch = false;
				std::printf("     rate %.0f: reported %u, formula %u (sum %llu frames, D_resampler %u)\n",
					static_cast<double>(rate), plugin.reportedLatencySamples(), expected,
					static_cast<unsigned long long>(totalFrames), plugin.resamplerDelaySamples());
			}
		}

		check(allMatch,
			"the reported latency equals ceil((L + D_chain + D_codec) * R / 96000) + D_resampler(R) at every host rate");

		/* 4. The rate at which the resampler drops out entirely. */
		plugin.prepareToPlay(256u, 96000.0f);

		check(plugin.resamplerDelaySamples() == 0,
			"at a 96 kHz host rate the resampler is bypassed and D_resampler is 0");
		check(plugin.reportedLatencySamples() == totalFrames,
			"at a 96 kHz host rate the reported latency is the frame sum itself");

		/* 3. The rate at which the rounding direction is observable. */
		plugin.prepareToPlay(256u, 44100.0f);

		const double exact = static_cast<double>(totalFrames) * 44100.0 / static_cast<double>(G2_FRAME_RATE_HZ);

		check(exact != std::floor(exact),
			"at 44.1 kHz the sum lands on a fraction, so the rounding direction is observable here");
		check(plugin.reportedLatencySamples() >= static_cast<uint32_t>(std::ceil(exact)),
			"the reported figure rounds UP: it never claims less delay than the plugin takes");
		check(plugin.reportedLatencySamples() != static_cast<uint32_t>(exact) + plugin.resamplerDelaySamples(),
			"the reported figure is not the truncation, which would misalign the host's delay compensation");
	}

	if(g_failures)
	{
		std::printf("t0_latency_formula: %d of %d cases failed\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_latency_formula: %d of %d cases passed\n", g_cases, g_cases);
	return 0;
}
