// The ONE path in the G2 that writes DSP program memory. Every P-memory write
// the board makes -- the HDI08 download path's `0xC2` records, a blob injected
// for a test, anything -- calls the function below and nothing else calls
// dsp56k::Memory::set for the P area.
//
// The just-in-time compiler caches compiled blocks keyed by P address. A write
// that changes an instruction WITHOUT telling the compiler leaves the stale
// block in place, so the firmware's new code sits in memory while the OLD code
// keeps executing. Nothing faults and nothing logs. On macOS the same defect can
// present as a segmentation fault instead, because dsp56kBase/mmuarray.h
// force-disables the MMU path there and the fallback pre-allocates nothing.
//
// `.github/workflows/track-board.yml` carries the `pmem-funnel-lint` step, which
// fails the build when any file under `source/nord/g2/` other than this pair
// names a P-memory write.
//
// The notification the funnel owes is a call on the DSP's compiler, so a funnel
// handed only a Memory could not honour its own contract. Taking the DSP makes
// the contract expressible in the signature.

#pragma once

#include "dsp56kEmu/types.h"

namespace dsp56k
{
	class DSP;
}

namespace g2
{
	// Write ONE 24-bit word into the DSP's P memory and tell the just-in-time
	// compiler that the word changed. Returns what the underlying memory write
	// returned: false means the address was not writable and the word did not
	// land.
	//
	// The notification is UNCONDITIONAL. dsp56k::DSP::memWriteP notifies only
	// when the value actually changed and only below the opcode cache size;
	// both conditions are correct for that function and neither is a
	// guarantee this one may lean on, so the funnel states the guarantee its
	// callers need -- after this call, no compiled block for _address
	// survives.
	bool writePMem(dsp56k::DSP& _dsp, dsp56k::TWord _address, dsp56k::TWord _word);
}
