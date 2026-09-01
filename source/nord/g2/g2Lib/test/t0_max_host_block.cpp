/* `B`, the largest host block the plugin accepts.
 *
 * Tier T0 and ungated: no firmware artifact and no booted machine. The plugin
 * derives B from the host's maximum block and the host's rate alone, and
 * Scheduler::create's rejection of B = 0 is a Config-only rejection.
 *
 * 1. B = floor(maxBlock * 96000 / hostRate) + 1, at each host rate in the
 *    table. The `+1` is derived and is not margin: framesForBlock in
 *    g2/timebase.h answers either floor(n * r) or floor(n * r) + 1 for one
 *    block, depending on where its accumulator stands, so only the larger of
 *    the two bounds every block. The test drives framesForBlock itself over a
 *    long run of blocks and holds B against the largest frame count it ever
 *    returns -- so the +1 is checked against the accumulator's real behavior
 *    rather than against a second copy of the same arithmetic.
 *
 * 2. There is no max() against the 512-sample resampler pre-warm: at every
 *    rate in the table a 32-sample host block gives a B far below 512.
 *
 * 3. Scheduler::create refuses B = 0 with Status::BadMaxHostBlock and yields
 *    no object.
 *
 * 4. A second prepareToPlay with a larger maximum block re-derives B and
 *    re-creates the Scheduler. An equal or smaller maximum changes neither,
 *    because B is a ceiling. The re-create is counted rather than booted,
 *    because booting needs firmware.
 */

#include "g2JucePlugin/g2Plugin.h"
#include "g2JucePlugin/g2Device.h"

#include "board.h"
#include "executor.h"
#include "scheduler.h"

#include "g2/timebase.h"

#include <cstdint>
#include <cstdio>
#include <iterator>

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

	/* The host rates of the table. */
	constexpr float g_hostRates[] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f };

	/* The largest frame count framesForBlock ever answers for this block at
	 * this rate. The accumulator's period is den / gcd(num, den) blocks -- 147
	 * at 44.1 kHz, the longest in the table -- so this run covers every rate's
	 * period many times over and has seen both of the two adjacent values the
	 * count can take. */
	uint32_t observedWorstCaseFrames(const uint32_t _blockSamples, const float _hostRate)
	{
		const auto den = static_cast<uint32_t>(_hostRate);

		uint32_t acc   = 0;
		uint32_t worst = 0;

		for(uint32_t i = 0; i < 5000; ++i)
		{
			const uint32_t frames = framesForBlock(_blockSamples, G2_HOST_FRAMES_NUM, den, &acc);
			if(frames > worst)
				worst = frames;
		}

		return worst;
	}

	synthLib::Plugin::CallbackDeviceInvalid noReplacement()
	{
		return [](synthLib::Device*) -> synthLib::Device* { return nullptr; };
	}
}

