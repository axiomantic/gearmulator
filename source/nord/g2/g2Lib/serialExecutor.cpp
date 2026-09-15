/* The serial executor.
 *
 * It runs the jobs in order on the calling thread. There is no thread, queue,
 * mutex or condition variable here. A worker thread would make job completion
 * order depend on the host scheduler, which ends the bit-exactness claim this
 * design makes at the 96 kHz Q23 integer boundary.
 */

#include "executor.h"

namespace g2
{
	void SerialExecutor::run(const Job* const jobs, const size_t count) noexcept
	{
		/* run() is not RE-ENTRANT, and the REFUSAL is COUNTED RATHER THAN
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
