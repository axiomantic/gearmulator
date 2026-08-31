/* t0_resampler_passband_ripple.cpp -- the check of task PLG-11. Design
 * sections 14.2.2 (the revised target table) and 18.2 (the "Resampler
 * passband ripple" row).
 *
 * WHAT THIS CHECK IS. A stepped sine sweep from 20 Hz to 20 kHz driven
 * through synthLib::ResamplerInOut ALONE -- no firmware, no artifact, no
 * emulated machine, no g2::Device and no g2::Plugin -- in the mode PLG-8
 * adopted (Resampler::Mode::MameHq), at each of the six host rates section
 * 14.1's table lists. It reports the PEAK-TO-PEAK deviation from 0 dB across
 * the swept band and compares it against a committed target.
 *
 * THE TARGET IS A COMMITTED CONSTANT AND NOTHING IN THIS PROGRAM WRITES IT.
 * g_committedTargetDb below is the only place the number lives, it is a
 * constexpr in committed source, and only a deliberate edit changes it. This
 * file opens no file, reads no environment variable and writes nothing
 * anywhere: a test that rewrote its own target would pass for every possible
 * measurement, which is the defect the previous revision of PLG-11's row
 * carried and which section 14.2.2's "a measurement with no predicate is a
 * recording, not a check" names. When a measurement exceeds the target the
 * run FAILS and prints BOTH figures.
 *
 * THE HARD CEILING IS ALSO CHECKED, AND IT IS CHECKED AGAINST THE TARGET
 * ITSELF, not only against the measurement. PLG-11 bounds every committed
 * target at 0.10 dB, so a future edit that raises the target past the ceiling
 * must fail here rather than pass quietly; case 4 holds that.
 *
 * THE METHOD, STATED SO IT IS REPRODUCIBLE.
 *
 *  1. One ResamplerInOut per host rate, constructed with ZERO input channels
 *     and two output channels. The device rate is 96 kHz, which is what
 *     g2::Device::getSamplerate() answers unconditionally. Zero input
 *     channels is deliberate and it removes nothing that is measured: the
 *     sweep is generated INSIDE the process callback, at the device rate, so
 *     the host-to-device input resampler would only ever filter silence.
 *     Every AudioBuffer loop over an empty channel set iterates zero times
 *     and feedInput is guarded by if(m_channelCountIn), so the input half is
 *     inert rather than skipped by a special case. What is measured is the
 *     device-to-host output path -- the path every emulated sample takes.
 *
 *  2. The tones are stepped through ONE resampler in sequence, low to high,
 *     which is what makes this a sweep rather than a set of unrelated runs:
 *     the filter state carries across the step and the settle interval below
 *     is what flushes it.
 *
 *  3. EVERY TONE IS COHERENT WITH ITS OWN CAPTURE WINDOW, so the analysis is
 *     a leak-free rectangular-window DFT bin and needs no window function and
 *     no leakage budget. For a target frequency the code picks a whole number
 *     of cycles C and a capture length N with f = C * hostRate / N exactly,
 *     then nudges N until f lies inside [20 Hz, 20 kHz]. The reported
 *     amplitude is 2*|X_C|/N, which is exact for a sine of frequency
 *     C*hostRate/N sampled N times. A windowed estimate would have been an
 *     approximation with an error budget to argue about; this has neither.
 *
 *  4. A settle interval of g_settleSamples host samples is discarded before
 *     every capture. The output filter is 400 taps per lane at the 96 kHz
 *     device rate, so its group delay is about 200 device samples -- under
 *     100 host samples at 44.1 kHz. 2048 is more than an order of magnitude
 *     above that and also covers the step discontinuity at the tone change.
 *
 *  5. The figure compared against the target is max(dB) - min(dB) over the
 *     swept band, the standard definition of passband ripple. The maximum
 *     absolute deviation is printed beside it as a diagnostic; it is NOT the
 *     asserted figure, and it is labelled as a diagnostic where it is printed
 *     so it cannot be read as one.
 *
 * DETERMINISM. There is no random source anywhere: the tone grid is a fixed
 * logarithmic ladder, the phases start at zero, no clock is read and no
 * thread is created. Case 3 re-runs one whole sweep and requires the second
 * run's dB figures to be BIT-IDENTICAL to the first, so the claim is checked
 * rather than asserted in a comment.
 *
 * THE INSTRUMENT IS CALIBRATED BEFORE IT IS TRUSTED (case 1). The analyzer is
 * held against two synthesized tones whose amplitude is known exactly -- a
 * known positive at 1.0 (0 dB) and a known negative at 0.5 (-6.0206 dB) -- so
 * a stub analyzer that answered "0 dB" to everything would fail here. Without
 * this, a flat sweep would be indistinguishable from an analyzer that cannot
 * see anything at all.
 *
 * THE PERMANENT CONTROL (case 5). The same sweep at 44.1 kHz through
 * Mode::Legacy -- the framework default PLG-8 exists to displace -- must
 * produce a ripple figure that EXCEEDS the committed target by a wide margin.
 * That control is what proves this measurement CAN produce a failing figure,
 * so a green MameHq result is a property of the adopted filter and not of an
 * instrument that reports flat no matter what it is fed. Design section
 * 14.2.1 records why: Legacy puts the 44.1 kHz passband edge at 19,845 Hz,
 * below the 20 kHz this sweep requires to be flat.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() and nothing depends on
 * NDEBUG, so this file reports identically in every build type.
 */

