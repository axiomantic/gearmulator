#include "interruptController.h"

namespace rg2
{
    namespace
    {
        int internalRank(const uint8_t _icr)
        {
            const int ip = _icr & 0x03u;
            switch (ip)
            {
            case 3:
                return 0; // IP=11
            case 2:
                return 1; // IP=10
            case 1:
                return 3; // IP=01
            default:
                return 4; // IP=00
            }
        }

        int externalLevel(const ExternalPin _pin, const uint8_t _irqpar)
        {
            switch (_pin)
            {
            case ExternalPin::Irq7:
                return 7;
            case ExternalPin::Irq5:
                return (_irqpar & 0x04u) ? 4 : 5;
            case ExternalPin::Irq3:
                return (_irqpar & 0x02u) ? 6 : 3;
            case ExternalPin::Irq1:
                return (_irqpar & 0x01u) ? 2 : 1;
            }
            return 0;
        }
    } // namespace

    InterruptController::InterruptController(void* _user, InterruptPresentFn _present) :
        m_user(_user), m_present(_present)
    {
    }

    InterruptController::InterruptController(cf_ctx*& _ctx, void* _user, InterruptPresentFn _present) :
        m_ctxPtr(&_ctx), m_user(_user), m_present(_present)
    {
    }

    void InterruptController::attachContext(cf_ctx* _ctx)
    {
        m_ctxPtr = nullptr;
        m_attachedCtx = _ctx;
        if (m_attachedCtx)
        {
            // sync cached registers into context if any were written
            if (m_irqpar != 0)
            {
                cf_bus_status st = CF_BUS_OK;
                cf_mbar_write(m_attachedCtx, gIrqparOffset, 1, m_irqpar, &st);
            }
            if (m_avr != 0)
            {
                cf_bus_status st = CF_BUS_OK;
                cf_mbar_write(m_attachedCtx, gAvrOffset, 1, m_avr, &st);
            }
            for (uint32_t i = 0; i < gIcrCount; ++i)
            {
                if (m_icr[i] != 0)
                {
                    cf_bus_status st = CF_BUS_OK;
                    cf_mbar_write(m_attachedCtx, gIcrBase + i, 1, m_icr[i], &st);
                }
            }
            notifyPresent();
        }
    }

