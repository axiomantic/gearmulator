// Task BRD-20. The P-memory write funnel.
//
// Plan section 13.3, BRD-20. Design section 10.5, section 17 row 7.1.
//
// THIS FILE IS THE ONE THE LINT ALLOWS. See pmemFunnel.h for what the funnel
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

		// THE NOTIFICATION IS THE POINT OF THE FUNNEL AND IT DOES TWO JOBS,
		// not one. Jit::notifyProgramMemWrite reaches
		// JitBlockChain::notifyPMemWrite, which destroys any compiled block at
		// the address AND, for the current chain, grows the block-function
		// table to cover it. The first job is the one the task is named for:
		// without it the compiler keeps running code the write just replaced.
		// The second is why an omission is not merely silent -- an address the
		// table never grew to cover is read out of bounds on the fallback
		// allocation path that __APPLE__ takes unconditionally.
		_dsp.getJit().notifyProgramMemWrite(_address);

		return written;
	}
}
