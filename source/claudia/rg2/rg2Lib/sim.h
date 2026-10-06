#pragma once

#include "cpu/coldfire/sim.h"
#include "model.h"

namespace rg2
{
	using Sim = coldfire::Sim;
	inline constexpr uint32_t g_simSpaceSize = coldfire::g_simSpaceSize;
	inline constexpr uint32_t g_simUipcrOffset = coldfire::g_simUipcrOffset;
}
