// The CS5 latches.
//
// The CS5 latch window sits at 0x15000000, decoded on the panel board
// (schematic ModularG2_Panel Sheet 5) by U6 (74AC138) driven by CS5 and
// address lines A0..A2. The 3-to-8 decoder provides eight strobes driving
// 74HC374 octal latches:
// - Latch 0 (offset 0): holds the panel model identifier strap in bits 5:4.
//   Writes cannot alter the strap bits (R79/R80 resistors).
// - Latches 1..7 (offsets 1..7): output latches driving 15-LED encoder rings
//   and encoder delta multiplexing.
//
// Mainboard connector P7 connects to panel connector P1 via a 26-pin ribbon
// cable carrying CS5 on pin 13, A0..A2 on pins 14..16, and D24..D31 on pins
// 18..25 (schematic ModularG2_MainBoard Sheet 3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memoryMap.h"
#include "model.h"

namespace rg2
{
	// The panel identifier sits in bits 5:4 of the first latch. model.h carries
	// the map from a Model to those two bits, and what the OS does with each.
	constexpr uint32_t g_panelIdentifierOffset = 0u;
	constexpr int g_panelIdentifierShift = 4;
	constexpr uint8_t g_panelIdentifierMask = uint8_t(0x3u << g_panelIdentifierShift);

	constexpr uint8_t panelIdentifierByte(const Model _model)
	{
		return uint8_t(panelStrapCode(_model) << g_panelIdentifierShift);
	}

	class Panel;

	class Latches final : public BusTarget
	{
	public:
		static constexpr size_t kMaxEncoders = 8;
		static constexpr size_t kMaxLedRings = 8;

		explicit Latches(uint32_t _windowSize, Model _model = Model::G2X, Panel* _panel = nullptr);

		uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) override;

		void attachPanel(Panel* _panel) noexcept;

		void setEncoderDelta(uint8_t _encoderIndex, int8_t _delta) noexcept;
		int8_t getEncoderDelta(uint8_t _encoderIndex) const noexcept;

		uint16_t getLedRingState(uint8_t _ringIndex) const noexcept;
		void setLedRingState(uint8_t _ringIndex, uint16_t _state) noexcept;

	private:
		std::vector<uint8_t> m_latch;
		Panel* m_panel = nullptr;
		int8_t m_encoderDeltas[kMaxEncoders] = {};
		uint16_t m_ledRings[kMaxLedRings] = {};
	};
}
