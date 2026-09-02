/* The DSP-side run call. */

#include "runDspCycles.h"

#include "dsp56kEmu/dsp.h"

#include <cassert>

namespace g2
{
	uint32_t runDspCycles(dsp56k::DSP& dsp, const uint32_t wantCycles) noexcept
	{
		/* Debug only, and not the predicate of any check. The default build of
		 * this tree is Release and defines NDEBUG, so neither assertion below
		 * is in the shipped translation unit. T0_run_dsp_cycles_contract and
		 * t0_run_dsp_cycles drive the two cases that decide this function and
		 * neither reads an assertion. */
		assert(dsp56k::g_useJIT &&
			"m_cycles is written by the compiled block and not by the "
			"interpreter, so this loop cannot terminate in an interpreter "
			"build.");

		/* getCycles() RETURNS A REFERENCE, const uint64_t&, so the reads below
		 * observe the LIVE counter and not a snapshot. That is what makes the
		 * loop terminate. Do not bind it to a value and re-test the value. */
		const uint64_t start = dsp.getCycles();

		/* The test is before the exec(). See the header. */
		while(dsp.getCycles() - start < wantCycles)
			dsp.exec();

		const uint64_t spent = dsp.getCycles() - start;

		assert(spent <= 0xFFFFFFFFull &&
			"the narrowing is safe BY THE BOUND: the loop exits within one "
			"dispatch unit of wantCycles.");

		return static_cast<uint32_t>(spent);
	}
}
