#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "busTarget.h"
#include "timer.h"

namespace coldfire
{
	// The MBAR window this model answers. The programming model runs to
	// MBAR+$3D4, so one kilobyte covers all of it.
	constexpr uint32_t g_simSpaceSize = 0x400u;

	// UIPCR1, the one UART offset this model answers, because the firmware
	// reads its bit 0 as the Engine strap.
	constexpr uint32_t g_simUipcrOffset = 0x1D0u;

	class Sim final : public BusTarget
	{
	public:
		using PortAReadHook = std::function<uint16_t()>;

		explicit Sim(bool _engineStrap = false);

		template<typename ModelType, typename = std::enable_if_t<!std::is_same_v<std::decay_t<ModelType>, bool>>>
		explicit Sim(ModelType _model)
			: Sim(bool(isEngineStrapSet(_model)))
		{
		}

		uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		const std::vector<std::string>& log() const { return m_log; }
		void clearLog() { m_log.clear(); }

		Timer& timer1() { return m_timer1; }
		Timer& timer2() { return m_timer2; }

		void advanceTimers(uint32_t _inputClocks);

		void setInterruptController(InterruptController* _interrupts);

		void setPortAReadHook(PortAReadHook _hook) { m_portAReadHook = std::move(_hook); }

		template<typename PanelType>
		void setPanel(PanelType* _panel)
		{
			if(_panel)
				m_portAReadHook = [_panel]() { return _panel->getRowBits(); };
			else
				m_portAReadHook = nullptr;
		}

	private:
		void logLine(const char* _reason, bool _isWrite, int _size, uint32_t _offset);

		Timer* timerForByte(uint32_t _index, uint32_t& _blockOffset);

		uint8_t m_space[g_simSpaceSize] = {};
		uint8_t m_writeProtect[g_simSpaceSize] = {};

		Timer m_timer1{Timer::gTimer1InterruptIndex};
		Timer m_timer2{Timer::gTimer2InterruptIndex};

		PortAReadHook m_portAReadHook;

		std::vector<std::string> m_log;
	};
}
