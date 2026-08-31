/* CRC-16/CCITT, the XMODEM variant.
 *
 * Polynomial 0x1021, most significant bit first, initial value 0, no final
 * exclusive-or, stored big-endian.
 *
 * The coverage differs between the wire and the file, and that is why there
 * are two entry points rather than one:
 *
 *   - on the wire the CRC covers bytes 2 to n-1 and excludes the 2-byte
 *     length prefix. The exclusion is at the front and the trailing bytes are
 *     covered;
 *   - in a .pch2 file the same routine covers the version and type bytes and
 *     every chunk, and excludes only the trailing CRC. The exclusion is at
 *     the back and the ASCII header ahead of the version byte is not covered.
 *
 * A caller that reached for crc16() and passed a whole buffer would get a
 * value that is wrong for both cases and looks like neither.
 *
 * crc16File takes the offset of the version byte and does not look for it.
 * Finding the end of the ASCII header is a parse of the container, which this
 * module does not do.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace g2
{
	/* The parameter set applied to a range the caller has already chosen.
	 * An empty range returns the initial value, 0x0000. */
	uint16_t crc16(const uint8_t* _data, std::size_t _size) noexcept;

	/* The wire coverage: bytes 2 to _size-1 of a message whose first two
	 * bytes are the length prefix. A message of two bytes or fewer covers
	 * nothing and returns 0x0000. */
	uint16_t crc16Wire(const uint8_t* _message, std::size_t _size) noexcept;

	/* The file coverage: from _binaryHeaderOffset -- the version byte, which
	 * the type byte and then every chunk follow -- to the byte before the
	 * 2-byte stored CRC that ends the file. A file with nothing between the
	 * two covers nothing and returns 0x0000. */
	uint16_t crc16File(const uint8_t* _file, std::size_t _size, std::size_t _binaryHeaderOffset) noexcept;

	/* The storage order, big-endian, most significant byte first. */
	void crc16Store(uint8_t* _dst, uint16_t _crc) noexcept;
	uint16_t crc16Load(const uint8_t* _src) noexcept;
}
