// The panel.
//
// The panel display buffer and matrix scanning sit on CS4 (0x14000000,
// schematic ModularG2_MainBoard Sheet 3). Firmware scans matrix columns by
// writing a 16-bit walking zero word (0xFFFF7FFF, shifted right by 1 each step)
// to CS4. Sensed button return rows are reported on ColdFire parallel port
// PADAT (0x10000248).
//
// The panel board (schematic ModularG2_Panel Sheet 5) also hosts eight 74HC374
// octal latches addressed via CS5 for the 15-LED rings and encoder deltas.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace rg2
{
	class Latches;

	class Panel final : public BusTarget
	{
	public:
		static constexpr uint8_t kMaxRows = 8;
		static constexpr uint8_t kMaxCols = 16;
		static constexpr uint8_t kMaxLedRings = 8;
		static constexpr uint8_t kMaxEncoders = 8;

		explicit Panel(uint32_t _displaySize, Latches* _latches = nullptr);

		uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		void attachLatches(Latches* _latches) noexcept;

		// Button state management
		void setButtonPressed(uint8_t _row, uint8_t _col, bool _pressed) noexcept;
		bool isButtonPressed(uint8_t _row, uint8_t _col) const noexcept;

		// Encoder deltas
		void setEncoderDelta(uint8_t _encoderIndex, int8_t _delta) noexcept;
		int8_t getEncoderDelta(uint8_t _encoderIndex) const noexcept;

		// LED ring queries (updated by latch writes or direct setting)
		uint16_t getLedRingState(uint8_t _ringIndex) const noexcept;
		void setLedRingState(uint8_t _ringIndex, uint16_t _state) noexcept;

		// Matrix scanning: row bits for ColdFire PADAT (0x10000248) reads
		// getRowBits() returns active-low row bits (idle = 0xFFFF, pressed = 0)
		uint16_t getRowBits() const noexcept;
		uint16_t padatRowBits() const noexcept { return getRowBits(); }

		// Active-high row mask (idle = 0x0000, bit R = 1 when pressed)
		uint16_t getActiveRowMask() const noexcept;
		uint16_t getRowState() const noexcept { return getActiveRowMask(); }

		bool isRowActive(uint8_t _row) const noexcept;

		void tick(uint64_t _frameIndex) noexcept
		{
			(void)_frameIndex;
		}

		std::size_t stateSize() const noexcept
		{
			return 0;
		}

		void stateSave(void* _dst) const noexcept
		{
			(void)_dst;
		}

		void stateLoad(const void* _src) noexcept
		{
			(void)_src;
		}

	private:
		void updateMatrixScan(uint16_t _scanWord) noexcept;

		std::vector<uint8_t> m_display;
		Latches* m_latches = nullptr;

		bool m_buttons[kMaxRows][kMaxCols] = {};
		int8_t m_encoderDeltas[kMaxEncoders] = {};
		uint16_t m_ledRings[kMaxLedRings] = {};

		uint16_t m_lastScanWord = 0xFFFFu;
		uint16_t m_activeRowMask = 0u;
		uint16_t m_rowBits = 0xFFFFu;
	};
}
