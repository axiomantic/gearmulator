// Tier T0: this test needs no firmware artifact of any kind.
//
// Why a static status byte cannot satisfy this file. The interlock conditions
// the replay below asserts include a pair that are directly contradictory for
// any constant MBSR -- MBB must read CLEAR after a STOP and SET after the next
// START -- and a second pair that require MIF to be re-settable after a
// software clear.
//
// The address case is written first on purpose. A slave that acknowledges every
// address passes every bus-level case in this file while hiding a wrong
// address, so the discriminating case comes before the ones that depend on it.
//
// Every voltage and every reference in this file is this fixture's. The
// schematic's 3.033 V is a configuration argument and no shipped header carries
// it; the values below are powers of two so that every expected conversion
// result is an exact integer rather than a rounding of one.

#include "board.h"
#include "max1039.h"
#include "mbus.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
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

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << hex32(_expected)
		          << ">, got <" << hex32(_actual) << ">" << std::endl;
		++g_failures;
	}

	std::string joinBytes(const std::vector<uint8_t>& _bytes)
	{
		std::string result;
		for(const uint8_t byte : _bytes)
		{
			if(!result.empty())
				result += ',';
			result += std::to_string(unsigned(byte));
		}
		return result;
	}

	void checkBytes(const std::vector<uint8_t>& _actual, const std::vector<uint8_t>& _expected,
	                const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << joinBytes(_expected)
		          << ">, got <" << joinBytes(_actual) << ">" << std::endl;
		++g_failures;
	}

	void checkLog(const std::vector<std::string>& _actual, const std::vector<std::string>& _expected,
	              const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected " << _expected.size()
		          << " line(s), got " << _actual.size() << std::endl;
		for(const std::string& line : _actual)
			std::cout << "     got: " << line << std::endl;
		for(const std::string& line : _expected)
			std::cout << "     want: " << line << std::endl;
		++g_failures;
	}

	// -----------------------------------------------------------------------
	// The fixture's electrical values. Every one is a power of two so that
	// volts * 256 / reference is an exact integer for every channel below.
	// None of them is a claim about the machine: the schematic's 3.033 V is a
	// configuration argument and this fixture deliberately does not use it, so
	// a hardcoded reference cannot pass.
	constexpr float g_externalReferenceVolts = 4.0f;
	constexpr float g_supplyVolts            = 8.0f;
	constexpr float g_internalReferenceVolts = 1.0f;

	// Channel n carries 10*(n+1) counts at the external reference, so every
	// scan result names the channel that produced it.
	constexpr float g_countVolts = g_externalReferenceVolts / 256.0f;

	float channelInput(const unsigned _channel)
	{
		return g_countVolts * float(10u * (_channel + 1u));
	}

	uint8_t channelCode(const unsigned _channel)
	{
		return uint8_t(10u * (_channel + 1u));
	}

	g2::Max1039Config makeAdcConfig()
	{
		g2::Max1039Config config;
		config.externalReferenceVolts = g_externalReferenceVolts;
		config.supplyVolts            = g_supplyVolts;
		config.internalReferenceVolts = g_internalReferenceVolts;
		return config;
	}

	void driveChannels(g2::Max1039& _adc)
	{
		for(unsigned channel = 0; channel < g2::Max1039::g_settableChannels; ++channel)
			_adc.setChannelVolts(uint8_t(channel), channelInput(channel));
	}

	// -----------------------------------------------------------------------
	// The measured bytes. Each one is read from the firmware disassembly and
	// each is named rather than written as a literal at its use site.
	constexpr uint8_t g_addressWrite = 0xCAu;   // 0x65 << 1, R/W clear
	constexpr uint8_t g_addressRead  = 0xCBu;   // 0x65 << 1, R/W set
	constexpr uint8_t g_setupByte    = 0xAAu;   // SEL=010, CLK=1, BIP=0, RST=1
	constexpr uint8_t g_configByte   = 0x0Du;   // SCAN=00, CS=6, SGL/DIF=1

	// The scan length the measured configuration byte asks for.
	constexpr unsigned g_measuredScanLength = 7u;

	// -----------------------------------------------------------------------
	// Driving the controller the way the firmware does. Every access below is
	// a byte access, because every M-Bus register is byte-wide.
	constexpr int g_byteWidth = 8;

	uint8_t rd(g2::MBus& _bus, const uint32_t _offset)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		return uint8_t(_bus.read(_offset, g_byteWidth, status));
	}

	void wr(g2::MBus& _bus, const uint32_t _offset, const uint8_t _value)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		_bus.write(_offset, g_byteWidth, _value, status);
	}

	// The read-modify-write pair the firmware performs on MBCR.
	void setBits(g2::MBus& _bus, const uint32_t _offset, const uint8_t _mask)
	{
		wr(_bus, _offset, uint8_t(rd(_bus, _offset) | _mask));
	}

	void clearBits(g2::MBus& _bus, const uint32_t _offset, const uint8_t _mask)
	{
		wr(_bus, _offset, uint8_t(rd(_bus, _offset) & uint8_t(~_mask)));
	}

	bool mbsrBit(g2::MBus& _bus, const uint8_t _mask)
	{
		return (rd(_bus, g2::MBus::g_mbsr) & _mask) != 0u;
	}

	// -----------------------------------------------------------------------
	// The interlock conditions, observed at the instruction the firmware
	// observes them at. The replay below reproduces the measured transaction
	// sequence -- the recovery block, transaction A (setup byte), transaction B
	// (configuration byte), and the state machine's states 0 to 3 -- and
	// records what MBSR reported at each poll.
	struct Interlock
	{
		bool busIdleAtModuleEnable   = false;
		bool mifAfterAddressByte     = false;   // condition 1
		bool mifClearedBySoftware    = false;
		bool mifAfterSetupByte       = false;   // condition 2
		bool mbbClearAfterStop       = false;   // condition 3
		bool mbbClearInState0        = false;   // condition 4
		bool mbbSetAfterState1Start  = false;   // condition 5
		bool mifAfterEveryReadByte   = false;   // condition 6
		bool mifStaysClearAfterClear = false;   // condition 6, second half
		bool ackedAddressByte        = false;
		std::vector<uint8_t> received;
	};

	// One write transaction: START, address byte, one payload byte, STOP.
	void writeTransaction(g2::MBus& _bus, const uint8_t _payload)
	{
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_mtx);
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		wr(_bus, g2::MBus::g_mbdr, g_addressWrite);
		clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		wr(_bus, g2::MBus::g_mbdr, _payload);
		clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
	}

	Interlock replayFirmware(g2::MBus& _bus, const unsigned _bytesToRead)
	{
		Interlock out;

		// mbus_init, steps 1 to 6: module off, MFDR, MADR, MBSR cleared, MEN
		// set. The measured MFDR value is 0x0C and the firmware never reads it.
		wr(_bus, g2::MBus::g_mbcr, 0x00u);
		wr(_bus, g2::MBus::g_mfdr, 0x0Cu);
		wr(_bus, g2::MBus::g_madr, 0x00u);
		wr(_bus, g2::MBus::g_mbsr, 0x00u);
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_men);

		// Step 7: the recovery block is skipped when the bus is idle.
		out.busIdleAtModuleEnable = !mbsrBit(_bus, g2::MBus::g_mbb);

		// Transaction A, steps 18 to 32. The two conditions the address byte
		// and the setup byte impose are read at the two poll sites.
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_mtx);
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		wr(_bus, g2::MBus::g_mbdr, g_addressWrite);

		out.mifAfterAddressByte = mbsrBit(_bus, g2::MBus::g_mif);
		out.ackedAddressByte    = !mbsrBit(_bus, g2::MBus::g_rxak);

		clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		out.mifClearedBySoftware = !mbsrBit(_bus, g2::MBus::g_mif);

		wr(_bus, g2::MBus::g_mbdr, g_setupByte);
		out.mifAfterSetupByte = mbsrBit(_bus, g2::MBus::g_mif);

		clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		out.mbbClearAfterStop = !mbsrBit(_bus, g2::MBus::g_mbb);

		// Transaction B, steps 35 to 51: the configuration byte.
		writeTransaction(_bus, g_configByte);

		// State 0, steps 52 to 54: clear MSTA, then require MBB clear.
		clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		out.mbbClearInState0 = !mbsrBit(_bus, g2::MBus::g_mbb);

		// State 1, steps 55 to 60: START, then require MBB set.
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		out.mbbSetAfterState1Start = mbsrBit(_bus, g2::MBus::g_mbb);

		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_mtx);
		wr(_bus, g2::MBus::g_mbdr, g_addressRead);

		// State 2, steps 61 to 70: MIF must be set by the address byte, then
		// the controller drops to receive mode and the dummy read starts the
		// first conversion.
		const bool mifAfterReadAddress = mbsrBit(_bus, g2::MBus::g_mif);
		clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_mtx);
		setBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_txak);
		(void)rd(_bus, g2::MBus::g_mbdr);

		// State 3, steps 71 to 76, repeating for ever.
		bool everyByteSetMif = mifAfterReadAddress;
		bool staysClear      = true;

		for(unsigned byte = 0; byte < _bytesToRead; ++byte)
		{
			if(!mbsrBit(_bus, g2::MBus::g_mif))
			{
				everyByteSetMif = false;
				break;
			}

			clearBits(_bus, g2::MBus::g_mbsr, g2::MBus::g_mif);

			if(mbsrBit(_bus, g2::MBus::g_mif))
				staysClear = false;

			clearBits(_bus, g2::MBus::g_mbcr, g2::MBus::g_txak);
			out.received.push_back(rd(_bus, g2::MBus::g_mbdr));
		}

		out.mifAfterEveryReadByte   = everyByteSetMif;
		out.mifStaysClearAfterClear = staysClear;

		return out;
	}

	std::vector<uint8_t> expectedScan(const unsigned _length, const unsigned _repeats)
	{
		std::vector<uint8_t> expected;
		for(unsigned repeat = 0; repeat < _repeats; ++repeat)
		{
			for(unsigned channel = 0; channel < _length; ++channel)
				expected.push_back(channelCode(channel));
		}
		return expected;
	}

	// -----------------------------------------------------------------------
	// The board fixture for the routing cases. Only the MBAR window is
	// populated: the clause is about the router, and a window this test never
	// drives would only add ways for it to fail for an unrelated reason.
	constexpr uint32_t g_mbarBase = 0x16000000u;

	constexpr int g_byte = 1;
	constexpr int g_word = 2;

	g2::BoardConfig makeBoardConfig()
	{
		g2::BoardConfig config;
		config.memory.mbar = {g_mbarBase, g2::g_simSpaceSize};
		config.adc         = makeAdcConfig();
		return config;
	}

	uint32_t busRead(g2::Board& _board, const uint32_t _address, const int _size,
	                 mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, _size, &_status);
	}

	void busWrite(g2::Board& _board, const uint32_t _address, const int _size,
	              const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		g2::Board::onWrite(&_board, _address, _size, _value, &_status);
	}
}

