// t0_board_runmcu.cpp — BRD-21 unit test
//
// Verifies that Board::runMcu is a safe no-op when no ROM is loaded.
// When G2_LINK_MCF5307 is ON the MCF5307 reset vector is 0xFFFFFFFF (erased
// flash), so the CPU will fault on the first instruction fetch.  In either
// case runMcu() must return without crashing.

#include "board.h"
#include <cassert>
#include <iostream>

int main()
{
	// Construct a board with tiny flash regions — no ROM loaded.
	// cs0Base = 0x30000000, cs0Size = 64KB; cs2Base = 0x20000000, cs2Size = 64KB
	g2::Board board(0x30000000u, 0x00010000u, 0x20000000u, 0x00010000u);

	// runMcu with zero cycles must return 0 and must not crash.
	const uint32_t cycles0 = board.runMcu(0u);
	assert(cycles0 == 0u);

	// runMcu with a non-zero cycle count must also not crash.
	// When the MCF5307 is linked and a bad reset vector is present, the core
	// faults immediately and returns 0; when not linked it returns 0 directly.
	const uint32_t cycles1 = board.runMcu(1u);
	(void)cycles1; // may be 0 (no-op or fault) or >0 (valid execution)

	// After the above calls faulted() must be queryable without crashing.
	(void)board.faulted();

	// Accessor smoke-tests — must not crash or throw.
	(void)board.mcfContext();
	(void)board.flash().cs0Base();
	(void)board.hdi08().getPort(0);

	std::cout << "t0_board_runmcu passed" << std::endl;
	return 0;
}
