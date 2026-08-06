#include <cassert>
#include <iostream>
#include "interruptController.h"

int main()
{
	using namespace g2;

	InterruptController ic;

	// Initially all masked and no pending IRQ
	auto res = ic.evaluate();
	assert(res.level == MCF5307_IRQ_NONE);
	assert(res.winningSourceId == -1);

	// Configure Source 1: Level 3, Priority 1, Vector 64
	ic.setSourceConfig(1, 3, 1, 64, false);
	ic.setMasked(1, false); // Unmask
	ic.setPending(1, true);

	res = ic.evaluate();
	assert(res.level == 3);
	assert(res.vector == 64);
	assert(!res.autovector);
	assert(res.winningSourceId == 1);

	// Configure Source 2: Level 3, Priority 2 (Higher sub-priority), Vector 65
	ic.setSourceConfig(2, 3, 2, 65, false);
	ic.setMasked(2, false);
	ic.setPending(2, true);

	// Tier 2 test: Higher sub-priority within same IPL 3 wins
	res = ic.evaluate();
	assert(res.level == 3);
	assert(res.vector == 65);
	assert(res.winningSourceId == 2);

	// Configure Source 3: Level 5 (Higher IPL), Priority 0, Autovector
	ic.setSourceConfig(3, 5, 0, 0, true);
	ic.setMasked(3, false);
	ic.setPending(3, true);

	// Tier 1 test: Higher IPL 5 wins over IPL 3
	res = ic.evaluate();
	assert(res.level == 5);
	assert(res.autovector);
	assert(res.winningSourceId == 3);

	// Mask Source 3
	ic.setMasked(3, true);

	// Source 2 (Level 3, Priority 2) should win again
	res = ic.evaluate();
	assert(res.level == 3);
	assert(res.winningSourceId == 2);

	// Test IACK clears pending for winning source
	ic.handleIack(3, 65);
	assert(!ic.isPending(2));

	// Now Source 1 (Level 3, Priority 1) wins
	res = ic.evaluate();
	assert(res.level == 3);
	assert(res.winningSourceId == 1);

	// Clear pending Source 1
	ic.setPending(1, false);
	res = ic.evaluate();
	assert(res.level == MCF5307_IRQ_NONE);

	std::cout << "t0_interrupt_controller passed successfully." << std::endl;
	return 0;
}
