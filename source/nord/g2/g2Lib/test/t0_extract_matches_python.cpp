// The C++ firmware extractor against the Python oracle. Tier T0: this test
// needs no firmware artifact.
//
// The Python extractor in `axiomantic/nmg2-tools` is the oracle for the C++ one
// in g2Lib, and the two must produce byte-identical output over a synthetic
// container. This file builds that container, runs both implementations over
// it, and compares the two results byte for byte.
//
// Every container below is authored by this file from values it invents. The
// test never reads NMG2_ARTIFACTS and never opens a firmware file.
//
// Every case has three legs:
//
//   1. The C++ report equals the Python report, byte for byte.
//   2. The C++ report equals an expected report this file builds itself from
//      the payloads it authored. Leg 1 alone would be satisfied by two
//      implementations that both produced nothing.
//   3. The oracle process exited 0 and wrote a non-empty file. A Python that
//      failed to import would otherwise leave an empty file that leg 1 could
//      compare against an empty C++ report.
//
// A section whose table entry declares a compressed length of 0 is stored: it
// holds its plain bytes at the file offset and carries no LZO1X stream at all,
// so there is nothing to decompress and no compressed checksum that means
// anything. The loader-side field note is `+0x14 u32 compressed length (0 =>
// section stored uncompressed)`, and `nmg2_tools.container.Section.is_stored`
// implements it.
//
// Case P2 below separates the two readings: it declares a stored section whose
// compressed checksum field holds a value that matches nothing. An
// implementation that verified the compressed checksum of a stored section
// stops there; the correct implementation ignores that field and reads the
// plain bytes.

