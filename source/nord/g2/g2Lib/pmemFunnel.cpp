// This file is the one the lint allows. See pmemFunnel.h for what the funnel
// is and why it is enforced mechanically rather than by convention.

#include "pmemFunnel.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"

namespace g2
{
	bool writePMem(dsp56k::DSP& _dsp, const dsp56k::TWord _address, const dsp56k::TWord _word)
	{
		const bool written = _dsp.memory().set(dsp56k::MemArea_P, _address, _word);

		// Jit::notifyProgramMemWrite reaches JitBlockChain::notifyPMemWrite,
		// which destroys any compiled block at the address AND, for the current
		// chain, grows the block-function table to cover it. Without the first,
		// the compiler keeps running code the write just replaced. Without the
		// second, an address the table never grew to cover is read out of bounds
		// on the fallback allocation path that __APPLE__ takes unconditionally.
		_dsp.getJit().notifyProgramMemWrite(_address);

		return written;
	}
}
