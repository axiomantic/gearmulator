#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <type_traits>
#include "memoryMap.h"
#include "interruptController.h"
#include "flash.h"
#include "hdi08Adapter.h"
#include "mcf5307.h"

namespace g2
{
	/* BRD-21: The Board class.
	 * Concrete, non-polymorphic, non-copyable, non-movable, declared final.
	 */
	class Board final
	{
	public:
		Board(uint32_t _cs0Base = 0x30000000u, uint32_t _cs0Size = 0x00200000u,
		      uint32_t _cs2Base = 0x20000000u, uint32_t _cs2Size = 0x00200000u);
		~Board();

		Board(const Board&) = delete;
		Board& operator=(const Board&) = delete;
		Board(Board&&) = delete;
		Board& operator=(Board&&) = delete;

		uint32_t runMcu(uint32_t _cycles);
		bool faulted() const noexcept;
		void tickSofIfDue(uint64_t _frameIndex);

		size_t stateSize() const noexcept;
		void stateSave(uint8_t* _dst) const;
		void stateLoad(const uint8_t* _src);

		mcf5307_ctx* mcfContext() noexcept { return m_mcfCtx; }
		const mcf5307_ctx* mcfContext() const noexcept { return m_mcfCtx; }

		InterruptController& interruptController() noexcept { return m_intCtrl; }
		const InterruptController& interruptController() const noexcept { return m_intCtrl; }

		Flash& flash() noexcept { return m_flash; }
		const Flash& flash() const noexcept { return m_flash; }

		Hdi08Adapter& hdi08() noexcept { return m_hdi08; }
		const Hdi08Adapter& hdi08() const noexcept { return m_hdi08; }

	private:
		static uint32_t staticRead(void* _user, uint32_t _addr, int _size, mcf5307_bus_status* _status);
		static void staticWrite(void* _user, uint32_t _addr, int _size, uint32_t _val, mcf5307_bus_status* _status);
		static void staticIack(void* _user, int _level, uint8_t _vector);

		uint32_t handleRead(uint32_t _addr, int _size, mcf5307_bus_status* _status);
		void handleWrite(uint32_t _addr, int _size, uint32_t _val, mcf5307_bus_status* _status);
		void handleIack(int _level, uint8_t _vector);

		mcf5307_ctx* m_mcfCtx{nullptr};
		isp1181_ctx* m_usbCtx{nullptr};

		std::vector<uint8_t> m_ram;
		InterruptController m_intCtrl;
		Flash m_flash;
		Hdi08Adapter m_hdi08;
		bool m_faulted{false};
	};

	static_assert(!std::is_polymorphic_v<Board>, "Board must not be polymorphic");
	static_assert(!std::is_copy_constructible_v<Board>, "Board must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<Board>, "Board must not be copy assignable");
	static_assert(!std::is_move_constructible_v<Board>, "Board must not be move constructible");
	static_assert(!std::is_move_assignable_v<Board>, "Board must not be move assignable");
}
