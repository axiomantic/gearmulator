#include "isp1181.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

extern "C"
{
	struct isp1181_ctx;
	using isp1181_irq_fn = void (*)(void* user, int asserted);
	using isp1181_tx_fn = void (*)(void* user, int endpoint, const uint8_t* data, size_t length);

	__attribute__((weak)) isp1181_ctx* isp1181_create(void* user, isp1181_irq_fn irq, isp1181_tx_fn tx)
	{
		(void)user;
		(void)irq;
		(void)tx;
		return nullptr;
	}
	__attribute__((weak)) void isp1181_destroy(isp1181_ctx* ctx)
	{
		(void)ctx;
	}
	__attribute__((weak)) void isp1181_tick(isp1181_ctx* ctx, uint32_t sofFrames)
	{
		(void)ctx;
		(void)sofFrames;
	}
}

namespace hwLib
{
	namespace
	{
		const std::array<std::string, 5> g_fifoNames = {
			"endpoint 0 OUT", "endpoint 0 IN", "endpoint 1", "endpoint 2", "endpoint 3"};

		const std::array<std::pair<size_t, size_t>, 5> g_fifoShape = {
			std::make_pair(size_t(64), size_t(1)), // endpoint 0 OUT
			std::make_pair(size_t(64), size_t(1)), // endpoint 0 IN
			std::make_pair(size_t(16), size_t(2)), // endpoint 1
			std::make_pair(size_t(64), size_t(2)), // endpoint 2
			std::make_pair(size_t(64), size_t(1))  // endpoint 3
		};

		const std::array<size_t, 4> g_outFifoOfEndpoint = {0, 2, 3, 4};
		const std::array<size_t, 4> g_inBufferOfEndpoint = {1, 2, 3, 4};
		const std::array<size_t, 5> g_interruptBitOfFifo = {8, 9, 10, 11, 12};

		constexpr size_t kOutFifoOfEndpoint0 = 0;
		constexpr size_t kInFifoOfEndpoint0 = 1;

		std::string toHex(uint32_t _val, int _width = 2)
		{
			std::ostringstream ss;
			ss << std::uppercase << std::hex << std::setfill('0') << std::setw(_width) << _val;
			return ss.str();
		}

		uint32_t computeFnv1a(const uint8_t* _buf, size_t _len)
		{
			uint32_t c = 2166136261u;
			for (size_t i = 0; i < _len; ++i)
				c = (c ^ _buf[i]) * 16777619u;
			return c;
		}

		int nonIsoBufferBytes(uint8_t _ffosz)
		{
			switch (_ffosz & 0x0Fu)
			{
			case 0b0000: return 8;
			case 0b0001: return 16;
			case 0b0010: return 32;
			case 0b0011: return 64;
			default: return -1;
			}
		}

		std::string slotEndpointName(size_t _slot)
		{
			if (_slot < 5)
				return g_fifoNames[_slot];
			return "endpoint " + std::to_string(_slot - 1);
		}

		std::string slotBufferName(size_t _slot)
		{
			if (_slot < 5)
				return "buffer " + std::to_string(_slot);
			return "no buffer in this model";
		}
	} // namespace

	Isp1181::Isp1181()
		: Isp1181(nullptr, nullptr)
	{
	}

	Isp1181::Isp1181(IrqCallback _irqCb, TxCallback _txCb)
		: m_irqCb(std::move(_irqCb))
		, m_txCb(std::move(_txCb))
	{
		for (size_t i = 0; i < kFifoCount; ++i)
		{
			m_fifos[i].capacityBytes = g_fifoShape[i].first;
			m_fifos[i].bufferCount = g_fifoShape[i].second;
		}
		clearState();
		m_ctx = reinterpret_cast<isp1181_ctx*>(this);
	}

	Isp1181::Isp1181(void* _user, RawIrqCallback _irqCb, RawTxCallback _txCb)
		: m_user(_user)
		, m_rawIrqCb(_irqCb)
		, m_rawTxCb(_txCb)
	{
		for (size_t i = 0; i < kFifoCount; ++i)
		{
			m_fifos[i].capacityBytes = g_fifoShape[i].first;
			m_fifos[i].bufferCount = g_fifoShape[i].second;
		}
		clearState();

		if (isp1181_create)
			m_ctx = isp1181_create(_user, _irqCb, _txCb);
		if (!m_ctx)
			m_ctx = reinterpret_cast<isp1181_ctx*>(this);
	}

	Isp1181::~Isp1181()
	{
		if (m_ctx && m_ctx != reinterpret_cast<isp1181_ctx*>(this) && isp1181_destroy)
		{
			isp1181_destroy(m_ctx);
			m_ctx = nullptr;
		}
		if (m_logEntries)
		{
			for (size_t i = 0; i < m_logCount; ++i)
				std::free(m_logEntries[i].text);
			std::free(m_logEntries);
			m_logEntries = nullptr;
		}
	}

