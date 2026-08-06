/* runDspCycles.h -- the DSP-side run call. Task SCH-8.
 * Design sections 13.10.3 and 26.
 *
 * THIS IS ctx.run(want) OF DESIGN SECTION 13.4.6 FOR A DSP CONTEXT, and it is
 * the counterpart of Board::runMcu.
 *
 * IT EXISTS BECAUSE dsp56k::DSP HAS NO BUDGETED CALL. DSP::exec is
 *   ASMJIT_FORCE_INLINE void exec() noexcept
 * -- no argument, no return value, ONE dispatch unit -- so no cycle-bounded run
 * call exists in the library at all. This is an adapter this project writes.
 * An earlier design draft asserted that DSP::exec returns a uint32_t and takes
 * a budget; a reader who trusted it would have written dsp.exec(want), which
 * does not compile.
 *
 * THE RETURN NARROWS uint64_t TO uint32_t, and that is safe BY THE BOUND and
 * not by the types: the loop exits within one dispatch unit of wantCycles, and
 * both are far below 2^32 at a DSP allocation of about 1,562 cycles. A debug
 * build asserts that the difference fits before it casts.
 *
 * NO FLOATING-POINT TYPE APPEARS HERE. The determinism claim is made at the
 * 96 kHz Q23 integer boundary and a float inside it would end that claim.
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
	 * THE TEST IS BEFORE EACH exec(), NEVER AFTER IT. That choice is what makes
	 * the return AT LEAST wantCycles, which is what design section 13.4.6's
	 * debt rule assumes when it computes `spent - want` and floors the result
	 * at zero. A test-after loop would return LESS than wantCycles on the
	 * quantum that lands exactly on the deadline, and the floor would then
	 * discard the shortfall as though the context had idled -- a slow drift
	 * with no counter watching it.
	 *
	 * A wantCycles OF 0 RUNS NOTHING, which is correct: design section 13.4.6
	 * takes the `want <= 0` branch before it ever calls this.
	 *
	 * THE COUNTER IS JUST-IN-TIME ONLY. m_cycles is written by the compiled
	 * block and NOT by execInterpreter, so this loop would never terminate in
	 * an interpreter build. Design section 11.4.3's single-backend rule
	 * guarantees no Scheduler exists in such a build -- SCH-17 makes
	 * Scheduler::create refuse it -- and a debug build asserts
	 * dsp56k::g_useJIT here as a second guard, because a loop that cannot
	 * terminate deserves one.
	 *
	 * OVERSHOOT IS ONE DISPATCH UNIT. What bounds that unit is
	 * maxDispatchCost, which measurement register row 1 owns and which has no
	 * value until spike criterion SPK-5 reports. Every test that names the
	 * bound takes it from its own fixture. */
	uint32_t runDspCycles(dsp56k::DSP& dsp, uint32_t wantCycles) noexcept;
}
