/* dspContext.h -- JobFault, JobContext and DspContext. Task SCH-6.
 * Design section 13.10.3.
 *
 * THE ERROR CHANNEL, AS A TYPE. An earlier design draft said a fault "sets the
 * fault field of that job's own context" and declared no context type, no
 * fault field and no accessor, so the path after Executor::run could not be
 * written at all. The three declarations below close it.
 *
 * The eight DspContext objects are the whole job array. They are declared here
 * because Job points at one, and because a `void* user` gave the fault field
 * nowhere to live.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "g2/timebase.h"

/* THE TWO LIBRARY TYPES ARE FORWARD-DECLARED AND NOT INCLUDED. Every member
 * below that names one is a POINTER, so the pointee needs no definition here,
 * and a context header that pulled in the whole ESAI and DSP headers would
 * make every consumer of this file pay for them.
 *
 * NO EsaiClock IS DECLARED, CONSTRUCTED OR NAMED. An EsaiClock cannot follow a
 * rational cycles-for-each-frame rate -- its consumer is a uint32_t phase
 * accumulator and the DSP rational is not a whole number -- so the scheduler
 * drives the ESAI frame and this context carries the two PORTS instead of a
 * clock. Design sections 13.4.1 and 13.10.3. */
namespace dsp56k
{
	class DSP;
	class Esai;
}

namespace g2
{
	/* The faults a job can report.
	 *
	 * CoreHalted is THE MCU CONTEXT ONLY. Board::faulted() is true: the MCF5307
	 * core took a fault during exception stacking and halted. No DSP job ever
	 * writes this value and no MCU fault ever carries one of the three above
	 * it.
	 *
	 * IT IS NOT A SECOND FAULT CODE ON THE BOARD. Board::faulted() stays one
	 * bit, because the address, the width and the direction are already
	 * recorded by mcf5307_bus_status and a second code could disagree with
	 * that record. This enumerator is the SCHEDULER's mapping of that one bit
	 * into its own channel, so that contextFault(i) != JobFault::None is a
	 * valid fault test for EVERY context index and not for the DSPs only.
	 * Without it the MCU row of that test is a false negative. */
	enum class JobFault : uint32_t
	{
		None               = 0,
		IllegalInstruction = 1,  /* one of the 28 of design section 11.3, or
		                            an opcode the backend refuses           */
		MemoryFault        = 2,  /* an access dsp56300 could not complete   */
		BackendFault       = 3,  /* the just-in-time compiler could not
		                            compile a block                        */
		CoreHalted         = 4   /* THE MCU CONTEXT ONLY                    */
	};

	/* Every job's context BEGINS with this block, and the Executor never reads
	 * it -- the Scheduler does, after run() returns. Making it the head of the
	 * context rather than a separate array keeps a fault beside the state that
	 * produced it, which is what a diagnostic needs.
	 *
	 * The field is STICKY. A job sets it and never clears it; only
	 * Scheduler::reset clears it. */
	struct JobContext
	{
		JobFault fault;
	};

	/* The DSP context.
	 *
	 * STANDARD LAYOUT IS LOAD-BEARING, NOT INCIDENTAL. Job::ctx is a
	 * JobContext*, and the job body must recover the DspContext from it. That
	 * recovery is legal ONLY because DspContext is a standard-layout type,
	 * which makes it pointer-interconvertible with its first member. Every
	 * member below is a scalar or a standard-layout C struct, there is no
	 * virtual method, no private member and no second access-control section,
	 * so the property holds. A later member that broke it would make every job
	 * body undefined behaviour with NO DIAGNOSTIC, so the assertions below sit
	 * beside the declaration and the recovery uses reinterpret_cast on the
	 * pointer and nothing else:
	 *
	 *     auto* c = reinterpret_cast<DspContext*>(ctx);   // ctx is &c->base
	 */
	struct DspContext
	{
		JobContext base;      /* MUST be first. Job::ctx points here.       */
		unsigned   position;  /* 0 .. dspCount-1, the chain position        */
		Rational   rate;      /* cycles for each frame, design 13.4.1       */
		uint32_t   acc;       /* the rational accumulator, design 13.4.1    */
		int64_t    debt;      /* the cycle debt, design 13.4.6              */
		uint64_t   longDispatchQuanta;   /* the rule 4 counter, 13.4.6      */
		dsp56k::DSP* dsp;     /* borrowed; the Scheduler owns the DSP set   */

		/* THE FOUR MEMBERS BELOW EXIST BECAUSE THE SCHEDULER DRIVES THE ESAI
		 * FRAME. Without them the job body cannot name the port it must
		 * advance, and it cannot decide whether this quantum is inside the
		 * second bus's advance window. */
		dsp56k::Esai* audioEsai;   /* borrowed; the X-space ESAI, 11.1      */
		dsp56k::Esai* secondEsai;  /* borrowed; the Y-space ESAI_1, 11.1    */

		/* The scheduler's virtual frame index for the quantum ABOUT TO RUN.
		 * The Scheduler writes it into every DspContext before it calls
		 * Executor::run, and NO job writes it. It is a copy and not a pointer,
		 * so a job cannot observe another job's state. */
		uint64_t   frameIndex;

		/* Design section 12.3's D, from G2_SECOND_BUS_FRAME_DIVIDER. Fixed at
		 * construction. The job body advances the second bus only when
		 * frameIndex % secondBusFrameDivider == 0, which is the SAME window
		 * ChainAdapter::advanceAll uses. Both read one symbol, and the same
		 * code decides both. It is never 0: Scheduler::create returns
		 * Status::BadDivider and no object for that value, so the modulo
		 * cannot divide by zero. */
		unsigned   secondBusFrameDivider;
	};

	/* THE TWO ASSERTIONS THAT MAKE THE RECOVERY LEGAL. They live here, at the
	 * declaration site, so every consumer of this header carries them and not
	 * the test alone. A release build removes an assert(); it does not remove
	 * these. */
	static_assert(std::is_standard_layout_v<DspContext>,
		"DspContext must be standard-layout. The executor recovers it from a "
		"pointer to its first member, and that is legal only for a "
		"standard-layout type.");
	static_assert(offsetof(DspContext, base) == 0,
		"JobContext base must be the FIRST member of DspContext. Job::ctx "
		"points at it.");
}
