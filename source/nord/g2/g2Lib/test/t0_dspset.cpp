#include <cassert>
#include <iostream>
#include <vector>

#include "dspSet.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/peripherals56311.h"

int main()
{
	using namespace g2;

	// Test 1: Lifecycle and instance count (default 8 instances)
	DspSet dspSet;
	assert(dspSet.count() == 8);

	for (size_t i = 0; i < dspSet.count(); ++i)
	{
		auto& dsp = dspSet.getDsp(i);
		auto& periph = dspSet.getPeripherals(i);
		auto& mem = dspSet.getMemory(i);

		assert(periph.getType() == dsp56k::PeripheralType::Peripherals56311);

		// Arm one DMA channel on each instance
		periph.getDMA().setDCR(0, (0b01011 << 11));
		assert(periph.getDMA().getDCR(0) == (0b01011 << 11));
	}

	// Test 2: Custom instance count
	DspSet dspSet3(3);
	assert(dspSet3.count() == 3);

	// Test 3: State size, save and load
	const size_t size = dspSet.stateSize();
	assert(size > 0);

	std::vector<uint8_t> buffer(size);

	// Test BadSize handling
	assert(dspSet.stateSave(buffer.data(), size - 1) == Status::BadSize);
	assert(dspSet.stateLoad(buffer.data(), size - 1) == Status::BadSize);

	// Test successful state save and load round-trip
	assert(dspSet.stateSave(buffer.data(), size) == Status::Ok);
	assert(dspSet.stateLoad(buffer.data(), size) == Status::Ok);

	std::cout << "t0_dspset passed successfully." << std::endl;
	return 0;
}