	void Isp1181::clearState()
	{
		m_pending = -1;
		m_transfer = Transfer::None;
		m_width = 0;
		m_index = 0;
		m_latch = 0;
		m_hwConfig = 0;
		m_mode = 0;
		m_deviceAddress = 0;
		m_interruptEnable = 0;
		m_interruptRegister = 0;
		for (size_t i = 0; i < kConfigSlotCount; ++i)
		{
			m_endpointConfig[i] = 0;
			m_configWritten[i] = false;
			m_configOrdinal[i] = 0;
		}
		m_selected = 0;
		m_stage.clear();
		m_stageFifo = -1;
		m_readBuf.clear();
		for (size_t i = 0; i < kFifoCount; ++i)
		{
			m_fifos[i].clear();
			m_stalled[i] = false;
		}
		m_setupHeld = false;
		m_setupUnacknowledged = false;
		if (m_logEntries)
		{
			for (size_t i = 0; i < m_logCount; ++i)
				std::free(m_logEntries[i].text);
			m_logCount = 0;
		}
		updateIrq();
	}

	void Isp1181::note(const char* _line)
	{
		if (!_line)
			return;
		++m_events;
		++m_written;
		if (m_logCount < kLogCapacity)
		{
			const size_t len = std::strlen(_line);
			char* copy = static_cast<char*>(std::malloc(len + 1));
			if (copy)
			{
				std::memcpy(copy, _line, len + 1);
				if (m_logCount == m_logAllocated)
				{
					const size_t newAlloc = m_logAllocated == 0 ? 64 : m_logAllocated * 2;
					auto* newEntries = static_cast<LogEntry*>(std::realloc(m_logEntries, newAlloc * sizeof(LogEntry)));
					if (newEntries)
					{
						m_logEntries = newEntries;
						m_logAllocated = newAlloc;
					}
				}
				if (m_logCount < m_logAllocated)
				{
					m_logEntries[m_logCount].text = copy;
					m_logEntries[m_logCount].ordinal = m_events;
					++m_logCount;
				}
				else
				{
					std::free(copy);
				}
			}
		}
	}

	void Isp1181::note(const std::string& _line)
	{
		note(_line.c_str());
	}

	void Isp1181::updateIrq()
	{
		const bool want = (m_interruptRegister & m_interruptEnable) != 0;
		if (want == m_asserted)
			return;
		m_asserted = want;
		if (m_irqCb)
			m_irqCb(want);
		if (m_rawIrqCb)
			m_rawIrqCb(m_user, want ? 1 : 0);
	}

	void Isp1181::raiseInterrupt(const uint32_t _mask)
	{
		m_interruptRegister |= _mask;
		updateIrq();
	}

	void Isp1181::clearInterrupt(const uint32_t _mask)
	{
		m_interruptRegister &= ~_mask;
		updateIrq();
	}

	Isp1181::BufferDirection Isp1181::directionOfBuffer(const size_t _index) const
	{
		if (_index <= kInFifoOfEndpoint0)
			return (_index == kInFifoOfEndpoint0) ? BufferDirection::In : BufferDirection::Out;
		return (m_endpointConfig[_index] & kEpdirBit) != 0 ? BufferDirection::In : BufferDirection::Out;
	}

	bool Isp1181::setBackend(const Backend _backend)
	{
		m_backend = _backend;
		return true;
	}

