#pragma once

#include <coldfire.h>
#include <cstdint>

namespace rg2
{
    enum class ExternalPin : int
    {
        Irq7 = CF_IRQ_PIN_7,
        Irq5 = CF_IRQ_PIN_5,
        Irq3 = CF_IRQ_PIN_3,
        Irq1 = CF_IRQ_PIN_1,
    };

    using InterruptPresentFn = void (*)(void* _user, int _level, uint8_t _vector, int _autovector);

    class InterruptController final
    {
    public:
        static constexpr uint32_t gAvrOffset = 0x04Bu;
        static constexpr uint32_t gIcrBase = 0x04Cu;
        static constexpr uint32_t gIcrCount = 12u;
        static constexpr uint32_t gIrqparOffset = 0x006u;
        static constexpr int gInternalSourceCount = 10;

        explicit InterruptController(void* _user = nullptr, InterruptPresentFn _present = nullptr);
        explicit InterruptController(cf_ctx*& _ctx, void* _user = nullptr, InterruptPresentFn _present = nullptr);
        ~InterruptController() = default;

        void attachContext(cf_ctx* _ctx);
        void notifyPresent();

        void writeRegister(uint32_t _offset, uint8_t _value);
        uint8_t readRegister(uint32_t _offset) const;

        void setInternalPending(int _index, bool _asserted);
        void setExternalPending(ExternalPin _pin, bool _asserted);

        void setInternalVector(int _index, uint8_t _vector);
        void setExternalVector(ExternalPin _pin, uint8_t _vector);

        void setPresentCallback(void* _user, InterruptPresentFn _present);

        int presentedLevel() const;
        uint8_t presentedVector() const;
        int presentedAutovector() const;

    private:
        cf_ctx* activeCtx() const;

        struct Winner
        {
            bool valid = false;
            int level = 0;
            uint8_t vector = 0;
            int autovector = 0;
        };

        Winner arbitrate() const;
        void recomputeAndPresent();

        cf_ctx** m_ctxPtr = nullptr;
        cf_ctx* m_attachedCtx = nullptr;
        void* m_user = nullptr;
        InterruptPresentFn m_present = nullptr;

        uint8_t m_irqpar = 0x00u;
        uint8_t m_avr = 0x00u;
        uint8_t m_icr[gIcrCount] = {};

        bool m_internalPending[gInternalSourceCount] = {};
        bool m_externalPending[4] = {};

        uint8_t m_internalVector[gInternalSourceCount] = {};
        uint8_t m_externalVector[4] = {};

        int m_lastLevel = 0;
        uint8_t m_lastVector = 0;
        int m_lastAutovector = 0;
    };
} // namespace rg2

namespace coldfire
{
    using ExternalPin = rg2::ExternalPin;
    using InterruptPresentFn = rg2::InterruptPresentFn;
    using InterruptController = rg2::InterruptController;
} // namespace coldfire
