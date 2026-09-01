/* JobFault, JobContext and DspContext: the scheduler's error channel, as a
 * type. The eight DspContext objects are the whole job array. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "g2/timebase.h"

/* The two library types are forward-declared and not included: every member
 * below that names one is a pointer, and a context header that pulled in the
 * whole ESAI and DSP headers would make every consumer pay for them.
 *
 * No EsaiClock is declared, constructed or named. An EsaiClock cannot follow a
 * rational cycles-for-each-frame rate -- its consumer is a uint32_t phase
 * accumulator and the DSP rational is not a whole number -- so the scheduler
 * drives the ESAI frame and this context carries the two ports instead. */
namespace dsp56k
{
	class DSP;
	class Esai;
}

namespace g2
{
	/* The faults a job can report.
	 *
	 * CoreHalted is the MCU context only: Board::faulted() is true, the MCF5307
	 * core took a fault during exception stacking and halted. No DSP job writes
	 * it and no MCU fault carries one of the three above it.
	 *
	 * It is not a second fault code on the board. Board::faulted() stays one
	 * bit, because the address, the width and the direction are already
	 * recorded by mcf5307_bus_status and a second code could disagree with that
	 * record. This enumerator is the scheduler's mapping of that one bit into
	 * its own channel, so that contextFault(i) != JobFault::None is a valid
	 * fault test for every context index and not for the DSPs only. */
	enum class JobFault : uint32_t
	{
		None               = 0,
		IllegalInstruction = 1,  /* an opcode the backend refuses           */
		MemoryFault        = 2,  /* an access dsp56300 could not complete   */
		BackendFault       = 3,  /* the just-in-time compiler could not
		                            compile a block                        */
		CoreHalted         = 4   /* the MCU context only                    */
	};

	/* Every job's context begins with this block, and the Executor never reads
	 * it -- the Scheduler does, after run() returns.
	 *
	 * The field is sticky. A job sets it and never clears it; only
	 * Scheduler::reset clears it. */
	struct JobContext
	{
		JobFault fault;
	};

	/* The DSP context.
	 *
	 * Standard layout is load-bearing. Job::ctx is a JobContext*, and the job
	 * body recovers the DspContext from it; that is legal only because
	 * DspContext is a standard-layout type, which makes it
	 * pointer-interconvertible with its first member. A later member that broke
	 * the property -- a virtual method, a private member, a second
	 * access-control section -- would make every job body undefined behaviour
	 * with no diagnostic, so the assertions below sit beside the declaration:
	 *
	 *     auto* c = reinterpret_cast<DspContext*>(ctx);   // ctx is &c->base
	 */
	struct DspContext
	{
		JobContext base;      /* MUST be first. Job::ctx points here.       */
		unsigned   position;  /* 0 .. dspCount-1, the chain position        */
		Rational   rate;      /* cycles for each frame                      */
		uint32_t   acc;       /* the rational accumulator                   */
		int64_t    debt;      /* the cycle debt                             */
		uint64_t   longDispatchQuanta;
		dsp56k::DSP* dsp;     /* borrowed; the Scheduler owns the DSP set   */

		/* dspJob overwrites both at the top of every quantum and no other code
		 * reads them. The divisor must bound the dispatch count. */
		uint32_t   slotBudgetDivisor;    /* the divisor the quantum derived */
		uint32_t   slotDispatches;       /* times the callback actually fired */

		/* THE MEMBERS BELOW EXIST BECAUSE THE SCHEDULER DRIVES THE ESAI
		 * FRAME. Without them the job body cannot name the port it must
		 * advance, and it cannot decide whether this quantum is inside the
		 * second bus's advance window. */
		dsp56k::Esai* audioEsai;   /* borrowed; the X-space ESAI, 11.1      */
		dsp56k::Esai* secondEsai;  /* borrowed; the Y-space ESAI_1, 11.1    */

		/* The scheduler's virtual frame index for the quantum about to run.
		 * The Scheduler writes it into every DspContext before it calls
		 * Executor::run, and no job writes it. It is a copy and not a pointer,
		 * so a job cannot observe another job's state. */
		uint64_t   frameIndex;

		/* Whether this slot's program has landed. Borrowed, and a pointer
		 * rather than a bool by value: the producer sets its own flag once the
		 * firmware download completes, and a copy taken at construction could
		 * never see that.
		 *
		 * NULL means NOT landed. The job body reads it as the gate on step 2,
		 * and the direction is chosen so that an unwired gate stops the slot
		 * rather than running one whose program memory holds nothing but
		 * no-operations. Without the initializer a context declared without
		 * braces holds an indeterminate pointer, and the gate opens on whatever
		 * the storage carried. */
		const bool* programLanded = nullptr;  /* borrowed; NULL means NOT landed */

		/* From G2_SECOND_BUS_FRAME_DIVIDER. Fixed at
		 * construction. The job body advances the second bus only when
		 * frameIndex % secondBusFrameDivider == 0, which is the SAME window
		 * ChainAdapter::advanceAll uses. Both read one symbol, and the same
		 * code decides both. It is never 0: Scheduler::create returns
		 * Status::BadDivider and no object for that value, so the modulo
		 * cannot divide by zero. */
		unsigned   secondBusFrameDivider;
	};

	/* The two assertions that make the recovery legal. They live at the
	 * declaration site, so every consumer of this header carries them. A
	 * release build removes an assert(); it does not remove these. */
	static_assert(std::is_standard_layout_v<DspContext>,
		"DspContext must be standard-layout. The executor recovers it from a "
		"pointer to its first member, and that is legal only for a "
		"standard-layout type.");
	static_assert(offsetof(DspContext, base) == 0,
		"JobContext base must be the FIRST member of DspContext. Job::ctx "
		"points at it.");
}
