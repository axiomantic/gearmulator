/* The audio path's own arming predicate, shared by the tests that boot the real
 * firmware and then measure audio.
 *
 * It lives in a header because a boot drive that leaves on `programLanded` is
 * asserting about the kernel download and says nothing about audio, and a
 * second retyped copy of the predicate that replaces it is a copy that can
 * drift away from the first. `g2TestConsole/main.cpp` keeps its own copy: it is
 * a different target with its own include path, and widening that path is not
 * this header's business.
 *
 * What it reads: Dma::hasTrigger(EsaiReceiveData) on every chain position. That
 * is the DMA request registration itself and not a proxy for it.
 *
 * What would make it wrong, stated here because a predicate whose failure mode
 * is unstated is a predicate nobody can re-check:
 *
 *   - hasTrigger is sticky only because finishTransfer clears DE without
 *     calling removeTriggerTarget. Were dsp56300 to unregister on completion,
 *     the predicate would go false between transfers and a healthy machine
 *     could run a drive to its bound.
 *   - setDCR unregisters the trigger target on any reconfiguration, so a kernel
 *     that arms the channel and then rewrites DCR shows a window of false. A
 *     caller that polls every quantum and leaves on the first all-armed reading
 *     cannot miss such a window, but what it saw would be an arming since
 *     withdrawn.
 *   - It reports registration, not traffic. A channel registered against a
 *     source that never asserts satisfies it forever. Reading the destination
 *     buffers is a separate instrument.
 */

#pragma once

#include "../board.h"

#include "dsp56kEmu/dma.h"
#include "dsp56kEmu/peripherals.h"

namespace g2test
{
	inline unsigned countRxArmed(g2::Board& _board, const unsigned _dspCount)
	{
		unsigned armed = 0;

		for(unsigned port = 0; port < _dspCount; ++port)
		{
			if(_board.dspSet().peripherals(port).getDMA().hasTrigger(
				dsp56k::DmaChannel::RequestSource::EsaiReceiveData))
				++armed;
		}

		return armed;
	}
}