	Isp1181::Command Isp1181::classify(const uint8_t _opcode) const
	{
		// 1. Buffer write family
		if (_opcode == 0x00)
			return {CommandClass::Illegal, "write control OUT buffer", "the endpoint is read-only"};
		if (_opcode == 0x01)
			return {CommandClass::Implemented, "control IN buffer write", ""};
		if (_opcode >= 0x02 && _opcode <= 0x0F)
		{
			const int ep = _opcode - 0x02 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " buffer write";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 2. Buffer read family
		if (_opcode == 0x11)
			return {CommandClass::Illegal, "read control IN buffer", "the endpoint is write-only"};
		if (_opcode == 0x10)
			return {CommandClass::Implemented, "control OUT buffer read", ""};
		if (_opcode >= 0x12 && _opcode <= 0x1F)
		{
			const int ep = _opcode - 0x12 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " buffer read";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 3. Configuration family
		if (_opcode == 0x20)
			return {CommandClass::Implemented, "control OUT configuration", ""};
		if (_opcode == 0x21)
			return {CommandClass::Implemented, "control IN configuration", ""};
		if (_opcode >= 0x22 && _opcode <= 0x2F)
		{
			const int ep = _opcode - 0x22 + 1;
			return {CommandClass::Implemented, "endpoint " + std::to_string(ep) + " configuration", ""};
		}

		// 4. Stall family
		if (_opcode == 0x40)
			return {CommandClass::Implemented, "control OUT stall", ""};
		if (_opcode == 0x41)
			return {CommandClass::Implemented, "control IN stall", ""};
		if (_opcode >= 0x42 && _opcode <= 0x4F)
		{
			const int ep = _opcode - 0x42 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " stall";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 5. Status family
		if (_opcode == 0x50)
			return {CommandClass::Implemented, "control OUT status", ""};
		if (_opcode == 0x51)
			return {CommandClass::Implemented, "control IN status", ""};
		if (_opcode >= 0x52 && _opcode <= 0x5F)
		{
			const int ep = _opcode - 0x52 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " status";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 6. Buffer validate family
		if (_opcode == 0x60)
			return {CommandClass::Illegal, "validate control OUT buffer", "validating an OUT buffer is unpredictable"};
		if (_opcode == 0x61)
			return {CommandClass::Implemented, "control IN buffer validate", ""};
		if (_opcode >= 0x62 && _opcode <= 0x6F)
		{
			const int ep = _opcode - 0x62 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " buffer validate";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 7. Buffer clear family
		if (_opcode == 0x71)
			return {CommandClass::Illegal, "clear control IN buffer", "clearing an IN buffer is unpredictable"};
		if (_opcode == 0x70)
			return {CommandClass::Implemented, "control OUT buffer clear", ""};
		if (_opcode >= 0x72 && _opcode <= 0x7F)
		{
			const int ep = _opcode - 0x72 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " buffer clear";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 8. Unstall family
		if (_opcode == 0x80)
			return {CommandClass::Implemented, "control OUT unstall", ""};
		if (_opcode == 0x81)
			return {CommandClass::Implemented, "control IN unstall", ""};
		if (_opcode >= 0x82 && _opcode <= 0x8F)
		{
			const int ep = _opcode - 0x82 + 1;
			const std::string name = "endpoint " + std::to_string(ep) + " unstall";
			if (ep < 4)
				return {CommandClass::Implemented, name, ""};
			return {CommandClass::NotImplemented, name, ""};
		}

		// 9. Standard commands
		switch (_opcode)
		{
		case 0xF6: return {CommandClass::Implemented, "reset", ""};
		case 0xBA: return {CommandClass::Implemented, "write hardware configuration", ""};
		case 0xBB: return {CommandClass::Implemented, "read hardware configuration", ""};
		case 0xB8: return {CommandClass::Implemented, "write mode", ""};
		case 0xB9: return {CommandClass::Implemented, "read mode", ""};
		case 0xB6: return {CommandClass::Implemented, "write device address", ""};
		case 0xB7: return {CommandClass::Implemented, "read device address", ""};
		case 0xD2: return {CommandClass::Implemented, "peek", ""};
		case 0xC0: return {CommandClass::Implemented, "read interrupt register", ""};
		case 0xC2: return {CommandClass::Implemented, "write interrupt enable", ""};
		case 0xC3: return {CommandClass::Implemented, "read interrupt enable", ""};
		case 0xF4: return {CommandClass::Implemented, "acknowledge setup", ""};
		case 0xB5: return {CommandClass::Implemented, "chip identifier", ""};
		case 0xB4: return {CommandClass::Implemented, "frame number", ""};
		case 0xF0:
		case 0xF1:
		case 0xF2:
		case 0xF3: return {CommandClass::NotImplemented, "DMA", ""};
		default: return {CommandClass::Unspecified, "", ""};
		}
	}

	void Isp1181::beginRefused(const uint8_t _opcode)
	{
		m_pending = int(_opcode);
		m_transfer = Transfer::Refused;
		m_width = 0;
		m_index = 0;
	}

	void Isp1181::noteInterlock(const uint8_t _opcode, const std::string& _name)
	{
		note("isp1181: command 0x" + toHex(_opcode) + " (" + _name +
			 ") is disabled until the set-up packet is acknowledged with 0xF4; nothing is done");
		beginRefused(_opcode);
	}

	bool Isp1181::directionRefused(const uint8_t _opcode, const std::string& _name, const int _endpoint,
								   const size_t _index, const BufferDirection _needs)
	{
		if (directionOfBuffer(_index) == _needs)
			return false;

		const std::string want = (_needs == BufferDirection::In) ? "IN" : "OUT";
		const std::string held = (_needs == BufferDirection::In) ? "OUT" : "IN";
		const std::string bit = (_needs == BufferDirection::In) ? "0" : "1";

		note("isp1181: command 0x" + toHex(_opcode) + " (" + _name + ") addresses the " + want +
			 " buffer of endpoint " + std::to_string(_endpoint) + ", and EPDIR is " + bit +
			 " in its DcEndpointConfiguration - the endpoint is configured " + held +
			 "; the authority documents this access as unprotected and its result as unpredictable, so nothing is done");
		beginRefused(_opcode);
		return true;
	}

	uint8_t Isp1181::statusByte(const size_t _index) const
	{
		const size_t pending = m_fifos[_index].pending();
		uint8_t result = 0;
		if (m_stalled[_index])
			result |= 0x80u;
		if (pending >= 2)
			result |= 0x40u;
		if (pending >= 1)
			result |= 0x20u;
		if (_index == kOutFifoOfEndpoint0 && m_setupHeld)
			result |= 0x04u;
		return result;
	}

	void Isp1181::beginBufferRead(const uint8_t _opcode, const size_t _index)
	{
		const auto head = m_fifos[_index].peek();
		std::vector<uint8_t> bytes(2);
		if (head.first && !m_fifos[_index].isEmpty())
		{
			const auto& pkt = m_fifos[_index].front();
			bytes[0] = static_cast<uint8_t>(pkt.length & 0xFFu);
			bytes[1] = static_cast<uint8_t>((pkt.length >> 8) & 0xFFu);
			bytes.insert(bytes.end(), pkt.data.begin(), pkt.data.begin() + pkt.length);
		}
		else
		{
			bytes[0] = 0;
			bytes[1] = 0;
		}

		m_readBuf = std::move(bytes);
		m_pending = int(_opcode);
		m_transfer = Transfer::BufferRead;
		m_width = m_readBuf.size();
		m_index = 0;
	}

	void Isp1181::beginBufferWrite(const uint8_t _opcode, const size_t _index)
	{
		m_stage.clear();
		m_stageFifo = static_cast<int>(_index);
		m_pending = int(_opcode);
		m_transfer = Transfer::BufferWrite;
		m_width = 2 + m_fifos[_index].capacityBytes;
		m_index = 0;
	}

	void Isp1181::commitValidate(const int _endpoint, const size_t _index)
	{
		if (m_stageFifo != static_cast<int>(_index))
		{
			note("isp1181: a validate for " + g_fifoNames[_index] +
				 " found no buffer write staged for it; nothing is validated");
			return;
		}
		if (m_stage.size() < 2)
		{
			note("isp1181: a validate for " + g_fifoNames[_index] + " found " + std::to_string(m_stage.size()) +
				 " staged byte" + (m_stage.size() == 1 ? "" : "s") + " and the length prefix alone is 2; nothing is validated");
			m_stage.clear();
			m_stageFifo = -1;
			return;
		}

		const size_t declared = size_t(m_stage[0]) | (size_t(m_stage[1]) << 8);
		const size_t payloadLen = m_stage.size() - 2;
		std::vector<uint8_t> payload(m_stage.begin() + 2, m_stage.end());
		m_stage.clear();
		m_stageFifo = -1;

		if (declared != payloadLen)
		{
			note("isp1181: a validate for " + g_fifoNames[_index] + " declared " + std::to_string(declared) +
				 " byte" + (declared == 1 ? "" : "s") + " and " + std::to_string(payloadLen) +
				 " followed; nothing is validated");
			return;
		}

		queueIn(_endpoint, payload.data(), payload.size());
	}

	void Isp1181::beginTransfer(const uint8_t _opcode, const Transfer _kind, const size_t _width, const uint32_t _value)
	{
		m_pending = int(_opcode);
		m_transfer = _kind;
		m_width = _width;
		m_index = 0;
		m_latch = _value;
	}

	void Isp1181::writeCommand(const uint8_t _opcode)
	{
		const Command command = classify(_opcode);
		switch (command.cls)
		{
		case CommandClass::NotImplemented:
			note("isp1181: command 0x" + toHex(_opcode) + " (" + command.name + ") is not implemented; the read answers 0x00");
			beginRefused(_opcode);
			return;
		case CommandClass::Unspecified:
			note("isp1181: command 0x" + toHex(_opcode) + " is not in the specified command set; the read answers 0x00");
			beginRefused(_opcode);
			return;
		case CommandClass::Illegal:
			note("isp1181: command 0x" + toHex(_opcode) + " (" + command.name + ") is illegal - " + command.detail +
				 "; nothing is done");
			beginRefused(_opcode);
			return;
		case CommandClass::Implemented:
			break;
		}

		// Buffer read: 0x10, 0x12..0x14
		if (_opcode == 0x10 || (_opcode >= 0x12 && _opcode <= 0x14))
		{
			const int ep = (_opcode == 0x10) ? 0 : (_opcode - 0x12 + 1);
			const size_t index = g_outFifoOfEndpoint[ep];
			if (directionRefused(_opcode, command.name, ep, index, BufferDirection::Out))
				return;
			beginBufferRead(_opcode, index);
			return;
		}

		// Buffer clear: 0x70, 0x72..0x74
		if (_opcode == 0x70 || (_opcode >= 0x72 && _opcode <= 0x74))
		{
			const int ep = (_opcode == 0x70) ? 0 : (_opcode - 0x72 + 1);
			const size_t index = g_outFifoOfEndpoint[ep];
			if (directionRefused(_opcode, command.name, ep, index, BufferDirection::Out))
				return;
			if (index == kOutFifoOfEndpoint0 && m_setupUnacknowledged)
			{
				noteInterlock(_opcode, command.name);
				return;
			}
			m_fifos[index].take();
			beginTransfer(_opcode, Transfer::None, 0, 0);
			return;
		}

		// Buffer write: 0x01, 0x02..0x04
		if (_opcode == 0x01 || (_opcode >= 0x02 && _opcode <= 0x04))
		{
			const int ep = (_opcode == 0x01) ? 0 : (_opcode - 0x02 + 1);
			const size_t index = g_inBufferOfEndpoint[ep];
			if (directionRefused(_opcode, command.name, ep, index, BufferDirection::In))
				return;
			beginBufferWrite(_opcode, index);
			return;
		}

		// Buffer validate: 0x61, 0x62..0x64
		if (_opcode == 0x61 || (_opcode >= 0x62 && _opcode <= 0x64))
		{
			const int ep = (_opcode == 0x61) ? 0 : (_opcode - 0x62 + 1);
			const size_t index = g_inBufferOfEndpoint[ep];
			if (directionRefused(_opcode, command.name, ep, index, BufferDirection::In))
				return;
			if (index == kInFifoOfEndpoint0 && m_setupUnacknowledged)
			{
				noteInterlock(_opcode, command.name);
				return;
			}
			commitValidate(ep, index);
			beginTransfer(_opcode, Transfer::None, 0, 0);
			return;
		}

		// Stall: 0x40..0x4F
		if (_opcode >= 0x40 && _opcode <= 0x4F)
		{
			const size_t offset = _opcode - 0x40;
			const size_t index = (offset <= 1) ? offset : g_outFifoOfEndpoint[offset - 1];
			if (index < kFifoCount)
				m_stalled[index] = true;
			beginTransfer(_opcode, Transfer::None, 0, 0);
			return;
		}

		// Unstall: 0x80..0x8F
		if (_opcode >= 0x80 && _opcode <= 0x8F)
		{
			const size_t offset = _opcode - 0x80;
			const size_t index = (offset <= 1) ? offset : g_outFifoOfEndpoint[offset - 1];
			if (index < kFifoCount)
				m_stalled[index] = false;
			beginTransfer(_opcode, Transfer::None, 0, 0);
			return;
		}

		// Status: 0x50..0x5F
		if (_opcode >= 0x50 && _opcode <= 0x5F)
		{
			const size_t offset = _opcode - 0x50;
			const size_t index = (offset <= 1) ? offset : g_outFifoOfEndpoint[offset - 1];
			if (index < kFifoCount)
			{
				clearInterrupt(1u << g_interruptBitOfFifo[index]);
				beginTransfer(_opcode, Transfer::Read, 1, uint32_t(statusByte(index)));
			}
			else
			{
				beginTransfer(_opcode, Transfer::Read, 1, 0);
			}
			return;
		}

		// Configuration: 0x20..0x2F
		if (_opcode >= 0x20 && _opcode <= 0x2F)
		{
			m_selected = _opcode - 0x20;
			beginTransfer(_opcode, Transfer::Write, 1, 0);
			return;
		}

		// Standard registers
		switch (_opcode)
		{
		case 0xF6: // Reset
			clearState();
			beginTransfer(_opcode, Transfer::None, 0, 0);
			break;
		case 0xBA: // Write Hardware Config
			beginTransfer(_opcode, Transfer::Write, 2, 0);
			break;
		case 0xBB: // Read Hardware Config
			beginTransfer(_opcode, Transfer::Read, 2, uint32_t(m_hwConfig));
			break;
		case 0xB8: // Write Mode
			beginTransfer(_opcode, Transfer::Write, 1, 0);
			break;
		case 0xB9: // Read Mode
			beginTransfer(_opcode, Transfer::Read, 1, uint32_t(m_mode));
			break;
		case 0xB6: // Write Device Address
			beginTransfer(_opcode, Transfer::Write, 1, 0);
			break;
		case 0xB7: // Read Device Address
			beginTransfer(_opcode, Transfer::Read, 1, uint32_t(m_deviceAddress));
			break;
		case 0xC2: // Write Interrupt Enable
			beginTransfer(_opcode, Transfer::Write, 4, 0);
			break;
		case 0xC3: // Read Interrupt Enable
			beginTransfer(_opcode, Transfer::Read, 4, m_interruptEnable);
			break;
		case 0xC0: // Read Interrupt Register
			beginTransfer(_opcode, Transfer::Read, 4, m_interruptRegister);
			break;
		case 0xD2: // Peek
		{
			const size_t index = m_selected;
			if (index >= kFifoCount)
			{
				m_absentNote = "isp1181: peek follows the configuration of slot " + std::to_string(index) +
							   ", which this model carries no buffer for; the read answers 0x00";
				beginTransfer(_opcode, Transfer::Absent, 1, 0);
				return;
			}
			const auto head = m_fifos[index].peek();
			if (head.first)
			{
				beginTransfer(_opcode, Transfer::Read, 1, uint32_t(head.second));
			}
			else
			{
				m_absentNote = "isp1181: peek on " + g_fifoNames[index] + " found no packet; the read answers 0x00";
				beginTransfer(_opcode, Transfer::Absent, 1, 0);
			}
			break;
		}
		case 0xF4: // Acknowledge Setup
			m_setupUnacknowledged = false;
			m_setupHeld = false;
			beginTransfer(_opcode, Transfer::None, 0, 0);
			break;
		case 0xB5: // Chip ID (ISP1181A is 0x8141)
			beginTransfer(_opcode, Transfer::Read, 2, uint32_t(kChipIdA));
			break;
		case 0xB4: // Frame Number
			beginTransfer(_opcode, Transfer::Read, 2, uint32_t(m_frameNumber));
			break;
		default:
			break;
		}
	}

	void Isp1181::commitOperand()
	{
		switch (m_pending)
		{
		case 0xBA:
			m_hwConfig = static_cast<uint16_t>(m_latch & 0xFFFFu);
			break;
		case 0xB8:
			m_mode = static_cast<uint8_t>(m_latch & 0xFFu);
			break;
		case 0xB6:
			m_deviceAddress = static_cast<uint8_t>(m_latch & 0xFFu);
			break;
		case 0xC2:
			m_interruptEnable = m_latch;
			updateIrq();
			break;
		default:
			if (m_pending >= 0x20 && m_pending <= 0x2F)
			{
				const size_t slot = static_cast<size_t>(m_pending - 0x20);
				m_endpointConfig[slot] = static_cast<uint8_t>(m_latch & 0xFFu);
				m_configWritten[slot] = true;
				++m_events;
				m_configOrdinal[slot] = m_events;

				if (slot >= kFifoCount && (m_endpointConfig[slot] & kFifoEnableBit) != 0)
				{
					note("isp1181: configuration slot " + std::to_string(slot) + " was enabled with 0x" +
						 toHex(m_endpointConfig[slot]) +
						 " and this model carries no buffer for it; the register is recorded and no buffer memory is allocated");
				}
			}
			break;
		}
	}

	void Isp1181::writeData(const uint8_t _value)
	{
		if (m_transfer == Transfer::BufferWrite)
		{
			if (m_stage.size() >= m_width)
			{
				const Command cmd = classify(static_cast<uint8_t>(m_pending));
				note("isp1181: command 0x" + toHex(static_cast<uint8_t>(m_pending)) + " (" + cmd.name +
					 ") takes at most " + std::to_string(m_width) + " bytes and a further one was written; the byte is discarded");
				return;
			}
			m_stage.push_back(_value);
			return;
		}

		if (m_transfer == Transfer::Refused)
		{
			note("isp1181: command 0x" + toHex(static_cast<uint8_t>(m_pending)) + " was refused and takes no operand; the byte is discarded");
			return;
		}

		if (m_transfer != Transfer::Write)
		{
			note("isp1181: a data port write of 0x" + toHex(_value) + " arrived with no command pending; the byte is discarded");
			return;
		}

		if (m_index >= m_width)
		{
			const Command cmd = classify(static_cast<uint8_t>(m_pending));
			note("isp1181: command 0x" + toHex(static_cast<uint8_t>(m_pending)) + " (" + cmd.name +
				 ") takes " + std::to_string(m_width) + " byte" + (m_width == 1 ? "" : "s") +
				 " and a further byte was written; the byte is discarded");
			++m_index;
			return;
		}

		m_latch |= (static_cast<uint32_t>(_value) << (8 * m_index));
		++m_index;
		if (m_index == m_width)
			commitOperand();
	}

	uint8_t Isp1181::readData()
	{
		if (m_transfer == Transfer::Absent && m_index < m_width)
		{
			note(m_absentNote);
			++m_index;
			return kBenignValue;
		}

		if (m_transfer == Transfer::Refused)
		{
			note("isp1181: command 0x" + toHex(static_cast<uint8_t>(m_pending)) + " was refused; the read answers 0x00");
			return kBenignValue;
		}

		if (m_transfer != Transfer::Read && m_transfer != Transfer::Absent && m_transfer != Transfer::BufferRead)
		{
			note("isp1181: a data port read arrived with no command pending; the read answers 0x00");
			return kBenignValue;
		}

		if (m_index >= m_width)
		{
			const Command cmd = classify(static_cast<uint8_t>(m_pending));
			note("isp1181: command 0x" + toHex(static_cast<uint8_t>(m_pending)) + " (" + cmd.name +
				 ") yields " + std::to_string(m_width) + " byte" + (m_width == 1 ? "" : "s") +
				 " and a further byte was read; the read answers 0x00");
			++m_index;
			return kBenignValue;
		}

		if (m_transfer == Transfer::BufferRead)
		{
			const uint8_t b = m_readBuf[m_index++];
			return b;
		}

		const uint8_t b = static_cast<uint8_t>((m_latch >> (8 * m_index)) & 0xFFu);
		++m_index;
		return b;
	}

	uint8_t Isp1181::read(const uint32_t _addr)
	{
		if (m_backend == Backend::Stub)
			return kBenignValue;

		if ((_addr & kCommandSelect) != 0)
			return kBenignValue;

		return readData();
	}

	void Isp1181::write(const uint32_t _addr, const uint8_t _value)
	{
		if (m_backend == Backend::Stub)
			return;

		if ((_addr & kCommandSelect) != 0)
			writeCommand(_value);
		else
			writeData(_value);
	}

	bool Isp1181::deliver(const int _endpoint, const uint8_t* const _data, const size_t _len)
	{
		if (_endpoint < 0 || _endpoint >= static_cast<int>(g_outFifoOfEndpoint.size()))
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "isp1181: a packet reached endpoint %d, which this model does not implement; the packet is dropped", _endpoint);
			note(buf);
			return false;
		}

		const size_t index = g_outFifoOfEndpoint[_endpoint];
		if (directionOfBuffer(index) == BufferDirection::In)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "isp1181: endpoint %d is configured IN - EPDIR is 1 in its DcEndpointConfiguration - so it has no OUT buffer; the packet is dropped", _endpoint);
			note(buf);
			return false;
		}

		if (!m_fifos[index].accept(_data, _len))
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "isp1181: %s refused a packet of %zu bytes", g_fifoNames[index].c_str(), _len);
			note(buf);
			return false;
		}

		raiseInterrupt(1u << g_interruptBitOfFifo[index]);
		return true;
	}