#include "firmwareExtract.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	// A printable rendering of a report, so that a failure names the byte that
	// differs instead of dumping two binary blobs.
	std::string describeDifference(const std::vector<uint8_t>& _left, const std::vector<uint8_t>& _right)
	{
		const size_t common = _left.size() < _right.size() ? _left.size() : _right.size();

		for(size_t i = 0; i < common; ++i)
		{
			if(_left[i] == _right[i])
				continue;

			char buffer[160];
			std::snprintf(buffer, sizeof(buffer),
				"first difference at offset %zu: 0x%02X against 0x%02X (lengths %zu and %zu)",
				i, unsigned(_left[i]), unsigned(_right[i]), _left.size(), _right.size());
			return buffer;
		}

		if(_left.size() == _right.size())
			return "identical";

		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "one is a prefix of the other: lengths %zu and %zu",
			_left.size(), _right.size());
		return buffer;
	}

	// The first line of a report, so that a failure message says whether the
	// two sides even agreed on success against failure.
	std::string firstLine(const std::vector<uint8_t>& _report)
	{
		std::string text;
		for(const uint8_t byte : _report)
		{
			if(byte == '\n')
				break;
			text += char(byte);
		}
		return text;
	}

	std::string asText(const std::vector<uint8_t>& _report)
	{
		return std::string(_report.begin(), _report.end());
	}

	// ---------------------------------------------------------------------
	// Byte building. Every multi-byte field of the container is BIG-ENDIAN,
	// because the m68k is.

	void appendBe16(std::vector<uint8_t>& _out, const uint16_t _value)
	{
		_out.push_back(uint8_t(_value >> 8));
		_out.push_back(uint8_t(_value));
	}

	void appendBe32(std::vector<uint8_t>& _out, const uint32_t _value)
	{
		_out.push_back(uint8_t(_value >> 24));
		_out.push_back(uint8_t(_value >> 16));
		_out.push_back(uint8_t(_value >> 8));
		_out.push_back(uint8_t(_value));
	}

	void appendBytes(std::vector<uint8_t>& _out, const std::vector<uint8_t>& _bytes)
	{
		_out.insert(_out.end(), _bytes.begin(), _bytes.end());
	}

	void appendText(std::vector<uint8_t>& _out, const std::string& _text)
	{
		_out.insert(_out.end(), _text.begin(), _text.end());
	}

	void writeBe32(std::vector<uint8_t>& _image, const size_t _offset, const uint32_t _value)
	{
		_image[_offset + 0] = uint8_t(_value >> 24);
		_image[_offset + 1] = uint8_t(_value >> 16);
		_image[_offset + 2] = uint8_t(_value >> 8);
		_image[_offset + 3] = uint8_t(_value);
	}

	// The checksum, computed here and not taken from the code under test.
	// g2::containerChecksum is one of the things this test exists to check, so
	// an expected value built with it would be a tautology.
	uint32_t expectedChecksum(const std::vector<uint8_t>& _data)
	{
		uint32_t sum = 0;
		for(const uint8_t byte : _data)
			sum += byte;
		return ~sum;
	}

	std::vector<uint8_t> concat(const std::vector<uint8_t>& _a, const std::vector<uint8_t>& _b)
	{
		std::vector<uint8_t> result = _a;
		appendBytes(result, _b);
		return result;
	}

	std::vector<uint8_t> textBytes(const std::string& _text)
	{
		return std::vector<uint8_t>(_text.begin(), _text.end());
	}

	// ---------------------------------------------------------------------
	// The synthetic payloads and the LZO1X streams that carry them.
	//
	// Every stream below is written out byte by byte, from the wire format
	// alone, so that the streams exercise chosen instruction forms rather than
	// whatever a compressor happened to emit.

	// 295 bytes with no repeat a match could exploit.
	std::vector<uint8_t> sramPlain()
	{
		std::vector<uint8_t> plain;
		for(int i = 0; i < 295; ++i)
			plain.push_back(uint8_t((i * 37 + 11) & 0xFF));
		return plain;
	}

	// Opcode 0 in the instruction state: a LITERAL RUN whose length continues
	// in an extension chain with a base of 15. The chain byte 0x00 adds 255 and
	// 0x16 adds 22, so the count is 15 + 255 + 22 = 292 and the run is 295
	// bytes. The stream then stops at the end marker 0x11 0x00 0x00.
	std::vector<uint8_t> sramStream()
	{
		std::vector<uint8_t> stream;
		stream.push_back(0x00);
		stream.push_back(0x00);
		stream.push_back(0x16);
		appendBytes(stream, sramPlain());
		stream.push_back(0x11);
		stream.push_back(0x00);
		stream.push_back(0x00);
		return stream;
	}

	// "Abcdefgh" "abcdef" "xy" "abc" "abcabcabca".
	std::vector<uint8_t> codePlain()
	{
		return textBytes("ABCDEFGHABCDEFXYABCABCABCABCA");
	}

	// Four match forms and the head literal run:
	//
	//   0x19            the first byte above 17: a literal run of 8 bytes.
	//   0x24 0x1E 0x00  a MEDIUM match, opcode 32 to 63. Length (0x24 & 31) + 2
	//                   = 6, distance 1 + (0x001E >> 2) = 8, and the low two
	//                   bits of the word are 2 TRAILING LITERALS.
	//   "XY"            those two trailing literals.
	//   0x5C 0x01       a SHORT match, opcode 64 to 255. Length (0x5C >> 5) + 1
	//                   = 3, distance 1 + ((0x5C >> 2) & 7) + (0x01 << 3) = 16.
	//   0x28 0x08 0x00  a medium match with length 10 and distance 3, so the
	//                   source and the destination OVERLAP and the last three
	//                   bytes repeat.
	//   0x11 0x00 0x00  the end marker.
	std::vector<uint8_t> codeStream()
	{
		std::vector<uint8_t> stream;
		stream.push_back(0x19);
		appendText(stream, "ABCDEFGH");
		stream.push_back(0x24);
		stream.push_back(0x1E);
		stream.push_back(0x00);
		appendText(stream, "XY");
		stream.push_back(0x5C);
		stream.push_back(0x01);
		stream.push_back(0x28);
		stream.push_back(0x08);
		stream.push_back(0x00);
		stream.push_back(0x11);
		stream.push_back(0x00);
		stream.push_back(0x00);
		return stream;
	}

	// ---------------------------------------------------------------------
	// A section that drives every instruction form the decoder carries.
	//
	// The two streams above leave four branches of the decoder unexercised: the
	// short match that follows a literal run, the two-byte short match that
	// follows trailing literals, the long-distance match, and both
	// extended-length chains of a match. A branch no case reaches is a branch
	// the parity assertion says nothing about.
	//
	// The long-distance forms reach back more than 16,384 bytes, so the section
	// opens with a 40,000-byte literal run to give them somewhere to reach.
	//
	// The bytes are a linear congruential sequence rather than a formula in the
	// index. An index formula repeats every 256 bytes, and against periodic data
	// a match at the wrong distance copies the right bytes anyway.
	std::vector<uint8_t> lcgBytes(const size_t _count)
	{
		std::vector<uint8_t> data;
		uint32_t state = 0x1234567u;
		for(size_t i = 0; i < _count; ++i)
		{
			state = state * 1103515245u + 12345u;
			data.push_back(uint8_t((state >> 16) & 0xFFu));
		}
		return data;
	}

	// One instruction's worth of expected output, written the way the
	// instruction reads: "copy _length bytes that start _distance bytes back".
	// This is the test STATING what each hand-written instruction means. It is
	// not a decoder: it has no opcode, no stream and no state.
	void expectBackReference(std::vector<uint8_t>& _plain, const size_t _distance, const size_t _length)
	{
		const size_t start = _plain.size() - _distance;
		for(size_t i = 0; i < _length; ++i)
			_plain.push_back(_plain[start + i]);
	}

	std::vector<uint8_t> everyFormPlain()
	{
		std::vector<uint8_t> plain = lcgBytes(40000);

		expectBackReference(plain, 16385, 5);      // long-distance match
		expectBackReference(plain, 32770, 5);      // long-distance match, bit 3 set
		appendText(plain, "Q");                    // one trailing literal
		expectBackReference(plain, 3, 2);          // two-byte match after trailing literals
		expectBackReference(plain, 100, 38);       // medium match, extended length
		expectBackReference(plain, 16389, 12);     // long-distance match, extended length
		appendText(plain, "HELLO");                // a short literal run
		expectBackReference(plain, 2050, 3);       // short match after a literal run
		appendText(plain, "ZZ");                   // two trailing literals

		return plain;
	}

	std::vector<uint8_t> everyFormStream()
	{
		std::vector<uint8_t> stream;

		// Opcode 0 in the instruction state: a literal run of 40,000 bytes. The
		// chain reads 156 zero bytes, each worth 255, and stops at 0xCA, so the
		// count is 15 + 39,780 + 202 = 39,997 and the run is 40,000.
		stream.push_back(0x00);
		for(int i = 0; i < 156; ++i)
			stream.push_back(0x00);
		stream.push_back(0xCA);
		appendBytes(stream, lcgBytes(40000));

		// 0x13 0x04 0x00: a LONG-DISTANCE match. Length (0x13 & 7) + 2 = 5;
		// the word 0x0004 gives a back of 1 above the 0x4000 base, so the
		// distance is 16,385; the low two bits of the word are 0 trailing
		// literals.
		stream.push_back(0x13);
		stream.push_back(0x04);
		stream.push_back(0x00);

		// 0x1B 0x09 0x00: the same form with BIT 3 of the OPCODE SET, which
		// adds 0x4000 to the back. Length 5, back 16,384 + 2, distance 32,770,
		// and 1 trailing literal.
		stream.push_back(0x1B);
		stream.push_back(0x09);
		stream.push_back(0x00);
		appendText(stream, "Q");

		// 0x08 0x00: the two-byte SHORT MATCH that follows trailing literals.
		// Length 2, distance 1 + (0x08 >> 2) = 3.
		stream.push_back(0x08);
		stream.push_back(0x00);

		// 0x20 0x05 0x8C 0x01: a MEDIUM match whose length did not fit the
		// opcode. The chain gives 31 + 5 = 36, so the length is 38; the word
		// 0x018C gives a distance of 100.
		stream.push_back(0x20);
		stream.push_back(0x05);
		stream.push_back(0x8C);
		stream.push_back(0x01);

		// 0x10 0x03 0x14 0x00: a LONG-DISTANCE match whose length did not fit
		// the opcode. The chain gives 7 + 3 = 10, so the length is 12; the word
		// 0x0014 gives a back of 5 and a distance of 16,389.
		stream.push_back(0x10);
		stream.push_back(0x03);
		stream.push_back(0x14);
		stream.push_back(0x00);

		// 0x02: a literal run of 5 bytes in the instruction state.
		stream.push_back(0x02);
		appendText(stream, "HELLO");

		// 0x06 0x00: the SHORT MATCH that FOLLOWS A LITERAL RUN. The same
		// opcode value after trailing literals would read as the two-byte form
		// above, which is why the position is part of the decoder's state.
		// Length 3, distance 0x801 + 1 = 2,050, and 2 trailing literals.
		stream.push_back(0x06);
		stream.push_back(0x00);
		appendText(stream, "ZZ");

		stream.push_back(0x11);
		stream.push_back(0x00);
		stream.push_back(0x00);
		return stream;
	}

	// The `start` state's other branch: a first byte above 17 whose run is
	// shorter than four bytes is a run of trailing literals, and a match
	// follows it rather than another literal run.
	//
	// The instruction after the run is below 16, and that is the point. An
	// opcode of 16 or more is read the same way from both of the states this
	// branch chooses between, so a stream that went straight to the end marker
	// would pass whichever state the decoder picked. Only an opcode below 16
	// separates them: after a short head run it is a two-byte match at a base
	// of 1, and after a full literal run it is a three-byte match at a base of
	// 0x801, which three bytes of output cannot reach.
	std::vector<uint8_t> shortHeadPlain()
	{
		return textBytes("abcab");
	}

	std::vector<uint8_t> shortHeadStream()
	{
		std::vector<uint8_t> stream;
		stream.push_back(0x14);                    // 20 - 17 = a run of 3 bytes
		appendText(stream, "abc");
		stream.push_back(0x08);                    // length 2, distance 1 + (8 >> 2) = 3
		stream.push_back(0x00);
		stream.push_back(0x11);
		stream.push_back(0x00);
		stream.push_back(0x00);
		return stream;
	}

	// The section that is STORED. Its bytes in the image are its plain bytes.
	std::vector<uint8_t> storedPlain()
	{
		std::vector<uint8_t> plain;
		for(int i = 0; i < 64; ++i)
			plain.push_back(uint8_t((i * 5 + 200) & 0xFF));
		return plain;
	}

	// ---------------------------------------------------------------------
	// The container.

	constexpr uint32_t g_headerSize = 0x14u;
	constexpr uint32_t g_entryStride = 0x2Cu;

	struct Entry
	{
		std::string tag;
		std::vector<uint8_t> data;        // the bytes that go in the image
		uint32_t uncompressedLength = 0;
		uint32_t loadAddress = 0;
		uint32_t plainChecksum = 0;
		uint32_t compressedLength = 0;
		uint32_t compressedChecksum = 0;
		uint32_t reserved = 0;
	};

	// A compressed entry: the image carries the stream, and both checksums are
	// real.
	Entry compressedEntry(const std::string& _tag, const std::vector<uint8_t>& _stream,
		const std::vector<uint8_t>& _plain, const uint32_t _loadAddress)
	{
		Entry entry;
		entry.tag = _tag;
		entry.data = _stream;
		entry.uncompressedLength = uint32_t(_plain.size());
		entry.loadAddress = _loadAddress;
		entry.plainChecksum = expectedChecksum(_plain);
		entry.compressedLength = uint32_t(_stream.size());
		entry.compressedChecksum = expectedChecksum(_stream);
		return entry;
	}

	// A stored entry: the image carries the plain bytes and the compressed
	// length is 0. The compressed checksum field is not a checksum of anything,
	// and the caller chooses what it holds.
	Entry storedEntry(const std::string& _tag, const std::vector<uint8_t>& _plain,
		const uint32_t _loadAddress, const uint32_t _compressedChecksumField)
	{
		Entry entry;
		entry.tag = _tag;
		entry.data = _plain;
		entry.uncompressedLength = uint32_t(_plain.size());
		entry.loadAddress = _loadAddress;
		entry.plainChecksum = expectedChecksum(_plain);
		entry.compressedLength = 0;
		entry.compressedChecksum = _compressedChecksumField;
		return entry;
	}

	// Builds the image, and reports where each entry's table row starts so that
	// a negative case can corrupt one field of one row.
	std::vector<uint8_t> buildContainer(const uint16_t _version, const uint16_t _secondWord,
		const uint32_t _unresolved, const std::vector<Entry>& _entries,
		std::vector<size_t>* _entryOffsets = nullptr)
	{
		const uint32_t count = uint32_t(_entries.size());
		const uint32_t dataStart = g_headerSize + count * g_entryStride;

		std::vector<uint8_t> table;
		std::vector<uint8_t> data;

		uint32_t cursor = dataStart;
		for(const Entry& entry : _entries)
		{
			if(_entryOffsets)
				_entryOffsets->push_back(g_headerSize + table.size());

			appendText(table, entry.tag);
			appendBe32(table, cursor);
			appendBe32(table, entry.uncompressedLength);
			appendBe32(table, entry.loadAddress);
			appendBe32(table, entry.plainChecksum);
			appendBe32(table, entry.compressedLength);
			appendBe32(table, entry.compressedChecksum);
			appendBe32(table, entry.reserved);

			// The last 12 bytes of an entry carry no meaning this project
			// knows, and NOTHING may READ THEM. They are filled with 0xAA
			// rather than zero, so a walk that folded them into a field shows
			// up as a wrong value instead of as a zero that looks plausible.
			for(int i = 0; i < 12; ++i)
				table.push_back(0xAA);

			appendBytes(data, entry.data);
			cursor += uint32_t(entry.data.size());
		}

		std::vector<uint8_t> image;
		appendBe16(image, _version);
		appendBe16(image, _secondWord);
		appendBe32(image, _unresolved);
		for(int i = 0; i < 8; ++i)
			image.push_back(0x00);
		appendBe32(image, count);
		appendBytes(image, table);
		appendBytes(image, data);
		return image;
	}

	// The three-section container every case starts from.
	//
	// SRAM is compressed with an extended literal run, CODE is compressed with
	// four match forms, and STOR is STORED.
	std::vector<Entry> standardEntries()
	{
		std::vector<Entry> entries;
		entries.push_back(compressedEntry("SRAM", sramStream(), sramPlain(), 0x20000800u));
		entries.push_back(compressedEntry("CODE", codeStream(), codePlain(), 0x30000400u));
		entries.push_back(storedEntry("STOR", storedPlain(), 0x30400000u, 0x5A5A5A5Au));
		return entries;
	}

	// ---------------------------------------------------------------------
	// The report. Both implementations write this shape, and the two are
	// compared byte for byte.

	void appendSectionReport(std::vector<uint8_t>& _report, const Entry& _entry,
		const uint32_t _fileOffset, const std::vector<uint8_t>& _plain)
	{
		appendText(_report, _entry.tag);
		appendBe32(_report, _fileOffset);
		appendBe32(_report, _entry.uncompressedLength);
		appendBe32(_report, _entry.loadAddress);
		appendBe32(_report, _entry.plainChecksum);
		appendBe32(_report, _entry.compressedLength);
		appendBe32(_report, _entry.compressedChecksum);
		appendBe32(_report, _entry.reserved);
		appendBe32(_report, uint32_t(_plain.size()));
		appendBytes(_report, _plain);
	}

	// The expected report of a whole successful load, built from the payloads
	// this file authored and from no implementation of the format.
	std::vector<uint8_t> expectedSuccessReport(const uint16_t _version, const uint16_t _secondWord,
		const uint32_t _unresolved, const std::vector<Entry>& _entries,
		const std::vector<std::vector<uint8_t>>& _plains, const std::string& _versionText)
	{
		std::vector<uint8_t> report;
		appendText(report, "OK\n");
		appendBe32(report, _version);
		appendBe32(report, _secondWord);
		appendBe32(report, _unresolved);
		appendBe32(report, uint32_t(_entries.size()));
		appendBe32(report, uint32_t(_versionText.size()));
		appendText(report, _versionText);

		uint32_t cursor = g_headerSize + uint32_t(_entries.size()) * g_entryStride;
		for(size_t i = 0; i < _entries.size(); ++i)
		{
			appendSectionReport(report, _entries[i], cursor, _plains[i]);
			cursor += uint32_t(_entries[i].data.size());
		}
		return report;
	}

	std::vector<uint8_t> expectedErrorReport(const std::string& _message)
	{
		std::vector<uint8_t> report;
		appendText(report, "ERR\n");
		appendText(report, _message);
		return report;
	}

	// ---------------------------------------------------------------------
	// The C++ half of a case.

	std::vector<uint8_t> cppReport(const std::vector<uint8_t>& _image)
	{
		std::vector<uint8_t> report;

		g2::Container container;
		std::string error;

		if(!g2::parseHeader(_image, container, error))
			return expectedErrorReport(error);

		const std::string text = g2::versionText(container.version);

		std::vector<uint8_t> body;
		for(const g2::ContainerSection& section : container.sections)
		{
			std::vector<uint8_t> plain;
			if(!g2::loadSection(_image, section, plain, error))
				return expectedErrorReport(error);

			appendText(body, section.tag);
			appendBe32(body, section.fileOffset);
			appendBe32(body, section.uncompressedLength);
			appendBe32(body, section.loadAddress);
			appendBe32(body, section.plainChecksum);
			appendBe32(body, section.compressedLength);
			appendBe32(body, section.compressedChecksum);
			appendBe32(body, section.reserved);
			appendBe32(body, uint32_t(plain.size()));
			appendBytes(body, plain);
		}

		appendText(report, "OK\n");
		appendBe32(report, container.version);
		appendBe32(report, container.secondWord);
		appendBe32(report, container.unresolved);
		appendBe32(report, uint32_t(container.sections.size()));
		appendBe32(report, uint32_t(text.size()));
		appendText(report, text);
		appendBytes(report, body);
		return report;
	}

	// ---------------------------------------------------------------------
	// The Python half of a case.
	//
	// The oracle program is written out at run time and is not a committed
	// file: the driver lives in this translation unit and is spilled to the
	// build tree when it runs.

	const char* g_oracleProgram = R"PYTHON(
