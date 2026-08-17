// Task BRD-20. The P-memory write funnel.
//
// Plan section 13.3, BRD-20. Design section 10.5, section 17 row 7.1.
//
// WHAT THIS FILE IS. The ONE path in the G2 that writes DSP program memory.
// Every P-memory write the board makes -- the HDI08 download path's `0xC2`
// records, a blob injected for a test, anything -- calls the function below
// and nothing else calls dsp56k::Memory::set for the P area.
//
// WHY ONE PATH. The just-in-time compiler caches compiled blocks keyed by P
// address. A write that changes an instruction WITHOUT telling the compiler
// leaves the stale block in place, so the firmware's new code sits in memory
// while the OLD code keeps executing. Nothing faults and nothing logs. On
// macOS the same defect can present as a segmentation fault instead, because
// dsp56kBase/mmuarray.h force-disables the MMU path there and the fallback
// pre-allocates nothing -- but the crash is a symptom of one platform, not the
// defect. The defect is silent everywhere else, which is what makes a single
// funnel worth enforcing rather than a convention worth documenting.
//
// THE ENFORCEMENT IS NOT THIS COMMENT. `.github/workflows/track-board.yml`
// carries the `pmem-funnel-lint` step, which fails the build when any file
// under `source/nord/g2/` other than this pair names a P-memory write. A rule
// that lives only in prose is a rule that a future contributor never reads.
//
// P MEMORY IS REACHED THROUGH THE DSP AND NOT THROUGH A Memory REFERENCE. The
// notification the funnel owes is a call on the DSP's compiler, so a funnel
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
