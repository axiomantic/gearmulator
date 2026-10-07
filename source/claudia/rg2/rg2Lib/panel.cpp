// The panel.
//
// The panel display buffer and matrix scanning sit on CS4 (0x14000000,
// schematic ModularG2_MainBoard Sheet 3). Firmware scans matrix columns by
// writing a 16-bit walking zero word (0xFFFF7FFF, shifted right by 1 each step)
// to CS4. Sensed button return rows are reported on ColdFire parallel port
// PADAT (0x10000248).
//
// A freshly built panel reads zero everywhere, which is the quiescent report:
// no key down, no encoder moving, no button pressed.

#include "panel.h"
#include "latches.h"

namespace rg2
{
	namespace
	{
		bool isLegalWidth(const int _size) { return _size == 8 || _size == 16 || _size == 32; }
	} // namespace

	Panel::Panel(const uint32_t _displaySize, Latches* const _latches) :
		m_display(_displaySize, 0u), m_latches(_latches)
	{
		if (m_latches != nullptr)
			m_latches->attachPanel(this);
	}

	void Panel::attachLatches(Latches* const _latches) noexcept
	{
		m_latches = _latches;
		if (m_latches != nullptr)
			m_latches->attachPanel(this);
	}

	void Panel::setButtonPressed(const uint8_t _row, const uint8_t _col, const bool _pressed) noexcept
	{
		if (_row < kMaxRows && _col < kMaxCols)
		{
			m_buttons[_row][_col] = _pressed;
			if (_pressed)
				m_buttonRowMask[_row] |= uint16_t(1u << (15u - _col));
			else
				m_buttonRowMask[_row] &= uint16_t(~(1u << (15u - _col)));

			uint16_t any = 0;
			for (uint8_t r = 0; r < kMaxRows; ++r)
				any |= m_buttonRowMask[r];
			m_hasAnyButtonPressed = (any != 0);

			updateMatrixScan(m_lastScanWord);
		}
	}

	void Panel::setButtonState(const uint8_t _col, const uint8_t _row, const bool _isDown) noexcept
	{
		setButtonPressed(_row, _col, _isDown);
	}

	bool Panel::isButtonPressed(const uint8_t _row, const uint8_t _col) const noexcept
	{
		if (_row < kMaxRows && _col < kMaxCols)
			return m_buttons[_row][_col];
		return false;
	}

	void Panel::setEncoderDelta(const uint8_t _encoderIndex, const int8_t _delta) noexcept
	{
		if (_encoderIndex < kMaxEncoders)
		{
			m_encoderDeltas[_encoderIndex] = _delta;
			if (m_latches != nullptr)
				m_latches->setEncoderDelta(_encoderIndex, _delta);
		}
	}

	int8_t Panel::getEncoderDelta(const uint8_t _encoderIndex) const noexcept
	{
		if (_encoderIndex < kMaxEncoders)
			return m_encoderDeltas[_encoderIndex];
		return 0;
	}

	uint16_t Panel::getLedRingState(const uint8_t _ringIndex) const noexcept
	{
		if (m_latches != nullptr)
			return m_latches->getLedRingState(_ringIndex);
		if (_ringIndex < kMaxLedRings)
			return m_ledRings[_ringIndex];
		return 0u;
	}

	void Panel::setLedRingState(const uint8_t _ringIndex, const uint16_t _state) noexcept
	{
		if (_ringIndex < kMaxLedRings)
		{
			m_ledRings[_ringIndex] = _state;
			if (m_latches != nullptr)
				m_latches->setLedRingState(_ringIndex, _state);
		}
	}

	uint16_t Panel::getRowBits() const noexcept { return m_rowBits; }

	uint16_t Panel::getActiveRowMask() const noexcept { return m_activeRowMask; }

	bool Panel::isRowActive(const uint8_t _row) const noexcept
	{
		if (_row < kMaxRows)
			return (m_activeRowMask & (1u << _row)) != 0u;
		return false;
	}

	void Panel::updateMatrixScan(const uint16_t _scanWord) noexcept
	{
		m_lastScanWord = _scanWord;
		m_activeRowMask = 0u;
		m_rowBits = 0xFFFFu;

		if (!m_hasAnyButtonPressed)
			return;

		const uint16_t activeCols = uint16_t(~_scanWord);
		for (uint8_t r = 0; r < kMaxRows; ++r)
		{
			if ((m_buttonRowMask[r] & activeCols) != 0u)
			{
				m_activeRowMask |= uint16_t(1u << r);
				m_rowBits &= uint16_t(~(1u << r));
			}
		}
	}

	uint32_t Panel::read(const uint32_t _offset, const int _size, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if (!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			return 0;
		}

		// The part is big-endian. A byte beyond the window reads zero rather
		// than reading past the end of the model.
		const uint32_t bytes = uint32_t(_size) / 8u;
		uint32_t value = 0;

		for (uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			value <<= 8;
			if (index < m_display.size())
				value |= m_display[index];
		}

		return value;
	}

	void Panel::write(const uint32_t _offset, const int _size, const uint32_t _value, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if (!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			return;
		}

		if (_offset < 4u)
		{
			const uint16_t scanWord = uint16_t(_value & 0xFFFFu);
			updateMatrixScan(scanWord);
		}

		const uint32_t bytes = uint32_t(_size) / 8u;
		bool modified = false;

		for (uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			if (index >= m_display.size())
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			const uint8_t val = uint8_t((_value >> shift) & 0xffu);
			if (m_display[index] != val)
			{
				m_display[index] = val;
				modified = true;
			}
		}

		if (modified)
			++m_displayVersion;
	}

	std::string_view Panel::readDisplayView(const uint32_t _offset, const uint32_t _length) const noexcept
	{
		if (_offset >= m_display.size())
			return {};
		const size_t avail = m_display.size() - _offset;
		const size_t len = (static_cast<size_t>(_length) <= avail) ? static_cast<size_t>(_length) : avail;
		return std::string_view(reinterpret_cast<const char*>(m_display.data() + _offset), len);
	}

	std::string Panel::readDisplayText(const uint32_t _offset, const uint32_t _length) const
	{
		return std::string(readDisplayView(_offset, _length));
	}
} // namespace rg2
