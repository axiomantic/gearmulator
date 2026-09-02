/* CRC-16/CCITT (XMODEM) against known vectors.
 * Tier T0: no artifact, no firmware, no file outside this repository.
 *
 * The coverage differs between the wire and the file and the two are tested
 * separately, which is the whole reason this file exists rather than one
 * vector list:
 *
 *   - on the wire the CRC covers bytes 2 to n-1 and excludes the 2-byte
 *     length prefix -- an exclusion at the front;
 *   - in a .pch2 file the same routine covers the version and type bytes and
 *     every chunk and excludes only the trailing CRC -- an exclusion at the
 *     back.
 *
 * A single routine that silently applied one range to both cases would pass a
 * vector list and be wrong. The case named "wire and file over one buffer"
 * below feeds one array to both entry points and pins both results, so that
 * mistake is a red test and not a matter of reading.
 *
 * 0x31C3 is the published catalogue check value of CRC-16/XMODEM for the ASCII
 * string "123456789". Every other constant here was computed by a bit-by-bit
 * implementation in another language that reproduces that published value, and
 * is written out as a literal.
 *
 * The vectors are built so that the covered range is exactly "123456789" in
 * the wire case and in the file case. A routine that included the length
 * prefix, or the ASCII header, or the trailing CRC, would not land on 0x31C3
 * -- so the expected value tests the range and not only the polynomial.
 */

