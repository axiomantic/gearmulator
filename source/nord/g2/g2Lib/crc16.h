/* crc16.h -- CRC-16/CCITT, the XMODEM variant. Task PROTO-1.
 * Design sections 15.3, 15.7 and 18.2.
 *
 * THE PARAMETERS. Polynomial 0x1021, most significant bit first, initial
 * value 0, no final exclusive-or, stored big-endian. One routine computes
 * them; nothing here is configurable, because the protocol uses one set.
 *
 * THE COVERAGE DIFFERS BETWEEN THE WIRE AND THE FILE, AND THAT IS WHY THERE
 * ARE TWO ENTRY POINTS RATHER THAN ONE. Design section 15.3 states both:
 *
 *   - on the WIRE the CRC covers bytes 2 to n-1 and EXCLUDES the 2-byte
 *     length prefix. The exclusion is at the FRONT and the trailing bytes are
 *     covered;
 *   - in a .pch2 FILE the same routine covers the version and type bytes and
 *     every chunk, and excludes only the trailing CRC. The exclusion is at
 *     the BACK and the ASCII header ahead of the version byte is not covered.
 *
 * A caller that reached for crc16() and passed a whole buffer would get a
 * value that is wrong for BOTH cases and looks like neither, so the two ranges
 * are named here and not left to each call site.
 *
 * crc16File TAKES THE OFFSET OF THE VERSION BYTE AND DOES NOT LOOK FOR IT.
 * Finding the end of the ASCII header is a parse of the container, which this
 * module does not do and which task PROTO-1 does not declare. A checksum
 * module that guessed at a header terminator would put a parse decision
 * somewhere no parser test would reach it.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace g2
{
	/* The parameter set applied to a range the caller has already chosen.
	 * An empty range returns the initial value, 0x0000. */
	uint16_t crc16(const uint8_t* _data, std::size_t _size) noexcept;

	/* The WIRE coverage: bytes 2 to _size-1 of a message whose first two
	 * bytes are the length prefix. A message of two bytes or fewer covers
	 * nothing and returns 0x0000. */
	uint16_t crc16Wire(const uint8_t* _message, std::size_t _size) noexcept;

	/* The FILE coverage: from _binaryHeaderOffset -- the version byte, which
	 * the type byte and then every chunk follow -- to the byte before the
	 * 2-byte stored CRC that ends the file. A file with nothing between the
	 * two covers nothing and returns 0x0000. */
	uint16_t crc16File(const uint8_t* _file, std::size_t _size, std::size_t _binaryHeaderOffset) noexcept;

	/* The storage order, big-endian, most significant byte first. */
	void crc16Store(uint8_t* _dst, uint16_t _crc) noexcept;
	uint16_t crc16Load(const uint8_t* _src) noexcept;
}
