#include "isp1181.h"

#include <coldfire.h>
#include <isp1181.h>

#include <cstring>
#include <utility>

namespace hwLib
{
    Isp1181::Isp1181() : Isp1181(nullptr, nullptr) {}

    Isp1181::Isp1181(IrqCallback _irqCb, TxCallback _txCb) : m_irqCb(std::move(_irqCb)), m_txCb(std::move(_txCb))
    {
        m_ctx = isp1181_create(this, &Isp1181::cIrqTrampoline, &Isp1181::cTxTrampoline);
    }

    Isp1181::Isp1181(void* _user, RawIrqCallback _irqCb, RawTxCallback _txCb)
    {
        m_ctx = isp1181_create(_user, _irqCb, _txCb);
    }

    Isp1181::~Isp1181()
    {
        if (m_ctx)
        {
            isp1181_destroy(m_ctx);
            m_ctx = nullptr;
        }
    }

    void Isp1181::cIrqTrampoline(void* const _user, const int _asserted)
    {
        auto* const self = static_cast<Isp1181*>(_user);
        if (self && self->m_irqCb)
            self->m_irqCb(_asserted != 0);
    }

    void Isp1181::cTxTrampoline(void* const _user, const int _endpoint, const uint8_t* const _data, const size_t _len)
    {
        auto* const self = static_cast<Isp1181*>(_user);
        if (!self)
            return;

        if (self->m_captureBuffer && self->m_captureMaxLen > 0)
        {
            const size_t copyLen = _len < self->m_captureMaxLen ? _len : self->m_captureMaxLen;
            if (_data && copyLen > 0)
                std::memcpy(self->m_captureBuffer, _data, copyLen);
            if (self->m_captureActualLen)
                *self->m_captureActualLen = _len;
        }

        if (self->m_txCb)
            self->m_txCb(_endpoint, _data, _len);
    }

    bool Isp1181::setBackend(const Backend _backend)
    {
        if (!m_ctx)
            return false;
        return isp1181_set_backend(m_ctx, static_cast<int>(_backend)) == 1;
    }

    uint8_t Isp1181::read(const uint32_t _addr)
    {
        if (!m_ctx)
            return 0;
        return isp1181_read(m_ctx, _addr);
    }

    void Isp1181::write(const uint32_t _addr, const uint8_t _value)
    {
        if (m_ctx)
            isp1181_write(m_ctx, _addr, _value);
    }

    int Isp1181::rx(const int _endpoint, const uint8_t* const _data, const size_t _len)
    {
        if (!m_ctx)
            return 0;
        return isp1181_rx(m_ctx, _endpoint, _data, _len);
    }

    int Isp1181::setup(const uint8_t* const _data, const size_t _len)
    {
        if (!m_ctx)
            return 0;
        return isp1181_setup(m_ctx, _data, _len);
    }

    int Isp1181::inToken(const int _endpoint)
    {
        if (!m_ctx)
            return 0;
        return isp1181_in_token(m_ctx, _endpoint);
    }

    int Isp1181::inToken(const int _endpoint, uint8_t* const _buffer, const size_t _maxLen, size_t* const _actualLen)
    {
        if (!m_ctx)
            return 0;

        m_captureBuffer = _buffer;
        m_captureMaxLen = _maxLen;
        m_captureActualLen = _actualLen;
        if (m_captureActualLen)
            *m_captureActualLen = 0;

        const int result = isp1181_in_token(m_ctx, _endpoint);

        m_captureBuffer = nullptr;
        m_captureMaxLen = 0;
        m_captureActualLen = nullptr;

        return result;
    }

    void Isp1181::tick(const uint32_t _sofFrames)
    {
        if (m_ctx)
            isp1181_tick(m_ctx, _sofFrames);
    }

    size_t Isp1181::getLogWritten() const
    {
        if (!m_ctx)
            return 0;
        return isp1181_log_written(m_ctx);
    }

    size_t Isp1181::getLogRetained() const
    {
        if (!m_ctx)
            return 0;
        return isp1181_log_retained(m_ctx);
    }

    std::string Isp1181::getLogLine(const size_t _index) const
    {
        if (!m_ctx)
            return {};
        const size_t needed = isp1181_log_line(m_ctx, _index, nullptr, 0);
        if (needed <= 1)
            return {};
        std::string buf(needed - 1, '\0');
        (void)isp1181_log_line(m_ctx, _index, &buf[0], needed);
        return buf;
    }

    size_t Isp1181::getConfigSlots() { return isp1181_config_slots(); }

    int Isp1181::getConfigSlot(const size_t _slot, uint8_t& _value) const
    {
        if (!m_ctx)
            return -1;
        return isp1181_config_slot(m_ctx, _slot, &_value);
    }

    int Isp1181::getSlotBuffer(const size_t _slot, size_t& _maxPacketBytes, size_t& _bufferCount) const
    {
        if (!m_ctx)
            return -1;
        return isp1181_slot_buffer(m_ctx, _slot, &_maxPacketBytes, &_bufferCount);
    }

    std::string Isp1181::getReport() const
    {
        if (!m_ctx)
            return {};
        const size_t needed = isp1181_report(m_ctx, nullptr, 0);
        if (needed <= 1)
            return {};
        std::string buf(needed - 1, '\0');
        (void)isp1181_report(m_ctx, &buf[0], needed);
        return buf;
    }

    size_t Isp1181::getStateSize() { return isp1181_state_size(); }

    void Isp1181::saveState(void* const _dst) const
    {
        if (m_ctx && _dst)
            isp1181_state_save(m_ctx, _dst);
    }

    void Isp1181::loadState(const void* const _src)
    {
        if (m_ctx && _src)
            isp1181_state_load(m_ctx, _src);
    }
} // namespace hwLib
