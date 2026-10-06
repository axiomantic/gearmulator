/* Passband ripple of synthLib::ResamplerInOut, measured on its own: a stepped
 * sine sweep from 20 Hz to 20 kHz in Resampler::Mode::MameHq, at each host
 * rate in g_hostRates, with no firmware, no emulated machine and no
 * rg2::Device anywhere. It reports the peak-to-peak deviation from 0 dB over
 * the swept band and compares it against g_committedTargetDb.
 *
 * Nothing here writes the target, opens a file, reads an environment variable
 * or creates a thread, and no assertion is a language assert(), so the file
 * reports identically in every build type.
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

	// The target for passband ripple to 20 kHz. No code path writes it, so
	// only a deliberate edit moves it, and the evidence belongs in that commit.
	constexpr double g_committedTargetDb = 0.01;

	// No target may sit above this ceiling: MameHq is adopted rather than
	// written here, so a measurement needing a looser target is a defect in
	// the adoption and not a number to raise.
	constexpr double g_hardCeilingDb = 0.10;

	constexpr double g_pi = 3.14159265358979323846;

	// rg2::Device::getSamplerate() answers 96 kHz unconditionally; a literal,
	// because this sweep constructs no device at all.
	constexpr float g_deviceRate = 96000.0f;

	constexpr float g_hostRates[] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f };
	constexpr uint32_t g_hostRateCount = static_cast<uint32_t>(sizeof(g_hostRates) / sizeof(g_hostRates[0]));

	// The swept band and the logarithmic ladder across it.
	constexpr double g_bandLowHz = 20.0;
	constexpr double g_bandHighHz = 20000.0;
	constexpr uint32_t g_toneCount = 41;

	// The shortest capture the coherence search aims at, and the fewest whole
	// cycles it will accept in one. The lowest tones get a longer capture
	// because eight cycles of 20 Hz cannot fit in fewer samples.
	constexpr uint32_t g_minCaptureSamples = 2048;
	constexpr uint32_t g_minCycles = 8;

	// Discarded before every capture. The output filter is 400 taps per lane
	// at the 96 kHz device rate, so its group delay is about 200 device
	// samples -- under 100 host samples at 44.1 kHz. This also covers the step
	// discontinuity at the tone change.
	constexpr uint32_t g_settleSamples = 2048;
	constexpr uint32_t g_blockSize = 128;

	// A tone the analysis can see exactly: f == cycles * hostRate / samples, so
	// the capture holds a whole number of periods. That coherence is what makes
	// the analysis a leak-free rectangular-window DFT bin, needing no window
	// function and no leakage budget.
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

	// The rectangular-window DFT bin the coherence above makes exact: 2*|X_C|/N
	// is exact for a sine of frequency C*hostRate/N sampled N times. The
	// returned figure is the amplitude of a sine, not its RMS.
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
		double rippleDb = 0.0;			// max - min, the standard definition of passband ripple; the figure the target bounds
		double maxAbsDeviationDb = 0.0;	// diagnostic only, never the assertion
		double lowestToneHz = 0.0;
		double highestToneHz = 0.0;
		bool channelsAgree = true;
		std::vector<double> perToneDb;
	};

	// One resampler, the tones stepped through it low to high: the filter state
	// carries across each step and g_settleSamples is what flushes it, which is
	// what makes this a sweep rather than a set of unrelated runs.
	Sweep runSweep(const float _hostRate, const synthLib::Resampler::Mode _mode)
	{
		// Zero input channels removes nothing that is measured. The sweep is
		// generated inside the process callback at the device rate, so the
		// host-to-device resampler would only ever filter silence; feedInput is
		// guarded by if(m_channelCountIn), so the input half goes inert rather
		// than being skipped by a special case. What is measured is the
		// device-to-host output path, which every emulated sample takes.
		synthLib::ResamplerInOut resampler(0, 2);
		resampler.setResamplerMode(_mode);
		resampler.setSamplerates(_hostRate, g_deviceRate);

		double frequency = 0.0;
		uint64_t deviceIndex = 0;

		// The signal source. It runs at the device rate -- the callback is
		// handed the resampler's device-side buffer -- and it sets rather
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
	// Case 1. Calibrate the analyzer before trusting it, on two synthesized
	// tones of exactly known amplitude: 1.0 (0 dB) and 0.5 (-6.0206 dB). An
	// analyzer answering 0 dB unconditionally fails the second, which is what
	// separates a flat sweep from a blind instrument.
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

	// Case 2. The sweep at each host rate, held against the target; a failure
	// names both figures.
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

	// Case 3. Determinism, checked rather than assumed: a second run of the
	// 44.1 kHz sweep -- the most awkward ratio to 96 kHz -- must reproduce the
	// first bit for bit. The tone grid is fixed, phases start at zero, no clock
	// is read and no thread is created.
	{
		const Sweep again = runSweep(g_hostRates[0], synthLib::Resampler::Mode::MameHq);
		check(again.perToneDb == sweeps[0].perToneDb,
			"a second run of the same sweep reproduces every per-tone figure bit for bit: no random source, no clock");
	}

	// Case 4. The ceiling bounds the target itself, not only the measurement,
	// so an edit raising the target past it fails here rather than passing
	// quietly.
	check(g_committedTargetDb > 0.0 && g_committedTargetDb <= g_hardCeilingDb,
		"the committed target is positive and at or below the hard ceiling of 0.10 dB");

	// Case 5. The control, which proves this measurement can fail at all:
	// Mode::Legacy at 44.1 kHz puts the passband edge at 19,845 Hz, below the
	// 20 kHz the sweep requires to be flat, so its ripple must exceed the
	// target by a wide margin. Without it a green MameHq result would be
	// indistinguishable from an instrument that reports flat whatever it is fed.
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