#include "synthLib/resamplerInOut.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

	/* ------------------------------------------------------------------
	 * THE COMMITTED TARGET. Section 14.2.2's revised target table, the
	 * "Passband ripple, to 20 kHz" row. Editing this line is the ONLY way
	 * the target moves, and PLG-11 requires the operator to record the
	 * evidence for a move in the same commit. No code path writes it. */
	constexpr double g_committedTargetDb = 0.01;

	/* PLG-11's hard ceiling. No committed target may sit above it; a
	 * measurement that needs one is a defect in the resampler adoption and
	 * not a target to raise. MameHq is adopted, so the project does not own
	 * the filter and cannot tune it. */
	constexpr double g_hardCeilingDb = 0.10;

	constexpr double g_pi = 3.14159265358979323846;

	/* The device rate is fixed by the machine: g2::Device::getSamplerate()
	 * answers 96 kHz unconditionally (design section 14.1). It is written as
	 * a literal rather than read from g2::Device because PLG-11 requires this
	 * sweep to construct no device at all. */
	constexpr float g_deviceRate = 96000.0f;

	/* Section 14.1's six host rates, in the order that table lists them. */
	constexpr float g_hostRates[] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f };
	constexpr uint32_t g_hostRateCount = static_cast<uint32_t>(sizeof(g_hostRates) / sizeof(g_hostRates[0]));

	/* The swept band, and the ladder across it. 41 points over three decades
	 * is about 13.6 per decade. */
	constexpr double g_bandLowHz = 20.0;
	constexpr double g_bandHighHz = 20000.0;
	constexpr uint32_t g_toneCount = 41;

	/* The shortest capture the coherence search aims at, and the fewest
	 * whole cycles it will accept in one. The lowest tones get a longer
	 * capture because eight cycles of 20 Hz cannot fit in fewer samples. */
	constexpr uint32_t g_minCaptureSamples = 2048;
	constexpr uint32_t g_minCycles = 8;

	constexpr uint32_t g_settleSamples = 2048;
	constexpr uint32_t g_blockSize = 128;

	/* A tone the analysis can see exactly: f == cycles * hostRate / samples,
	 * so the capture holds a whole number of periods. */
	struct Tone
	{
		double frequency = 0.0;
		uint32_t cycles = 0;
		uint32_t captureSamples = 0;
	};

	Tone coherentTone(const double _targetHz, const double _hostRate)
	{
		Tone t;
		t.cycles = std::max(g_minCycles, static_cast<uint32_t>(std::lround(_targetHz * static_cast<double>(g_minCaptureSamples) / _hostRate)));

		auto samplesFor = [&](const double _hz) { return static_cast<uint32_t>(std::max(2L, std::lround(static_cast<double>(t.cycles) * _hostRate / _hz))); };
		auto frequencyFor = [&](const uint32_t _n) { return static_cast<double>(t.cycles) * _hostRate / static_cast<double>(_n); };

		uint32_t n = samplesFor(_targetHz);

		// Lengthening the capture lowers the tone and shortening it raises
		// the tone, so both ends of the band are reachable by moving n. The
		// step is well under the band width at every rung of the ladder, so
		// neither loop can run away.
		while(frequencyFor(n) > g_bandHighHz)
			++n;
		while(n > 2 && frequencyFor(n) < g_bandLowHz)
			--n;

		t.captureSamples = n;
		t.frequency = frequencyFor(n);
		return t;
	}

	/* The rectangular-window DFT bin the coherence above makes exact. The
	 * returned figure is the amplitude of a sine, not its RMS. */
	double coherentAmplitude(const std::vector<float>& _samples, const uint32_t _cycles, const uint32_t _count)
	{
		double re = 0.0;
		double im = 0.0;

		const double w = 2.0 * g_pi * static_cast<double>(_cycles) / static_cast<double>(_count);

		for(uint32_t n = 0; n < _count; ++n)
		{
			const double a = w * static_cast<double>(n);
			const double x = static_cast<double>(_samples[n]);
			re += x * std::cos(a);
			im -= x * std::sin(a);
		}

		return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(_count);
	}

	struct Sweep
	{
		double rippleDb = 0.0;			// max - min: the figure the target bounds
		double maxAbsDeviationDb = 0.0;	// diagnostic only, never the assertion
		double lowestToneHz = 0.0;
		double highestToneHz = 0.0;
		bool channelsAgree = true;
		std::vector<double> perToneDb;
	};

	/* The sweep. One resampler, the tones stepped through it in order. */
	Sweep runSweep(const float _hostRate, const synthLib::Resampler::Mode _mode)
	{
		synthLib::ResamplerInOut resampler(0, 2);
		resampler.setResamplerMode(_mode);
		resampler.setSamplerates(_hostRate, g_deviceRate);

		double frequency = 0.0;
		uint64_t deviceIndex = 0;

		// The signal source. It runs at the DEVICE rate -- the callback is
		// handed the resampler's device-side buffer -- and it SETS rather
		// than accumulates, which is the contract both of the framework's
		// two callback paths expect.
		const synthLib::ResamplerInOut::TProcessFunc generate =
			[&](const synthLib::TAudioInputs&, const synthLib::TAudioOutputs& _outs, const size_t _count, const synthLib::ResamplerInOut::TMidiVec&, synthLib::ResamplerInOut::TMidiVec&)
			{
				for(size_t i = 0; i < _count; ++i)
				{
					const double turns = std::fmod(frequency * static_cast<double>(deviceIndex) / static_cast<double>(g_deviceRate), 1.0);
					const auto v = static_cast<float>(std::sin(2.0 * g_pi * turns));

					_outs[0][i] = v;
					_outs[1][i] = v;

					++deviceIndex;
				}
			};

		std::vector<float> block0(g_blockSize, 0.0f);
		std::vector<float> block1(g_blockSize, 0.0f);

		synthLib::TAudioInputs ins{};
		ins.fill(nullptr);

		synthLib::TAudioOutputs outs{};
		outs.fill(nullptr);
		outs[0] = block0.data();
		outs[1] = block1.data();

		std::vector<float> capture0;
		std::vector<float> capture1;

		auto pump = [&](const uint32_t _hostSamples, const bool _collect)
		{
			uint32_t done = 0;

			while(done < _hostSamples)
			{
				const uint32_t n = std::min(g_blockSize, _hostSamples - done);

				synthLib::ResamplerInOut::TMidiVec midiOut;
				resampler.process(ins, outs, synthLib::ResamplerInOut::TMidiVec(), midiOut, n, generate);

				if(_collect)
				{
					capture0.insert(capture0.end(), block0.begin(), block0.begin() + n);
					capture1.insert(capture1.end(), block1.begin(), block1.begin() + n);
				}

				done += n;
			}
		};

		Sweep sweep;
		sweep.perToneDb.reserve(g_toneCount);

		for(uint32_t i = 0; i < g_toneCount; ++i)
		{
			const double decade = std::log10(g_bandHighHz / g_bandLowHz) * static_cast<double>(i) / static_cast<double>(g_toneCount - 1);
			const double target = g_bandLowHz * std::pow(10.0, decade);

			const Tone tone = coherentTone(target, static_cast<double>(_hostRate));

			frequency = tone.frequency;
			deviceIndex = 0;

			pump(g_settleSamples, false);

			capture0.clear();
			capture1.clear();
			pump(tone.captureSamples, true);

			if(capture0 != capture1)
				sweep.channelsAgree = false;

			const double amplitude = coherentAmplitude(capture0, tone.cycles, tone.captureSamples);
			const double db = 20.0 * std::log10(amplitude);

			sweep.perToneDb.push_back(db);

			if(i == 0)
				sweep.lowestToneHz = tone.frequency;
			if(i == g_toneCount - 1)
				sweep.highestToneHz = tone.frequency;
		}

		const auto minmax = std::minmax_element(sweep.perToneDb.begin(), sweep.perToneDb.end());
		sweep.rippleDb = *minmax.second - *minmax.first;
		sweep.maxAbsDeviationDb = std::max(std::fabs(*minmax.second), std::fabs(*minmax.first));

		return sweep;
	}
}

