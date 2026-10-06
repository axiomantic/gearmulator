#include "sim.h"
#include "interruptController.h"

namespace rg2
{
    namespace
    {
        enum class Access
        {
            ReadWrite,
            ReadOnly
        };

        struct RegisterSpec
        {
            uint32_t offset;
            uint32_t widthBytes;
            Access access;
            bool byteAccessOnly;
            uint32_t resetValue;
            uint32_t strapBits;
            const char* name;
        };

        constexpr RegisterSpec g_registers[] = {
            {0x080, 2, Access::ReadWrite, false, 0, 0, "CSAR0"},
            {0x084, 4, Access::ReadWrite, false, 0, 0, "CSMR0"},
            {0x08a, 2, Access::ReadWrite, false, 0, 0, "CSCR0"},
            {0x08c, 2, Access::ReadWrite, false, 0, 0, "CSAR1"},
            {0x090, 4, Access::ReadWrite, false, 0, 0, "CSMR1"},
            {0x096, 2, Access::ReadWrite, false, 0, 0, "CSCR1"},
            {0x098, 2, Access::ReadWrite, false, 0, 0, "CSAR2"},
            {0x09c, 4, Access::ReadWrite, false, 0, 0, "CSMR2"},
            {0x0a2, 2, Access::ReadWrite, false, 0, 0, "CSCR2"},
            {0x0a4, 2, Access::ReadWrite, false, 0, 0, "CSAR3"},
            {0x0a8, 4, Access::ReadWrite, false, 0, 0, "CSMR3"},
            {0x0ae, 2, Access::ReadWrite, false, 0, 0, "CSCR3"},
            {0x0b0, 2, Access::ReadWrite, false, 0, 0, "CSAR4"},
            {0x0b4, 4, Access::ReadWrite, false, 0, 0, "CSMR4"},
            {0x0ba, 2, Access::ReadWrite, false, 0, 0, "CSCR4"},
            {0x0bc, 2, Access::ReadWrite, false, 0, 0, "CSAR5"},
            {0x0c0, 4, Access::ReadWrite, false, 0, 0, "CSMR5"},
            {0x0c6, 2, Access::ReadWrite, false, 0, 0, "CSCR5"},

            // The DRAM controller.
            {0x100, 2, Access::ReadWrite, false, 0, 0, "DCR"},
            {0x108, 4, Access::ReadWrite, false, 0, 0, "DACR0"},
            {0x10c, 4, Access::ReadWrite, false, 0, 0, "DMR0"},
            {0x110, 4, Access::ReadWrite, false, 0, 0, "DACR1"},
            {0x114, 4, Access::ReadWrite, false, 0, 0, "DMR1"},

            // The two timers.
            {0x140, 2, Access::ReadWrite, false, 0x0000, 0, "TMR1"},
            {0x144, 2, Access::ReadWrite, false, 0xffff, 0, "TRR1"},
            {0x148, 2, Access::ReadOnly, false, 0x0000, 0, "TCR1"},
            {0x14c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN1"},
            {0x151, 1, Access::ReadWrite, false, 0x00, 0, "TER1"},
            {0x180, 2, Access::ReadWrite, false, 0x0000, 0, "TMR2"},
            {0x184, 2, Access::ReadWrite, false, 0xffff, 0, "TRR2"},
            {0x188, 2, Access::ReadOnly, false, 0x0000, 0, "TCR2"},
            {0x18c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN2"},
            {0x191, 1, Access::ReadWrite, false, 0x00, 0, "TER2"},

            // The model strap.
            {0x1d0, 1, Access::ReadOnly, true, 0x0e, 0x00, "UIPCR"},

            // The parallel port. Port A bit 9 is an input strap.
            {0x244, 2, Access::ReadWrite, false, 0x0000, 0x0000, "PADDR"},
            {0x248, 2, Access::ReadWrite, false, 0x0000, 0x0200, "PADAT"},
        };

        constexpr uint8_t protectByte(const Access _access, const uint32_t _strapBits, const uint32_t _widthBytes,
                                      const uint32_t _byte)
        {
            const int shift = int(8 * (_widthBytes - 1 - _byte));
            const uint8_t base = _access == Access::ReadOnly ? 0xffu : 0x00u;
            return uint8_t(base | uint8_t((_strapBits >> shift) & 0xffu));
        }

        const RegisterSpec* find(const uint32_t _offset)
        {
            for (const RegisterSpec& spec : g_registers)
            {
                if (_offset >= spec.offset && (_offset - spec.offset) < spec.widthBytes)
                    return &spec;
            }
            return nullptr;
        }

        bool isLegalWidth(const int _size) { return _size == 8 || _size == 16 || _size == 32; }

        std::string hex32(const uint32_t _value)
        {
            static const char* digits = "0123456789abcdef";
            std::string result = "0x";
            for (int shift = 28; shift >= 0; shift -= 4)
                result += digits[(_value >> shift) & 0xfu];
            return result;
        }
    } // namespace

    Sim::Sim(const bool _engineStrap)
    {
        for (uint8_t& protect : m_writeProtect)
            protect = 0xffu;

        for (const RegisterSpec& spec : g_registers)
        {
            for (uint32_t byte = 0; byte < spec.widthBytes; ++byte)
            {
                const int shift = int(8 * (spec.widthBytes - 1 - byte));
                const uint32_t index = spec.offset + byte;

                m_space[index] = uint8_t((spec.resetValue >> shift) & 0xffu);
                m_writeProtect[index] = protectByte(spec.access, spec.strapBits, spec.widthBytes, byte);
            }
        }

        if (_engineStrap)
            m_space[g_simUipcrOffset] |= 0x01u;
    }

    Sim::Sim(const Model _model) : Sim(isEngineStrapSet(_model)) {}

    Sim::Sim(cf_ctx*& _ctx) : Sim(false) { m_ctxPtr = &_ctx; }

    Sim::~Sim()
    {
        if (m_ownedCtx)
        {
            cf_destroy(m_ownedCtx);
            m_ownedCtx = nullptr;
        }
    }

    cf_ctx* Sim::activeCtx() const
    {
        if (m_ctxPtr && *m_ctxPtr)
            return *m_ctxPtr;
        return m_ownedCtx;
    }

    Timer* Sim::timerForByte(const uint32_t _index, uint32_t& _blockOffset)
    {
        if (_index >= Timer::gTimer1Base && _index < Timer::gTimer1Base + Timer::gBlockSize)
        {
            const uint32_t offset = _index - Timer::gTimer1Base;
            if (Timer::coversByte(offset))
            {
                _blockOffset = offset;
                return &m_timer1;
            }
        }

        if (_index >= Timer::gTimer2Base && _index < Timer::gTimer2Base + Timer::gBlockSize)
        {
            const uint32_t offset = _index - Timer::gTimer2Base;
            if (Timer::coversByte(offset))
            {
                _blockOffset = offset;
                return &m_timer2;
            }
        }

        return nullptr;
    }

    void Sim::setInterruptController(InterruptController* _interrupts)
    {
        m_interrupts = _interrupts;
        m_timer1.setInterruptController(_interrupts);
        m_timer2.setInterruptController(_interrupts);
        if (m_interrupts && activeCtx())
        {
            m_interrupts->attachContext(activeCtx());
        }
    }

    void Sim::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
    {
        m_log.push_back(std::string("sim: ") + _reason + (_isWrite ? " write of " : " read of ") +
                        std::to_string(_size) + " bits at offset " + hex32(_offset));
    }

    uint16_t Sim::onPortATrampoline(void* _user)
    {
        auto* s = static_cast<Sim*>(_user);
        if (s && s->m_portAReadHook)
        {
            return s->m_portAReadHook();
        }
        return 0xFFFFu;
    }

    void Sim::setPortAReadHook(PortAReadHook _hook)
    {
        m_portAReadHook = std::move(_hook);
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_sim_set_port_a_hook(ctx, &Sim::onPortATrampoline, this);
        }
    }

