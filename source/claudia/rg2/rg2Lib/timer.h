#pragma once

#include <coldfire.h>
#include <cstdint>
#include "interruptController.h"

namespace rg2
{
    class Timer final
    {
    public:
        static constexpr uint32_t gTimer1Base = 0x140u;
        static constexpr uint32_t gTimer2Base = 0x180u;

        static constexpr uint32_t gTmrOffset = 0x00u;
        static constexpr uint32_t gTrrOffset = 0x04u;
        static constexpr uint32_t gTcrOffset = 0x08u;
        static constexpr uint32_t gTcnOffset = 0x0Cu;
        static constexpr uint32_t gTerOffset = 0x11u;
        static constexpr uint32_t gBlockSize = 0x12u;

        static constexpr int gTimer1InterruptIndex = 1;
        static constexpr int gTimer2InterruptIndex = 2;

        static constexpr uint16_t gTmrRst = 0x0001u;
        static constexpr uint16_t gTmrFrr = 0x0008u;
        static constexpr uint16_t gTmrOri = 0x0010u;
        static constexpr int gTmrPrescalerShift = 8;

        static constexpr uint8_t gTerCap = 0x01u;
        static constexpr uint8_t gTerRef = 0x02u;

        explicit Timer(int _interruptIndex = gTimer1InterruptIndex, InterruptController* _interrupts = nullptr);
        explicit Timer(cf_ctx*& _ctx, int _interruptIndex = gTimer1InterruptIndex);
        ~Timer();

        void setInterruptController(InterruptController* _interrupts);

        uint16_t tmr() const;
        uint16_t trr() const;
        uint16_t tcr() const;
        uint16_t tcn() const;
        uint8_t ter() const;

        void writeTmr(uint16_t _value);
        void writeTrr(uint16_t _value);
        void writeTcn(uint16_t _value);
        void writeTer(uint8_t _value);

        static bool coversByte(uint32_t _blockOffset);

        uint8_t readByte(uint32_t _blockOffset) const;
        void writeByte(uint32_t _blockOffset, uint8_t _value);

        void advance(uint32_t _inputClocks);

    private:
        cf_ctx* activeCtx() const;
        uint32_t baseAddress() const;

        int m_interruptIndex;
        cf_ctx** m_ctxPtr = nullptr;
        cf_ctx* m_ownedCtx = nullptr;
        InterruptController* m_interrupts = nullptr;
    };
} // namespace rg2

namespace coldfire
{
    using Timer = rg2::Timer;
}
