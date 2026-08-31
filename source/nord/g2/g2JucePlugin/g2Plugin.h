/* g2Plugin.h -- the synthLib::Plugin subclass. Task PLG-8.
 *
 * Design sections 14.2.2, 18.2 and 24.
 *
 * WHAT THIS CLASS IS. The one place the design's resampler decision becomes
 * code: the constructor calls synthLib::Plugin::setResamplerMode with
 * Resampler::Mode::MameHq, so the mode is SET here and never inherited. The
 * framework default is Legacy in two places -- the Resampler constructor
 * default (resampler.h:30) and ResamplerInOut::m_mode's initializer
 * (resamplerInOut.h:43) -- and Legacy is about 57 dB short of the design's
 * stopband figure and puts the 44.1 kHz passband edge at 19,845 Hz, below the
 * 20 kHz section 14.2.2 requires to be flat. Neither failure has a symptom a
 * listener would attribute to the resampler, which is why the assertion is
 * one line and the defect two.
 *
 * WHY THE CONSTRUCTOR BODY IS THE RIGHT MOMENT. The synthLib::Plugin
 * constructor runs no resampler work at all: m_resampler is constructed with
 * both samplerates at 0, and ResamplerInOut::recreate() early-returns while
 * either is below 1 (resamplerInOut.cpp:62), so the 512-sample pre-warm has
 * not run when the body executes. setResamplerMode is therefore a pure
 * member write here; the pre-warm runs LATER, at the first setHostSamplerate
 * (the framework's prepareToPlay analogue), with the mode already MameHq.
 * That is the ordering design section 14.2.2 demands -- "during construction,
 * before the first prepareToPlay" -- and it is what makes the reported
 * latency of PLG-9's D_resampler term MameHq's from the first block.
 *
 * THE SIDE EFFECT PLG-15 MUST PLAN FOR. setResamplerMode is a no-op only
 * while the mode already matches; on a change it calls recreate(), and
 * recreate() runs the pre-warm with an EMPTY process callback -- the Device
 * is never invoked during it (design section 14.2.2's latency table, PLG-9's
 * "+1 is derived, not margin" note). A fixture that counts Device callbacks
 * must therefore set the mode BEFORE it starts counting, or the one recreate
 * fired by the mode change pollutes the count. This file sets it in the
 * constructor, so for every g2::Plugin instance the pre-warm is over before
 * any caller could begin counting.
 *
 * THE OBSERVABLE. The framework exposes no getter for the mode --
 * ResamplerInOut keeps m_mode private and Resampler::getMode() sits on the
 * inner resamplers, which exist only after recreate. The subclass therefore
 * records what it set and reports it through resamplerMode(); t0_resampler_mode
 * holds that against the framework default and, behaviorally, against the
 * resampler latency the pre-warm computes (Legacy and MameHq differ
 * measurably there).
 */

#pragma once

#include "synthLib/plugin.h"
#include "synthLib/resampler.h"

#include "scheduler.h"

#include <cstdint>

namespace g2
{
	class Device;

	/* `L`, the lookahead, in 96 kHz frames.
	 *
	 * The worst-case synchronous patch compile is 695.38 microseconds, taken
	 * on a 71-word download through the lazy trigger. One 96 kHz frame is
	 * 1/96000 s = 10.4167 microseconds, and 695.38 / 10.4167 = 66.76, so 67
	 * whole frames is the first count that covers it.
	 *
	 * THE COMPILE WORK WAS MEASURED THROUGH PROXY CODE. The real prologue
	 * words have never been captured, so the measurement drove sliding windows
	 * of the 573-word kernel -- right length, wrong content -- and per-module
	 * patch code is unmeasured. A larger true figure moves this constant and
	 * nothing else, because every consumer derives from it. */
	constexpr unsigned kLookaheadFrames = 67;

	/* `D_codec`, in 96 kHz frames. Zero: no converter delay is modelled.
	 *
	 * It is a NAMED TERM rather than an omitted one, so that a decision to
	 * model the converter is one edit at one site. The consequence of the zero
	 * is bounded and known: against hardware measured at the analogue jacks
	 * this emulation is up to 2 frames -- about 21 microseconds at 96 kHz --
	 * shorter than the machine. */
	constexpr unsigned kDelayCodecFrames = 0;

