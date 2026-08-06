// Task BRD-6. The C++ firmware extractor.
//
// Plan section 13.2, BRD-6. Design sections 7.3 and 20.2.
//
// WHAT THIS FILE IS, because the licence makes it matter.
//
// `gearmulator` is not GPL. The container layout below is a FACT about a data
// format -- which field sits at which offset, how wide it is, and in which byte
// order it is written -- and design section 7.3 states every one of those facts
// independently. Facts are not copyrightable. The one file in the workspace
// that decodes the same container describes itself as a byte-exact port of a
// GPL-2.0 decompressor; only its STATEMENT OF THE LAYOUT was read, never its
// body. The LZO1X decoder here is written from the wire format: the instruction
// encoding, the rules for the length of a literal run and of a match, and the
// three distance encodings.
//
// THE PYTHON HALF IS THE ORACLE, NOT THE SOURCE. Design section 20.2 makes
// `nmg2_tools.container` the oracle for this file, and BRD-6's test asserts the
// two produce byte-identical output over a synthetic container. That includes
// the FAILURE messages: the names and the wording below are the ones the oracle
// writes, so that "both implementations stop and name the same section" is an
// assertion on a whole message and not on a substring of it.
//
// THIS FILE THROWS NOTHING. Design sections 5.3 and 13.10 forbid exceptions, so
// every entry point reports through a bool return and an out-parameter, in the
// shape g2::ArtifactResolver already uses.
//
// -------------------------------------------------------------------------
// A SECTION WHOSE COMPRESSED LENGTH IS 0 IS STORED, AND THE DESIGN DOES NOT SAY
// SO. Plan section 1.3 rule 5, reported here rather than worked around quietly.
//
// Design section 7.3 step 3 reads: "For each section: verify the compressed
// checksum, decompress with LZO1X, then verify the plain checksum. Both checks
// are mandatory." Read literally that is WRONG for a section whose table entry
// declares a compressed length of 0. Such a section holds its PLAIN bytes at
// the file offset: there is no LZO1X stream to decompress, and the compressed
// checksum field is a checksum of nothing.
//
// The rule's source is the loader-side field note
// `+0x14 u32 compressed length (0 => section stored uncompressed)`, and
// `nmg2_tools.container.Section.is_stored` implements it. An implementation
// built from design section 7.3 alone omits the case, agrees with the oracle on
// every container that has no stored section, and diverges first on a real
// firmware image. `isStored()` below is that case, and BRD-6's test case P2
// drives it with a compressed checksum field that matches nothing.
// -------------------------------------------------------------------------

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
	// read. THE STRIDE IS 0x2C AND NOT 0x20: a walk that used the size of the
	// known fields as the stride desynchronizes after the first entry.
	constexpr uint32_t g_containerEntryStride = 0x2Cu;

	// The one fixed value in the header, at +0x02.
	constexpr uint16_t g_containerSecondWord = 0x0100u;

	/* The container checksum. Design section 7.3 step 4:
	 *
	 *     cksum = (~sum(data)) & 0xFFFFFFFF
	 *
	 * It is a plain sum, so it detects a CHANGED byte and it does NOT detect a
	 * permutation of the same bytes. A caller that needs to know the bytes are
	 * in the right order needs something else. */
	uint32_t containerChecksum(const uint8_t* _data, size_t _size);
	uint32_t containerChecksum(const std::vector<uint8_t>& _data);

	/* One LZO1X stream to plain bytes.
	 *
	 * Returns false and writes `_error` when the stream is truncated, when it
	 * holds no end marker, or when a match reaches before the start of the
	 * output. A truncated stream is ALWAYS a named failure and never a partial
	 * result, so `_out` is cleared on a failure.
	 *
	 * THERE IS NO CURSOR AND NO OUTPUT CEILING. The decoder stops at the FIRST
	 * end marker and IGNORES whatever follows it, which is what makes
	 * loadSection's consumed-length identity necessary. There is no ceiling on
	 * the output because none is needed: a length-extension chain adds 255 for
	 * each byte it reads, so the output grows to about 255 bytes for each byte
	 * of input and no further. The exposure is LINEAR in the input size, and
	 * the declared uncompressed length is the only bound there is. */
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

		// A section that holds plain bytes and no LZO1X stream. See the report
		// at the top of this file.
		bool isStored() const { return compressedLength == 0; }
	};

	/* The parsed header and section table.
	 *
	 * `version` is the RAW 16-BIT WORD and not text. Design section 15.5 item 5
	 * saves it in plugin state, so it is kept as it was read. */
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
	 * `uncompressedLength`, then the CONSUMED length against `compressedLength`,
	 * then the plain checksum. Design section 7.3 step 3 requires both checksum
	 * verifications and this adds the two length identities.
	 *
	 * WHY THE CONSUMED LENGTH IS CHECKED AT ALL. The decoder stops at the end
	 * marker and ignores what follows, so a section whose declared compressed
	 * extent is LONGER than the stream it holds decodes cleanly and passes BOTH
	 * checksums -- the compressed checksum is computed over the declared extent
	 * and therefore covers the junk as well. Without this identity that case
	 * could not fail. The decoder returns no cursor, so the identity is
	 * recovered the way the oracle recovers it: the decoder stops at the FIRST
	 * end marker, so no shorter prefix of a well-formed stream can decode, and
	 * the stream used every declared byte if and only if the same stream ONE
	 * BYTE SHORTER fails. That costs one more decompression of the section.
	 *
	 * A STORED section, `compressedLength == 0`: the plain bytes are the bytes
	 * at the file offset, there is no stream, the compressed checksum field is
	 * NOT read, and the plain checksum is the one check.
	 *
	 * Returns false and writes `_error`, which opens with
	 * CONTAINER-SECTION-OUT-OF-RANGE, CONTAINER-COMPRESSED-CHECKSUM,
	 * CONTAINER-LENGTH-MISMATCH, CONTAINER-TRAILING-BYTES or
	 * CONTAINER-PLAIN-CHECKSUM, and names the section; or with an LZO- name
	 * when the stream itself is malformed, so that a caller can tell the two
	 * layers apart. */
	bool loadSection(const std::vector<uint8_t>& _image, const ContainerSection& _section,
		std::vector<uint8_t>& _plain, std::string& _error);

	/* Parses the container and loads every section, IN TABLE ORDER. Stops at the
	 * first failure and leaves `_loaded` holding nothing. */
	bool loadSections(const std::vector<uint8_t>& _image,
		std::vector<std::pair<ContainerSection, std::vector<uint8_t>>>& _loaded, std::string& _error);
}