int main()
{
	/* ---------------------------------------------------------------
	 * Case 1. THE INSTRUMENT IS CALIBRATED BEFORE IT IS TRUSTED. A known
	 * positive and a known negative on synthesized tones of exactly known
	 * amplitude. An analyzer that answered 0 dB unconditionally -- the way a
	 * flat sweep and a blind instrument look identical -- fails the second
	 * of these. */
	{
		constexpr uint32_t count = 4096;
		constexpr uint32_t cycles = 137;

		std::vector<float> unity(count);
		std::vector<float> half(count);

		for(uint32_t n = 0; n < count; ++n)
		{
			const double a = 2.0 * g_pi * static_cast<double>(cycles) * static_cast<double>(n) / static_cast<double>(count);
			unity[n] = static_cast<float>(std::sin(a));
			half[n] = static_cast<float>(0.5 * std::sin(a));
		}

		const double unityDb = 20.0 * std::log10(coherentAmplitude(unity, cycles, count));
		const double halfDb = 20.0 * std::log10(coherentAmplitude(half, cycles, count));

		std::printf("info  analyzer known positive %.6f dB, known negative %.6f dB\n", unityDb, halfDb);

		check(std::fabs(unityDb) < 1.0e-4,
			"THE KNOWN POSITIVE: the analyzer reads a unit-amplitude coherent tone as 0 dB");
		check(std::fabs(halfDb - (-6.020599913279624)) < 1.0e-4,
			"THE KNOWN NEGATIVE: the analyzer reads a half-amplitude coherent tone as -6.0206 dB, so it is not answering 0 dB blindly");
	}

	/* ---------------------------------------------------------------
	 * Case 2. THE SWEEP, AT EACH OF THE SIX HOST RATES. The measured ripple
	 * is held against the committed target, and a failure names both
	 * figures. */
	std::vector<Sweep> sweeps;
	sweeps.reserve(g_hostRateCount);

	for(uint32_t r = 0; r < g_hostRateCount; ++r)
	{
		const float hostRate = g_hostRates[r];
		const Sweep sweep = runSweep(hostRate, synthLib::Resampler::Mode::MameHq);

		std::printf("info  host %.1f Hz: ripple %.6f dB peak-to-peak over %.2f..%.2f Hz (diagnostic: max |deviation| %.6f dB)\n",
			static_cast<double>(hostRate), sweep.rippleDb, sweep.lowestToneHz, sweep.highestToneHz, sweep.maxAbsDeviationDb);

		char what[256];

		std::snprintf(what, sizeof(what),
			"host %.1f Hz: measured passband ripple %.6f dB is within the committed target %.6f dB",
			static_cast<double>(hostRate), sweep.rippleDb, g_committedTargetDb);
		check(sweep.rippleDb <= g_committedTargetDb, what);

		std::snprintf(what, sizeof(what),
			"host %.1f Hz: the sweep spans the band the target is stated over (%.2f Hz to %.2f Hz)",
			static_cast<double>(hostRate), sweep.lowestToneHz, sweep.highestToneHz);
		check(sweep.lowestToneHz >= g_bandLowHz && sweep.lowestToneHz < 21.0 &&
			sweep.highestToneHz <= g_bandHighHz && sweep.highestToneHz > 19900.0, what);

		std::snprintf(what, sizeof(what),
			"host %.1f Hz: both output channels carried the same samples, so the figure is the filter's and not one channel's",
			static_cast<double>(hostRate));
		check(sweep.channelsAgree, what);

		sweeps.push_back(sweep);
	}

	/* ---------------------------------------------------------------
	 * Case 3. THE MEASUREMENT IS DETERMINISTIC, AND IT IS CHECKED. A second
	 * run of the 44.1 kHz sweep -- the rate with the most awkward ratio to
	 * 96 kHz -- must reproduce the first bit for bit. */
	{
		const Sweep again = runSweep(g_hostRates[0], synthLib::Resampler::Mode::MameHq);
		check(again.perToneDb == sweeps[0].perToneDb,
			"a second run of the same sweep reproduces every per-tone figure bit for bit: no random source, no clock");
	}

	/* ---------------------------------------------------------------
	 * Case 4. THE HARD CEILING BOUNDS THE TARGET ITSELF. PLG-11 forbids a
	 * committed target above 0.10 dB, so an edit that raised the constant
	 * past the ceiling must be caught here and not pass quietly. */
	check(g_committedTargetDb > 0.0 && g_committedTargetDb <= g_hardCeilingDb,
		"the committed target is positive and at or below PLG-11's hard ceiling of 0.10 dB");

	/* ---------------------------------------------------------------
	 * Case 5. THE PERMANENT CONTROL: THE SAME SWEEP THROUGH THE FRAMEWORK
	 * DEFAULT MUST FAIL THE TARGET. Legacy at 44.1 kHz puts the passband
	 * edge at 19,845 Hz, below the 20 kHz this sweep requires to be flat, so
	 * its ripple figure is far above the target. This is what proves the
	 * measurement can produce a failing figure at all -- without it, a green
	 * MameHq result would be indistinguishable from an instrument that
	 * reports flat whatever it is fed. */
	{
		const Sweep control = runSweep(g_hostRates[0], synthLib::Resampler::Mode::Legacy);

		std::printf("info  control, host %.1f Hz through Mode::Legacy: ripple %.6f dB peak-to-peak\n",
			static_cast<double>(g_hostRates[0]), control.rippleDb);

		check(control.rippleDb > g_committedTargetDb,
			"THE CONTROL: the same sweep through the framework default Legacy exceeds the committed target, so this measurement can fail");
		check(control.rippleDb > sweeps[0].rippleDb,
			"THE CONTROL: Legacy's ripple is worse than the adopted MameHq figure at the same host rate");
	}

	if(g_failures)
	{
		std::printf("t0_resampler_passband_ripple: %d of %d cases failed\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_resampler_passband_ripple: %d of %d cases passed\n", g_cases, g_cases);
	return 0;
}
