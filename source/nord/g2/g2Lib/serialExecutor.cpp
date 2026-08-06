/* serialExecutor.cpp -- the serial executor. Task SCH-7.
 * Design sections 13.3 and 13.10.3.
 *
 * IT RUNS THE JOBS IN ORDER ON THE CALLING THREAD. There is no thread here, no
 * queue, no mutex and no condition variable, and neither <thread> nor
 * std::thread is named in this file. A worker thread would make job completion
 * order depend on the host scheduler, which ends the bit-exactness claim this
 * design makes at the 96 kHz Q23 integer boundary.
 */

#include "executor.h"

namespace g2
{
	void SerialExecutor::run(const Job* const jobs, const size_t count) noexcept
	{
		/* run() IS NOT RE-ENTRANT, AND THE REFUSAL IS COUNTED RATHER THAN
		 * ASSERTED. A release build removes an assert(); it does not remove
		 * this. See the header for the measurement that decided it. */
		if(m_depth != 0)
		{
			++m_reentries;
			return;
		}

		++m_depth;

		for(size_t i = 0; i < count; ++i)
			jobs[i].fn(jobs[i].ctx);

		--m_depth;
	}

	bool SerialExecutor::isSerial() const noexcept
	{
		return true;
	}

	uint32_t SerialExecutor::depth() const noexcept
	{
		return m_depth;
	}

	uint64_t SerialExecutor::reentryCount() const noexcept
	{
		return m_reentries;
	}
}
