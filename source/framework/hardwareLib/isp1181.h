#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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
		Backend backend() const { return m_backend; }

		uint8_t read(uint32_t _addr);
		void write(uint32_t _addr, uint8_t _value);

		int rx(int _endpoint, const uint8_t* _data, size_t _len);
		int setup(const uint8_t* _data, size_t _len);
		int inToken(int _endpoint);
		int inToken(int _endpoint, uint8_t* _buffer, size_t _maxLen, size_t* _actualLen = nullptr);

		void tick(uint32_t _sofFrames = 1);
		uint16_t frameNumber() const { return m_frameNumber; }

		bool irqAsserted() const { return m_asserted; }
		uint8_t mode() const { return m_mode; }
		uint16_t hwConfig() const { return m_hwConfig; }
		uint8_t deviceAddress() const { return m_deviceAddress; }
		uint32_t interruptRegister() const { return m_interruptRegister; }
		uint32_t interruptEnable() const { return m_interruptEnable; }

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
		static constexpr size_t kConfigSlotCount = 16;
		static constexpr size_t kFifoCount = 5;
		static constexpr size_t kControlSlotCount = 2;
		static constexpr size_t kControlFifoBytes = 64;
		static constexpr size_t kControlFifoBuffers = 1;
		static constexpr uint16_t kUsbFrameCount = 2048;
		static constexpr size_t kLogCapacity = 4096;

		static constexpr uint8_t kBenignValue = 0x00;
		static constexpr uint8_t kEpdirBit = 0x40;
		static constexpr uint8_t kFifoEnableBit = 0x80;
		static constexpr uint8_t kDblbufBit = 0x20;
		static constexpr uint8_t kFfoisoBit = 0x10;
		static constexpr uint8_t kFfoszMask = 0x0F;
		static constexpr uint8_t kSoftctBit = 0x01;
		static constexpr uint32_t kCommandSelect = 0x10;

		static constexpr uint16_t kChipIdA = 0x8141;

		struct Packet
		{
			std::array<uint8_t, 64> data{};
			size_t length = 0;
		};

		struct Fifo
		{
			size_t capacityBytes = 0;
			size_t bufferCount = 0;
			std::array<Packet, 2> slots{};
			size_t head = 0;
			size_t tail = 0;
			size_t count = 0;

			bool isFull() const { return count >= bufferCount; }
			bool isEmpty() const { return count == 0; }
			size_t pending() const { return count; }
			void clear()
			{
				head = 0;
				tail = 0;
				count = 0;
			}

			bool accept(const uint8_t* _data, size_t _len)
			{
				if (isFull() || _len > capacityBytes || _len > 64)
					return false;
				if (_len > 0 && _data)
					std::memcpy(slots[tail].data.data(), _data, _len);
				slots[tail].length = _len;
				tail = (tail + 1) % 2;
				++count;
				return true;
			}

			std::pair<bool, uint8_t> peek() const
			{
				if (isEmpty() || slots[head].length == 0)
					return {false, 0};
				return {true, slots[head].data[0]};
			}

			Packet take()
			{
				if (isEmpty())
					return {};
				Packet pkt = slots[head];
				head = (head + 1) % 2;
				--count;
				return pkt;
			}

			const Packet& front() const
			{
				return slots[head];
			}
		};

		enum class Transfer : uint8_t
		{
			None,
			Write,
			Read,
			Absent,
			BufferRead,
			BufferWrite,
			Refused
		};

		enum class BufferDirection : uint8_t
		{
			Out,
			In
		};

		enum class CommandClass : uint8_t
		{
			Unspecified,
			Implemented,
			NotImplemented,
			Illegal
		};

		struct Command
		{
			CommandClass cls = CommandClass::Unspecified;
			std::string name;
			std::string detail;
		};

		struct LogEntry
		{
			char* text = nullptr;
			int ordinal = 0;
		};

		void clearState();
		void note(const char* _line);
		void note(const std::string& _line);
		void updateIrq();
		void raiseInterrupt(uint32_t _mask);
		void clearInterrupt(uint32_t _mask);

		BufferDirection directionOfBuffer(size_t _index) const;
		Command classify(uint8_t _opcode) const;

		void beginRefused(uint8_t _opcode);
		void noteInterlock(uint8_t _opcode, const std::string& _name);
		bool directionRefused(uint8_t _opcode, const std::string& _name, int _endpoint, size_t _index, BufferDirection _needs);

		uint8_t statusByte(size_t _index) const;
		void beginBufferRead(uint8_t _opcode, size_t _index);
		void beginBufferWrite(uint8_t _opcode, size_t _index);
		void commitValidate(int _endpoint, size_t _index);
		void beginTransfer(uint8_t _opcode, Transfer _kind, size_t _width, uint32_t _value);

		void writeCommand(uint8_t _opcode);
		void writeData(uint8_t _value);
		uint8_t readData();
		void commitOperand();

		bool deliver(int _endpoint, const uint8_t* _data, size_t _len);
		bool deliverSetup(const uint8_t* _data, size_t _len);
		bool queueIn(int _endpoint, const uint8_t* _data, size_t _len);
		bool transmit(int _endpoint);

		void* m_user = nullptr;
		RawIrqCallback m_rawIrqCb = nullptr;
		RawTxCallback m_rawTxCb = nullptr;
		IrqCallback m_irqCb;
		TxCallback m_txCb;

		Backend m_backend = Backend::Stub;
		uint16_t m_frameNumber = 0;

		int m_pending = -1;
		Transfer m_transfer = Transfer::None;
		size_t m_width = 0;
		size_t m_index = 0;
		uint32_t m_latch = 0;
		std::string m_absentNote;

		uint16_t m_hwConfig = 0;
		uint8_t m_mode = 0;
		uint8_t m_deviceAddress = 0;
		uint32_t m_interruptEnable = 0;
		uint32_t m_interruptRegister = 0;

		std::array<uint8_t, kConfigSlotCount> m_endpointConfig{};
		std::array<bool, kConfigSlotCount> m_configWritten{};
		std::array<int, kConfigSlotCount> m_configOrdinal{};
		size_t m_selected = 0;

		bool m_setupHeld = false;
		bool m_setupUnacknowledged = false;
		bool m_asserted = false;

		std::array<Fifo, kFifoCount> m_fifos;
		std::array<bool, kFifoCount> m_stalled{};

		std::vector<uint8_t> m_stage;
		int m_stageFifo = -1;
		std::vector<uint8_t> m_readBuf;

		LogEntry* m_logEntries = nullptr;
		size_t m_logCount = 0;
		size_t m_logAllocated = 0;
		size_t m_written = 0;
		int m_events = 0;

		uint8_t* m_captureBuffer = nullptr;
		size_t m_captureMaxLen = 0;
		size_t* m_captureActualLen = nullptr;

		isp1181_ctx* m_ctx = nullptr;
	};
} // namespace hwLib
