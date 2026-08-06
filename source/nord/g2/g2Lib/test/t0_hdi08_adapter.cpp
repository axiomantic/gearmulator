#include "hdi08Adapter.h"
#include <cassert>
#include <iostream>
#include <vector>

int main()
{
	g2::Hdi08Adapter adapter;

	// 1. Verify decode for eight expanded addresses and broadcast
	for (size_t i = 0; i < 8; ++i)
	{
		uint32_t a3a10 = (~(1u << i)) & 0xFFu;
		uint32_t addr = (a3a10 << 3);
		uint8_t mask = g2::Hdi08Adapter::decodePortMask(addr);
		assert(mask == (1u << i));
	}

	// Broadcast address (a3a10 == 0)
	uint8_t bcastMask = g2::Hdi08Adapter::decodePortMask(0x00u);
	assert(bcastMask == 0xFFu);

	// Unselected address (a3a10 == 0xFF)
	uint8_t noneMask = g2::Hdi08Adapter::decodePortMask(0xFFu << 3);
	assert(noneMask == 0x00u);

	// 2. Set writeTx callbacks on all 8 ports to capture transmitted 24-bit words
	std::vector<uint32_t> receivedWords[8];
	for (size_t i = 0; i < 8; ++i)
	{
		adapter.getPort(i).setWriteTxCallback([&receivedWords, i](uint32_t word) {
			receivedWords[i].push_back(word);
		});
	}

	// Test byte-at-a-time path to Port 0 (a3a10 = 0xFE, addr = 0xFE << 3 = 0x7F0)
	uint32_t port0Addr = (0xFEu << 3);
	adapter.write8(port0Addr + 5, 0x12u); // TXH
	adapter.write8(port0Addr + 6, 0x34u); // TXM
	adapter.write8(port0Addr + 7, 0x56u); // TXL -> triggers TX callback
	assert(receivedWords[0].size() == 1);
	assert(receivedWords[0][0] == 0x123456u);

	// Test longword store path to Port 1 (a3a10 = 0xFD, addr = 0xFD << 3 = 0x7E8)
	uint32_t port1Addr = (0xFDu << 3);
	// 32-bit store at addr + 4 writes bytes 4 (unused), 5 (TXH), 6 (TXM), 7 (TXL)
	adapter.write32(port1Addr + 4, 0x00ABCDEFu);
	assert(receivedWords[1].size() == 1);
	assert(receivedWords[1][0] == 0xABCDEFu);

	// Test broadcast write (addr = 0x00, 32-bit store at offset 4)
	adapter.write32(0x04u, 0x00778899u);
	for (size_t i = 0; i < 8; ++i)
	{
		assert(!receivedWords[i].empty());
		assert(receivedWords[i].back() == 0x778899u);
	}

	std::cout << "t0_hdi08_adapter passed" << std::endl;
	return 0;
}