	class Plugin : public synthLib::Plugin
	{
	public:
		Plugin(synthLib::Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid);

		/* The mode this class set, as the plan's check reads it: the
		 * constructed plugin reports MameHq and not the framework default
		 * Legacy. */
		synthLib::Resampler::Mode resamplerMode() const noexcept { return m_resamplerMode; }

		/* THE HOST'S ONE MOMENT. Both `B` and the reported latency are
		 * computed here and nowhere else, from the same host block and the
		 * same host rate, so the two can never disagree about which
		 * prepareToPlay they belong to.
		 *
		 * Answers whether the machine is ready to render: false when the
		 * arguments are refused, and false when a growing `B` forced a
		 * re-create that did not boot. The derived figures below are written
		 * on every accepted call regardless, because a host asks for the
		 * latency whether or not there is firmware to run. */
		bool prepareToPlay(uint32_t _maxHostBlockSamples, float _hostSamplerate);

		/* `B`, the largest host block the plugin accepts, in 96 kHz frames.
		 * Zero before the first accepted prepareToPlay. */
		uint32_t maxHostBlockFrames() const noexcept { return m_maxHostBlockFrames; }

		/* `D_total(R)`, in host samples at the rate the last accepted
		 * prepareToPlay was given. Zero before that call. */
		uint32_t reportedLatencySamples() const noexcept { return m_reportedLatencySamples; }

		/* `D_chain`, in 96 kHz frames: one hop for each link between the
		 * DSPs, taken from the configuration this plugin will build the
		 * Scheduler from. */
		uint32_t chainDelayFrames() const noexcept;

		/* How many times a prepareToPlay found a LARGER maximum block and
		 * therefore had to re-create the Scheduler. An equal or smaller
		 * maximum leaves this alone, because `B` is a ceiling. */
		uint32_t schedulerRecreations() const noexcept { return m_schedulerRecreations; }

		/* The configuration the next re-create will use. `lookaheadFrames` is
		 * `kLookaheadFrames` from construction and `maxHostBlockFrames` is the
		 * running ceiling. */
		const Scheduler::Config& schedulerConfig() const noexcept { return m_schedulerConfig; }

		/* `B` from a host block and a host rate, with no side effect.
		 *
		 * THE `+1` IS DERIVED AND IS NOT MARGIN. `framesForBlock` in
		 * g2/timebase.h answers either floor(n * r) or floor(n * r) + 1 for a
		 * given block, depending on where its accumulator stands, so the
		 * larger of the two is the only value that bounds every block.
		 *
		 * THE 512-SAMPLE RESAMPLER PRE-WARM PLAYS NO PART. It calls process()
		 * with an empty callback and never reaches the Device, so there is no
		 * max() against it here. */
		static uint32_t maxHostBlockFramesFor(uint32_t _maxHostBlockSamples, float _hostSamplerate) noexcept;

		/* `D_resampler(R)`: the framework resampler's own input and output
		 * delay, in host samples, as the 512-sample pre-warm computed it.
		 *
		 * ResamplerInOut's two getters are private to the framework's Plugin.
		 * The public input-to-output figure carries them plus the host block
		 * term and the Device's internal latency; subtracting the block term
		 * leaves the resampler's contribution, and this Device declares no
		 * internal latency of its own. At a 96 kHz host rate the resampler is
		 * bypassed and this is 0. */
		uint32_t resamplerDelaySamples() const noexcept;

	private:
		bool recreateScheduler();

		synthLib::Resampler::Mode m_resamplerMode = synthLib::Resampler::Mode::Legacy;

		Device*  m_device                 = nullptr;
		Scheduler::Config m_schedulerConfig{};
		uint32_t m_blockSizeSamples       = 0;
		uint32_t m_maxHostBlockFrames     = 0;
		uint32_t m_reportedLatencySamples = 0;
		uint32_t m_schedulerRecreations   = 0;
	};
}