	bool Isp1181::deliverSetup(const uint8_t* const _data, const size_t _len)
	{
		if (_len == 0)
		{
			note("isp1181: a set-up packet of zero bytes reached " + g_fifoNames[kOutFifoOfEndpoint0] +
				 " and a SETUP transaction carries eight; nothing is delivered");
			return false;
		}

		if (m_fifos[kOutFifoOfEndpoint0].isFull())
		{
			note("isp1181: a set-up packet reached " + g_fifoNames[kOutFifoOfEndpoint0] +
				 ", which already holds a packet; dropped rather than overwriting");
			return false;
		}

		if (!m_fifos[kOutFifoOfEndpoint0].accept(_data, _len))
		{
			note("isp1181: " + g_fifoNames[kOutFifoOfEndpoint0] + " refused a set-up packet of " + std::to_string(_len) + " bytes");
			return false;
		}

		m_fifos[kInFifoOfEndpoint0].clear();
		m_stalled[kOutFifoOfEndpoint0] = false;
		m_stalled[kInFifoOfEndpoint0] = false;
		m_setupHeld = true;
		m_setupUnacknowledged = true;
		raiseInterrupt(1u << g_interruptBitOfFifo[kOutFifoOfEndpoint0]);
		return true;
	}

	bool Isp1181::queueIn(const int _endpoint, const uint8_t* const _data, const size_t _len)
	{
		if (_endpoint < 0 || _endpoint >= static_cast<int>(g_inBufferOfEndpoint.size()))
		{
			note("isp1181: a transmit was queued for endpoint " + std::to_string(_endpoint) +
				 ", which this model does not implement; nothing is queued");
			return false;
		}

		const size_t index = g_inBufferOfEndpoint[_endpoint];
		if (directionOfBuffer(index) == BufferDirection::Out)
		{
			note("isp1181: endpoint " + std::to_string(_endpoint) +
				 " is configured OUT - EPDIR is 0 in its DcEndpointConfiguration - so it has no IN buffer; nothing is queued");
			return false;
		}

		if (_len == 0)
		{
			note("isp1181: an empty packet was queued for " + g_fifoNames[index] + "; nothing is queued");
			return false;
		}

		if (!m_fifos[index].accept(_data, _len))
		{
			note("isp1181: " + g_fifoNames[index] + " refused an IN packet of " + std::to_string(_len) + " bytes");
			return false;
		}

		return true;
	}

