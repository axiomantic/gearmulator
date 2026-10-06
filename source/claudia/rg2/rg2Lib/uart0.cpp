#include "uart0.h"

namespace rg2
{
    Uart0::Uart0(InterruptController* _interrupts) : m_interrupts(_interrupts)
    {
        cf_runtime_init();
        cf_config cfg{CF_ISA_A, 0xFFFFFFFFu, nullptr, nullptr, nullptr, nullptr};
        m_ownedCtx = cf_create(&cfg);
        cf_reset(m_ownedCtx, 0, 0);

        if (m_interrupts)
        {
            m_interrupts->setInternalVector(gUart0InterruptIndex, gUart0Vector);
            m_interrupts->attachContext(m_ownedCtx);
        }
    }

    Uart0::Uart0(cf_ctx*& _ctx) : m_ctxPtr(&_ctx) {}

    Uart0::~Uart0()
    {
        if (m_ownedCtx)
        {
            cf_destroy(m_ownedCtx);
            m_ownedCtx = nullptr;
        }
    }

    cf_ctx* Uart0::activeCtx() const
    {
        if (m_ctxPtr && *m_ctxPtr)
            return *m_ctxPtr;
        return m_ownedCtx;
    }

    void Uart0::setInterruptController(InterruptController* _interrupts)
    {
        m_interrupts = _interrupts;
        if (m_interrupts)
        {
            m_interrupts->setInternalVector(gUart0InterruptIndex, gUart0Vector);
            if (activeCtx())
            {
                m_interrupts->attachContext(activeCtx());
            }
        }
    }

    void Uart0::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
    {
        m_log.push_back(std::string(_reason) + ": " + (_isWrite ? "write" : "read") + " size=" + std::to_string(_size) +
                        " offset=" + std::to_string(_offset));
    }

    uint32_t Uart0::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
    {
        if (_offset < gUart0Base || _offset >= gUart1Base + gUartModuleSize)
        {
            _status = CF_BUS_UNMAPPED;
            logLine("UNMAPPED", false, _size, _offset);
            return 0;
        }

        if (_size != 8)
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", false, _size, _offset);
            return 0;
        }

        if (_offset >= gUart1Base && _offset < gUart1Base + gUartModuleSize)
        {
            _status = CF_BUS_OK;
            const uint32_t local = _offset - gUart1Base;
            if (local == 0x30u)
                return 0x0Fu;
            if (local == 0x10u)
                return 0x0Eu;
            return 0x00u;
        }

        const uint32_t local = _offset - gUart0Base;
        if (local == 0x18u || local == 0x1Cu)
        {
            _status = CF_BUS_OK;
            return 0x00u;
        }

        cf_ctx* ctx = activeCtx();
        if (!ctx)
        {
            _status = CF_BUS_FAULT;
            return 0;
        }

        _status = CF_BUS_OK;
        uint32_t val = cf_mbar_read(ctx, _offset, 1, &_status);
        if (_status == CF_BUS_SIZE_ILLEGAL)
        {
            logLine("SIZE_ILLEGAL", false, _size, _offset);
        }

        if (local == 0x14u)
        {
            if (val & 0x04u)
                val |= 0x02u;
        }

        if (m_interrupts)
        {
            m_interrupts->notifyPresent();
        }

        return val;
    }

    void Uart0::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
    {
        if (_offset < gUart0Base || _offset >= gUart1Base + gUartModuleSize)
        {
            _status = CF_BUS_UNMAPPED;
            logLine("UNMAPPED", true, _size, _offset);
            return;
        }

        if (_size != 8)
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", true, _size, _offset);
            return;
        }

        if (_offset >= gUart1Base && _offset < gUart1Base + gUartModuleSize)
        {
            _status = CF_BUS_OK;
            return;
        }

        const uint32_t local = _offset - gUart0Base;
        uint32_t val = _value;
        if (local == 0x14u)
        {
            if (val & 0x02u)
                val |= 0x04u;
        }
        else if (local == 0x0Cu)
        {
            m_txHoldingValid = true;
        }
        else if (local == 0x30u)
        {
            if (m_interrupts)
                m_interrupts->setInternalVector(gUart0InterruptIndex, uint8_t(val & 0xFFu));
        }

        cf_ctx* ctx = activeCtx();
        if (!ctx)
        {
            _status = CF_BUS_FAULT;
            return;
        }

        _status = CF_BUS_OK;
        cf_mbar_write(ctx, _offset, 1, val, &_status);
        if (_status == CF_BUS_SIZE_ILLEGAL)
        {
            logLine("SIZE_ILLEGAL", true, _size, _offset);
        }

        if (m_interrupts)
        {
            m_interrupts->notifyPresent();
        }
    }

    void Uart0::onTxTrampoline(void* _user, const int, const uint8_t _byte)
    {
        auto* u = static_cast<Uart0*>(_user);
        if (u && u->m_midiOut)
        {
            u->m_midiOut(u->m_midiOutUser, _byte);
        }
    }

    void Uart0::setMidiOut(MidiOutFn _fn, void* _user)
    {
        m_midiOut = _fn;
        m_midiOutUser = _user;
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_uart_set_tx_handler(ctx, CF_UART_CH0, &Uart0::onTxTrampoline, this);
        }
    }

    void Uart0::receive(const uint8_t _byte)
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_uart_rx_byte(ctx, CF_UART_CH0, _byte);
            if (m_interrupts)
            {
                m_interrupts->notifyPresent();
            }
        }
    }

    void Uart0::transmitComplete() { m_txHoldingValid = false; }

    uint8_t Uart0::usr() const
    {
        cf_ctx* ctx = activeCtx();
        uint8_t val = 0;
        if (ctx)
        {
            val = cf_uart_get_usr(ctx, CF_UART_CH0);
        }
        if (m_txHoldingValid)
        {
            val &= ~0x04u;
            val &= ~0x08u;
        }
        else
        {
            val |= 0x08u;
        }
        return val;
    }

    bool Uart0::interruptAsserted() const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            if (cf_intc_get_presented_level(ctx) > 0)
            {
                const uint8_t vec = m_interrupts ? m_interrupts->presentedVector() : cf_intc_get_presented_vector(ctx);
                return vec == gUart0Vector || (vec == 0x0Fu && cf_intc_get_presented_autovector(ctx) == 0);
            }
        }
        return false;
    }

    uint8_t Uart0::uivr() const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_bus_status st = CF_BUS_OK;
            return uint8_t(cf_mbar_read(ctx, gUart0Base + 0x30, 1, &st) & 0xFFu);
        }
        return 0x0Fu;
    }
} // namespace rg2
