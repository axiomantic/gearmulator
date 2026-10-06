#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <coldfire.h>
#include "busTarget.h"
#include "interruptController.h"

namespace rg2
{
    class Uart0 final : public BusTarget
    {
    public:
        static constexpr uint32_t gUart0Base = 0x1C0u;
        static constexpr uint32_t gUart1Base = 0x200u;
        static constexpr uint32_t gUartModuleSize = 0x40u;

        static constexpr uint8_t gUart0Vector = 0x42u;
        static constexpr int gUart0InterruptIndex = 4;

        static constexpr uint16_t gBaudDivider = 0x0036u;

        static constexpr uint8_t gUmr18n1 = 0x0Bu;
        static constexpr uint8_t gUmr28n1 = 0x07u;

        using MidiOutFn = void (*)(void* _user, uint8_t _byte);

        explicit Uart0(InterruptController* _interrupts = nullptr);
        explicit Uart0(cf_ctx*& _ctx);
        ~Uart0();

        void setInterruptController(InterruptController* _interrupts);

        uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
        void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

        void setMidiOut(MidiOutFn _fn, void* _user);
        void receive(uint8_t _byte);
        void transmitComplete();

        uint8_t usr() const;
        bool interruptAsserted() const;

        uint8_t uivr() const;

        const std::vector<std::string>& log() const { return m_log; }
        void clearLog() { m_log.clear(); }

    private:
        cf_ctx* activeCtx() const;
        void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

        static void onTxTrampoline(void* _user, int _channel, uint8_t _byte);

        cf_ctx** m_ctxPtr = nullptr;
        cf_ctx* m_ownedCtx = nullptr;
        InterruptController* m_interrupts = nullptr;

        MidiOutFn m_midiOut = nullptr;
        void* m_midiOutUser = nullptr;

        std::vector<std::string> m_log;
        bool m_txHoldingValid = false;
    };
} // namespace rg2

namespace coldfire
{
    using Uart0 = rg2::Uart0;
}