	bool Isp1181::transmit(const int _endpoint)
	{
		if (_endpoint < 0 || _endpoint >= static_cast<int>(g_inBufferOfEndpoint.size()))
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "isp1181: a transmit was requested for endpoint %d, which this model does not implement; nothing is transmitted", _endpoint);
			note(buf);
			return false;
		}

		const size_t index = g_inBufferOfEndpoint[_endpoint];
		if (directionOfBuffer(index) == BufferDirection::Out)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "isp1181: endpoint %d is configured OUT - EPDIR is 0 in its DcEndpointConfiguration - so it has no IN buffer; nothing is transmitted", _endpoint);
			note(buf);
			return false;
		}

		if (m_fifos[index].isEmpty())
		{
			return false;
		}

		if (m_fifos[index].front().length == 0)
		{
			return false;
		}

		const auto pkt = m_fifos[index].take();

		if (m_captureBuffer && m_captureMaxLen > 0)
		{
			const size_t copyLen = std::min(pkt.length, m_captureMaxLen);
			std::memcpy(m_captureBuffer, pkt.data.data(), copyLen);
			if (m_captureActualLen)
				*m_captureActualLen = pkt.length;
		}

		if (m_txCb)
			m_txCb(_endpoint, pkt.data.data(), pkt.length);
		if (m_rawTxCb)
			m_rawTxCb(m_user, _endpoint, pkt.data.data(), pkt.length);

		raiseInterrupt(1u << g_interruptBitOfFifo[index]);
		return true;
	}

	int Isp1181::rx(const int _endpoint, const uint8_t* const _data, const size_t _len)
	{
		if (m_backend == Backend::Stub || !_data || _len == 0)
			return 0;
		return deliver(_endpoint, _data, _len) ? 1 : 0;
	}

	int Isp1181::setup(const uint8_t* const _data, const size_t _len)
	{
		if (m_backend == Backend::Stub || !_data || _len == 0)
			return 0;
		return deliverSetup(_data, _len) ? 1 : 0;
	}

	int Isp1181::inToken(const int _endpoint)
	{
		if (m_backend == Backend::Stub)
			return 0;
		return transmit(_endpoint) ? 1 : 0;
	}

	int Isp1181::inToken(const int _endpoint, uint8_t* const _buffer, const size_t _maxLen, size_t* const _actualLen)
	{
		if (m_backend == Backend::Stub)
			return 0;

		m_captureBuffer = _buffer;
		m_captureMaxLen = _maxLen;
		m_captureActualLen = _actualLen;
		if (m_captureActualLen)
			*m_captureActualLen = 0;

		const int result = transmit(_endpoint) ? 1 : 0;

		m_captureBuffer = nullptr;
		m_captureMaxLen = 0;
		m_captureActualLen = nullptr;
		return result;
	}

	bool Isp1181::hasPendingIn(const int _endpoint) const
	{
		if (_endpoint < 0 || _endpoint >= static_cast<int>(g_inBufferOfEndpoint.size()))
			return false;
		const size_t index = g_inBufferOfEndpoint[_endpoint];
		return !m_fifos[index].isEmpty() && m_fifos[index].front().length > 0;
	}

	bool Isp1181::hasPendingIn() const
	{
		for (const auto ep : {0, 1, 2})
		{
			if (hasPendingIn(ep))
				return true;
		}
		return false;
	}

	void Isp1181::tick(const uint32_t _sofFrames)
	{
		if (m_ctx && m_ctx != reinterpret_cast<isp1181_ctx*>(this) && isp1181_tick)
			isp1181_tick(m_ctx, _sofFrames);

		m_frameNumber = static_cast<uint16_t>((m_frameNumber + _sofFrames) % kUsbFrameCount);
	}

	size_t Isp1181::getLogWritten() const
	{
		return m_written;
	}

	size_t Isp1181::getLogRetained() const
	{
		return m_logCount;
	}

	std::string Isp1181::getLogLine(const size_t _index) const
	{
		if (_index >= m_logCount || !m_logEntries || !m_logEntries[_index].text)
			return {};
		return m_logEntries[_index].text;
	}

	size_t Isp1181::getConfigSlots()
	{
		return kConfigSlotCount;
	}

	int Isp1181::getConfigSlot(const size_t _slot, uint8_t& _value) const
	{
		if (_slot >= kConfigSlotCount)
			return -1;
		if (!m_configWritten[_slot])
			return 0;
		_value = m_endpointConfig[_slot];
		return 1;
	}

	int Isp1181::getSlotBuffer(const size_t _slot, size_t& _maxPacketBytes, size_t& _bufferCount) const
	{
		if (_slot >= kConfigSlotCount)
			return -1;
		if (_slot < kControlSlotCount)
		{
			_maxPacketBytes = kControlFifoBytes;
			_bufferCount = kControlFifoBuffers;
			return 1;
		}
		if (_slot >= kFifoCount)
			return 0;
		if (!m_configWritten[_slot])
			return 0;

		const uint8_t cfg = m_endpointConfig[_slot];
		if ((cfg & kFifoEnableBit) == 0)
			return 0;
		if ((cfg & kFfoisoBit) != 0)
			return -1;

		const int bytes = nonIsoBufferBytes(cfg);
		if (bytes < 0)
			return -1;

		_maxPacketBytes = static_cast<size_t>(bytes);
		_bufferCount = (cfg & kDblbufBit) != 0 ? 2 : 1;
		return 1;
	}

	std::string Isp1181::getReport() const
	{
		std::ostringstream ss;
		const size_t dropped = (m_written > m_logCount) ? (m_written - m_logCount) : 0;

		ss << "isp1181: report begins\n";
		ss << "isp1181: log written=" << m_written << " retained=" << m_logCount << " dropped=" << dropped << "\n";
		if (dropped == 0)
			ss << "isp1181: log COMPLETE - every line the model wrote is below\n";
		else
			ss << "isp1181: log TRUNCATED - " << dropped << " lines the model wrote are NOT below.\n";

		ss << "isp1181: configuration slots=" << kConfigSlotCount << "\n";
		for (size_t slot = 0; slot < kConfigSlotCount; ++slot)
		{
			ss << "isp1181: config slot " << slot << " command 0x" << toHex(uint32_t(0x20 + slot)) << " "
			   << slotEndpointName(slot) << " " << slotBufferName(slot) << ": ";
			if (!m_configWritten[slot])
			{
				ss << "NEVER WRITTEN\n";
			}
			else
			{
				const uint8_t val = m_endpointConfig[slot];
				const bool epdir = (val & kEpdirBit) != 0;
				ss << "written 0x" << toHex(val) << " EPDIR=" << (epdir ? "1 IN" : "0 OUT") << " at event "
				   << m_configOrdinal[slot] << "\n";
			}
		}

		ss << "isp1181: log lines follow, " << m_logCount << " retained\n";
		for (size_t i = 0; i < m_logCount; ++i)
			ss << "isp1181: log[" << i << "] event " << m_logEntries[i].ordinal << ": " << m_logEntries[i].text << "\n";
		ss << "isp1181: report ends\n";

		return ss.str();
	}

	size_t Isp1181::getStateSize()
	{
		return 19;
	}

	void Isp1181::saveState(void* const _dst) const
	{
		if (!_dst)
			return;

		auto* const buf = static_cast<uint8_t*>(_dst);

		// Header (12 bytes)
		buf[0] = 'I';
		buf[1] = 'S';
		buf[2] = 'P';
		buf[3] = '1';

		buf[4] = 0;
		buf[5] = 0;
		buf[6] = 0;
		buf[7] = 1; // Version 1

		buf[8] = 0;
		buf[9] = 0;
		buf[10] = 0;
		buf[11] = 3; // Payload bytes

		// Payload (3 bytes)
		buf[12] = static_cast<uint8_t>(m_backend);
		buf[13] = static_cast<uint8_t>((m_frameNumber >> 8) & 0xFFu);
		buf[14] = static_cast<uint8_t>(m_frameNumber & 0xFFu);

		// Checksum (4 bytes)
		const uint32_t c = computeFnv1a(buf, 15);
		buf[15] = static_cast<uint8_t>((c >> 24) & 0xFFu);
		buf[16] = static_cast<uint8_t>((c >> 16) & 0xFFu);
		buf[17] = static_cast<uint8_t>((c >> 8) & 0xFFu);
		buf[18] = static_cast<uint8_t>(c & 0xFFu);
	}

	void Isp1181::loadState(const void* const _src)
	{
		if (!_src)
			return;

		const auto* const buf = static_cast<const uint8_t*>(_src);

		if (buf[0] != 'I' || buf[1] != 'S' || buf[2] != 'P' || buf[3] != '1')
			return;

		const uint32_t version = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) | (uint32_t(buf[6]) << 8) | buf[7];
		if (version != 1)
			return;

		const uint32_t payloadLen = (uint32_t(buf[8]) << 24) | (uint32_t(buf[9]) << 16) | (uint32_t(buf[10]) << 8) | buf[11];
		if (payloadLen != 3)
			return;

		const uint32_t expectedChecksum = (uint32_t(buf[15]) << 24) | (uint32_t(buf[16]) << 16) | (uint32_t(buf[17]) << 8) | buf[18];
		if (computeFnv1a(buf, 15) != expectedChecksum)
			return;

		if (buf[12] > 1)
			return;

		m_backend = static_cast<Backend>(buf[12]);
		m_frameNumber = (uint16_t(buf[13]) << 8) | buf[14];
	}
} // namespace hwLib
