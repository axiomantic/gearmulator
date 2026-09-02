// Tier T0: this test runs with NMG2_ARTIFACTS unset and is registered UNGATED,
// because it is the check for the CFI blocker and a gated test cannot report on
// a blocker that gates the gate.
//
// Why query mode and not stored bytes. The CS2 container header OVERLAPS the
// CFI window completely. Measured from the L1 container builder and re-derived
// here from the same two constants: HEADER_SIZE = 0x14, ENTRY_STRIDE = 0x2C and
// an entry of a 4-byte tag followed by seven 32-bit fields, so section-table
// entry 0 spans 0x14 to 0x3F and its fields land at
//   tag 0x14, file offset 0x18, uncompressed length 0x1C, load address 0x20,
//   plain checksum 0x24, compressed length 0x28, compressed checksum 0x2C.
// The CFI signature reads at 0x20 and 0x22 fall inside the LOAD ADDRESS, the
// reads at 0x24 and 0x26 fall inside the PLAIN CHECKSUM, and the read at 0x28
// falls inside the COMPRESSED LENGTH whose zero names the stored form. Storing
// "QRY" at those offsets would corrupt the exact container the loader parses to
// find the OS. The firmware reads the same offsets through two paths and real
// hardware separates them only by mode, so the 0x0098 write is the mode
// selector and not a write to be accepted and ignored.
//
// The cases assert the transition and not the bytes: a CFI response is bytes
// that look like data, so each case pins the mode before the command and
// after it, and the mode is per Flash instance rather than per process.
//
// The Primary Vendor Command Set ID this model returns is 2, AMD/Fujitsu
// Standard. The source is flash.cpp's own comment -- "the erased state of an
// AMD-style flash" -- so 2 is what the existing model IMPLIES. That is a
// reading of a comment and not a firmware measurement, and the firmware accepts
// either value, so nothing here forecloses 1 if a measurement later says so.
//
// The ID is formed as the firmware combines it: the BYTE at CS2 + 0x26 is the
// high half and the BYTE at CS2 + 0x28 is the low half, which is read8 at those
// two addresses. A strict JEDEC 16-bit part carries each datum in the LOW byte
// of its word -- at 0x27 and 0x29 -- which is where Q, R and Y go here. The two
// placements disagree for the ID pair and cannot both hold; the firmware's own
// combination at 0x30004396 is what this model satisfies.

#include "../board.h"
#include "../flash.h"

