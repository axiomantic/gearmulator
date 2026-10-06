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

    void testLifecycleAndBackend()
    {
        Isp1181 dev;
        require(dev.rawContext() != nullptr, "device context created");

        // Stub backend answers 0 on reads
        require(dev.read(0x00) == 0, "stub read answers 0");

        // Switching to full model succeeds
        const bool ok = dev.setBackend(Isp1181::Backend::FullModel);
        require(ok, "setBackend FullModel succeeded");
    }

    void testCallbacksAndTokens()
    {
        bool irqFired = false;
        bool txFired = false;
        int txEndpoint = -1;
        std::vector<uint8_t> txBytes;

        Isp1181 dev([&irqFired](bool _asserted) { irqFired = _asserted; },
                    [&txFired, &txEndpoint, &txBytes](int _ep, const uint8_t* _data, size_t _len)
                    {
                        txFired = true;
                        txEndpoint = _ep;
                        if (_data && _len > 0)
                            txBytes.assign(_data, _data + _len);
                    });

        require(dev.setBackend(Isp1181::Backend::FullModel), "full model initialized");

        // IN token on empty endpoint returns 0 (NAK) and does not fire TX callback
        const int nak = dev.inToken(0);
        require(nak == 0, "inToken on empty endpoint returns 0");
        require(!txFired, "tx callback did not fire on NAK");

        // Tick advances SOF
        dev.tick(1);
        dev.tick(10);
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

    void testStateSaveLoad()
    {
        Isp1181 dev;
        require(dev.setBackend(Isp1181::Backend::FullModel), "backend set");

        const size_t stateSize = Isp1181::getStateSize();
        require(stateSize > 0, "state size > 0");

        std::vector<uint8_t> stateBuf(stateSize, 0);
        dev.saveState(stateBuf.data());

        Isp1181 dev2;
        dev2.loadState(stateBuf.data());
    }
} // namespace

int main()
{
    testLifecycleAndBackend();
    testCallbacksAndTokens();
    testBufferAndConfigInspection();
    testStateSaveLoad();

    std::cout << "hwLib::Isp1181 tests passed." << std::endl;
    return 0;
}
