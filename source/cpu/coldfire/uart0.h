#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "busTarget.h"
#include "interruptController.h"

namespace coldfire
{
	class Uart0 final : public BusTarget
	{
	public:
		// Offsets are MBAR-relative; the decode produces them.
		static constexpr uint32_t gUart0Base = 0x1C0u;
		static constexpr uint32_t gUart1Base = 0x200u;
		static constexpr uint32_t gUartModuleSize = 0x40u;

		static constexpr uint8_t gUart0Vector = 0x42u;
		static constexpr int gUart0InterruptIndex = 4; // ICR4, UM Table 8-2

		static constexpr uint16_t gBaudDivider = 0x0036u;

		static constexpr uint8_t gUmr18n1 = 0x0Bu;
		static constexpr uint8_t gUmr28n1 = 0x07u;

		using MidiOutFn = void (*)(void* _user, uint8_t _byte);

		explicit Uart0(InterruptController* _interrupts = nullptr);

		void setInterruptController(InterruptController* _interrupts);

		uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		void setMidiOut(MidiOutFn _fn, void* _user);
		void receive(uint8_t _byte);
		void transmitComplete();

		uint8_t usr() const;
		bool interruptAsserted() const;

		uint8_t uivr() const { return m_uivr; }

		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

	private:
		struct UartLoc
		{
			bool inModule = false;
			bool uart1 = false;
			uint32_t local = 0;
		};

		UartLoc locate(uint32_t _offset) const;
		bool isByteAccess(int _size) const { return _size == 8; }
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		uint8_t readUart0(uint32_t _local);
		void writeUart0(uint32_t _local, uint8_t _value);
		uint8_t readReset(uint32_t _local) const;

		void recomputeInterrupt();
		void setPending(bool _asserted);

		uint8_t m_rxFifo[4] = {};
		int m_rxFifoCount = 0;
		int m_rxFifoHead = 0;

		bool m_txEnabled = false;
		bool m_txHoldingValid = false;
		uint8_t m_txHolding = 0;

		bool m_rxEnabled = false;

		uint8_t m_umr1 = 0x00u;
		uint8_t m_umr2 = 0x00u;
		bool m_modeUmr1 = true;
		uint8_t m_ucsr = 0x00u;
		uint8_t m_uacr = 0x00u;
		uint8_t m_uimr = 0x00u;
		uint8_t m_ubg1 = 0x00u;
		uint8_t m_ubg2 = 0x00u;
		uint8_t m_uivr = 0x0Fu;

		bool m_interruptAsserted = false;

		InterruptController* m_interrupts = nullptr;
		MidiOutFn m_midiOut = nullptr;
		void* m_midiOutUser = nullptr;

		std::vector<std::string> m_log;
	};
}