#include <mcf5307.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

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

	// ---------------------------------------------------------------- fixture
	//
	// The CS0 and CS2 bases and sizes come from this fixture: no authority
	// records CS0's or CS2's base, so no shipped header carries a number and
	// this test supplies its own. The CS2 base is the one the firmware itself
	// programs: the loader reset stub writes CSAR2 = 0x1200, so
	// the window is 0x12000000 and the floor's addresses are absolute against
	// it. The SIZE is this fixture's own and is only large enough to hold the
	// header the cases read; the measured window is 8 MB and nothing here
	// depends on that.
	constexpr uint32_t kCs0Base = 0x30000000u;
	constexpr uint32_t kCs0Size = 0x00001000u;
	constexpr uint32_t kCs2Base = 0x12000000u;
	constexpr uint32_t kCs2Size = 0x00001000u;

	// The CFI protocol, from the floor and from the JEDEC standard that
	// corroborates it. Byte offset 0xAA is word address 0x55, and command 0x98
	// there is query-enter.
	constexpr uint32_t kQueryEnterOffset = 0x000000AAu;
	constexpr uint16_t kQueryEnterCommand = 0x0098u;

	// The exit command. 0xF0 is the AMD/Fujitsu reset, and this model advertises
	// AMD/Fujitsu Standard, so 0xF0 is the command it must honour. The firmware
	// writes it at 0x4eb0 in the loader and at 0x300043f4 in the OS, on the
	// branch it takes when the ID reads 2.
	constexpr uint16_t kResetCommand = 0x00F0u;

	// The command this model deliberately does NOT honour. 0xFF is the Intel
	// read-array command, written by the firmware at 0x4e7c and 0x300043c4 on
	// the branch it takes when the ID reads 1 -- a branch this model never
	// selects, because it answers 2. An AMD part does not reset on 0xFF, and a
	// model that answered to a command set it does not advertise would hide a
	// firmware taking the wrong branch.
	constexpr uint16_t kIntelReadArrayCommand = 0x00FFu;

	// The floor's own offsets, byte-absolute within the CS2 window.
	constexpr uint32_t kSignatureQ = 0x20u;
	// The probe reads a word, and the word is at 0x26. 0x30004350 is
	// `MOVEA.L #0x12000026,A4` and the loop below it reads a word through A4, so
	// the Primary Vendor Command Set ID occupies bytes 0x26 and 0x27. The
	// derivation is the firmware's, taken from the instruction and not from the
	// model this test checks.
	constexpr uint32_t kIdHighHalf = 0x26u;
	constexpr uint32_t kIdLowHalf  = 0x27u;

	// The block every mode assertion compares. It starts at the load address and
	// ends at the last byte of the compressed length: 0x20 to 0x2B.
	constexpr uint32_t kBlockStart = 0x20u;
	constexpr uint32_t kBlockLength = 0x0Cu;

	// The Primary Vendor Command Set ID this model returns. See the file header
	// for the citation and for the caveat that it is an implication.
	constexpr uint32_t kPrimaryVendorCommandSetId = 2u;

	// ------------------------------------------------- the L1 container header
	//
	// The three fields the CFI window collides with. Their values are this
	// test's own and carry no Clavia byte behind them, but their OFFSETS are
	// the measured L1 layout. Each byte the cases assert differs from the byte
	// the same address answers in query mode wherever the layout allows it, so
	// the mode transition is provable per byte and not only per block.
	//
	// The 0xFF hazard is avoided deliberately. An unloaded CS2 image is 0xFF
	// fill, so a test asserting against 0xFF cannot separate "the model
	// answered" from "nothing was ever loaded". Every asserted header byte
	// below is neither 0xFF nor a value the CFI table returns at that offset.
	constexpr uint32_t kLoadAddress   = 0x30A1B2C3u;
	constexpr uint32_t kPlainChecksum = 0x4D5E6F70u;

	// Zero names the STORED form, which is the value the real builder writes
	// for an uncompressed section. It is kept at zero for that reason, and it
	// is the one place where a header byte and a query-mode byte agree.
	constexpr uint32_t kCompressedLength = 0x00000000u;

	void poke32(std::vector<uint8_t>& _image, const uint32_t _offset, const uint32_t _value)
	{
		_image[_offset + 0] = static_cast<uint8_t>((_value >> 24) & 0xffu);
		_image[_offset + 1] = static_cast<uint8_t>((_value >> 16) & 0xffu);
		_image[_offset + 2] = static_cast<uint8_t>((_value >>  8) & 0xffu);
		_image[_offset + 3] = static_cast<uint8_t>( _value        & 0xffu);
	}

	// The container header, built at the measured L1 offsets: HEADER_SIZE 0x14,
	// entry 0 at 0x14 with a 4-byte tag then seven 32-bit fields.
	std::vector<uint8_t> makeContainerImage()
	{
		std::vector<uint8_t> image(kCs2Size, 0x00u);

		// The fixed header, 0x00 to 0x13. Distinctive so a case that read the
		// wrong offset would not silently match.
		for(uint32_t i = 0; i < 0x14u; ++i)
			image[i] = static_cast<uint8_t>(0x10u + i);

		image[0x14] = 'B';   // entry 0 tag
		image[0x15] = 'O';
		image[0x16] = 'O';
		image[0x17] = 'T';
		poke32(image, 0x18u, 0x00000200u);   // file offset
		poke32(image, 0x1Cu, 0x00012345u);   // uncompressed length
		poke32(image, 0x20u, kLoadAddress);
		poke32(image, 0x24u, kPlainChecksum);
		poke32(image, 0x28u, kCompressedLength);
		poke32(image, 0x2Cu, 0x6A7B8C9Du);   // compressed checksum

		return image;
	}

	// ------------------------------------------------------------- read helpers

	std::string toHex(const std::vector<uint8_t>& _bytes)
	{
		std::stringstream ss;
		ss << std::hex << std::setfill('0');
		for(size_t i = 0; i < _bytes.size(); ++i)
		{
			if(i)
				ss << ' ';
			ss << std::setw(2) << static_cast<uint32_t>(_bytes[i]);
		}
		return ss.str();
	}

	// The 0x20 to 0x2B block, read through the Flash model one byte at a time.
	std::string readBlock(const g2::Flash& _flash, const uint32_t _base)
	{
		std::vector<uint8_t> bytes;
		bytes.reserve(kBlockLength);
		for(uint32_t i = 0; i < kBlockLength; ++i)
			bytes.push_back(_flash.read8(_base + kBlockStart + i));
		return toHex(bytes);
	}

	// The expected block, in hex, for the container header the fixture loads.
	std::string expectedContainerBlock()
	{
		const std::vector<uint8_t> image = makeContainerImage();
		std::vector<uint8_t> bytes(image.begin() + kBlockStart,
		                           image.begin() + kBlockStart + kBlockLength);
		return toHex(bytes);
	}

	// The expected block, in hex, for the CFI answer. Q, R and Y sit in the LOW
	// bytes of the words at 0x20, 0x22 and 0x24 -- floor clause 1. The ID's high
	// half is the BYTE at 0x26 and its low half is the BYTE at 0x28 -- floor
	// clause 2, taken literally. Word addresses past 0x14 are not part of the
	// floor, so the model answers the image there and 0x2A and 0x2B stay the
	// container's compressed-length bytes.
	std::string expectedQueryBlock()
	{
		const std::vector<uint8_t> image = makeContainerImage();
		const std::vector<uint8_t> bytes = {
			0x00u, 0x51u,                                                  // word 0x10, 'Q'
			0x00u, 0x52u,                                                  // word 0x11, 'R'
			0x00u, 0x59u,                                                  // word 0x12, 'Y'
			// The ID is the word at 0x26, so its high half is 0x26 and its low
			// half is 0x27. Word 0x14 at 0x28/0x29 is the Extended Table
			// pointer, which this model does not advertise and answers as zero.
			static_cast<uint8_t>((kPrimaryVendorCommandSetId >> 8) & 0xffu),
			static_cast<uint8_t>( kPrimaryVendorCommandSetId       & 0xffu),  // 0x26, 0x27
			0x00u, 0x00u,                                                     // 0x28, 0x29
			image[0x2Au], image[0x2Bu]                                     // past the floor
		};
		return toHex(bytes);
	}

	// Floor clause 2's own formula, applied to whatever the model answers.
	uint32_t derivePrimaryVendorCommandSetId(const g2::Flash& _flash, const uint32_t _base)
	{
		return (static_cast<uint32_t>(_flash.read8(_base + kIdHighHalf)) << 8) |
		        static_cast<uint32_t>(_flash.read8(_base + kIdLowHalf));
	}

	// ------------------------------------------------------- the board fixture

	g2::BoardConfig makeBoardConfig()
	{
		g2::BoardConfig config;
		config.memory.cs0 = {kCs0Base, kCs0Size};
		config.memory.cs2 = {kCs2Base, kCs2Size};
		return config;
	}

	// Board::onRead and Board::onWrite are the function pointers handed to the
	// MCF5307 core, so they are what the firmware will drive. They are called
	// here rather than busRead and busWrite for exactly that reason.
	//
	// That makes the size argument the core's unit, which is a count of BYTES.
	// mcf5307.h states it twice, once per callback typedef. The three
	// constants below are named rather than written as bare 1 and 2, because
	// a file that passes 8 and 16 here -- the MemoryMap's unit -- and a
	// silent swap of one unit for another is the defect the conversion in
	// board.cpp exists to prevent.
	constexpr int g_byte = 1;
	constexpr int g_word = 2;

	uint32_t boardRead(g2::Board& _board, const uint32_t _address, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, _size, &_status);
	}

	void boardWrite(g2::Board& _board, const uint32_t _address, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		g2::Board::onWrite(&_board, _address, _size, _value, &_status);
	}

	std::string readBoardBlock(g2::Board& _board)
	{
		std::vector<uint8_t> bytes;
		bytes.reserve(kBlockLength);
		for(uint32_t i = 0; i < kBlockLength; ++i)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			bytes.push_back(static_cast<uint8_t>(
				boardRead(_board, kCs2Base + kBlockStart + i, g_byte, status) & 0xffu));
		}
		return toHex(bytes);
	}
}

