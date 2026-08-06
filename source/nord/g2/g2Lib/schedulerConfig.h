#pragma once

// schedulerConfig.h — SCH-18
//
// Declares Scheduler::Config — compile-time / run-time knobs for the G2
// quantum scheduler.

#include <cstdint>

namespace g2
{
	class Scheduler
	{
	public:
		/* Config — SCH-18
		 *
		 * All fields are in consistent SI-ish units:
		 *   audioSampleRate          — audio frames per second (e.g. 44100)
		 *   audioFramesPerQuantum    — audio frames per scheduler quantum
		 *                             (e.g. 64)
		 *   mcuCyclesPerAudioFrame   — MCF5307 bus cycles to step per audio
		 *                             frame (derived from bus clock / sample
		 *                             rate; 0 = no MCU stepping)
		 *   usbSofPerQuantum         — USB start-of-frame ticks to deliver
		 *                             per quantum (0 = USB not active)
		 */
		struct Config
		{
			uint32_t audioSampleRate{44100u};
			uint32_t audioFramesPerQuantum{64u};
			uint32_t mcuCyclesPerAudioFrame{0u};
			uint32_t usbSofPerQuantum{0u};
		};
	};
} // namespace g2