import struct
import sys

sys.path.insert(0, sys.argv[1])

from nmg2_tools.container import ContainerError, load_section, parse_header, version_text
from nmg2_tools.lzo1x import Lzo1xError


def be32(value):
    return struct.pack(">I", value & 0xFFFFFFFF)


with open(sys.argv[2], "rb") as handle:
    image = handle.read()

try:
    container = parse_header(image)
    text = version_text(container.version).encode("ascii")

    body = b""
    for section in container.sections:
        plain = load_section(image, section)
        body += section.tag.encode("ascii")
        body += be32(section.file_offset)
        body += be32(section.uncompressed_length)
        body += be32(section.load_address)
        body += be32(section.plain_checksum)
        body += be32(section.compressed_length)
        body += be32(section.compressed_checksum)
        body += be32(section.reserved)
        body += be32(len(plain))
        body += plain

    report = b"OK\n"
    report += be32(container.version)
    report += be32(container.second_word)
    report += be32(container.unresolved)
    report += be32(len(container.sections))
    report += be32(len(text))
    report += text
    report += body
except (ContainerError, Lzo1xError) as error:
    report = b"ERR\n" + str(error).encode("utf-8")

with open(sys.argv[3], "wb") as handle:
    handle.write(report)
)PYTHON";

	std::string g_workDirectory = G2_ORACLE_WORK_DIR;
	std::string g_pythonExecutable = G2_ORACLE_PYTHON;
	std::string g_toolsDirectory = G2_ORACLE_TOOLS_DIR;

	std::string workPath(const std::string& _name)
	{
		return g_workDirectory + "/t0_extract_matches_python." + _name;
	}

	bool writeFile(const std::string& _path, const std::vector<uint8_t>& _bytes)
	{
		std::ofstream stream(_path.c_str(), std::ios::binary | std::ios::trunc);
		if(!stream.is_open())
			return false;
		if(!_bytes.empty())
			stream.write(reinterpret_cast<const char*>(_bytes.data()), std::streamsize(_bytes.size()));
		stream.close();
		return stream.good();
	}

	bool readFile(const std::string& _path, std::vector<uint8_t>& _bytes)
	{
		std::ifstream stream(_path.c_str(), std::ios::binary);
		if(!stream.is_open())
			return false;
		_bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		return true;
	}

	// Not NAMED `quoted`. An argument of type std::string reaches std::quoted
	// through argument-dependent lookup, and the two overloads are ambiguous.
	std::string quotedArgument(const std::string& _text)
	{
		return "\"" + _text + "\"";
	}

	// Runs the oracle over one image and returns its report.
	//
	// A non-zero exit status, a missing file or an EMPTY file is a FAILURE of
	// the case and never an empty report that would compare equal to an empty
	// C++ report. That is leg 3 of the three legs at the top of this file.
	bool oracleReport(const std::vector<uint8_t>& _image, std::vector<uint8_t>& _report, std::string& _why)
	{
		const std::string scriptPath = workPath("oracle.py");
		const std::string imagePath = workPath("container.bin");
		const std::string reportPath = workPath("oracle.out");

		std::remove(reportPath.c_str());

		if(!writeFile(scriptPath, textBytes(g_oracleProgram)))
		{
			_why = "the oracle program could not be written to " + scriptPath;
			return false;
		}

		if(!writeFile(imagePath, _image))
		{
			_why = "the container could not be written to " + imagePath;
			return false;
		}

		std::string command = quotedArgument(g_pythonExecutable) + " " + quotedArgument(scriptPath) + " "
			+ quotedArgument(g_toolsDirectory) + " " + quotedArgument(imagePath) + " "
			+ quotedArgument(reportPath);

#ifdef _WIN32
		// cmd.exe strips the outer pair of quotes of the whole command line,
		// so a command whose first token is quoted needs one more pair.
		command = "\"" + command + "\"";
#endif

		const int status = std::system(command.c_str());
		if(status != 0)
		{
			_why = "the oracle exited with status " + std::to_string(status) + ": " + command;
			return false;
		}

		if(!readFile(reportPath, _report))
		{
			_why = "the oracle wrote no file at " + reportPath;
			return false;
		}

		if(_report.empty())
		{
			_why = "the oracle wrote an empty file at " + reportPath;
			return false;
		}

		return true;
	}

	// ---------------------------------------------------------------------
	// One case: the three legs.

	void runCase(const std::string& _name, const std::vector<uint8_t>& _image,
		const std::vector<uint8_t>& _expected)
	{
		const std::vector<uint8_t> fromCpp = cppReport(_image);

		std::vector<uint8_t> fromPython;
		std::string why;

		if(!oracleReport(_image, fromPython, why))
		{
			check(false, _name + ": the oracle ran and wrote a report -- " + why);
			return;
		}

		check(true, _name + ": the oracle ran and wrote " + std::to_string(fromPython.size()) + " bytes");

		check(fromCpp == fromPython,
			_name + ": the C++ report and the Python report are byte-identical ("
				+ firstLine(fromCpp) + " against " + firstLine(fromPython) + ", "
				+ describeDifference(fromCpp, fromPython) + ")");

		check(fromCpp == _expected,
			_name + ": the C++ report is the one this test authored ("
				+ describeDifference(fromCpp, _expected) + ")");
	}
}

