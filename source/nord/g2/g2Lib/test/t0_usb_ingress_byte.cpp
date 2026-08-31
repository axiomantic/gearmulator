/* t0_usb_ingress_byte.cpp -- the end-to-end check that a patch byte reaches
 * the device register file the firmware reads.
 * Tier T0: no artifact, no firmware image, no file outside this repository.
 *
 * The property this file exists to hold: a byte of a `.pch2` object that a
 * plugin originates is readable, as that byte and not as the benign 0x00, at
 * the CS3 data port -- through Board::onRead, which is the exact function
 * pointer handed to mcf5307_create and therefore the exact path the MCU core
 * takes when the firmware executes a load from the ISP1181.
 *
 * It is the check of the Board selecting MCF5307_ISP1181_BACKEND_FULL_MODEL on
 * the handle it just created. Remove that call and the handle keeps the Stub
 * backend, which discards every packet and answers 0x00 at every offset, and
 * case 2 below reads 0x00.
 *
 * The instrument is the part's own peek command (0xD2) issued at the CS3
 * command port and read back at the CS3 data port, with the peek target
 * selected by the endpoint-configuration command (0x20 + the endpoint's
 * configuration slot, which is not its number). A non-zero reading means
 * nothing on its own, so it is never reported without a reading that is zero,
 * taken on the same handle through the same two bus calls:
 *
 *   control A (temporal)  the same board, the same endpoint, peeked before
 *                         any frame has crossed -- reads 0x00.
 *   control B (negative)  a board configured to deliver on endpoint 9, which
 *                         the device model refuses; every other step is
 *                         identical -- reads 0x00.
 *
 * Control B is what makes case 2 a measurement of delivery rather than of the
 * peek command answering something. Both controls run the identical code path
 * as the query; nothing here succeeds through a path the query does not take.
 *
 * Nothing in this file is an assert() and nothing catches an exception: NDEBUG
 * must change no case.
 */
#include "../board.h"
#include "../memoryMap.h"
#include "../internalClient.h"
#include "../transportHub.h"
#include "../crc16.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
	int failures = 0;

	void checkEqualByte(const uint8_t observed, const uint8_t expected,
		const char* const what)
	{
		if(observed == expected)
			return;
		printf("FAIL %s: observed 0x%02X, expected 0x%02X\n", what,
			unsigned(observed), unsigned(expected));
		++failures;
	}

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	/* The CS3 window. The base is memoryMap.h's; the size is the workspace
	 * logbook's 64 KiB derived from CSMR3, the same figure t0_cs3_wire states
	 * independently for the same reason -- this fixture builds its own
	 * BoardConfig. The two port offsets are the part's A0-on-CPU-A4 wiring:
	 * bit 4 set selects the command port. */
	constexpr uint32_t g_cs3Size    = 0x00010000u;
	constexpr uint32_t g_dataPort   = g2::g_cs3Base + 0x00u;
	constexpr uint32_t g_commandPort = g2::g_cs3Base + 0x10u;

	constexpr int g_byte = 1;

	/* The endpoint under test is the shipped default and not a literal here.
	 * This file measures the ingress path of the endpoint the protocol runs
	 * over, and `board.h` is where that is recorded; a literal would go stale
	 * silently the next time the default moves. */
	const int g_protocolEndpoint = g2::BoardConfig{}.usbProtocolEndpoint;

	constexpr uint8_t g_endpointConfigBase = 0x20u;
	constexpr uint8_t g_peekCommand        = 0xD2u;

	g2::BoardConfig makeConfig(const int _endpoint)
	{
		g2::BoardConfig config;
		config.memory.cs3 = {g2::g_cs3Base, g_cs3Size};
		config.usbProtocolEndpoint = _endpoint;
		return config;
	}

	/* The configuration slot order, which is not the endpoint number. ISP1362
	 * Rev. 06 section 15.1.1 orders the sixteen `0x20`..`0x2F` slots control
	 * OUT, control IN, then endpoints 1 to 14, so endpoint 0 is slot 0,
	 * endpoint 1 is slot 2, endpoint 2 is slot 3 and endpoint 3 is slot 4. The
	 * peek answers about the buffer the last configuration command selected,
	 * and that operand is one of these. Passing an endpoint number straight
	 * through selects a buffer one place low for every endpoint above 0; the
	 * read still succeeds and answers the model's benign 0x00, so the wrong
	 * answer arrives looking exactly like an empty buffer. */
	constexpr int g_bufferSlotOfEndpoint[4] = {0, 2, 3, 4};

	int bufferSlotOfEndpoint(const int _endpoint)
	{
		if(_endpoint < 0 || _endpoint >= 4)
			return -1;
		return g_bufferSlotOfEndpoint[_endpoint];
	}

	/* The instrument. Two writes and one read, all three through the same
	 * public bus callbacks the core drives. It reads the head byte of the OUT
	 * buffer the given endpoint delivers into, and answers the model's benign
	 * 0x00 when that buffer holds nothing. */
	uint8_t peekHeadByte(g2::Board& _board, const int _endpoint)
	{
		const int slot = bufferSlotOfEndpoint(_endpoint);

		if(slot < 0)
			return 0x00u;

		mcf5307_bus_status status = MCF5307_BUS_OK;

		g2::Board::onWrite(&_board, g_commandPort, g_byte,
			uint32_t(g_endpointConfigBase) + uint32_t(slot), &status);
		g2::Board::onWrite(&_board, g_commandPort, g_byte,
			uint32_t(g_peekCommand), &status);

		const uint32_t value = g2::Board::onRead(&_board, g_dataPort, g_byte, &status);

		check(status == MCF5307_BUS_OK,
			"the CS3 data-port read that carries the peek result completes with BUS_OK");

		return uint8_t(value & 0xffu);
	}

	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
	}

	void appendObject(std::vector<uint8_t>& _dst, const uint8_t _type,
		const size_t _length, const uint32_t _seed)
	{
		_dst.push_back(_type);
		_dst.push_back(static_cast<uint8_t>((_length >> 8) & 0xffu));
		_dst.push_back(static_cast<uint8_t>(_length & 0xffu));

		const size_t at = _dst.size();
		_dst.resize(at + _length);
		fillPattern(_dst.data() + at, _length, _seed);
	}

	/* The same in-process container t0_board_transport builds, for the same
	 * reason: a file read here would make this case tier T1. */
	std::vector<uint8_t> buildContainer(const std::vector<uint8_t>& _objects)
	{
		std::vector<uint8_t> file;

		const char* const ascii = "Version=Nord Modular G2 File Format 1\n";
		for(const char* p = ascii; *p != 0; ++p)
			file.push_back(static_cast<uint8_t>(*p));
		file.push_back(0);

		const size_t binaryHeader = file.size();

		file.push_back(0x17);
		file.push_back(0x00);

		for(const uint8_t b : _objects)
			file.push_back(b);

		file.push_back(0);
		file.push_back(0);

		const uint16_t crc = g2::crc16File(file.data(), file.size(), binaryHeader);
		g2::crc16Store(file.data() + file.size() - 2, crc);

		return file;
	}

	/* Load one single-object container into a board's hub through the same
	 * pch2Load a plugin calls, then cross one quantum boundary so the Board's
	 * own pump drains it into the device. Answers the object's first byte,
	 * which is what the peek must then read back. */
	uint8_t deliverOneObject(g2::Board& _board, const uint8_t _type,
		const size_t _payloadLength, const uint32_t _seed)
	{
		std::vector<uint8_t> objects;
		appendObject(objects, _type, _payloadLength, _seed);

		const std::vector<uint8_t> file = buildContainer(objects);

		g2::InternalClient client(_board.transport(), 512, 4);

		const g2::Pch2LoadResult result = g2::pch2Load(file.data(), file.size(), client);

		check(result == g2::Pch2LoadResult::Loaded,
			"the in-process container loads through pch2Load");

		_board.tickSofIfDue(0);

		return objects[0];
	}
}

