/* The DSP-side run call, the counterpart of Board::runMcu.
 *
 * It exists because dsp56k::DSP has no budgeted call. DSP::exec is
 *   ASMJIT_FORCE_INLINE void exec() noexcept
 * -- no argument, no return value, one dispatch unit -- so no cycle-bounded run
 * call exists in the library at all. This is an adapter this project writes.
 *
 * The return narrows uint64_t to uint32_t, and that is safe by the bound and
 * not by the types: the loop exits within one dispatch unit of wantCycles, and
 * both are far below 2^32 at a DSP allocation of about 1,562 cycles. A debug
 * build asserts that the difference fits before it casts.
 *
 * No floating-point type appears here: the determinism claim is made at the
 * 96 kHz Q23 integer boundary.
 */

#pragma once

#include <cstdint>

namespace dsp56k
{
	class DSP;
}

namespace g2
{
	/* Runs at least wantCycles emulated cycles and returns the cycles actually
	 * spent.
	 *
	 * The test is before each exec(), never after it. That choice is what makes
	 * the return at least wantCycles, which the debt rule assumes when it
	 * computes `spent - want` and floors the result at zero. A test-after loop
	 * would return less than wantCycles on the quantum that lands exactly on
	 * the deadline, and the floor would then discard the shortfall as though
	 * the context had idled -- a slow drift with no counter watching it.
	 *
	 * A wantCycles of 0 runs nothing.
	 *
	 * The counter is just-in-time only. M_cycles is written by the compiled
	 * block and not by execInterpreter, so this loop would never terminate in
	 * an interpreter build. The single-backend rule guarantees no Scheduler
	 * exists in such a build, and a debug build asserts dsp56k::g_useJIT here
	 * as a second guard.
	 *
	 * Overshoot is one dispatch unit, bounded by maxDispatchCost. Every test
	 * that names the bound takes it from its own fixture. */
	uint32_t runDspCycles(dsp56k::DSP& dsp, uint32_t wantCycles) noexcept;
}
