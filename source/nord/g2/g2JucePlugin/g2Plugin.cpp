/* g2Plugin.cpp -- the synthLib::Plugin subclass body. Task PLG-8.
 *
 * Design sections 14.2.2, 18.2 and 24. See g2Plugin.h for the full
 * statement of the ordering (set before any prepareToPlay), the side
 * effect PLG-15's fixture must plan for, and the observable.
 *
 * Minimal shell by design: PLG-9 and PLG-10 extend this file (prepareToPlay
 * computation of B and D_total) and PLG-12 may extend the wiring further.
 */

#include "g2Plugin.h"

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
	}
}
