/* `McuContext` does not enter the Executor's job array, which stays at exactly
 * `kJobCount`: the panel and the MCU both run serially in the Scheduler,
 * outside the Executor. It carries no receive step and no transmit step: this
 * block is the whole of its quantum.
 *
 * The four members are the four `g2::runQuantum` reads, with the exact types
 * cycleDebt.h names, which is what lets the one block be instantiated against
 * this type and against `DspContext` without a common base class. A member
 * renamed or re-typed here is a compile error at the call site rather than a
 * second block that resembles the first.
 *
 * `rate` comes from `Scheduler::Config::mcuRate` and from nowhere else, so a
 * test supplies its own pair without editing a shipped header.
 *
 * The index this context answers to is 0. `contextFaulted`, `contextFault`,
 * `cycleDebt` and `longDispatchQuanta` take a context index of 0 .. dspCount:
 * index 0 is the MCU and indices 1 .. dspCount are the DSPs.
 */

#pragma once

#include <cstdint>

#include "g2/timebase.h"

namespace g2
{
	struct McuContext
	{
		/* The MCU's cycles-for-each-frame rational. */
		Rational rate{};

		/* The rational accumulator. Zero at construction and not written by any
		 * other site: runQuantum reads it before it ever writes one. */
		uint32_t acc = 0;

		/* Signed, because the block carries a signed difference before it
		 * floors it at zero. */
		int64_t  debt = 0;

		/* Quanta in which the previous quantum had already overrun this
		 * quantum's whole budget. */
		uint64_t longDispatchQuanta = 0;
	};
}