int main()
{
	/* 1 and 2. The formula, and the absence of a max() against 512.
	 *
	 * The derivation is driven through the static function here, over every
	 * rate and block of the table, and the case below then holds one
	 * prepareToPlay against that same function. Constructing a Device for each
	 * pair would stand up the whole DSP set again and again to re-check one
	 * line of arithmetic. */
	{
		bool bounds      = true;
		bool tight       = true;
		bool allBelow512 = true;

		for(const float rate : g_hostRates)
		{
			for(const uint32_t block : { 32u, 64u, 128u, 256u, 512u, 1024u })
			{
				const uint32_t b     = g2::Plugin::maxHostBlockFramesFor(block, rate);
				const uint32_t worst = observedWorstCaseFrames(block, rate);

				// B must bound every block, and must not be slack beyond the
				// one derived frame. Where the two rates are equal the
				// accumulator never varies and the derived frame is never
				// claimed; everywhere else the accumulator does reach
				// floor(n * r) + 1 and B must already be there.
				if(b < worst)
				{
					bounds = false;
					std::printf("     rate %.0f block %u: B = %u is BELOW the worst observed %u\n",
						static_cast<double>(rate), block, b, worst);
				}
				if(b > worst + 1u)
				{
					tight = false;
					std::printf("     rate %.0f block %u: B = %u is SLACK against the worst observed %u\n",
						static_cast<double>(rate), block, b, worst);
				}

				if(block == 32u && b >= 512u)
					allBelow512 = false;
			}
		}

		check(bounds,
			"B bounds the largest frame count framesForBlock ever returns, at every rate and block of the table");
		check(tight,
			"B is the formula's figure and is slack by at most the one derived frame");
		check(allBelow512,
			"a 32-sample host block gives a B far below 512: nothing takes a max() against the pre-warm");
	}

	/* prepareToPlay reports that same derivation and does not carry a second
	 * one of its own. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, noReplacement());

		bool allMatch = true;

		// Highest rate first, so that every step lowers the host rate and
		// therefore raises B. B is a ceiling, so only a rising B is written
		// back; walking the table the other way would leave the first, largest
		// figure standing and the equality below would hold vacuously.
		for(std::size_t i = std::size(g_hostRates); i-- > 0; )
		{
			const float rate = g_hostRates[i];

			plugin.prepareToPlay(1024u, rate);

			if(plugin.maxHostBlockFrames() != g2::Plugin::maxHostBlockFramesFor(1024u, rate))
			{
				allMatch = false;
				std::printf("     rate %.0f: prepareToPlay reports %u, the derivation gives %u\n",
					static_cast<double>(rate), plugin.maxHostBlockFrames(),
					g2::Plugin::maxHostBlockFramesFor(1024u, rate));
			}
		}

		check(allMatch,
			"prepareToPlay's B is the static derivation's figure at every rate of the table");
	}

	/* The two figures the formula pins exactly, so a silent change of shape
	 * is visible as a number and not only as a disagreement. */
	{
		check(g2::Plugin::maxHostBlockFramesFor(512u, 44100.0f) == 1115u,
			"512 samples at 44.1 kHz is floor(512 * 96000 / 44100) + 1 = 1115 frames");
		check(g2::Plugin::maxHostBlockFramesFor(512u, 96000.0f) == 513u,
			"512 samples at 96 kHz is 512 frames plus the derived one");
	}

	/* 3. Scheduler::create refuses B = 0. */
	{
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.lookaheadFrames    = g2::kLookaheadFrames;
		config.maxHostBlockFrames = 0;

		g2::Status status{};
		const auto scheduler = g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr,
			"Scheduler::create yields no object for B = 0");
		check(status == g2::Status::BadMaxHostBlock,
			"Scheduler::create reports Status::BadMaxHostBlock for B = 0");
	}

	/* 4. B is a ceiling: growth re-creates, equal or smaller does not. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, noReplacement());

		plugin.prepareToPlay(256u, 48000.0f);

		const uint32_t first       = plugin.maxHostBlockFrames();
		const uint32_t afterFirst  = plugin.schedulerRecreations();

		check(first == g2::Plugin::maxHostBlockFramesFor(256u, 48000.0f),
			"the first prepareToPlay derives B from the host's maximum block");
		check(afterFirst == 1u,
			"the first prepareToPlay re-creates the Scheduler once");

		plugin.prepareToPlay(128u, 48000.0f);
		check(plugin.maxHostBlockFrames() == first,
			"a SMALLER maximum block leaves B alone: B is a ceiling");
		check(plugin.schedulerRecreations() == afterFirst,
			"a smaller maximum block re-creates nothing");

		plugin.prepareToPlay(256u, 48000.0f);
		check(plugin.maxHostBlockFrames() == first,
			"an EQUAL maximum block leaves B alone");
		check(plugin.schedulerRecreations() == afterFirst,
			"an equal maximum block re-creates nothing");

		plugin.prepareToPlay(1024u, 48000.0f);
		check(plugin.maxHostBlockFrames() == g2::Plugin::maxHostBlockFramesFor(1024u, 48000.0f),
			"a LARGER maximum block re-derives B");
		check(plugin.maxHostBlockFrames() > first,
			"the re-derived B is larger than the one it replaced");
		check(plugin.schedulerRecreations() == afterFirst + 1u,
			"a larger maximum block re-creates the Scheduler exactly once more");
		check(plugin.schedulerConfig().maxHostBlockFrames == plugin.maxHostBlockFrames(),
			"the configuration the re-create used carries the new B, not the old one");
	}

	/* The arguments a host must not be able to turn into a Config. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		g2::Plugin plugin(&device, noReplacement());

		check(!plugin.prepareToPlay(0u, 48000.0f),
			"a zero maximum block is refused: it would derive a B of 1 from nothing");
		check(!plugin.prepareToPlay(256u, 0.0f),
			"a zero host rate is refused: it is the denominator of the derivation");
		check(plugin.maxHostBlockFrames() == 0u,
			"a refused prepareToPlay leaves B unwritten");
		check(plugin.schedulerRecreations() == 0u,
			"a refused prepareToPlay re-creates nothing");
	}

	if(g_failures)
	{
		std::printf("t0_max_host_block: %d of %d cases failed\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_max_host_block: %d of %d cases passed\n", g_cases, g_cases);
	return 0;
}
