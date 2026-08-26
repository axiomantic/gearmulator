/* status.h -- g2::Status, the type the state surfaces and Scheduler::create
 * report through. */

#pragma once

#include <cstdint>

namespace g2
{
	/* SCOPED, over a fixed underlying type. An unscoped enumeration converts to
	 * int and through int to bool, so a caller writing the natural if(st) would
	 * read every failure as true and the one success value as false.
	 *
	 * Ok IS NOT THE ZERO VALUE, and Unset is. create() reports through an
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

		/* A STATE IMAGE THIS OBJECT WILL NOT TAKE BACK. The version word does
		 * not match the one this build writes, or the image's own geometry
		 * header describes a differently-shaped object. Both are refused
		 * BEFORE the first write, so a refused load changes nothing.
		 *
		 * IT IS ONE VALUE AND NOT TWO because a caller can act on neither
		 * differently: an image this build cannot read is an image this build
		 * cannot read. The refusing object's own comment carries which of the
		 * two conditions it tested. */
		BadStateImage,

		/* A CHAIN ORDER THAT IS NOT ONE. attachChainCallbacks takes the
		 * position-to-port map and refuses a map whose length is not the slot
		 * count, or that names a slot twice or names one that does not exist.
		 * Such a map would leave a DSP unwired while two chain positions drove
		 * another, which nothing downstream reports. */
		BadChainOrder,

		Count
	};
}