int main()
{
	/* ------------------------------------------------------------- case 1.
	 * Control A, temporal. The instrument reads 0x00 on a board across which
	 * no frame has crossed. Without this, a non-zero reading in case 2 could
	 * be the peek command answering rather than a packet arriving. */
	{
		g2::Board board(makeConfig(g_protocolEndpoint));

		checkEqualByte(peekHeadByte(board, g_protocolEndpoint), 0x00u,
			"case 1: the peek reads 0x00 before any frame has crossed");
	}

	/* ------------------------------------------------------------- case 2.
	 * The measurement. A patch object crosses the quantum boundary and its
	 * first byte -- the object's type byte, the first byte of the frame the
	 * Board hands isp1181_rx -- is what the CS3 data port answers.
	 *
	 * The expected value is taken from the container this process built, not
	 * written here as a literal, so the case cannot pass against a device that
	 * answers a fixed byte that happens to match. */
	{
		g2::Board board(makeConfig(g_protocolEndpoint));

		const uint8_t firstByte = deliverOneObject(board, 0x21u, 15u, 71u);

		check(firstByte != 0x00u,
			"case 2 precondition: the object's first byte is not the benign value");

		checkEqualByte(peekHeadByte(board, g_protocolEndpoint), firstByte,
			"case 2: the CS3 data port answers the patch object's first byte");
	}

	/* ------------------------------------------------------------- case 3.
	 * The same measurement with a different byte. A device that answered one
	 * fixed non-zero value would pass case 2 and fail here. */
	{
		g2::Board board(makeConfig(g_protocolEndpoint));

		const uint8_t firstByte = deliverOneObject(board, 0x4Au, 20u, 72u);

		checkEqualByte(peekHeadByte(board, g_protocolEndpoint), firstByte,
			"case 3: a different object's first byte is answered as that byte");
	}

	/* ------------------------------------------------------------- case 4.
	 * Control B, negative. Endpoint 9 is one the device model refuses -- its
	 * endpoint table names 0 to 3 -- so the delivery is dropped inside the
	 * model. Everything else is identical: the same container, the same pump,
	 * the same two bus writes and the same bus read on a handle of the same
	 * kind. The reading is 0x00, which is what makes case 2's reading a
	 * measurement of delivery.
	 *
	 * The peek still selects the protocol endpoint, because the peek target is
	 * set by the configuration command and 9 names no buffer to peek at. */
	{
		g2::Board board(makeConfig(9));

		const uint8_t firstByte = deliverOneObject(board, 0x21u, 15u, 71u);

		check(firstByte == 0x21u,
			"case 4 precondition: the refused board carries the same first byte as case 2");

		checkEqualByte(peekHeadByte(board, g_protocolEndpoint), 0x00u,
			"case 4: a delivery on an endpoint the model refuses leaves the buffer empty");
	}

	if(failures != 0)
	{
		printf("t0_usb_ingress_byte: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_usb_ingress_byte: all cases passed\n");
	return 0;
}
