// The C++ firmware extractor.
//
// The LZO1X decoder here is written from the wire format: the instruction
// encoding, the rules for the length of a literal run and of a match, and the
// three distance encodings.
//
// `nmg2_tools.container` is the oracle for this file, and the two must produce
// byte-identical output over a synthetic container. That includes the failure
// messages: the names and the wording below are the ones the oracle writes, so
// that "both implementations stop and name the same section" is an assertion on
// a whole message and not on a substring of it.
//
// This file throws nothing. Every entry point reports through a bool return and
// an out-parameter.
//
// A section whose table entry declares a compressed length of 0 holds its plain
// bytes at the file offset: there is no LZO1X stream to decompress, and the
// compressed checksum field is a checksum of nothing. The loader-side field
// note is `+0x14 u32 compressed length (0 => section stored uncompressed)`. An
// implementation that omits the case agrees with the oracle on every container
// that has no stored section, and diverges first on a real firmware image.
// `isStored()` below is that case.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace g2
{
	// The header ends, and the section table starts, at this offset.
	constexpr uint32_t g_containerHeaderSize = 0x14u;

	// One section table entry. 0x20 bytes of known fields and 12 that are not
	// read. The stride is 0x2C and not 0x20: a walk that used the size of the
	// known fields as the stride desynchronizes after the first entry.
	constexpr uint32_t g_containerEntryStride = 0x2Cu;

	// The one fixed value in the header, at +0x02.
	constexpr uint16_t g_containerSecondWord = 0x0100u;

	/* The container checksum:
	 *
	 *     cksum = (~sum(data)) & 0xFFFFFFFF
	 *
	 * It is a plain sum, so it detects a CHANGED byte and it does not detect a
	 * permutation of the same bytes. A caller that needs to know the bytes are
	 * in the right order needs something else. */
	uint32_t containerChecksum(const uint8_t* _data, size_t _size);
	uint32_t containerChecksum(const std::vector<uint8_t>& _data);

	/* One LZO1X stream to plain bytes.
	 *
	 * Returns false and writes `_error` when the stream is truncated, when it
	 * holds no end marker, or when a match reaches before the start of the
	 * output. A truncated stream is always a named failure and never a partial
	 * result, so `_out` is cleared on a failure.
	 *
	 * There is no cursor and no output ceiling. The decoder stops at the first
	 * end marker and ignores whatever follows it, which is what makes
	 * loadSection's consumed-length identity necessary. No ceiling is needed: a
	 * length-extension chain adds 255 for each byte it reads, so the output
	 * grows to about 255 bytes for each byte of input and no further. The
	 * exposure is linear in the input size. */
	bool lzo1xDecompress(const uint8_t* _src, size_t _size, std::vector<uint8_t>& _out, std::string& _error);

	// One row of the section table.
	struct ContainerSection
	{
		std::string tag;                     // `SRAM` or `CODE`, four bytes
		uint32_t fileOffset = 0;
		uint32_t uncompressedLength = 0;
		uint32_t loadAddress = 0;
		uint32_t plainChecksum = 0;
		uint32_t compressedLength = 0;
		uint32_t compressedChecksum = 0;
		uint32_t reserved = 0;               // recorded, never verified

		// A section that holds plain bytes and no LZO1X stream.
		bool isStored() const { return compressedLength == 0; }
	};

	/* The parsed header and section table.
	 *
	 * `version` is the raw 16-bit word and not text, because plugin state saves
	 * it as it was read. */
	struct Container
	{
		uint16_t version = 0;
		uint16_t secondWord = 0;
		uint32_t unresolved = 0;             // most probably the begin checksum
		std::vector<ContainerSection> sections;
	};

	/* The release number a version word states. `0x00A2` is 162, which is
	 * release 1.62: the word is a plain integer and the release splits it at
	 * the hundreds. */
	std::string versionText(uint16_t _version);

	/* Parses the header and the WHOLE section table.
	 *
	 * Returns false and writes `_error` when the image is shorter than the
	 * header, when the fixed word at +0x02 is wrong, when the table does not fit
	 * the image, or when a tag is not ASCII. The message opens with a name:
	 * CONTAINER-TRUNCATED-HEADER, CONTAINER-BAD-SECOND-WORD,
	 * CONTAINER-TRUNCATED-SECTION-TABLE or CONTAINER-BAD-SECTION-TAG. */
	bool parseHeader(const std::vector<uint8_t>& _image, Container& _container, std::string& _error);

	/* The plain bytes of one section, both checksums verified.
	 *
	 * A COMPRESSED section: the compressed checksum over exactly
	 * `compressedLength` bytes, then LZO1X, then the produced length against
	 * `uncompressedLength`, then the consumed length against `compressedLength`,
	 * then the plain checksum.
	 *
	 * The consumed length is checked because the decoder stops at the end
	 * marker and ignores what follows, so a section whose declared compressed
	 * extent is longer than the stream it holds decodes cleanly and passes both
	 * checksums -- the compressed checksum is computed over the declared extent
	 * and therefore covers the junk as well. Without this identity that case
	 * could not fail. The decoder returns no cursor, so the identity is
	 * recovered the way the oracle recovers it: the decoder stops at the FIRST
	 * end marker, so no shorter prefix of a well-formed stream can decode, and
	 * the stream used every declared byte if and only if the same stream one
	 * BYTE SHORTER fails. That costs one more decompression of the section.
	 *
	 * A STORED section, `compressedLength == 0`: the plain bytes are the bytes
	 * at the file offset, there is no stream, the compressed checksum field is
	 * Not read, and the plain checksum is the one check.
	 *
	 * Returns false and writes `_error`, which opens with
	 * CONTAINER-SECTION-OUT-of-RANGE, CONTAINER-COMPRESSED-CHECKSUM,
	 * CONTAINER-LENGTH-MISMATCH, CONTAINER-TRAILING-BYTES or
	 * CONTAINER-PLAIN-CHECKSUM, and names the section; or with an LZO- name
	 * when the stream itself is malformed, so that a caller can tell the two
	 * layers apart. */
	bool loadSection(const std::vector<uint8_t>& _image, const ContainerSection& _section,
		std::vector<uint8_t>& _plain, std::string& _error);

	/* Parses the container and loads every section, in TABLE ORDER. Stops at the
	 * first failure and leaves `_loaded` holding nothing. */
	bool loadSections(const std::vector<uint8_t>& _image,
		std::vector<std::pair<ContainerSection, std::vector<uint8_t>>>& _loaded, std::string& _error);
}