int main()
{
	// ==================================================================
	// Clause 5 case 1 -- the address discriminates. Written first because a
	// slave that answers every address passes every other case in this file.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());

		checkEqual(adc.address(), g2::g_max1039Address,
		           "the shipped default slave address is the measured 0x65");

		unsigned acknowledged = 0;
		unsigned matchAcked   = 0;

		for(unsigned address = 0; address < 128u; ++address)
		{
			if(!adc.start(uint8_t(address), false))
				continue;
			adc.stop();
			++acknowledged;
			if(address == g2::g_max1039Address)
				++matchAcked;
		}

		checkEqual(acknowledged, 1u,
		           "exactly one of the 128 seven-bit addresses is acknowledged");
		checkEqual(matchAcked, 1u,
		           "the one acknowledged address is the modelled slave address");

		check(adc.start(g2::g_max1039Address, true),
		      "the modelled address is acknowledged for a read as well as a write");
		adc.stop();

		// The HS-mode master code 0000 1xxx is not acknowledged, and that is
		// the expected response rather than an error. It needs no rule of its
		// own: the master code's seven address bits are 0x04 to 0x07, which
		// the sweep above already found unacknowledged.
		for(unsigned code = 0x08u; code < 0x10u; ++code)
		{
			check(!adc.start(uint8_t(code >> 1), (code & 1u) != 0u),
			      "the HS-mode master code byte " + std::to_string(code)
			      + " is not acknowledged");
		}

		// A slave built at a different address answers there and nowhere else,
		// which is what makes the address a parameter rather than a constant.
		g2::Max1039Config otherConfig = makeAdcConfig();
		otherConfig.address = 0x64u;
		g2::Max1039 other(otherConfig);

		check(other.start(0x64u, false), "a slave built at 0x64 acknowledges 0x64");
		other.stop();
		check(!other.start(g2::g_max1039Address, false),
		      "a slave built at 0x64 does not acknowledge 0x65");
	}

	// ==================================================================
	// Clause 5 case 1, through the controller. RXAK carries the slave's
	// answer, so a wrong address is visible on the bus and not only in the
	// slave's own return value.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		g2::MBus bus(&adc);

		wr(bus, g2::MBus::g_mbcr, uint8_t(g2::MBus::g_men | g2::MBus::g_mtx));
		setBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		wr(bus, g2::MBus::g_mbdr, g_addressWrite);
		check(!mbsrBit(bus, g2::MBus::g_rxak),
		      "the measured address byte 0xCA is acknowledged and RXAK stays clear");

		clearBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		clearBits(bus, g2::MBus::g_mbsr, g2::MBus::g_mif);
		setBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		wr(bus, g2::MBus::g_mbdr, 0xC8u);
		check(mbsrBit(bus, g2::MBus::g_rxak),
		      "an address byte for 0x64 is not acknowledged and RXAK is set");

		// A controller with no slave behind it must report the same NACK, or
		// an absent slave is indistinguishable from a present one.
		g2::MBus empty(nullptr);
		wr(empty, g2::MBus::g_mbcr, uint8_t(g2::MBus::g_men | g2::MBus::g_mtx));
		setBits(empty, g2::MBus::g_mbcr, g2::MBus::g_msta);
		wr(empty, g2::MBus::g_mbdr, g_addressWrite);
		check(mbsrBit(empty, g2::MBus::g_rxak),
		      "a controller with no slave attached reports RXAK set");
	}

	// ==================================================================
	// Clause 1 -- the busy bit is dynamic. The two named cases first, then the
	// conditions the measured firmware imposes.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		g2::MBus bus(&adc);

		check(!mbsrBit(bus, g2::MBus::g_mbb),
		      "MBSR reports the bus idle before any MSTA write");

		setBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		check(mbsrBit(bus, g2::MBus::g_mbb),
		      "MBSR reports the busy bit SET after the MSTA write to MBCR");

		clearBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		check(!mbsrBit(bus, g2::MBus::g_mbb),
		      "MBSR reports the busy bit CLEAR after the STOP transition");
	}

	{
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);
		g2::MBus bus(&adc);

		const Interlock observed = replayFirmware(bus, g_measuredScanLength * 3u);

		check(observed.busIdleAtModuleEnable,
		      "the bus reads idle at module enable, so the recovery block is skipped");
		check(observed.ackedAddressByte,
		      "the measured address byte is acknowledged during the replay");

		check(observed.mifAfterAddressByte,
		      "condition 1: MIF goes SET after the address byte with MSTA newly set");
		check(observed.mifClearedBySoftware,
		      "a software write of zero to MBSR bit 1 clears MIF");
		check(observed.mifAfterSetupByte,
		      "condition 2: MIF goes SET again after the setup byte");
		check(observed.mbbClearAfterStop,
		      "condition 3: MBB goes CLEAR after MSTA is cleared");
		check(observed.mbbClearInState0,
		      "condition 4: MBB reads CLEAR in the state machine's state 0");
		check(observed.mbbSetAfterState1Start,
		      "condition 5: MBB goes SET after state 1's MSTA write");
		check(observed.mifAfterEveryReadByte,
		      "condition 6: MIF goes SET after every received byte");
		check(observed.mifStaysClearAfterClear,
		      "condition 6: MIF stays clear after each software clear");

		checkBytes(observed.received, expectedScan(g_measuredScanLength, 3u),
		           "the replay reads three whole scans of the measured configuration");
	}

	// ==================================================================
	// The control register is modelled and not discarded.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		g2::MBus bus(&adc);

		wr(bus, g2::MBus::g_mbcr, 0x98u);
		checkEqual(rd(bus, g2::MBus::g_mbcr), 0x98u,
		           "MBCR returns the value written to it");

		// The read-modify-write pair the firmware performs on MBCR throughout
		// the transaction.
		setBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		checkEqual(rd(bus, g2::MBus::g_mbcr), 0xB8u,
		           "a read-modify-write of MBCR preserves the bits it did not touch");

		clearBits(bus, g2::MBus::g_mbcr, g2::MBus::g_mtx);
		checkEqual(rd(bus, g2::MBus::g_mbcr), 0xA8u,
		           "a second read-modify-write of MBCR preserves the rest again");
	}

	{
		g2::Max1039 adc(makeAdcConfig());
		g2::MBus bus(&adc);

		// The busy bit responds to the MSTA write to MBCR ...
		setBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);
		check(mbsrBit(bus, g2::MBus::g_mbb),
		      "the MBSR busy bit responds to a write of MBCR");
		clearBits(bus, g2::MBus::g_mbcr, g2::MBus::g_msta);

		// ... and to nothing else on the module. The same bit pattern written
		// to the two registers that carry no control bits must leave it clear.
		wr(bus, g2::MBus::g_madr, g2::MBus::g_msta);
		check(!mbsrBit(bus, g2::MBus::g_mbb),
		      "a write of the MSTA bit pattern to MADR leaves the busy bit clear");

		wr(bus, g2::MBus::g_mfdr, g2::MBus::g_msta);
		check(!mbsrBit(bus, g2::MBus::g_mbb),
		      "a write of the MSTA bit pattern to MFDR leaves the busy bit clear");

		checkEqual(rd(bus, g2::MBus::g_madr), g2::MBus::g_msta,
		           "MADR stores and returns what was written");
		checkEqual(rd(bus, g2::MBus::g_mfdr), g2::MBus::g_msta,
		           "MFDR stores and returns what was written");
	}

	// ==================================================================
	// The access widths and the window bound. Every M-Bus register is
	// byte-wide, so a 16- or 32-bit access is rejected and logged with the
	// offset, the width and the direction.
	// ==================================================================
	{
		const uint32_t registers[] =
		{
			g2::MBus::g_madr, g2::MBus::g_mfdr, g2::MBus::g_mbcr,
			g2::MBus::g_mbsr, g2::MBus::g_mbdr
		};

		for(const uint32_t offset : registers)
		{
			g2::Max1039 adc(makeAdcConfig());
			g2::MBus bus(&adc);

			mcf5307_bus_status status = MCF5307_BUS_OK;
			bus.write(offset, 8, 0x00u, status);
			checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
			           "a byte write at " + hex32(offset) + " is legal");

			status = MCF5307_BUS_OK;
			(void)bus.read(offset, 8, status);
			checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
			           "a byte read at " + hex32(offset) + " is legal");

			bus.clearLog();

			status = MCF5307_BUS_OK;
			bus.write(offset, 16, 0x00u, status);
			checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
			           "a 16-bit write at " + hex32(offset) + " is rejected");
			checkLog(bus.log(),
			         {"mbus: SIZE_ILLEGAL write of 16 bits at offset " + hex32(offset)},
			         "the rejected 16-bit write at " + hex32(offset) + " writes one log line");

			bus.clearLog();

			status = MCF5307_BUS_OK;
			(void)bus.read(offset, 32, status);
			checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
			           "a 32-bit read at " + hex32(offset) + " is rejected");
			checkLog(bus.log(),
			         {"mbus: SIZE_ILLEGAL read of 32 bits at offset " + hex32(offset)},
			         "the rejected 32-bit read at " + hex32(offset) + " writes one log line");
		}
	}

	{
		// An offset inside the module window that carries no register is
		// UNMODELLED, which is the answer sim.cpp already gives and which this
		// model does not re-decide.
		g2::Max1039 adc(makeAdcConfig());
		g2::MBus bus(&adc);

		mcf5307_bus_status status = MCF5307_BUS_OK;
		(void)bus.read(g2::MBus::g_madr + 1u, 8, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "an unmodelled offset inside the module window completes");
		checkLog(bus.log(),
		         {"mbus: UNMODELLED read of 8 bits at offset "
		          + hex32(g2::MBus::g_madr + 1u)},
		         "an unmodelled offset inside the module window writes one log line");
	}

	// ==================================================================
	// The router reaches the new unit and still reaches the older two. Every
	// access goes through the installed callback, because that is the path the
	// core takes.
	// ==================================================================
	{
		g2::Board board(makeBoardConfig());
		mcf5307_bus_status status = MCF5307_BUS_OK;

		// The M-Bus arm. The discriminator is the interlock and not a stored
		// byte: the SIM models no register in this range and would store a
		// written byte and hand it back, so a write-then-read of one offset
		// would pass against the SIM answering. Writing MSTA to MBCR and
		// reading the busy bit out of MBSR is a behaviour only the M-Bus model
		// produces.
		busWrite(board, g_mbarBase + g2::MBus::g_mbcr, g_byte, g2::MBus::g_msta, status);
		checkEqual(uint32_t(status), uint32_t(MCF5307_BUS_OK),
		           "a byte write of MBCR through the board completes");

		const uint32_t mbsr = busRead(board, g_mbarBase + g2::MBus::g_mbsr, g_byte, status);
		checkEqual(mbsr & g2::MBus::g_mbb, uint32_t(g2::MBus::g_mbb),
		           "MBAR+0x28C reaches the M-Bus and reports the busy bit the MSTA write set");

		// The second, independent piece of evidence for the same routing: the
		// M-Bus restricts every register to byte access and the SIM does not
		// restrict this offset, because the SIM models no register there.
		mcf5307_bus_status wideStatus = MCF5307_BUS_OK;
		(void)busRead(board, g_mbarBase + g2::MBus::g_mbcr, g_word, wideStatus);
		checkEqual(uint32_t(wideStatus), uint32_t(MCF5307_BUS_SIZE_ILLEGAL),
		           "a 16-bit read of an M-Bus register is rejected by the M-Bus");

		// Every register is reachable.
		for(uint32_t offset = g2::MBus::g_madr; offset <= g2::MBus::g_mbdr; offset += 4u)
		{
			mcf5307_bus_status regStatus = MCF5307_BUS_OK;
			(void)busRead(board, g_mbarBase + offset, g_byte, regStatus);
			checkEqual(uint32_t(regStatus), uint32_t(MCF5307_BUS_OK),
			           "MBAR+" + hex32(offset) + " reaches a modelled M-Bus register");
		}

		// The window bound. Both units answer zero at an offset neither models,
		// so the value cannot say which one replied. Each writes its own
		// UNMODELLED line instead, and the two logs are what separate them.
		const uint32_t belowModule = g2::MBus::g_madr - 4u;
		const uint32_t aboveModule = g2::MBus::g_mbdr + 4u;

		board.sim().clearLog();
		board.mbus().clearLog();

		(void)busRead(board, g_mbarBase + belowModule, g_byte, status);
		(void)busRead(board, g_mbarBase + aboveModule, g_byte, status);

		checkLog(board.mbus().log(), {},
		         "the M-Bus answers nothing outside its own module window");
		checkLog(board.sim().log(),
		         {"sim: UNMODELLED read of 8 bits at offset " + hex32(belowModule),
		          "sim: UNMODELLED read of 8 bits at offset " + hex32(aboveModule)},
		         "the offsets either side of the module window still reach the SIM");

		// And the converse: an offset inside the module window that carries no
		// register is the M-Bus's, not the SIM's.
		board.sim().clearLog();
		board.mbus().clearLog();

		(void)busRead(board, g_mbarBase + g2::MBus::g_madr + 1u, g_byte, status);

		checkLog(board.sim().log(), {},
		         "the SIM answers nothing inside the M-Bus module window");
		checkLog(board.mbus().log(),
		         {"mbus: UNMODELLED read of 8 bits at offset "
		          + hex32(g2::MBus::g_madr + 1u)},
		         "an unmodelled offset inside the module window reaches the M-Bus");

		// The regression cases: the pre-existing arms are driven too, not only
		// the new offsets.
		busWrite(board, g_mbarBase + 0x080u, g_word, 0xA6A6u, status);
		checkEqual(busRead(board, g_mbarBase + 0x080u, g_word, status), 0xA6A6u,
		           "the SIM still answers CSAR0 at MBAR+0x080");

		checkEqual(busRead(board, g_mbarBase + 0x1D0u, g_byte, status), 0x0Eu,
		           "the SIM still answers the UIPCR strap at MBAR+0x1D0");

		const uint32_t uivr = g_mbarBase + g2::Uart0::gUart0Base + 0x30u;
		checkEqual(busRead(board, uivr, g_byte, status), 0x0Fu,
		           "UART0 still answers its UIVR reset value at MBAR+0x1F0");

		busWrite(board, uivr, g_byte, g2::Uart0::gUart0Vector, status);
		checkEqual(busRead(board, uivr, g_byte, status), g2::Uart0::gUart0Vector,
		           "UART0 still returns the vector written through the board");
	}

	// ==================================================================
	// The two write-only registers are routed by bit 7 and not by position.
	// The part accepts one or two bytes in either order.
	// ==================================================================
	{
		g2::Max1039 forward(makeAdcConfig());
		check(forward.start(g2::g_max1039Address, false), "the forward-order transaction opens");
		check(forward.write(g_setupByte), "the setup byte is accepted first");
		check(forward.write(g_configByte), "the configuration byte is accepted second");
		forward.stop();

		g2::Max1039 reversed(makeAdcConfig());
		check(reversed.start(g2::g_max1039Address, false), "the reversed-order transaction opens");
		check(reversed.write(g_configByte), "the configuration byte is accepted first");
		check(reversed.write(g_setupByte), "the setup byte is accepted second");
		reversed.stop();

		check(forward.referenceSource() == reversed.referenceSource(),
		      "the reversed order selects the same reference source");
		check(forward.externalClock() == reversed.externalClock(),
		      "the reversed order selects the same clock mode");
		checkEqual(forward.channelSelect(), reversed.channelSelect(),
		           "the reversed order selects the same channel");
		checkEqual(forward.scanMode(), reversed.scanMode(),
		           "the reversed order selects the same scan mode");
		check(forward.referencePinIsReference() == reversed.referencePinIsReference(),
		      "the reversed order makes the same use of the reference pin");
	}

	{
		// One byte only, of each kind, leaves the other register untouched.
		// The power-on state is the baseline: single-ended, unipolar,
		// single-channel, internal clock, SEL 000.
		g2::Max1039 fresh(makeAdcConfig());
		check(fresh.referenceSource() == g2::Max1039::ReferenceSource::Supply,
		      "the power-on reference source is the supply");
		check(!fresh.externalClock(), "the power-on clock mode is internal");
		checkEqual(fresh.channelSelect(), 0u, "the power-on channel select is zero");
		checkEqual(fresh.scanMode(), 0u, "the power-on scan mode is zero");

		g2::Max1039 setupOnly(makeAdcConfig());
		check(setupOnly.start(g2::g_max1039Address, false), "the setup-only transaction opens");
		check(setupOnly.write(g_setupByte), "the lone setup byte is accepted");
		setupOnly.stop();
		check(setupOnly.referenceSource() == g2::Max1039::ReferenceSource::External,
		      "a lone setup byte reaches the setup register");
		checkEqual(setupOnly.channelSelect(), 0u,
		           "a lone setup byte leaves the configuration register untouched");
		checkEqual(setupOnly.scanMode(), 0u,
		           "a lone setup byte leaves the scan mode untouched");

		g2::Max1039 configOnly(makeAdcConfig());
		check(configOnly.start(g2::g_max1039Address, false), "the config-only transaction opens");
		check(configOnly.write(g_configByte), "the lone configuration byte is accepted");
		configOnly.stop();
		checkEqual(configOnly.channelSelect(), 6u,
		           "a lone configuration byte reaches the configuration register");
		check(configOnly.referenceSource() == g2::Max1039::ReferenceSource::Supply,
		      "a lone configuration byte leaves the setup register untouched");
		check(!configOnly.externalClock(),
		      "a lone configuration byte leaves the clock mode untouched");
	}

	// ==================================================================
	// This firmware's actual order is measured, so the test asserts it: setup
	// first, then configuration, in two separate transactions, each opened by
	// its own address byte and closed by its own STOP. The model must accept
	// either order because the part does; the firmware happens to send this
	// one.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);
		g2::MBus bus(&adc);

		wr(bus, g2::MBus::g_mbcr, g2::MBus::g_men);
		writeTransaction(bus, g_setupByte);

		check(adc.referenceSource() == g2::Max1039::ReferenceSource::External,
		      "the measured setup byte arrives in its own transaction and selects the external reference");
		check(adc.externalClock(),
		      "the measured setup byte selects external clock mode");
		checkEqual(adc.channelSelect(), 0u,
		           "the measured setup byte's transaction carries no configuration byte");

		writeTransaction(bus, g_configByte);

		checkEqual(adc.channelSelect(), 6u,
		           "the measured configuration byte arrives in a second transaction");
		checkEqual(adc.scanMode(), 0u,
		           "the measured configuration byte selects the scan-up mode");
		check(adc.singleEnded(),
		      "the measured configuration byte selects single-ended conversion");

		// The companion of case 2, inherited by 2b: the scan below is driven on
		// a channel other than 0, so ignoring the configuration byte cannot
		// pass.
		checkBytes(adc.scanChannels(), {0u, 1u, 2u, 3u, 4u, 5u, 6u},
		           "the measured configuration byte asks for channels 0 through 6");
	}

	// ==================================================================
	// CH11 is the reference pin, not an unwired channel. With SEL1 set it is
	// excluded from a multichannel scan, and a direct single-ended selection of
	// it returns GND.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);

		// SCAN=00, CS=1011, SGL/DIF=1 -- a scan up to AIN11.
		constexpr uint8_t sweepToEleven = 0x17u;

		check(adc.start(g2::g_max1039Address, false), "the CH11 sweep transaction opens");
		check(adc.write(g_setupByte), "the SEL1 setup byte is accepted");
		check(adc.write(sweepToEleven), "the sweep-to-AIN11 configuration byte is accepted");
		adc.stop();

		check(adc.referencePinIsReference(),
		      "the measured setup byte makes AIN11 a reference pin");
		checkBytes(adc.scanChannels(), {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u},
		           "a multichannel scan while SEL1 is set yields no AIN11 result");

		// SCAN=11, CS=1011, SGL/DIF=1 -- convert the selected channel only.
		constexpr uint8_t directEleven = 0x77u;

		check(adc.start(g2::g_max1039Address, false), "the direct AIN11 transaction opens");
		check(adc.write(directEleven), "the direct AIN11 configuration byte is accepted");
		adc.stop();

		checkBytes(adc.scanChannels(), {11u},
		           "a direct single-ended selection of CS=1011 selects AIN11 alone");

		check(adc.start(g2::g_max1039Address, true), "the direct AIN11 read transaction opens");
		checkEqual(adc.read(), 0u,
		           "a direct single-ended read of AIN11/REF returns GND while SEL1 is set");
		adc.stop();
	}

	// ==================================================================
	// The scale is derived from the reference, and the setup byte's SEL field
	// is what selects the reference.
	// ==================================================================
	{
		// The same input at two different reference values.
		g2::Max1039Config halfReference = makeAdcConfig();
		halfReference.externalReferenceVolts = g_externalReferenceVolts * 0.5f;

		g2::Max1039 nominal(makeAdcConfig());
		g2::Max1039 halved(halfReference);

		driveChannels(nominal);
		driveChannels(halved);

		for(g2::Max1039* adc : {&nominal, &halved})
		{
			check(adc->start(g2::g_max1039Address, false), "the reference-scale transaction opens");
			check(adc->write(g_setupByte), "the external-reference setup byte is accepted");
			check(adc->write(g_configByte), "the measured configuration byte is accepted");
			adc->stop();
		}

		checkEqual(nominal.convert(0u), channelCode(0u),
		           "the nominal reference converts channel 0 to its own code");
		checkEqual(halved.convert(0u), uint8_t(channelCode(0u) * 2u),
		           "halving the reference doubles the same channel's result");

		// The reference is a live parameter and not a decoration: the whole
		// scan moves, not one channel of it.
		checkEqual(nominal.convert(4u), channelCode(4u),
		           "the nominal reference converts channel 4 to its own code");
		checkEqual(halved.convert(4u), uint8_t(channelCode(4u) * 2u),
		           "halving the reference doubles channel 4 as well");

		// The transfer function saturates rather than wrapping.
		nominal.setChannelVolts(1u, g_externalReferenceVolts * 2.0f);
		checkEqual(nominal.convert(1u), 255u,
		           "an input above the reference saturates at full scale");
		nominal.setChannelVolts(1u, -g_externalReferenceVolts);
		checkEqual(nominal.convert(1u), 0u,
		           "an input below ground saturates at zero");
	}

	{
		// SEL[2:0] = 000 -- the supply is the reference and AIN11/REF is an
		// ordinary analogue input. Both the reference source and the AIN11
		// handling must change, which is what forces the model to decode SEL
		// rather than hardcoding the external reference this firmware
		// selects.
		constexpr uint8_t supplyReferenceSetup  = 0x82u;   // SEL=000, CLK=0, RST=1
		constexpr uint8_t internalReferenceSetup = 0xC2u;  // SEL=100, CLK=0, RST=1
		constexpr uint8_t sweepToEleven          = 0x17u;

		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);

		check(adc.start(g2::g_max1039Address, false), "the supply-reference transaction opens");
		check(adc.write(supplyReferenceSetup), "the SEL=000 setup byte is accepted");
		check(adc.write(sweepToEleven), "the sweep-to-AIN11 configuration byte is accepted");
		adc.stop();

		check(adc.referenceSource() == g2::Max1039::ReferenceSource::Supply,
		      "a SEL=000 setup byte selects the supply as the reference");
		check(!adc.referencePinIsReference(),
		      "a SEL=000 setup byte makes AIN11 an ordinary analogue input");
		checkBytes(adc.scanChannels(), {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u},
		           "AIN11 joins the multichannel scan once SEL1 is clear");

		// AIN11's own input is the potential the board feeds into pin 13, and
		// the conversion now runs against the supply rather than against it.
		checkEqual(adc.convert(11u),
		           uint8_t(g_externalReferenceVolts * 256.0f / g_supplyVolts),
		           "AIN11 converts its pin potential against the supply reference");
		checkEqual(adc.convert(0u),
		           uint8_t(channelInput(0u) * 256.0f / g_supplyVolts),
		           "every channel converts against the supply reference");

		check(adc.start(g2::g_max1039Address, false), "the internal-reference transaction opens");
		check(adc.write(internalReferenceSetup), "the SEL=100 setup byte is accepted");
		adc.stop();

		check(adc.referenceSource() == g2::Max1039::ReferenceSource::Internal,
		      "a SEL=100 setup byte selects the internal reference");
		check(!adc.referencePinIsReference(),
		      "a SEL=100 setup byte leaves AIN11 an ordinary analogue input");
		checkEqual(adc.convert(0u),
		           uint8_t(channelInput(0u) * 256.0f / g_internalReferenceVolts),
		           "the internal reference scales the conversion in its turn");
	}

	// ==================================================================
	// The clock mode is decoded, and external clock mode is what this firmware
	// selects. The wrong model is the plausible one: internal clock is the
	// part's power-on default.
	// ==================================================================
	{
		g2::Max1039 external(makeAdcConfig());
		check(external.start(g2::g_max1039Address, false), "the CLK=1 transaction opens");
		check(external.write(g_setupByte), "the measured CLK=1 setup byte is accepted");
		external.stop();

		check(external.externalClock(), "the measured setup byte selects external clock mode");
		check(!external.stretchesClock(),
		      "the part does NOT stretch the clock in external clock mode");

		constexpr uint8_t internalClockSetup = 0xA2u;   // SEL=010, CLK=0, RST=1

		g2::Max1039 internal(makeAdcConfig());
		check(internal.start(g2::g_max1039Address, false), "the CLK=0 transaction opens");
		check(internal.write(internalClockSetup), "the CLK=0 setup byte is accepted");
		internal.stop();

		check(!internal.externalClock(), "a CLK=0 setup byte selects internal clock mode");
		check(internal.stretchesClock(),
		      "the part DOES stretch the clock in internal clock mode");
	}

	// ==================================================================
	// The read side supplies bytes indefinitely and is never not-acknowledged.
	// The firmware clears TXAK unconditionally with no index test, never writes
	// the state variable back, and issues no STOP for the read transaction. The
	// consequence of getting this wrong is not a wrong value; it is a hang.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);
		g2::MBus bus(&adc);

		// Whole scans and a fraction, read through the controller exactly as
		// state 3 reads them, with no STOP anywhere in it.
		constexpr unsigned bytesRead = g_measuredScanLength * 10u + 3u;

		const Interlock observed = replayFirmware(bus, bytesRead);

		checkEqual(unsigned(observed.received.size()), bytesRead,
		           "the slave keeps supplying bytes well past one scan's worth");
		check(observed.mifAfterEveryReadByte,
		      "MIF goes set for every one of those bytes, including the last of each scan");

		std::vector<uint8_t> expected;
		for(unsigned index = 0; index < bytesRead; ++index)
			expected.push_back(channelCode(index % g_measuredScanLength));

		checkBytes(observed.received, expected,
		           "the bytes past the first scan are the scan repeating, not a stall");
	}

	// ==================================================================
	// The scan sequence is what the configuration byte asked for, and its
	// length moves with CS.
	// ==================================================================
	{
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);

		check(adc.start(g2::g_max1039Address, false), "the measured-scan transaction opens");
		check(adc.write(g_setupByte), "the measured setup byte is accepted");
		check(adc.write(g_configByte), "the measured configuration byte is accepted");
		adc.stop();

		checkEqual(adc.scanMode(), 0u, "the measured configuration byte decodes SCAN as 00");
		checkEqual(adc.channelSelect(), 6u, "the measured configuration byte decodes CS as 6");
		check(adc.singleEnded(), "the measured configuration byte decodes SGL/DIF as single-ended");

		check(adc.start(g2::g_max1039Address, true), "the measured-scan read transaction opens");

		std::vector<uint8_t> readBack;
		for(unsigned index = 0; index < g_measuredScanLength * 2u; ++index)
			readBack.push_back(adc.read());

		checkBytes(readBack, expectedScan(g_measuredScanLength, 2u),
		           "the measured configuration byte emits channels 0 through 6, in order, repeating");
		adc.stop();
	}

	{
		// A different CS moves the length of the sequence, which the case above
		// does not by itself establish.
		g2::Max1039 adc(makeAdcConfig());
		driveChannels(adc);

		constexpr uint8_t sweepToTwo = 0x05u;   // SCAN=00, CS=0010, SGL/DIF=1

		check(adc.start(g2::g_max1039Address, false), "the short-scan transaction opens");
		check(adc.write(g_setupByte), "the measured setup byte is accepted");
		check(adc.write(sweepToTwo), "the CS=2 configuration byte is accepted");
		adc.stop();

		checkEqual(adc.channelSelect(), 2u, "the CS=2 configuration byte decodes CS as 2");
		checkBytes(adc.scanChannels(), {0u, 1u, 2u},
		           "a CS of 2 asks for three channels rather than seven");

		check(adc.start(g2::g_max1039Address, true), "the short-scan read transaction opens");

		std::vector<uint8_t> readBack;
		for(unsigned index = 0; index < 9u; ++index)
			readBack.push_back(adc.read());

		checkBytes(readBack, expectedScan(3u, 3u),
		           "the shorter sequence repeats at its own length, not at seven");
		adc.stop();
	}

	if(g_failures)
	{
		std::cout << "t0_mbus: " << g_failures << " of " << g_cases
		          << " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_mbus: " << g_cases << " of " << g_cases
	          << " cases passed" << std::endl;
	return 0;
}
