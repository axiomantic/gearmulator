/* status.h -- g2::Status, the type the state surfaces and Scheduler::create
 * report through. */

#pragma once

#include <cstdint>

namespace g2
{
	/* Scoped, over a fixed underlying type. An unscoped enumeration converts to
	 * int and through int to bool, so a caller writing the natural if(st) would
	 * read every failure as true and the one success value as false.
	 *
	 * Ok is NOT the zero value, and Unset is. create() reports through an
	 * out-param the CALLER declares; a caller writes `g2::Status st{};` and a
	 * path that returns without writing it leaves the zero value in place. If
	 * Ok were zero, a status nobody ever wrote would read as success.
	 *
	 * Count is a roster terminator and is never a reported status. */
	enum class Status : uint32_t
	{
		Unset = 0,
		Ok,
		BadDspCount,
		BadFramesPerQuantum,
		BadBackend,
		BadHopFrames,
		BadRational,
		BadLookahead,
		BadDivider,
		BadMaxHostBlock,
		BridgesAttached,
		Count
	};
}