int main()
{
	if(g_pythonExecutable.empty() || g_toolsDirectory.empty())
	{
		// Not a skip. The check carries no gate, so an oracle this build could
		// not locate is a failure. A skip here would be a green run that
		// compared nothing.
		std::cout << "FAIL t0_extract_matches_python: the build located no Python oracle. "
			<< "python=<" << g_pythonExecutable << "> tools=<" << g_toolsDirectory << ">" << std::endl;
		return 1;
	}

	const uint16_t version = 0x00A2u;      // 162, which is release 1.62
	const uint16_t secondWord = 0x0100u;
	const uint32_t unresolved = 0xDEADBEEFu;

	const std::vector<Entry> entries = standardEntries();
	const std::vector<std::vector<uint8_t>> plains = {sramPlain(), codePlain(), storedPlain()};

	// -----------------------------------------------------------------------
	// Case P1. The WHOLE CONTAINER LOADS and both IMPLEMENTATIONS AGREE.
	//
	// Three sections, two of them compressed with different instruction forms
	// and one of them STORED.
	{
		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries);
		runCase("P1 the three-section container", image,
			expectedSuccessReport(version, secondWord, unresolved, entries, plains, "1.62"));
	}

	// -----------------------------------------------------------------------
	// Case P2. A stored section ignores its compressed checksum field.
	//
	// The entry's compressed length is 0 and its compressed checksum field
	// holds 0x11111111, which is the checksum of nothing. An implementation
	// that verified that field stops with CONTAINER-COMPRESSED-CHECKSUM; the
	// correct one reads the plain bytes at the file offset and verifies the
	// plain checksum alone.
	{
		std::vector<Entry> stored;
		stored.push_back(storedEntry("STOR", storedPlain(), 0x30400000u, 0x11111111u));

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, stored);
		runCase("P2 a stored section whose compressed checksum field is meaningless", image,
			expectedSuccessReport(version, secondWord, unresolved, stored, {storedPlain()}, "1.62"));
	}

	// -----------------------------------------------------------------------
	// Case P3. The version word is recorded and its text splits at the
	// hundreds.
	{
		std::vector<Entry> stored;
		stored.push_back(storedEntry("STOR", storedPlain(), 0x30400000u, 0u));

		const std::vector<uint8_t> image = buildContainer(0x0064u, secondWord, 0u, stored);
		runCase("P3 a version word of 0x0064 reads as 1.00", image,
			expectedSuccessReport(0x0064u, secondWord, 0u, stored, {storedPlain()}, "1.00"));
	}

	// -----------------------------------------------------------------------
	// Case P4. Every INSTRUCTION FORM the DECODER CARRIES.
	//
	// One 40,000-byte section that reaches both long-distance forms, both
	// extended-length chains, the short match after a literal run, the two-byte
	// match after trailing literals and the medium match; and one three-byte
	// section that drives the `start` state's short-run branch. Without this
	// case four branches of the decoder are never executed and the parity
	// assertion says nothing about them.
	{
		std::vector<Entry> forms;
		forms.push_back(compressedEntry("LZOX", everyFormStream(), everyFormPlain(), 0x30000400u));
		forms.push_back(compressedEntry("HEAD", shortHeadStream(), shortHeadPlain(), 0x20000800u));

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, forms);
		runCase("P4 every instruction form the decoder carries", image,
			expectedSuccessReport(version, secondWord, unresolved, forms,
				{everyFormPlain(), shortHeadPlain()}, "1.62"));
	}

	// -----------------------------------------------------------------------
	// Case N1. One section's compressed checksum is corrupted, and both implementations
	// stop and name the same section. The two reports are compared in full, so
	// "name the same section" is asserted on the whole message and not on a
	// substring of it.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t stored = entries[1].compressedChecksum + 1u;
		writeBe32(image, offsets[1] + 0x18u, stored);

		char expected[128];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-COMPRESSED-CHECKSUM: section CODE stored 0x%08X, computed 0x%08X",
			stored, entries[1].compressedChecksum);

		runCase("N1 a corrupted compressed checksum", image, expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N2. A CORRUPTED PLAIN CHECKSUM on A COMPRESSED SECTION.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t stored = entries[0].plainChecksum ^ 0x00000001u;
		writeBe32(image, offsets[0] + 0x10u, stored);

		char expected[128];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-PLAIN-CHECKSUM: section SRAM stored 0x%08X, computed 0x%08X",
			stored, entries[0].plainChecksum);

		runCase("N2 a corrupted plain checksum on a compressed section", image, expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N3. A CORRUPTED PLAIN CHECKSUM on A STORED SECTION.
	//
	// The stored path verifies the plain checksum and nothing else, so this is
	// the one check a stored section can fail.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t stored = entries[2].plainChecksum ^ 0x00000080u;
		writeBe32(image, offsets[2] + 0x10u, stored);

		char expected[128];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-PLAIN-CHECKSUM: section STOR stored 0x%08X, computed 0x%08X",
			stored, entries[2].plainChecksum);

		runCase("N3 a corrupted plain checksum on a stored section", image, expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N4. A DECLARED COMPRESSED EXTENT LONGER THAN the STREAM.
	//
	// The decompressor stops at the first end marker and IGNORES what follows,
	// so a declared extent that is too long decodes cleanly and passes both
	// checksums. Only the consumed-length identity catches it.
	{
		std::vector<Entry> longer = entries;
		std::vector<uint8_t> stream = codeStream();
		stream.push_back(0x99);                       // junk after the end marker

		longer[1].data = stream;
		longer[1].compressedLength = uint32_t(stream.size());
		longer[1].compressedChecksum = expectedChecksum(stream);

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, longer);

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-TRAILING-BYTES: section CODE declared %u compressed bytes, "
			"the stream ended before the last of them", unsigned(stream.size()));

		runCase("N4 a compressed extent longer than the stream", image, expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N5. A stream that produces a different number of bytes than the
	// table declares.
	{
		std::vector<Entry> wrong = entries;
		wrong[1].uncompressedLength = uint32_t(codePlain().size()) + 1u;

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, wrong);

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-LENGTH-MISMATCH: section CODE declared %u bytes, the stream produced %u",
			unsigned(codePlain().size() + 1), unsigned(codePlain().size()));

		runCase("N5 a declared uncompressed length the stream does not produce", image,
			expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N6. The FIXED WORD at +0x02 is WRONG.
	{
		const std::vector<uint8_t> image = buildContainer(version, 0x0200u, unresolved, entries);
		runCase("N6 a wrong fixed word at +0x02", image,
			expectedErrorReport("CONTAINER-BAD-SECOND-WORD: 0x0200 at offset 0x02, expected 0x0100"));
	}

	// -----------------------------------------------------------------------
	// Case N7. An IMAGE SHORTER THAN the HEADER.
	{
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries);
		image.resize(0x10u);

		runCase("N7 an image shorter than the header", image,
			expectedErrorReport("CONTAINER-TRUNCATED-HEADER: 20 bytes needed, 16 available"));
	}

	// -----------------------------------------------------------------------
	// Case N8. A SECTION COUNT the IMAGE CANNOT CARRY.
	{
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries);
		writeBe32(image, 0x10u, 4000u);

		const size_t available = image.size() - g_headerSize;

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-TRUNCATED-SECTION-TABLE: 4000 entries need %u bytes at offset 0x14, %u available",
			unsigned(4000u * g_entryStride), unsigned(available));

		runCase("N8 a section count the image cannot carry", image, expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N9. A TAG that is not ASCII.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);
		image[offsets[1] + 1] = 0xC3u;

		runCase("N9 a section tag that is not ASCII", image,
			expectedErrorReport("CONTAINER-BAD-SECTION-TAG: entry 1 holds 43c34445, which is not ASCII"));
	}

	// -----------------------------------------------------------------------
	// Case N10. A SECTION that REACHES PAST the END of the IMAGE.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t beyond = uint32_t(image.size()) - 4u;
		writeBe32(image, offsets[0] + 0x04u, beyond);

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-SECTION-OUT-OF-RANGE: section SRAM needs %u bytes at offset 0x%02X, 4 available",
			unsigned(sramStream().size()), beyond);

		runCase("N10 a compressed section that reaches past the end of the image", image,
			expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N11. A STORED SECTION that REACHES PAST the END of the IMAGE.
	//
	// The stored path takes its length from the UNCOMPRESSED length, so its
	// range check is a different line from case N10's.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t beyond = uint32_t(image.size()) - 8u;
		writeBe32(image, offsets[2] + 0x04u, beyond);

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-SECTION-OUT-OF-RANGE: section STOR needs %u bytes at offset 0x%02X, 8 available",
			unsigned(storedPlain().size()), beyond);

		runCase("N11 a stored section that reaches past the end of the image", image,
			expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N12. A MATCH that REACHES BEFORE the START of the OUTPUT.
	//
	// The last instruction is replaced by a long-distance match with a non-zero
	// distance, so it is no longer the end marker and it reaches back further
	// than the output is long. The compressed checksum is recomputed, so the
	// case reaches the DECOMPRESSOR and is not stopped by the checksum first.
	{
		std::vector<Entry> broken = entries;
		std::vector<uint8_t> stream = codeStream();
		stream[stream.size() - 1] = 0x04u;            // word 0x0400: distance 0x4000 + 256

		broken[1].data = stream;
		broken[1].compressedLength = uint32_t(stream.size());
		broken[1].compressedChecksum = expectedChecksum(stream);

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, broken);

		runCase("N12 a match that reaches before the start of the output", image,
			expectedErrorReport("LZO-DISTANCE-BEFORE-START: distance 16640 is more than the 29 bytes written"));
	}

	// -----------------------------------------------------------------------
	// Case N13. A STREAM with no END MARKER.
	//
	// The end marker is replaced by a literal run that reaches past the end of
	// the declared extent.
	{
		std::vector<Entry> broken = entries;
		std::vector<uint8_t> stream = codeStream();
		stream.resize(stream.size() - 3);
		stream.push_back(0x0Au);                      // a literal run of 13 bytes, and none follow

		broken[1].data = stream;
		broken[1].compressedLength = uint32_t(stream.size());
		broken[1].compressedChecksum = expectedChecksum(stream);

		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, broken);

		char expected[160];
		std::snprintf(expected, sizeof(expected),
			"LZO-TRUNCATED-INPUT: 13 bytes needed at offset %u, 0 available", unsigned(stream.size()));

		runCase("N13 a literal run that reaches past the end of the stream", image,
			expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N14. An EMPTY SECTION TABLE.
	//
	// A container with no section at all is not an error, and both
	// implementations report the header alone.
	{
		const std::vector<Entry> none;
		const std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, none);
		runCase("N14 a container with no section", image,
			expectedSuccessReport(version, secondWord, unresolved, none, {}, "1.62"));
	}

	// -----------------------------------------------------------------------
	// Case N15. A SECTION COUNT WHOSE 32-BIT PRODUCT WRAPS to ZERO.
	//
	// 0x40000000 entries at the 0x2C stride is 47,244,640,256 bytes, and that
	// product is exactly 11 times 2^32. A table-fits check computed in 32 bits
	// therefore compares 0 against the bytes available, passes, and the walk
	// then reads a billion entries out of a 500-byte image. The check has to be
	// wider than the field it multiplies, and this is the case that says so.
	{
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries);
		writeBe32(image, 0x10u, 0x40000000u);

		const size_t available = image.size() - g_headerSize;

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-TRUNCATED-SECTION-TABLE: 1073741824 entries need 47244640256 bytes "
			"at offset 0x14, %u available", unsigned(available));

		runCase("N15 a section count whose 32-bit product wraps to zero", image,
			expectedErrorReport(expected));
	}

	// -----------------------------------------------------------------------
	// Case N16. A FILE OFFSET PAST the END of the IMAGE.
	//
	// Cases N10 and N11 put the offset INSIDE the image and the length past the
	// end, which is the ordinary short read. This puts the OFFSET itself past
	// the end, so "the bytes available at that offset" is a subtraction that
	// goes negative. Computed in unsigned arithmetic without a guard it wraps to
	// a number larger than any length, the range check passes, and the slice
	// then reads from an address the image does not own.
	{
		std::vector<size_t> offsets;
		std::vector<uint8_t> image = buildContainer(version, secondWord, unresolved, entries, &offsets);

		const uint32_t beyond = uint32_t(image.size()) + 16u;
		writeBe32(image, offsets[0] + 0x04u, beyond);

		char expected[192];
		std::snprintf(expected, sizeof(expected),
			"CONTAINER-SECTION-OUT-OF-RANGE: section SRAM needs %u bytes at offset 0x%02X, 0 available",
			unsigned(sramStream().size()), beyond);

		runCase("N16 a file offset past the end of the image", image, expectedErrorReport(expected));
	}

	if(g_failures)
	{
		std::cout << "t0_extract_matches_python: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_extract_matches_python: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
