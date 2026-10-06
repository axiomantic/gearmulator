// The CS5 latches.
//
// The panel identifier is a strap and not a register: Clavia's service manual
// records the model as two 0-ohm resistors, R79 and R80, on the panel board. A
// write reaches the identifier bits and changes nothing, and the model presents
// the same machine for the whole run.
//
// Panel Board Sheet 5: U6 (74AC138) decodes CS5 and A0..A2 into 8 strobes:
// - Latch 0 (offset 0): Model ID / strap bits in 5:4.
// - Latches 1..7 (offsets 1..7): Output latches for driving the 15-LED rings
//   and encoder delta multiplexing.

#include "latches.h"
#include "panel.h"

namespace rg2
{
	namespace
	{
		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}
	}

	Latches::Latches(const uint32_t _windowSize, const Model _model, Panel* const _panel)
		: m_latch(_windowSize, 0u)
		, m_panel(_panel)
	{
		if(m_latch.size() > g_panelIdentifierOffset)
			m_latch[g_panelIdentifierOffset] = panelIdentifierByte(_model);
	}

	void Latches::attachPanel(Panel* const _panel) noexcept
	{
		m_panel = _panel;
	}

	void Latches::setEncoderDelta(const uint8_t _encoderIndex, const int8_t _delta) noexcept
	{
		if(_encoderIndex < kMaxEncoders)
		{
			m_encoderDeltas[_encoderIndex] = _delta;
			const size_t latchIdx = size_t(_encoderIndex) + 1u;
			if(latchIdx < m_latch.size())
				m_latch[latchIdx] = uint8_t(_delta);
		}
	}

	int8_t Latches::getEncoderDelta(const uint8_t _encoderIndex) const noexcept
	{
		if(_encoderIndex < kMaxEncoders)
			return m_encoderDeltas[_encoderIndex];
		return 0;
	}

	uint16_t Latches::getLedRingState(const uint8_t _ringIndex) const noexcept
	{
		if(_ringIndex < kMaxLedRings)
			return m_ledRings[_ringIndex];
		return 0u;
	}

	void Latches::setLedRingState(const uint8_t _ringIndex, const uint16_t _state) noexcept
	{
		if(_ringIndex < kMaxLedRings)
			m_ledRings[_ringIndex] = _state;
	}

	uint32_t Latches::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			return 0;
		}

		// The part is big-endian. A byte beyond the window reads zero rather
		// than reading past the end of the model.
		const uint32_t bytes = uint32_t(_size) / 8u;
		uint32_t value = 0;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			value <<= 8;
			if(index < m_latch.size())
			{
				if(index >= 1u && index <= 7u && m_encoderDeltas[index - 1u] != 0)
					value |= uint8_t(m_encoderDeltas[index - 1u]);
				else
					value |= m_latch[index];
			}
		}

		return value;
	}

	void Latches::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			return;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			if(index >= m_latch.size())
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			const uint8_t incoming = uint8_t((_value >> shift) & 0xffu);

			if(index == g_panelIdentifierOffset)
			{
				// The identifier bits are strapped. Everything else in the
				// first byte is an output latch like any other.
				m_latch[index] = uint8_t((m_latch[index] & g_panelIdentifierMask)
					| (incoming & uint8_t(~g_panelIdentifierMask)));
				continue;
			}

			m_latch[index] = incoming;

			if(index >= 1u && index <= 7u)
			{
				const uint8_t ringIdx = uint8_t(index - 1u);
				if(_size == 16 && byte == 0)
					m_ledRings[ringIdx] = uint16_t(_value & 0xffffu);
				else
					m_ledRings[ringIdx] = incoming;

				if(m_panel != nullptr)
					m_panel->setLedRingState(ringIdx, m_ledRings[ringIdx]);
			}
		}
	}
}