    void InterruptController::notifyPresent()
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            const int lvl = cf_intc_get_presented_level(ctx);
            uint8_t vec = cf_intc_get_presented_vector(ctx);
            const int avec = cf_intc_get_presented_autovector(ctx);
            if (lvl > 0 && avec == 0 && vec == 0x0Fu && m_internalVector[4] != 0)
            {
                vec = m_internalVector[4];
            }
            if (lvl == m_lastLevel && vec == m_lastVector && avec == m_lastAutovector)
            {
                return;
            }
            m_lastLevel = lvl;
            m_lastVector = vec;
            m_lastAutovector = avec;
        }
        if (m_present)
        {
            m_present(m_user, m_lastLevel, m_lastVector, m_lastAutovector);
        }
    }

    cf_ctx* InterruptController::activeCtx() const
    {
        if (m_ctxPtr && *m_ctxPtr)
            return *m_ctxPtr;
        return m_attachedCtx;
    }

    void InterruptController::writeRegister(const uint32_t _offset, const uint8_t _value)
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_bus_status st = CF_BUS_OK;
            cf_mbar_write(ctx, _offset, 1, _value, &st);
            notifyPresent();
            return;
        }

        if (_offset == gIrqparOffset)
            m_irqpar = _value;
        else if (_offset == gAvrOffset)
            m_avr = _value;
        else if (_offset >= gIcrBase && _offset < gIcrBase + gIcrCount)
            m_icr[_offset - gIcrBase] = _value;
        else
            return;

        recomputeAndPresent();
    }

    uint8_t InterruptController::readRegister(const uint32_t _offset) const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_bus_status st = CF_BUS_OK;
            return uint8_t(cf_mbar_read(ctx, _offset, 1, &st) & 0xFFu);
        }

        if (_offset == gIrqparOffset)
            return m_irqpar;
        if (_offset == gAvrOffset)
            return m_avr;
        if (_offset >= gIcrBase && _offset < gIcrBase + gIcrCount)
            m_icr[_offset - gIcrBase];
        return 0x00u;
    }

    void InterruptController::setInternalPending(const int _index, const bool _asserted)
    {
        if (_index < 0 || _index >= gInternalSourceCount)
            return;
        m_internalPending[_index] = _asserted;
        recomputeAndPresent();
    }

    void InterruptController::setExternalPending(const ExternalPin _pin, const bool _asserted)
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            cf_set_irq_pin(ctx, static_cast<int>(_pin), _asserted ? 1 : 0);
            notifyPresent();
            return;
        }

        const int index = static_cast<int>(_pin);
        if (index < 0 || index > 3)
            return;
        m_externalPending[index] = _asserted;
        recomputeAndPresent();
    }

    void InterruptController::setInternalVector(const int _index, const uint8_t _vector)
    {
        if (_index < 0 || _index >= gInternalSourceCount)
            return;
        m_internalVector[_index] = _vector;
        recomputeAndPresent();
    }

    void InterruptController::setExternalVector(const ExternalPin _pin, const uint8_t _vector)
    {
        const int index = static_cast<int>(_pin);
        if (index < 0 || index > 3)
            return;
        m_externalVector[index] = _vector;
        recomputeAndPresent();
    }

    void InterruptController::setPresentCallback(void* _user, InterruptPresentFn _present)
    {
        m_user = _user;
        m_present = _present;
        if (activeCtx())
            notifyPresent();
        else
            recomputeAndPresent();
    }

    int InterruptController::presentedLevel() const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
            return cf_intc_get_presented_level(ctx);
        return m_lastLevel;
    }

    uint8_t InterruptController::presentedVector() const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
        {
            const uint8_t vec = cf_intc_get_presented_vector(ctx);
            if (cf_intc_get_presented_level(ctx) > 0 && cf_intc_get_presented_autovector(ctx) == 0 && vec == 0x0Fu &&
                m_internalVector[4] != 0)
            {
                return m_internalVector[4];
            }
            return vec;
        }
        return m_lastVector;
    }

    int InterruptController::presentedAutovector() const
    {
        cf_ctx* ctx = activeCtx();
        if (ctx)
            return cf_intc_get_presented_autovector(ctx);
        return m_lastAutovector;
    }

    InterruptController::Winner InterruptController::arbitrate() const
    {
        Winner best;
        int bestRank = 999;

        for (int i = 0; i < gInternalSourceCount; ++i)
        {
            if (!m_internalPending[i])
                continue;

            const uint8_t icr = m_icr[i];
            const int level = (icr >> 2) & 0x07u;
            if (level == 0)
                continue;

            const int rank = internalRank(icr);
            if (level > best.level || (level == best.level && rank < bestRank))
            {
                best.valid = true;
                best.level = level;
                best.vector = m_internalVector[i];
                best.autovector = (icr & 0x80u) ? 1 : 0;
                bestRank = rank;
            }
        }

        for (int p = 0; p < 4; ++p)
        {
            if (!m_externalPending[p])
                continue;

            const ExternalPin pin = static_cast<ExternalPin>(p);
            const int level = externalLevel(pin, m_irqpar);
            if (level == 0)
                continue;

            constexpr int pinRank = 2; // Table 8-3: between IP=10 and IP=01
            if (level > best.level || (level == best.level && pinRank < bestRank))
            {
                best.valid = true;
                best.level = level;
                best.vector = m_externalVector[p];
                best.autovector = (m_avr & (1u << level)) ? 1 : 0;
                bestRank = pinRank;
            }
        }

        return best;
    }

    void InterruptController::recomputeAndPresent()
    {
        const Winner w = arbitrate();
        m_lastLevel = w.level;
        m_lastVector = w.vector;
        m_lastAutovector = w.autovector;

        if (m_present)
            m_present(m_user, m_lastLevel, m_lastVector, m_lastAutovector);
    }
} // namespace rg2