int main()
{
	const std::string containerBlock = expectedContainerBlock();
	const std::string queryBlock     = expectedQueryBlock();

	// The two expected blocks must differ, or every mode assertion below would
	// pass against a model that never changed mode at all. This is a check on
	// the test itself and it runs first.
	check(containerBlock != queryBlock,
		"the container block and the CFI block differ, so a mode assertion can fail");

	// ---------------- case 1: out of query mode, the container header answers
	//
	// The addresses the CFI probe reads answer their ORDINARY CONTENT when the
	// device is not in query mode.

	g2::Flash flash(kCs0Base, kCs0Size, kCs2Base, kCs2Size);
	flash.loadCs2(makeContainerImage());

	check(!flash.cs2InQueryMode(),
		"a freshly constructed Flash is NOT in query mode");
	checkEqual(readBlock(flash, kCs2Base), containerBlock,
		"out of query mode the CFI offsets answer the container header, byte for byte");

	// The firmware's own branch: an ID outside {1, 2} falls through to a return
	// of 0 and the gate halts. Out of query mode the model must land outside.
	const std::vector<uint8_t> loadedImage = makeContainerImage();
	const uint32_t headerId = derivePrimaryVendorCommandSetId(flash, kCs2Base);
	checkEqual(headerId,
		uint32_t((static_cast<uint32_t>(loadedImage[kIdHighHalf]) << 8) |
		          static_cast<uint32_t>(loadedImage[kIdLowHalf])),
		"out of query mode the derived ID is the container's own bytes");
	check(headerId != 1u && headerId != 2u,
		"out of query mode the derived ID is OUTSIDE {1, 2}, the branch the firmware rejects");

	// ---------------- case 2: the query-enter write changes the MODE
	//
	// This is the whole mechanism. The assertion is on the mode flag, not on
	// the bytes, because bytes alone cannot separate a protocol from a store.

	flash.write16(kCs2Base + kQueryEnterOffset, kQueryEnterCommand);
	check(flash.cs2InQueryMode(),
		"the 0x0098 write to CS2 + 0xAA puts the device IN query mode");

	// ---------------- case 3: in query mode the CFI table answers

	checkEqual(readBlock(flash, kCs2Base), queryBlock,
		"in query mode the same offsets answer the CFI table, byte for byte");
	checkEqual(uint32_t(flash.read16(kCs2Base + kSignatureQ + 0)), uint32_t(0x0051u),
		"the word at CS2 + 0x20 carries 'Q' in its low byte");
	checkEqual(uint32_t(flash.read16(kCs2Base + kSignatureQ + 2)), uint32_t(0x0052u),
		"the word at CS2 + 0x22 carries 'R' in its low byte");
	checkEqual(uint32_t(flash.read16(kCs2Base + kSignatureQ + 4)), uint32_t(0x0059u),
		"the word at CS2 + 0x24 carries 'Y' in its low byte");
	checkEqual(derivePrimaryVendorCommandSetId(flash, kCs2Base), kPrimaryVendorCommandSetId,
		"the ID formed from the bytes at CS2 + 0x26 and CS2 + 0x28 is 2, AMD/Fujitsu Standard");

	// ---------------- case 4: query mode does not swallow the rest of the map
	//
	// The bytes just below the floor's window must still be the image.

	checkEqual(uint32_t(flash.read8(kCs2Base + 0x1Cu)), uint32_t(0x00u),
		"in query mode CS2 + 0x1C is still the container's uncompressed length");
	checkEqual(uint32_t(flash.read8(kCs2Base + 0x1Fu)), uint32_t(0x45u),
		"in query mode CS2 + 0x1F is still the container's uncompressed length");
	checkEqual(uint32_t(flash.read8(kCs0Base)), uint32_t(0xFFu),
		"CS2 query mode does not change what CS0 answers");

	// ---------------- case 5: the reset command EXITS, and the image survived
	//
	// The container header is compared byte for byte after the reset command:
	// a signature written into the image would have destroyed it.

	flash.write16(kCs2Base, kResetCommand);
	check(!flash.cs2InQueryMode(),
		"the 0x00F0 reset command takes the device OUT of query mode");
	checkEqual(readBlock(flash, kCs2Base), containerBlock,
		"after the reset the container header is INTACT: the signature was never stored");

	// ---------------- case 6: the address is part of the command
	//
	// 0x0098 written anywhere else must not enter query mode.

	flash.write16(kCs2Base + kQueryEnterOffset + 2u, kQueryEnterCommand);
	check(!flash.cs2InQueryMode(),
		"0x0098 written at CS2 + 0xAC does NOT enter query mode: the address is part of the command");

	flash.write16(kCs2Base + kQueryEnterOffset, 0x0099u);
	check(!flash.cs2InQueryMode(),
		"0x0099 at CS2 + 0xAA does NOT enter query mode: the value is part of the command");

	// ---------------- case 7: the Intel read-array command is NOT honoured
	//
	// This model advertises AMD/Fujitsu Standard, so 0xFF is not one of its
	// commands.

	flash.write16(kCs2Base + kQueryEnterOffset, kQueryEnterCommand);
	check(flash.cs2InQueryMode(),
		"re-entering query mode for the Intel-command case");
	flash.write16(kCs2Base, kIntelReadArrayCommand);
	check(flash.cs2InQueryMode(),
		"0x00FF does NOT exit query mode: this model advertises AMD/Fujitsu, not Intel");
	flash.write16(kCs2Base, kResetCommand);
	check(!flash.cs2InQueryMode(),
		"0x00F0 still exits after the ignored 0x00FF");

	// ---------------- case 8: the mode is PER INSTANCE
	//
	// A file-local static in flash.cpp would be process-global, and both
	// objects would report the same mode.

	{
		g2::Flash first (kCs0Base, kCs0Size, kCs2Base, kCs2Size);
		g2::Flash second(kCs0Base, kCs0Size, kCs2Base, kCs2Size);
		first.loadCs2(makeContainerImage());
		second.loadCs2(makeContainerImage());

		first.write16(kCs2Base + kQueryEnterOffset, kQueryEnterCommand);

		check(first.cs2InQueryMode(),
			"the first Flash is in query mode");
		check(!second.cs2InQueryMode(),
			"the second Flash in the SAME PROCESS is NOT in query mode");
		checkEqual(readBlock(first, kCs2Base), queryBlock,
			"the first Flash answers the CFI table");
		checkEqual(readBlock(second, kCs2Base), containerBlock,
			"the second Flash answers its container header: the mode did not leak");
	}

	// ---------------- case 9: an unloaded CS2 derives the ID the firmware rejects
	//
	// An erased CS2 gives 0xFF fill, an ID of 0xFFFF, the firmware falling
	// through to a return of 0 and the gate halting.

	{
		g2::Flash erased(kCs0Base, kCs0Size, kCs2Base, kCs2Size);
		checkEqual(derivePrimaryVendorCommandSetId(erased, kCs2Base), uint32_t(0xFFFFu),
			"an unloaded CS2 derives an ID of 0xFFFF, which the firmware rejects");
		erased.write16(kCs2Base + kQueryEnterOffset, kQueryEnterCommand);
		checkEqual(derivePrimaryVendorCommandSetId(erased, kCs2Base), kPrimaryVendorCommandSetId,
			"in query mode an unloaded CS2 still answers the ID: the table is not the image");
	}

	// ---------------- case 10: the entry points the CORE uses
	//
	// The firmware drives Board::onRead and Board::onWrite, so the whole
	// sequence is repeated here through the callbacks the core is given and
	// not only against the model, where the routing could drop the command.

	{
		g2::Board board(makeBoardConfig());
		board.flash().loadCs2(makeContainerImage());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		check(!board.flash().cs2InQueryMode(),
			"through the board: the device starts OUT of query mode");
		checkEqual(readBoardBlock(board), containerBlock,
			"through the board: the container header answers before the command");

		boardWrite(board, kCs2Base + kQueryEnterOffset, g_word, kQueryEnterCommand, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
			"through the board: the 0x0098 write COMPLETES without a bus fault");
		check(board.flash().cs2InQueryMode(),
			"through the board: the 0x0098 write puts the device IN query mode");
		checkEqual(readBoardBlock(board), queryBlock,
			"through the board: the CFI table answers after the command");

		boardWrite(board, kCs2Base, g_word, kResetCommand, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
			"through the board: the reset write COMPLETES without a bus fault");
		check(!board.flash().cs2InQueryMode(),
			"through the board: the reset command exits query mode");
		checkEqual(readBoardBlock(board), containerBlock,
			"through the board: the container header is INTACT after the round trip");
	}

	if(g_failures)
	{
		std::cout << "t0_cs2_cfi: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_cs2_cfi: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
