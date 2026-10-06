#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct isp1181_ctx;

namespace hwLib
{
    class Isp1181
    {
    public:
        using IrqCallback = std::function<void(bool _asserted)>;
        using TxCallback = std::function<void(int _endpoint, const uint8_t* _data, size_t _len)>;
        using RawIrqCallback = void (*)(void* _user, int _asserted);
        using RawTxCallback = void (*)(void* _user, int _endpoint, const uint8_t* _data, size_t _len);

        enum class Backend : int
        {
            Stub = 0,
            FullModel = 1
        };

        Isp1181();
        explicit Isp1181(IrqCallback _irqCb, TxCallback _txCb = nullptr);
        Isp1181(void* _user, RawIrqCallback _irqCb, RawTxCallback _txCb = nullptr);
        ~Isp1181();

        Isp1181(const Isp1181&) = delete;
        Isp1181& operator=(const Isp1181&) = delete;
        Isp1181(Isp1181&&) = delete;
        Isp1181& operator=(Isp1181&&) = delete;

        void setIrqCallback(IrqCallback _cb) { m_irqCb = std::move(_cb); }
        void setTxCallback(TxCallback _cb) { m_txCb = std::move(_cb); }

        bool setBackend(Backend _backend);

        uint8_t read(uint32_t _addr);
        void write(uint32_t _addr, uint8_t _value);

        int rx(int _endpoint, const uint8_t* _data, size_t _len);
        int setup(const uint8_t* _data, size_t _len);
        int inToken(int _endpoint);
        int inToken(int _endpoint, uint8_t* _buffer, size_t _maxLen, size_t* _actualLen = nullptr);

        void tick(uint32_t _sofFrames = 1);

        size_t getLogWritten() const;
        size_t getLogRetained() const;
        std::string getLogLine(size_t _index) const;

        static size_t getConfigSlots();
        int getConfigSlot(size_t _slot, uint8_t& _value) const;
        int getSlotBuffer(size_t _slot, size_t& _maxPacketBytes, size_t& _bufferCount) const;
        std::string getReport() const;

        static size_t getStateSize();
        void saveState(void* _dst) const;
        void loadState(const void* _src);

        isp1181_ctx* rawContext() const { return m_ctx; }

    private:
        static void cIrqTrampoline(void* _user, int _asserted);
        static void cTxTrampoline(void* _user, int _endpoint, const uint8_t* _data, size_t _len);

        isp1181_ctx* m_ctx = nullptr;
        IrqCallback m_irqCb;
        TxCallback m_txCb;

        uint8_t* m_captureBuffer = nullptr;
        size_t m_captureMaxLen = 0;
        size_t* m_captureActualLen = nullptr;
    };
} // namespace hwLib
