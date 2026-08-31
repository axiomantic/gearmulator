/* g2Plugin.cpp -- the synthLib::Plugin subclass body. Task PLG-8.
 *
 * Design sections 14.2.2, 18.2 and 24. See g2Plugin.h for the full
 * statement of the ordering (set before any prepareToPlay), the side
 * effect PLG-15's fixture must plan for, and the observable.
 *
 * prepareToPlay is the host's one moment, and it is here rather than in the
 * Device because it is the moment the host's block size and rate first exist.
 * It derives B and the reported latency together from that one pair, and it is
 * the call site that wires the host to the Device's boot.
 */

#include "g2Plugin.h"

#include "g2Device.h"

#include "g2/timebase.h"

#include <cmath>
#include <vector>

namespace g2
{
	Plugin::Plugin(synthLib::Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid)
		: synthLib::Plugin(_device, std::move(_callbackDeviceInvalid))
	{
		// THE ONE LINE THE DESIGN EXISTS FOR. The base constructor has run no
		// resampler work (both samplerates are 0, recreate() early-returns),
		// so this set is a pure member write; the 512-sample pre-warm runs
		// later, at the first setHostSamplerate, with the mode already
		// MameHq. Legacy is the framework default in two places
		// (resampler.h:30, resamplerInOut.h:43); inheriting it is the
		// two-line defect this one-line call forecloses.
		// ONE statement, deliberately: the record and the set are the same
		// act. Deleting either half of it deletes the other, so the
		// reported mode cannot outlive the call that made it true.
		setResamplerMode(m_resamplerMode = synthLib::Resampler::Mode::MameHq);

		// The lookahead is fixed for the life of the plugin and is written
		// once, here, so that every Scheduler this object ever creates carries
		// the same L. dynamic_cast rather than static_cast: a plugin handed a
		// device of another kind must leave m_device null and refuse to
		// re-create, not reinterpret it.
		m_schedulerConfig.lookaheadFrames = kLookaheadFrames;
		m_device = dynamic_cast<Device*>(_device);
	}

	uint32_t Plugin::chainDelayFrames() const noexcept
	{
		return (m_schedulerConfig.dspCount - 1u) * m_schedulerConfig.hopFrames;
	}

	uint32_t Plugin::maxHostBlockFramesFor(const uint32_t _maxHostBlockSamples, const float _hostSamplerate) noexcept
	{
		if(_maxHostBlockSamples == 0 || _hostSamplerate <= 0.0f)
			return 0;

		const double frames = static_cast<double>(_maxHostBlockSamples)
			* static_cast<double>(G2_FRAME_RATE_HZ) / static_cast<double>(_hostSamplerate);

		return static_cast<uint32_t>(frames) + 1u;
	}

	uint32_t Plugin::resamplerDelaySamples() const noexcept
	{
		const uint32_t blockTerm = m_blockSizeSamples * getLatencyBlocks();
		const uint32_t total     = getLatencyInputToOutput();

		return total > blockTerm ? total - blockTerm : 0;
	}

	bool Plugin::prepareToPlay(const uint32_t _maxHostBlockSamples, const float _hostSamplerate)
	{
		if(_maxHostBlockSamples == 0 || _hostSamplerate <= 0.0f)
			return false;

		// BEFORE THE TWO DERIVATIONS, AND IN THIS ORDER. setHostSamplerate
		// runs the resampler pre-warm, which is what fixes D_resampler(R);
		// setBlockSize then updates the framework's own latency figures
		// against it. Reading either derived figure between the two calls
		// would read a resampler delay that belongs to the previous rate.
		setHostSamplerate(_hostSamplerate, static_cast<float>(G2_FRAME_RATE_HZ));
		setBlockSize(_maxHostBlockSamples);
		m_blockSizeSamples = _maxHostBlockSamples;

		bool ready = true;

		const uint32_t b = maxHostBlockFramesFor(_maxHostBlockSamples, _hostSamplerate);

		// B IS A CEILING, so only growth costs anything. An equal or smaller
		// maximum block fits the queues that are already allocated and the
		// running machine keeps rendering.
		if(b > m_maxHostBlockFrames)
		{
			m_maxHostBlockFrames                 = b;
			m_schedulerConfig.maxHostBlockFrames = b;
			ready = recreateScheduler();
		}

		// D_total(R) = ceil((L + D_chain + D_codec) * R / G2_FRAME_RATE_HZ)
		//              + D_resampler(R).
		//
		// THE ceil IS DELIBERATE. The figure the host is told must be a whole
		// number of samples, and rounding down would claim less delay than the
		// plugin takes -- the direction that misaligns the host's delay
		// compensation. The sum in frames is bounded by Scheduler::create,
		// which is the one place that refuses a lookahead the framework would
		// silently clamp.
		const uint64_t frames = static_cast<uint64_t>(kLookaheadFrames)
			+ chainDelayFrames() + kDelayCodecFrames;

		const double samples = static_cast<double>(frames)
			* static_cast<double>(_hostSamplerate) / static_cast<double>(G2_FRAME_RATE_HZ);

		m_reportedLatencySamples = static_cast<uint32_t>(std::ceil(samples)) + resamplerDelaySamples();

		return ready;
	}

	bool Plugin::recreateScheduler()
	{
		++m_schedulerRecreations;

		if(m_device == nullptr)
			return false;

#if SYNTHLIB_DEMO_MODE == 0
		// THE SEVEN STATE ITEMS, AND THE MACHINE SNAPSHOT WITH THEM. getState
		// takes the snapshot inside the Device's hand-off window, which is the
		// only place it can be taken, and boot() destroys the Scheduler that
		// snapshot came from -- so it has to happen first. The snapshot is
		// empty on the first call, and boot() reads an empty one as "no state
		// to restore" and boots clean.
		std::vector<uint8_t> items;
		m_device->getState(items, synthLib::StateTypeGlobal);
#endif

		Device::BootRequest request;
		request.config          = m_schedulerConfig;
		request.machineSnapshot = &m_device->machineSnapshot();

		return m_device->boot(request).booted;
	}
}
