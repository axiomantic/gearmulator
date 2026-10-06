#include "timer.h"

namespace rg2
{
    Timer::Timer(const int _interruptIndex, InterruptController* _interrupts) :
        m_interruptIndex(_interruptIndex), m_interrupts(_interrupts)
    {
        cf_runtime_init();
        cf_config cfg{CF_ISA_A, 0xFFFFFFFFu, nullptr, nullptr, nullptr, nullptr};
        m_ownedCtx = cf_create(&cfg);
        cf_reset(m_ownedCtx, 0, 0);

        if (m_interrupts)
        {
            m_interrupts->attachContext(m_ownedCtx);
        }
    }

    Timer::Timer(cf_ctx*& _ctx, const int _interruptIndex) : m_interruptIndex(_interruptIndex), m_ctxPtr(&_ctx) {}

    Timer::~Timer()
    {
        if (m_ownedCtx)
        {
            cf_destroy(m_ownedCtx);
            m_ownedCtx = nullptr;
        }
    }

    cf_ctx* Timer::activeCtx() const
    {
        if (m_ctxPtr && *m_ctxPtr)
            return *m_ctxPtr;
        return m_ownedCtx;
    }

    uint32_t Timer::baseAddress() const
    {
        return (m_interruptIndex == gTimer2InterruptIndex) ? gTimer2Base : gTimer1Base;
    }

    void Timer::setInterruptController(InterruptController* _interrupts)
    {
        m_interrupts = _interrupts;
        if (m_interrupts && activeCtx())
        {
            m_interrupts->attachContext(activeCtx());
        }
    }

    uint16_t Timer::tmr() const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint16_t(cf_mbar_read(ctx, baseAddress() + gTmrOffset, 2, &st) & 0xFFFFu);
    }

    uint16_t Timer::trr() const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint16_t(cf_mbar_read(ctx, baseAddress() + gTrrOffset, 2, &st) & 0xFFFFu);
    }

    uint16_t Timer::tcr() const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint16_t(cf_mbar_read(ctx, baseAddress() + gTcrOffset, 2, &st) & 0xFFFFu);
    }

    uint16_t Timer::tcn() const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint16_t(cf_mbar_read(ctx, baseAddress() + gTcnOffset, 2, &st) & 0xFFFFu);
    }

    uint8_t Timer::ter() const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint8_t(cf_mbar_read(ctx, baseAddress() + gTerOffset, 1, &st) & 0xFFu);
    }

    void Timer::writeTmr(const uint16_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_bus_status st = CF_BUS_OK;
        cf_mbar_write(ctx, baseAddress() + gTmrOffset, 2, _value, &st);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }

    void Timer::writeTrr(const uint16_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_bus_status st = CF_BUS_OK;
        cf_mbar_write(ctx, baseAddress() + gTrrOffset, 2, _value, &st);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }

    void Timer::writeTcn(const uint16_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_bus_status st = CF_BUS_OK;
        cf_mbar_write(ctx, baseAddress() + gTcnOffset, 2, _value, &st);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }

    void Timer::writeTer(const uint8_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_bus_status st = CF_BUS_OK;
        cf_mbar_write(ctx, baseAddress() + gTerOffset, 1, _value, &st);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }

    bool Timer::coversByte(const uint32_t _blockOffset)
    {
        switch (_blockOffset)
        {
        case gTmrOffset:
        case gTmrOffset + 1:
        case gTrrOffset:
        case gTrrOffset + 1:
        case gTcrOffset:
        case gTcrOffset + 1:
        case gTcnOffset:
        case gTcnOffset + 1:
        case gTerOffset:
            return true;
        default:
            return false;
        }
    }

    uint8_t Timer::readByte(const uint32_t _blockOffset) const
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return 0;
        cf_bus_status st = CF_BUS_OK;
        return uint8_t(cf_mbar_read(ctx, baseAddress() + _blockOffset, 1, &st) & 0xFFu);
    }

    void Timer::writeByte(const uint32_t _blockOffset, const uint8_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_bus_status st = CF_BUS_OK;
        cf_mbar_write(ctx, baseAddress() + _blockOffset, 1, _value, &st);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }

    void Timer::advance(const uint32_t _inputClocks)
    {
        cf_ctx* ctx = activeCtx();
        if (!ctx)
            return;
        cf_timer_tick(ctx, _inputClocks);
        if (m_interrupts)
            m_interrupts->notifyPresent();
    }
} // namespace rg2
