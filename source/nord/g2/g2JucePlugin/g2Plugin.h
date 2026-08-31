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

namespace g2
{
	class Plugin : public synthLib::Plugin
	{
	public:
		Plugin(synthLib::Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid);

		/* The mode this class set, as the plan's check reads it: the
		 * constructed plugin reports MameHq and not the framework default
		 * Legacy. */
		synthLib::Resampler::Mode resamplerMode() const noexcept { return m_resamplerMode; }

	private:
		synthLib::Resampler::Mode m_resamplerMode = synthLib::Resampler::Mode::Legacy;
	};
}
