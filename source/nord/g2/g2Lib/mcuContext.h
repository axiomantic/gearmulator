/* mcuContext.h -- the MCU context and the four members the cycle-debt block
 * reads. Task SCH-21 step 6 (formerly SCH-30). Design section 13.4.6.
 *
 * WHY A STRUCT OF ITS OWN AND NOT A JobContext. `McuContext` does NOT enter
 * the Executor's job array, which stays at exactly `kJobCount`: the panel and
 * the MCU both run SERIALLY in the Scheduler, outside the Executor. It carries
 * no receive step and no transmit step either, because design section 13.4.6
 * states that "the MCU context has no such bracket and this block is the whole
 * of its quantum".
 *
 * THE FOUR MEMBERS ARE THE FOUR `g2::runQuantum` READS, with the exact types
 * cycleDebt.h names, which is what lets the ONE block of design section 13.4.6
 * be instantiated against this type and against `DspContext` without a common
 * base class. A member renamed or re-typed here is a compile error at the call
 * site rather than a second block that resembles the first.
 *
 * `rate` COMES FROM `Scheduler::Config::mcuRate` AND FROM NOWHERE ELSE, so a
 * test supplies its own pair without editing a shipped header -- which is the
 * reason section 4's measurement register gives for the rational being a
 * Config field at all.
 *
 * THE INDEX THIS CONTEXT ANSWERS TO IS 0. Design section 13.10.5 states that
 * `contextFaulted`, `contextFault`, `cycleDebt` and `longDispatchQuanta` take
 * a context index of 0 .. dspCount, that index 0 is the MCU and that indices
 * 1 .. dspCount are the DSPs.
 */

#pragma once

#include <cstdint>

#include "g2/timebase.h"

namespace g2
{
	struct McuContext
	{
		/* The MCU's cycles-for-each-frame rational, section 13.4.1. */
		Rational rate{};

		/* The rational accumulator, section 13.4.1. ZERO AT CONSTRUCTION AND
		 * NOT WRITTEN BY ANY OTHER SITE: runQuantum reads it before it ever
		 * writes one. */
		uint32_t acc = 0;

		/* The cycle debt, section 13.4.6. Signed, because the block carries a
		 * signed difference before it floors it at zero. */
		int64_t  debt = 0;

		/* Rule 4's counter: quanta in which the previous quantum had already
		 * overrun this quantum's whole budget. */
		uint64_t longDispatchQuanta = 0;
	};
}
