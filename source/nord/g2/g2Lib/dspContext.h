#pragma once

// dspContext.h — SCH-7
//
// Per-DSP context used by the scheduler quantum loop.

#include <vector>

namespace g2
{
	class DspSet;

	/* DspContext — SCH-7
	 *
	 * Holds everything the scheduler needs on a per-DSP basis:
	 *   • a non-owning pointer to the DspSet the DSP lives in,
	 *   • the zero-based index of this DSP within that set, and
	 *   • a per-context sample buffer that the scheduler writes into
	 *     before handing frames to the DSP.
	 *
	 * The DspSet pointer is non-owning (the Board owns the DspSet).
	 */
	struct DspContext
	{
		DspSet* dspSet{nullptr};
		int     dspIndex{0};
		std::vector<float> sampleBuffer;

		DspContext() = default;
		DspContext(DspSet* _set, int _index, size_t _bufferFrames = 0)
		    : dspSet(_set)
		    , dspIndex(_index)
		    , sampleBuffer(_bufferFrames, 0.0f)
		{
		}

		DspContext(const DspContext&) = delete;
		DspContext& operator=(const DspContext&) = delete;
		DspContext(DspContext&&) = default;
		DspContext& operator=(DspContext&&) = default;
	};
} // namespace g2
