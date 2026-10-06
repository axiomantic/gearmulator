#pragma once

#include <cstdint>
#include <type_traits>

#include "busTarget.h"
#include "interruptController.h"
#include "sim.h"
#include "uart0.h"

namespace coldfire
{
	class ColdfireSoc : public BusTarget
	{
	public:
		static constexpr uint32_t gMbusBase = 0x280u;
		static constexpr uint32_t gMbusSize = 0x14u;

		explicit ColdfireSoc(void* _irqUser = nullptr, InterruptPresentFn _irqPresent = nullptr, bool _engineStrap = false);

		template<typename ModelType, typename = std::enable_if_t<!std::is_same_v<std::decay_t<ModelType>, bool>>>
		explicit ColdfireSoc(void* _irqUser, InterruptPresentFn _irqPresent, ModelType _model)
			: ColdfireSoc(_irqUser, _irqPresent, bool(isEngineStrapSet(_model)))
		{
		}

		ColdfireSoc(Sim& _sim, Uart0& _uart0, BusTarget& _mbus, InterruptController& _interrupts);
		ColdfireSoc(Sim& _sim, Uart0& _uart0, BusTarget* _mbus, InterruptController& _interrupts);

		InterruptController& interrupts() { return *m_interrupts; }
		const InterruptController& interrupts() const { return *m_interrupts; }

		Sim& sim() { return *m_sim; }
		const Sim& sim() const { return *m_sim; }

		Uart0& uart0() { return *m_uart0; }
		const Uart0& uart0() const { return *m_uart0; }

		void setMBus(BusTarget* _mbus) { m_mbus = _mbus; }
		BusTarget* mbus() const { return m_mbus; }

		void advanceTimers(uint32_t _inputClocks) { m_sim->advanceTimers(_inputClocks); }

		uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

		static bool isUartOwned(uint32_t _offset);
		static bool isMbusOwned(uint32_t _offset);
		static bool isInterruptOwned(uint32_t _offset);

		BusTarget& select(uint32_t _offset);

	private:
		InterruptController  m_ownedIntc;
		Sim                  m_ownedSim;
		Uart0                m_ownedUart0;

		InterruptController* m_interrupts = nullptr;
		Sim*                 m_sim = nullptr;
		Uart0*               m_uart0 = nullptr;
		BusTarget*           m_mbus = nullptr;
	};
}