#include "../crc16.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const std::string& _what)
	{
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	void checkCrc(const uint16_t _actual, const uint16_t _expected, const std::string& _what)
	{
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s (expected 0x%04X, got 0x%04X)", _what.c_str(), _expected, _actual);
		check(_actual == _expected, buf);
	}

	std::vector<uint8_t> bytes(const std::string& _ascii)
	{
		return std::vector<uint8_t>(_ascii.begin(), _ascii.end());
	}

	/* The catalogue check string, and the parameter set's anchor. */
	const std::string g_check = "123456789";

	/* A wire message: 2-byte length prefix, then a 9-byte payload. The
	 * covered range is the payload, so the expected value is the anchor. */
	std::vector<uint8_t> wireMessage()
	{
		std::vector<uint8_t> m{0x00, 0x09};
		const std::vector<uint8_t> payload = bytes(g_check);
		m.insert(m.end(), payload.begin(), payload.end());
		return m;
	}

	/* A .pch2 image in miniature: an ASCII CRLF header terminated by a zero
	 * byte, then the version and type bytes, then chunk bytes, then the
	 * stored CRC. The version, type and chunk bytes spell the anchor string,
	 * so the covered range's expected value is 0x31C3 and the stored CRC is
	 * 0x31 0xC3 big-endian. */
	const size_t g_fileHeaderSize = 13;

	std::vector<uint8_t> fileImage()
	{
		std::vector<uint8_t> f = bytes("Version=23\r\n");
		f.push_back(0x00);
		const std::vector<uint8_t> covered = bytes(g_check);
		f.insert(f.end(), covered.begin(), covered.end());
		f.push_back(0x31);
		f.push_back(0xC3);
		return f;
	}

	void testCoreParameters()
	{
		const std::vector<uint8_t> anchor = bytes(g_check);
		checkCrc(g2::crc16(anchor.data(), anchor.size()), 0x31C3, "core: the published XMODEM check value for \"123456789\"");

		const uint8_t nothing = 0;
		checkCrc(g2::crc16(&nothing, 0), 0x0000, "core: an empty range is the initial value, 0x0000");

		const uint8_t zero = 0x00;
		checkCrc(g2::crc16(&zero, 1), 0x0000, "core: a leading zero byte does not move an initial value of 0");

		/* Bit order. With the most significant bit first, 0x80 shifts the
		 * polynomial in at once; a least-significant-bit-first routine
		 * cannot produce this value. */
		const uint8_t msb = 0x80;
		checkCrc(g2::crc16(&msb, 1), 0x9188, "core: 0x80 alone pins most-significant-bit-first order");
	}

	void testWireCoverage()
	{
		const std::vector<uint8_t> m = wireMessage();

		checkCrc(g2::crc16Wire(m.data(), m.size()), 0x31C3, "wire: covers bytes 2 to n-1, excluding the 2-byte length prefix");

		/* The same buffer with the prefix included, which is the defect this
		 * case exists to catch. The two values are different, so the
		 * assertion above cannot pass by accident. */
		checkCrc(g2::crc16(m.data(), m.size()), 0x14CD, "wire: including the length prefix gives a different value");
		check(g2::crc16Wire(m.data(), m.size()) != g2::crc16(m.data(), m.size()), "wire: the prefix exclusion is observable");

		/* A prefix with no payload leaves an empty range. */
		const std::vector<uint8_t> empty{0x00, 0x00};
		checkCrc(g2::crc16Wire(empty.data(), empty.size()), 0x0000, "wire: a message that is only the length prefix covers nothing");
	}

	void testFileCoverage()
	{
		const std::vector<uint8_t> f = fileImage();

		checkCrc(g2::crc16File(f.data(), f.size(), g_fileHeaderSize), 0x31C3,
			"file: covers the version and type bytes and every chunk, excluding the trailing CRC");

		/* The two ends of the file range, each shown to be a real boundary. */
		checkCrc(g2::crc16(f.data(), f.size() - 2), 0x2345, "file: including the ASCII header gives a different value");
		checkCrc(g2::crc16(f.data() + g_fileHeaderSize, f.size() - g_fileHeaderSize), 0x0000,
			"file: including the stored CRC gives a different value");

		/* The stored CRC is big-endian in the last two bytes, and it agrees
		 * with the computed one. */
		checkCrc(g2::crc16Load(f.data() + f.size() - 2), 0x31C3, "file: the stored CRC is read big-endian from the last two bytes");
		check(g2::crc16File(f.data(), f.size(), g_fileHeaderSize) == g2::crc16Load(f.data() + f.size() - 2),
			"file: a good image verifies");

		/* A file that carries nothing between the header and the stored CRC
		 * leaves an empty range. */
		const std::vector<uint8_t> bare{'V', 0x00, 0x00, 0x00};
		checkCrc(g2::crc16File(bare.data(), bare.size(), 2), 0x0000, "file: an image with no version, type or chunk covers nothing");
	}

	void testWireAndFileOverOneBuffer()
	{
		/* One array, both entry points, both results pinned. The wire form
		 * drops the first two bytes and keeps the last two; the file form
		 * drops the last two and keeps whatever precedes them from the given
		 * offset. Neither value may be produced by the other's range. */
		const std::vector<uint8_t> shared{0x00, 0x09, '1', '2', '3', '4', '5', '6', '7', '8', '9', 0xAA, 0x55};

		checkCrc(g2::crc16Wire(shared.data(), shared.size()), 0x276E, "one buffer: the wire form keeps the trailing two bytes");
		checkCrc(g2::crc16File(shared.data(), shared.size(), 2), 0x31C3, "one buffer: the file form drops the trailing two bytes");
		check(g2::crc16Wire(shared.data(), shared.size()) != g2::crc16File(shared.data(), shared.size(), 2),
			"one buffer: the wire coverage and the file coverage are not the same range");
	}

	void testPlantedCorruption()
	{
		/* A checksum that matches by luck and one that is correct look
		 * identical until something is perturbed. Each flip below is checked
		 * against the verdict a loader would read. */
		std::vector<uint8_t> f = fileImage();

		f[3] ^= 0x01;
		check(g2::crc16File(f.data(), f.size(), g_fileHeaderSize) == g2::crc16Load(f.data() + f.size() - 2),
			"planted: a flip inside the ASCII header still verifies, so the header is outside the range");
		checkCrc(g2::crc16File(f.data(), f.size(), g_fileHeaderSize), 0x31C3, "planted: the header flip did not move the value");
		f[3] ^= 0x01;

		f[g_fileHeaderSize] ^= 0x01;
		checkCrc(g2::crc16File(f.data(), f.size(), g_fileHeaderSize), 0xDAE0, "planted: a flip on the version byte moves the value");
		check(g2::crc16File(f.data(), f.size(), g_fileHeaderSize) != g2::crc16Load(f.data() + f.size() - 2),
			"planted: a flip on the version byte fails verification");
		f[g_fileHeaderSize] ^= 0x01;

		f[f.size() - 1] ^= 0x01;
		check(g2::crc16File(f.data(), f.size(), g_fileHeaderSize) != g2::crc16Load(f.data() + f.size() - 2),
			"planted: a flip on the stored CRC fails verification");
		f[f.size() - 1] ^= 0x01;

		check(g2::crc16File(f.data(), f.size(), g_fileHeaderSize) == g2::crc16Load(f.data() + f.size() - 2),
			"planted: the restored image verifies again");

		std::vector<uint8_t> m = wireMessage();
		m[5] ^= 0x01;
		checkCrc(g2::crc16Wire(m.data(), m.size()), 0x7463, "planted: a flip inside the wire payload moves the value");
	}

	void testBigEndianStorage()
	{
		uint8_t stored[2] = {0x00, 0x00};
		g2::crc16Store(stored, 0x31C3);
		check(stored[0] == 0x31 && stored[1] == 0xC3, "storage: the CRC is stored most significant byte first");
		checkCrc(g2::crc16Load(stored), 0x31C3, "storage: a stored CRC reads back unchanged");

		const uint8_t littleEndian[2] = {0xC3, 0x31};
		checkCrc(g2::crc16Load(littleEndian), 0xC331, "storage: the reverse byte order is a different value");
	}
}

int main()
{
	testCoreParameters();
	testWireCoverage();
	testFileCoverage();
	testWireAndFileOverOneBuffer();
	testPlantedCorruption();
	testBigEndianStorage();

	if(g_failures)
	{
		std::cout << "t0_crc16: " << g_failures << " failed" << std::endl;
		return 1;
	}

	std::cout << "t0_crc16: all checks passed" << std::endl;
	return 0;
}