    void Sim::advanceTimers(const uint32_t _inputClocks)
    {
        m_timer1.advance(_inputClocks);
        m_timer2.advance(_inputClocks);
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_timer_tick(ctx, _inputClocks);
        }
        if (m_interrupts)
        {
            m_interrupts->notifyPresent();
        }
    }

    uint32_t Sim::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
    {
        _status = CF_BUS_OK;

        if (!isLegalWidth(_size))
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", false, _size, _offset);
            return 0;
        }

        const RegisterSpec* const spec = find(_offset);

        if (!spec)
            logLine("UNMODELLED", false, _size, _offset);
        else if (spec->byteAccessOnly && _size != 8)
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", false, _size, _offset);
            return 0;
        }

        const uint32_t bytes = uint32_t(_size) / 8u;
        uint32_t value = 0;

        for (uint32_t byte = 0; byte < bytes; ++byte)
        {
            const uint32_t index = _offset + byte;
            value <<= 8;

            uint32_t blockOffset = 0;
            if (Timer* const timer = timerForByte(index, blockOffset))
                value |= timer->readByte(blockOffset);
            else if (m_portAReadHook && (index == 0x248 || index == 0x249))
            {
                const uint16_t rows = m_portAReadHook() & ~0x0200u;
                const uint8_t byteVal = (index == 0x248) ? uint8_t((rows >> 8) & 0xffu) : uint8_t(rows & 0xffu);
                value |= byteVal;
            }
            else if (index < g_simSpaceSize)
                value |= m_space[index];
        }

        return value;
    }

    void Sim::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
    {
        _status = CF_BUS_OK;

        if (!isLegalWidth(_size))
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", true, _size, _offset);
            return;
        }

        const RegisterSpec* const spec = find(_offset);

        if (!spec)
            logLine("UNMODELLED", true, _size, _offset);
        else if (spec->byteAccessOnly && _size != 8)
        {
            _status = CF_BUS_SIZE_ILLEGAL;
            logLine("SIZE_ILLEGAL", true, _size, _offset);
            return;
        }

        const uint32_t bytes = uint32_t(_size) / 8u;
        cf_ctx* ctx = activeCtx();

        for (uint32_t byte = 0; byte < bytes; ++byte)
        {
            const uint32_t index = _offset + byte;
            if (index >= g_simSpaceSize)
                continue;

            const int shift = int(8 * (bytes - 1 - byte));
            const uint8_t incoming = uint8_t((_value >> shift) & 0xffu);

            uint32_t blockOffset = 0;
            if (Timer* const timer = timerForByte(index, blockOffset))
            {
                timer->writeByte(blockOffset, incoming);
                if (ctx)
                {
                    cf_bus_status dummy = CF_BUS_OK;
                    cf_mbar_write(ctx, index, 1, incoming, &dummy);
                }
                continue;
            }

            const uint8_t protect = m_writeProtect[index];

            m_space[index] = uint8_t((m_space[index] & protect) | (incoming & ~protect));
            if (ctx)
            {
                cf_bus_status dummy = CF_BUS_OK;
                cf_mbar_write(ctx, index, 1, incoming, &dummy);
            }
        }

        if (m_interrupts)
        {
            m_interrupts->notifyPresent();
        }
    }
} // namespace rg2
