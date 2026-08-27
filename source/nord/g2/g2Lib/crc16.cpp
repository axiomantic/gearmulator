/* crc16.cpp -- see crc16.h for the parameters and for the two coverages.
 *
 * The loop is bit-by-bit rather than table-driven. The protocol checksums a
 * few kilobytes at a time, so a 512-byte table would buy nothing measurable
 * and would put the parameter set in generated data instead of in a line a
 * reader can hold against design section 15.3.
 */

#include "crc16.h"

namespace g2
{
	namespace
	{
		constexpr uint16_t g_polynomial = 0x1021;
	}

	uint16_t crc16(const uint8_t* _data, const std::size_t _size) noexcept
	{
		uint16_t crc = 0x0000;

		for(std::size_t i = 0; i < _size; ++i)
		{
			crc ^= static_cast<uint16_t>(_data[i] << 8);

			for(int bit = 0; bit < 8; ++bit)
			{
				if(crc & 0x8000)
					crc = static_cast<uint16_t>((crc << 1) ^ g_polynomial);
				else
					crc = static_cast<uint16_t>(crc << 1);
			}
		}

		return crc;
	}

	uint16_t crc16Wire(const uint8_t* _message, const std::size_t _size) noexcept
	{
		if(_size <= 2)
			return 0x0000;

		return crc16(_message + 2, _size - 2);
	}

	uint16_t crc16File(const uint8_t* _file, const std::size_t _size, const std::size_t _binaryHeaderOffset) noexcept
	{
		if(_size < _binaryHeaderOffset + 2)
			return 0x0000;

		return crc16(_file + _binaryHeaderOffset, _size - _binaryHeaderOffset - 2);
	}

	void crc16Store(uint8_t* _dst, const uint16_t _crc) noexcept
	{
		_dst[0] = static_cast<uint8_t>(_crc >> 8);
		_dst[1] = static_cast<uint8_t>(_crc & 0xff);
	}

	uint16_t crc16Load(const uint8_t* _src) noexcept
	{
		return static_cast<uint16_t>((_src[0] << 8) | _src[1]);
	}
}
