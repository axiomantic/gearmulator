#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <coldfire.h>
#include "busTarget.h"
#include "model.h"
#include "timer.h"

namespace rg2
{
    class InterruptController;

    constexpr uint32_t g_simSpaceSize = 0x400u;
    constexpr uint32_t g_simUipcrOffset = 0x1D0u;

    class Sim final : public BusTarget
    {
    public:
        using PortAReadHook = std::function<uint16_t()>;

        explicit Sim(bool _engineStrap = false);
        explicit Sim(Model _model);
        explicit Sim(cf_ctx*& _ctx);
        ~Sim();

        uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
        void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

        const std::vector<std::string>& log() const { return m_log; }
        void clearLog() { m_log.clear(); }

        Timer& timer1() { return m_timer1; }
        Timer& timer2() { return m_timer2; }

        void advanceTimers(uint32_t _inputClocks);
        void setInterruptController(InterruptController* _interrupts);

        void setPortAReadHook(PortAReadHook _hook);

        template <typename PanelType> void setPanel(PanelType* _panel)
        {
            if (_panel)
                setPortAReadHook([_panel]() { return _panel->getRowBits(); });
            else
                setPortAReadHook(nullptr);
        }

    private:
        void initRegisters(bool _engineStrap);
        cf_ctx* activeCtx() const;
        void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);
        static uint16_t onPortATrampoline(void* _user);

        Timer* timerForByte(uint32_t _index, uint32_t& _blockOffset);

        cf_ctx** m_ctxPtr = nullptr;
        cf_ctx* m_ownedCtx = nullptr;
        InterruptController* m_interrupts = nullptr;

        uint8_t m_space[g_simSpaceSize] = {};
        uint8_t m_writeProtect[g_simSpaceSize] = {};

        Timer m_timer1{Timer::gTimer1InterruptIndex};
        Timer m_timer2{Timer::gTimer2InterruptIndex};

        PortAReadHook m_portAReadHook;
        std::vector<std::string> m_log;
    };
} // namespace rg2

namespace coldfire
{
    using Sim = rg2::Sim;
    inline constexpr uint32_t g_simSpaceSize = rg2::g_simSpaceSize;
    inline constexpr uint32_t g_simUipcrOffset = rg2::g_simUipcrOffset;
} // namespace coldfire
