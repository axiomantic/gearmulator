#include "isp1181.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
	using namespace hwLib;

	void require(const bool _condition, const char* _msg = "")
	{
		if (!_condition)
		{
			std::cerr << "FAIL: " << _msg << std::endl;
			std::abort();
		}
	}

	void writeCommand(Isp1181& _dev, const uint8_t _cmd)
	{
		_dev.write(0x10, _cmd);
	}

	void writeVia(Isp1181& _dev, const uint8_t _cmd, const std::vector<uint8_t>& _bytes)
	{
		_dev.write(0x10, _cmd);
		for (const uint8_t b : _bytes)
			_dev.write(0x00, b);
	}

	std::vector<uint8_t> readVia(Isp1181& _dev, const uint8_t _cmd, const size_t _width)
	{
		_dev.write(0x10, _cmd);
		std::vector<uint8_t> res;
		res.reserve(_width);
		for (size_t i = 0; i < _width; ++i)
			res.push_back(_dev.read(0x00));
		return res;
	}

	void testLifecycleAndBackend()
	{
		Isp1181 dev;
		require(dev.backend() == Isp1181::Backend::Stub, "starts in Stub backend");

		// Stub backend answers 0 on reads
		require(dev.read(0x00) == 0, "stub read answers 0");

		// Switching to full model succeeds
		const bool ok = dev.setBackend(Isp1181::Backend::FullModel);
		require(ok, "setBackend FullModel succeeded");
		require(dev.backend() == Isp1181::Backend::FullModel, "backend is FullModel");
	}

	void testRegisterReadWrite()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");

		// Mode register: 0xB8 write, 0xB9 read
		writeVia(dev, 0xB8, {0x42});
		require(dev.mode() == 0x42, "mode register set in object");
		const auto modeRead = readVia(dev, 0xB9, 1);
		require(modeRead.size() == 1 && modeRead[0] == 0x42, "read Mode register 0xB9 returns 0x42");

		// Device address: 0xB6 write, 0xB7 read
		writeVia(dev, 0xB6, {0x05});
		require(dev.deviceAddress() == 0x05, "device address set");
		const auto addrRead = readVia(dev, 0xB7, 1);
		require(addrRead.size() == 1 && addrRead[0] == 0x05, "read Device Address register 0xB7 returns 0x05");

		// Hardware config: 0xBA write, 0xBB read (2 bytes, LSB first)
		writeVia(dev, 0xBA, {0x34, 0x12});
		require(dev.hwConfig() == 0x1234, "hw config set");
		const auto hwRead = readVia(dev, 0xBB, 2);
		require(hwRead.size() == 2 && hwRead[0] == 0x34 && hwRead[1] == 0x12, "read HW Config 0xBB returns 0x1234");

		// Interrupt enable: 0xC2 write, 0xC3 read (4 bytes, LSB first)
		writeVia(dev, 0xC2, {0x00, 0x01, 0x00, 0x00}); // bit 8
		require(dev.interruptEnable() == 0x00000100u, "interrupt enable set");
		const auto ieRead = readVia(dev, 0xC3, 4);
		require(ieRead.size() == 4 && ieRead[0] == 0x00 && ieRead[1] == 0x01 && ieRead[2] == 0x00 && ieRead[3] == 0x00,
				"read Interrupt Enable register 0xC3 returns 0x00000100");

		// Interrupt register: 0xC0 read (4 bytes, LSB first)
		const auto irqRead = readVia(dev, 0xC0, 4);
		require(irqRead.size() == 4 && irqRead[0] == 0x00 && irqRead[1] == 0x00 && irqRead[2] == 0x00 && irqRead[3] == 0x00,
				"read Interrupt register 0xC0 initially zero");

		// Buffer status: 0x50 read (endpoint 0 OUT status)
		const auto st0 = readVia(dev, 0x50, 1);
		require(st0.size() == 1 && st0[0] == 0x00, "endpoint 0 OUT status is 0 when empty");
	}

	void testChipId()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");

		// Command 0xB5 reads Chip ID (2 bytes, LSB first).
		// For ISP1181A, Chip ID is 0x8141 -> LSB 0x41, MSB 0x81.
		const auto chipId = readVia(dev, 0xB5, 2);
		require(chipId.size() == 2, "chip ID read 2 bytes");
		require(chipId[0] == 0x41, "chip ID LSB is 0x41");
		require(chipId[1] == 0x81, "chip ID MSB is 0x81");
	}

	void testSetupPacketAndClearBuffer()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");

		// Arm interrupt enable for endpoint 0 OUT (bit 8 = 0x0100)
		writeVia(dev, 0xC2, {0x00, 0x01, 0x00, 0x00});

		const uint8_t setupPkt[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00}; // GET_DESCRIPTOR
		const int accepted = dev.setup(setupPkt, sizeof(setupPkt));
		require(accepted == 1, "setup packet accepted");

		// Verify interrupt bit 8 is raised
		require((dev.interruptRegister() & (1u << 8)) != 0, "EP0 OUT interrupt raised");
		require(dev.irqAsserted(), "IRQ line asserted");

		// Status read on EP0 OUT (0x50) should show SETUPT (0x04) and buffer full (0x20) -> 0x24
		// Reading status 0x50 also clears interrupt bit 8
		auto st = readVia(dev, 0x50, 1);
		require(st.size() == 1 && st[0] == 0x24, "status byte has SETUPT (0x04) and buffer full (0x20)");
		require((dev.interruptRegister() & (1u << 8)) == 0, "status read cleared interrupt bit 8");

		// Read the setup packet from buffer (0x10): 2-byte length prefix (0x08, 0x00) + 8 bytes payload
		const auto bufRead = readVia(dev, 0x10, 10);
		require(bufRead.size() == 10, "read 10 bytes from control OUT buffer");
		require(bufRead[0] == 8 && bufRead[1] == 0, "length prefix is 8 bytes");
		for (size_t i = 0; i < 8; ++i)
			require(bufRead[2 + i] == setupPkt[i], "setup payload matches");

		// Interlock: Clear Buffer (0x70) while setup is unacknowledged must be refused!
		writeCommand(dev, 0x70);
		// Status must still show buffer full and SETUPT
		st = readVia(dev, 0x50, 1);
		require(st.size() == 1 && st[0] == 0x24, "Clear Buffer while unacknowledged was refused; buffer still held");

		// Send Acknowledge Setup (0xF4)
		writeCommand(dev, 0xF4);

		// Now Clear Buffer (0x70) is allowed and drains the packet
		writeCommand(dev, 0x70);

		// Buffer is now empty and SETUPT retired
		st = readVia(dev, 0x50, 1);
		require(st.size() == 1 && st[0] == 0x00, "buffer is empty and SETUPT retired");
	}

	void testInTokenAndPacketBuffers()
	{
		std::vector<uint8_t> captured;
		int txEp = -1;
		Isp1181 dev(nullptr, [&captured, &txEp](const int _ep, const uint8_t* const _data, const size_t _len) {
			txEp = _ep;
			if (_data && _len > 0)
				captured.assign(_data, _data + _len);
			else
				captured.clear();
		});
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");

		// Arm interrupt enable for EP0 IN (bit 9 = 0x0200) and EP1 (bit 10 = 0x0400)
		writeVia(dev, 0xC2, {0x00, 0x06, 0x00, 0x00});

		// 1. Control IN (Endpoint 0 IN): up to 64 bytes
		std::vector<uint8_t> ep0Payload(64);
		for (size_t i = 0; i < 64; ++i)
			ep0Payload[i] = static_cast<uint8_t>(i ^ 0x5A);

		std::vector<uint8_t> write0;
		write0.push_back(64);
		write0.push_back(0);
		write0.insert(write0.end(), ep0Payload.begin(), ep0Payload.end());
		writeVia(dev, 0x01, write0); // 0x01 = write control IN buffer
		writeCommand(dev, 0x61);     // 0x61 = validate control IN buffer

		// IN token on EP0 transmits the 64-byte packet
		uint8_t capBuf[64];
		size_t actualLen = 0;
		int res = dev.inToken(0, capBuf, sizeof(capBuf), &actualLen);
		require(res == 1, "inToken on EP0 returned 1");
		require(actualLen == 64, "actualLen is 64");
		require(txEp == 0, "tx callback received EP 0");
		require(captured == ep0Payload, "captured bytes match 64-byte payload");
		require((dev.interruptRegister() & (1u << 9)) != 0, "EP0 IN interrupt bit 9 raised after transmit");

		// Second inToken on EP0 returns NAK (0) because FIFO is empty
		res = dev.inToken(0);
		require(res == 0, "second inToken returns 0 (NAK)");

		// 2. Endpoint 1 (configured IN, 16 bytes max packet)
		// Configure slot 2 (0x22): 0xC1 = enabled (0x80) | EPDIR=IN (0x40) | size 16 (0x01)
		writeVia(dev, 0x22, {0xC1});

		const std::vector<uint8_t> ep1Payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
		std::vector<uint8_t> write1;
		write1.push_back(16);
		write1.push_back(0);
		write1.insert(write1.end(), ep1Payload.begin(), ep1Payload.end());
		writeVia(dev, 0x02, write1); // 0x02 = write endpoint 1 buffer
		writeCommand(dev, 0x62);     // 0x62 = validate endpoint 1 buffer

		captured.clear();
		txEp = -1;
		res = dev.inToken(1, capBuf, sizeof(capBuf), &actualLen);
		require(res == 1, "inToken on EP1 returned 1");
		require(actualLen == 16, "actualLen is 16");
		require(txEp == 1, "tx callback received EP 1");
		require(captured == ep1Payload, "captured bytes match 16-byte payload");
		require((dev.interruptRegister() & (1u << 10)) != 0, "EP1 interrupt bit 10 raised after transmit");
	}

	void testSofFrameTickAdvancement()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");

		require(dev.frameNumber() == 0, "initial frame number is 0");
		const auto fn0 = readVia(dev, 0xB4, 2);
		require(fn0[0] == 0 && fn0[1] == 0, "frame register reads 0");

		dev.tick(1);
		require(dev.frameNumber() == 1, "frame number is 1 after 1 tick");
		const auto fn1 = readVia(dev, 0xB4, 2);
		require(fn1[0] == 1 && fn1[1] == 0, "frame register reads 1");

		dev.tick(500);
		require(dev.frameNumber() == 501, "frame number is 501 after +500 ticks");

		// USB frame numbers wrap modulo 2048 (0..2047)
		dev.tick(1600); // 501 + 1600 = 2101 -> 2101 % 2048 = 53
		require(dev.frameNumber() == 53, "frame number wraps at 2048");
		const auto fnWrap = readVia(dev, 0xB4, 2);
		require(fnWrap[0] == 53 && fnWrap[1] == 0, "frame register reads 53");
	}

	void testBufferAndConfigInspection()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set");

		const size_t slots = Isp1181::getConfigSlots();
		require(slots > 0, "config slots > 0");

		uint8_t slotVal = 0;
		const int slotRes = dev.getConfigSlot(0, slotVal);
		require(slotRes >= 0, "slot 0 config queryable");

		size_t maxPacket = 0;
		size_t bufferCount = 0;
		const int bufRes = dev.getSlotBuffer(0, maxPacket, bufferCount);
		require(bufRes == 1, "slot 0 buffer query returns 1");
		require(maxPacket == 64, "control endpoint buffer is 64 bytes");
	}

	void testStateSaveRestore()
	{
		Isp1181 dev;
		require(dev.setBackend(Isp1181::Backend::FullModel), "backend set to FullModel");
		dev.tick(1234);
		require(dev.frameNumber() == 1234, "frame number set to 1234");

		const size_t stateSize = Isp1181::getStateSize();
		require(stateSize == 19, "state size is 19 bytes");

		std::vector<uint8_t> stateBuf(stateSize, 0);
		dev.saveState(stateBuf.data());

		// Check header
		require(stateBuf[0] == 'I' && stateBuf[1] == 'S' && stateBuf[2] == 'P' && stateBuf[3] == '1', "magic ISP1");
		require(stateBuf[7] == 1, "version 1");
		require(stateBuf[11] == 3, "payload length 3");
		require(stateBuf[12] == static_cast<uint8_t>(Isp1181::Backend::FullModel), "backend preserved");
		require(((uint16_t(stateBuf[13]) << 8) | stateBuf[14]) == 1234, "frame number preserved");

		// Restore into a fresh device
		Isp1181 dev2;
		require(dev2.backend() == Isp1181::Backend::Stub, "dev2 starts in Stub mode");
		require(dev2.frameNumber() == 0, "dev2 starts with frame 0");

		dev2.loadState(stateBuf.data());
		require(dev2.backend() == Isp1181::Backend::FullModel, "dev2 restored backend FullModel");
		require(dev2.frameNumber() == 1234, "dev2 restored frame number 1234");

		// Corrupt checksum and verify loadState rejects it
		stateBuf[18] ^= 0xFF;
		Isp1181 dev3;
		dev3.loadState(stateBuf.data());
		require(dev3.backend() == Isp1181::Backend::Stub, "corrupted checksum rejected; stays Stub");
	}
} // namespace

int main()
{
	testLifecycleAndBackend();
	testRegisterReadWrite();
	testChipId();
	testSetupPacketAndClearBuffer();
	testInTokenAndPacketBuffers();
	testSofFrameTickAdvancement();
	testBufferAndConfigInspection();
	testStateSaveRestore();

	std::cout << "hwLib::Isp1181 tests passed." << std::endl;
	return 0;
}
