#pragma once

#include <cstdint>
#include <vector>

namespace rg2
{
	class Board;

	// Maps audio chain positions to hardware HDI08 ports by reading the booted firmware table.
	// Returns number of positions resolved (returns 0 if table not yet built).
	unsigned readChainOrder(Board& _board, unsigned _count, std::vector<unsigned>& _portOfPosition);
} // namespace rg2
