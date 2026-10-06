// The front panel interaction: CS4 matrix scan with walking zeros and PADAT
// row return sensing, rotary encoder quadrature tick decoding to parameter deltas,
// 15-LED encoder ring and slot/variation LED buffer updates, and 5 LCD character buffers.

#include "latches.h"
#include "memoryMap.h"
#include "model.h"
#include "panel.h"
#include "sim.h"

#include <coldfire.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

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

	// Memory layout constants for the front panel interaction fixture.
	// CS4 base is configuration; 0x40000000 is used here as a distinct address space.
	constexpr uint32_t g_cs4Base = 0x40000000u;
	constexpr uint32_t g_displayBufferSize = 0x00000400u; // 1024 bytes
	constexpr uint32_t g_cs5Base = rg2::g_cs5Base;         // 0x15000000
	constexpr uint32_t g_latchWindowSize = 0x10u;         // 16 bytes
	constexpr uint32_t g_mbarBase = 0x10000000u;          // MBAR peripheral space
	constexpr uint32_t g_padatOffset = 0x248u;            // Port A Data Register (PADAT)

	class PanelFixture
	{
	public:
		explicit PanelFixture(const rg2::Model _model = rg2::Model::G2X)
			: m_panel(g_displayBufferSize)
			, m_latches(g_latchWindowSize, _model)
			, m_sim(_model)
		{
			m_panel.attachLatches(&m_latches);
			m_latches.attachPanel(&m_panel);
			m_sim.setPanel(&m_panel);

			rg2::MemoryMapConfig config;
			config.cs4 = {g_cs4Base, g_displayBufferSize};
			config.cs5 = {g_cs5Base, g_latchWindowSize};
			config.mbar = {g_mbarBase, rg2::g_simSpaceSize};

			m_map = new rg2::MemoryMap(config);
			m_map->attach(rg2::Region::Cs4, &m_panel);
			m_map->attach(rg2::Region::Cs5, &m_latches);
			m_map->attach(rg2::Region::Mbar, &m_sim);
		}

		~PanelFixture()
		{
			delete m_map;
		}

		uint32_t read(const uint32_t _address, const int _size, cf_bus_status& _status)
		{
			_status = CF_BUS_OK;
			return rg2::memoryMapRead(m_map, _address, _size, &_status);
		}

		void write(const uint32_t _address, const int _size, const uint32_t _value, cf_bus_status& _status)
		{
			_status = CF_BUS_OK;
			rg2::memoryMapWrite(m_map, _address, _size, _value, &_status);
		}

		uint32_t read(const uint32_t _address, const int _size)
		{
			cf_bus_status status = CF_BUS_OK;
			return read(_address, _size, status);
		}

		void write(const uint32_t _address, const int _size, const uint32_t _value)
		{
			cf_bus_status status = CF_BUS_OK;
			write(_address, _size, _value, status);
		}

		rg2::Panel& panel() { return m_panel; }
		rg2::Latches& latches() { return m_latches; }
		rg2::Sim& sim() { return m_sim; }

		// Helper to write ASCII text to CS4 display memory using big-endian 32-bit accesses.
		void writeText32(const uint32_t _displayOffset, const std::string& _text)
		{
			const uint32_t targetBase = g_cs4Base + _displayOffset;
			for(size_t i = 0; i < _text.size(); i += 4)
			{
				uint32_t word = 0;
				for(size_t b = 0; b < 4; ++b)
				{
					word <<= 8;
					if((i + b) < _text.size())
						word |= static_cast<uint8_t>(_text[i + b]);
				}
				write(targetBase + static_cast<uint32_t>(i), 32, word);
			}
		}

		// Helper to read ASCII string back from CS4 display memory.
		std::string readText(const uint32_t _displayOffset, const size_t _length)
		{
			std::string result;
			result.reserve(_length);
			for(size_t i = 0; i < _length; ++i)
			{
				const uint32_t byteVal = read(g_cs4Base + _displayOffset + static_cast<uint32_t>(i), 8);
				result.push_back(static_cast<char>(byteVal & 0xFFu));
			}
			return result;
		}

	private:
		rg2::Panel m_panel;
		rg2::Latches m_latches;
		rg2::Sim m_sim;
		rg2::MemoryMap* m_map = nullptr;
	};

	// 2-bit quadrature state machine decoder for optical/mechanical rotary encoders.
	// Gray code transitions determine rotation direction:
	// Clockwise (CW):        00 -> 01 -> 11 -> 10 -> 00 (+1 tick per 4 transitions)
	// Counter-Clockwise (CCW): 00 -> 10 -> 11 -> 01 -> 00 (-1 tick per 4 transitions)
	struct QuadratureEncoder
	{
		uint8_t state = 0; // bits: (PhaseA << 1) | PhaseB
		int quarterTicks = 0;

		void update(const bool _phaseA, const bool _phaseB)
		{
			const uint8_t newState = static_cast<uint8_t>(((_phaseA ? 1 : 0) << 1) | (_phaseB ? 1 : 0));
			// Transition matrix: -1 = CCW, +1 = CW, 0 = invalid or no movement
			static const int8_t deltaTable[4][4] = {
				{  0,  1, -1,  0 }, // from 00 to 00, 01, 10, 11
				{ -1,  0,  0,  1 }, // from 01 to 00, 01, 10, 11
				{  1,  0,  0, -1 }, // from 10 to 00, 01, 10, 11
				{  0, -1,  1,  0 }  // from 11 to 00, 01, 10, 11
			};
			quarterTicks += deltaTable[state & 3u][newState & 3u];
			state = newState;
		}

		int8_t consumeFullTicks()
		{
			const int full = quarterTicks / 4;
			quarterTicks %= 4;
			return static_cast<int8_t>(full);
		}
	};
}

int main()
{
	// -----------------------------------------------------------------------
	// Scenario 1: Button press state transitions in walking-zero CS4 matrix scan.
	//
	// Firmware drives 16-bit walking zeros across CS4 (0x40000000..0x4000000e)
	// pulling column c low (bit 15 - c == 0). Sensed button return rows are
	// presented on ColdFire parallel port PADAT (0x10000248), active-low with bit 9
	// reserved for the hardware strap.
	{
		PanelFixture fixture;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// 1. Quiescent state: all buttons unpressed.
		for(uint8_t col = 0; col < rg2::Panel::kMaxCols; ++col)
		{
			const uint16_t scanWord = static_cast<uint16_t>(~(1u << (15u - col)));
			// Walking zero write across CS4 base (offsets 0x0000..0x000e)
			const uint32_t scanAddr = g_cs4Base + (col % 8u) * 2u;
			fixture.write(scanAddr, 16, scanWord, status);
			checkEqual(status, CF_BUS_OK, "walking-zero scan write completes");
			checkEqual(fixture.panel().getActiveRowMask(), uint16_t(0u),
				"quiescent scan reports zero active rows");
			checkEqual(fixture.panel().getRowBits(), uint16_t(0xFFFFu),
				"quiescent scan keeps all row return bits idle high");

			// Port A PADAT read through SIM: all row bits high (0xFFFF), strap bit 9 low (~0x0200) -> 0xFDFF
			const uint32_t padat = fixture.read(g_mbarBase + g_padatOffset, 16, status);
			checkEqual(status, CF_BUS_OK, "PADAT 16-bit read completes");
			checkEqual(padat, uint32_t(0xFDFFu), "quiescent PADAT read returns 0xFDFF with strap bit 9 clear");
		}

		// 2. Single button state transition: press button at row 3, column 2.
		fixture.panel().setButtonPressed(3, 2, true);
		check(fixture.panel().isButtonPressed(3, 2), "button (3,2) recorded pressed");

		// Scanning column 0 (bit 15 low: 0x7FFF) should not sense button (3,2)
		fixture.write(g_cs4Base, 16, 0x7FFFu);
		check(!fixture.panel().isRowActive(3), "row 3 inactive during column 0 scan");
		checkEqual(fixture.panel().getActiveRowMask(), uint16_t(0u), "active mask 0 on column 0");

		// Scanning column 2 (bit 13 low: 0xDFFF) activates row 3
		fixture.write(g_cs4Base, 16, 0xDFFFu);
		check(fixture.panel().isRowActive(3), "row 3 sensed active during column 2 scan");
		checkEqual(fixture.panel().getActiveRowMask(), uint16_t(1u << 3), "active mask has bit 3 set");
		checkEqual(fixture.panel().getRowBits(), uint16_t(~(1u << 3)), "rowBits has bit 3 low");

		// Read PADAT: row 3 active-low (bit 3 = 0) and strap bit 9 clear (bit 9 = 0)
		uint32_t padatVal = fixture.read(g_mbarBase + g_padatOffset, 16);
		const uint16_t expectedPadat = static_cast<uint16_t>(0xFFFFu & ~(1u << 3) & ~(1u << 9));
		checkEqual(uint16_t(padatVal), expectedPadat, "PADAT reflects active-low row 3 during column 2 scan");

		// Scanning column 3 (bit 12 low: 0xEFFF) returns row 3 to inactive
		fixture.write(g_cs4Base, 16, 0xEFFFu);
		check(!fixture.panel().isRowActive(3), "row 3 transitions back to inactive on column 3");

		// 3. Multi-button rollover: simultaneous button presses.
		// Press button at row 1, col 5 AND row 6, col 5 (same column, different rows).
		fixture.panel().setButtonPressed(1, 5, true);
		fixture.panel().setButtonPressed(6, 5, true);

		// Scan column 5 (bit 10 low: 0xFBFF)
		fixture.write(g_cs4Base, 16, 0xFBFFu);
		check(fixture.panel().isRowActive(1), "row 1 active during column 5 scan");
		check(fixture.panel().isRowActive(6), "row 6 active during column 5 scan");
		check(!fixture.panel().isRowActive(3), "row 3 inactive during column 5 scan");
		const uint16_t expectedMultiMask = static_cast<uint16_t>((1u << 1) | (1u << 6));
		checkEqual(fixture.panel().getActiveRowMask(), expectedMultiMask, "both rows 1 and 6 active in mask");

		padatVal = fixture.read(g_mbarBase + g_padatOffset, 16);
		const uint16_t expectedMultiPadat = static_cast<uint16_t>(0xFFFFu & ~expectedMultiMask & ~(1u << 9));
		checkEqual(uint16_t(padatVal), expectedMultiPadat, "PADAT reflects both active-low rows 1 and 6");

		// 4. Button release transitions.
		fixture.panel().setButtonPressed(1, 5, false);
		fixture.write(g_cs4Base, 16, 0xFBFFu);
		check(!fixture.panel().isRowActive(1), "row 1 inactive after release");
		check(fixture.panel().isRowActive(6), "row 6 remains active");

		fixture.panel().setButtonPressed(6, 5, false);
		fixture.panel().setButtonPressed(3, 2, false);
		fixture.write(g_cs4Base, 16, 0xFBFFu);
		checkEqual(fixture.panel().getActiveRowMask(), uint16_t(0u), "all rows inactive after full release");
	}

	// -----------------------------------------------------------------------
	// Scenario 2: Rotary encoder quadrature tick decoding into parameter deltas.
	//
	// 2-phase quadrature signals on optical encoders decode into signed parameter
	// deltas. Setting encoder deltas on the panel exposes them on CS5 latch reads
	// (offsets 1..7 for encoders 0..6).
	{
		PanelFixture fixture;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// Verify initial quiescent deltas
		for(uint8_t i = 0; i < rg2::Panel::kMaxEncoders; ++i)
		{
			checkEqual(int(fixture.panel().getEncoderDelta(i)), 0, "encoder starts at delta 0");
		}

		// 1. Clockwise (CW) rotation on Encoder 0: 4 transitions produce +1 tick.
		QuadratureEncoder enc0;
		enc0.update(false, true);  // 00 -> 01
		enc0.update(true,  true);  // 01 -> 11
		enc0.update(true,  false); // 11 -> 10
		enc0.update(false, false); // 10 -> 00
		const int8_t tickCw = enc0.consumeFullTicks();
		checkEqual(int(tickCw), 1, "quadrature CW cycle decoded to +1 tick");

		fixture.panel().setEncoderDelta(0, tickCw);
		checkEqual(int(fixture.panel().getEncoderDelta(0)), 1, "panel encoder 0 delta updated to +1");

		// Read Latch 1 (CS5 offset 1) via bus
		uint32_t latchVal = fixture.read(g_cs5Base + 1u, 8, status);
		checkEqual(status, CF_BUS_OK, "reading latch 1 completes");
		checkEqual(latchVal, uint32_t(1u), "latch 1 bus read returns +1 delta");

		// 2. Fast CW rotation: accumulate 7 ticks on Encoder 2.
		QuadratureEncoder enc2;
		for(int step = 0; step < 7; ++step)
		{
			enc2.update(false, true);
			enc2.update(true,  true);
			enc2.update(true,  false);
			enc2.update(false, false);
		}
		const int8_t tickFastCw = enc2.consumeFullTicks();
		checkEqual(int(tickFastCw), 7, "quadrature 7 CW cycles decoded to +7 ticks");

		fixture.panel().setEncoderDelta(2, tickFastCw);
		checkEqual(int(fixture.panel().getEncoderDelta(2)), 7, "encoder 2 delta is +7");
		latchVal = fixture.read(g_cs5Base + 3u, 8); // Encoder 2 is Latch 3 (offset 3)
		checkEqual(latchVal, uint32_t(7u), "latch 3 bus read returns +7 delta");

		// 3. Counter-Clockwise (CCW) rotation on Encoder 4: reverse transitions produce -1 tick.
		QuadratureEncoder enc4;
		enc4.update(true,  false); // 00 -> 10
		enc4.update(true,  true);  // 10 -> 11
		enc4.update(false, true);  // 11 -> 01
		enc4.update(false, false); // 01 -> 00
		const int8_t tickCcw = enc4.consumeFullTicks();
		checkEqual(int(tickCcw), -1, "quadrature CCW cycle decoded to -1 tick");

		fixture.panel().setEncoderDelta(4, tickCcw);
		checkEqual(int(fixture.panel().getEncoderDelta(4)), -1, "encoder 4 delta is -1");
		latchVal = fixture.read(g_cs5Base + 5u, 8); // Encoder 4 is Latch 5 (offset 5)
		checkEqual(uint8_t(latchVal), uint8_t(0xFFu), "latch 5 bus read returns -1 as 0xFF in two's complement");

		// 4. Larger CCW decrement: -10 ticks on Encoder 5.
		QuadratureEncoder enc5;
		for(int step = 0; step < 10; ++step)
		{
			enc5.update(true,  false);
			enc5.update(true,  true);
			enc5.update(false, true);
			enc5.update(false, false);
		}
		const int8_t tickFastCcw = enc5.consumeFullTicks();
		checkEqual(int(tickFastCcw), -10, "quadrature 10 CCW cycles decoded to -10 ticks");

		fixture.panel().setEncoderDelta(5, tickFastCcw);
		latchVal = fixture.read(g_cs5Base + 6u, 8); // Encoder 5 is Latch 6
		checkEqual(uint8_t(latchVal), uint8_t(static_cast<int8_t>(-10)),
			"latch 6 bus read returns -10 as two's complement 0xF6");

		// 5. Delta clear/acknowledgment back to quiescent zero.
		fixture.panel().setEncoderDelta(0, 0);
		fixture.panel().setEncoderDelta(2, 0);
		fixture.panel().setEncoderDelta(4, 0);
		fixture.panel().setEncoderDelta(5, 0);
		checkEqual(fixture.read(g_cs5Base + 1u, 8), uint32_t(0u), "latch 1 returns to 0 on clear");
		checkEqual(fixture.read(g_cs5Base + 3u, 8), uint32_t(0u), "latch 3 returns to 0 on clear");
		checkEqual(fixture.read(g_cs5Base + 5u, 8), uint32_t(0u), "latch 5 returns to 0 on clear");
		checkEqual(fixture.read(g_cs5Base + 6u, 8), uint32_t(0u), "latch 6 returns to 0 on clear");
	}

	// -----------------------------------------------------------------------
	// Scenario 3: LED ring buffer updates across all 8 encoders & slot/variation LEDs.
	//
	// Each of the 8 encoders features a 15-LED circular ring (bits 0..14).
	// Latch 0 (CS5 offset 0) controls slot (bits 0..3) and variation (bits 6..7) LEDs
	// while preserving the hardware strap in bits 5:4.
	{
		PanelFixture fixture;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// 1. All 8 encoder rings: single-dot, bar, and off patterns.
		const uint16_t ringPatterns[8] = {
			0x0001u, // Ring 0: LED 1 (min)
			0x0040u, // Ring 1: LED 7
			0x0080u, // Ring 2: LED 8 (12 o'clock center)
			0x4000u, // Ring 3: LED 15 (max)
			0x0007u, // Ring 4: 3-LED bar
			0x003Fu, // Ring 5: 6-LED bar
			0x07FFu, // Ring 6: 11-LED bar
			0x7FFFu  // Ring 7: all 15 LEDs illuminated
		};

		for(uint8_t i = 0; i < 8; ++i)
		{
			fixture.panel().setLedRingState(i, ringPatterns[i]);
			checkEqual(fixture.panel().getLedRingState(i), ringPatterns[i],
				"panel reports configured LED ring pattern");
			checkEqual(fixture.latches().getLedRingState(i), ringPatterns[i],
				"latches reflect configured LED ring pattern");
		}

		// 2. Bus write updates across Latches 1..7 for Rings 0..6.
		// Write 16-bit word to Latch 1 (CS5 offset 1) driving 15-LED pattern 0x7FFF
		fixture.write(g_cs5Base + 1u, 16, 0x7FFFu, status);
		checkEqual(status, CF_BUS_OK, "16-bit write to latch 1 completes");
		checkEqual(fixture.panel().getLedRingState(0), uint16_t(0x7FFFu),
			"ring 0 updated to 15-LED full pattern via CS5 write");

		// Write 8-bit pattern to Latch 7 (CS5 offset 7) driving Ring 6
		fixture.write(g_cs5Base + 7u, 8, 0x55u, status);
		checkEqual(status, CF_BUS_OK, "8-bit write to latch 7 completes");
		checkEqual(fixture.panel().getLedRingState(6), uint16_t(0x55u),
			"ring 6 updated via latch 7 write");

		// 3. Slot and Variation LEDs on Latch 0 (CS5 offset 0).
		// G2X model strap has bits 5:4 = 0b11 (0x30).
		const uint32_t initLatch0 = fixture.read(g_cs5Base, 8);
		checkEqual(initLatch0, uint32_t(0x30u), "quiescent latch 0 has strap 0x30 with LEDs off");

		// Activate Slot A (bit 0 = 1) and Variation 1 (bit 6 = 1): pattern 0x41
		fixture.write(g_cs5Base, 8, 0x41u, status);
		checkEqual(status, CF_BUS_OK, "latch 0 write completes");
		// Read back: strap bits 5:4 (0x30) preserved + LED bits 0x41 = 0x71
		uint32_t latch0Val = fixture.read(g_cs5Base, 8);
		checkEqual(latch0Val, uint32_t(0x71u), "latch 0 read preserves strap 0x30 and activates slot A & var 1");

		// Activate Slots B, C, D (bits 1..3 = 0x0E) and Variation 2 (bit 7 = 0x80): pattern 0x8E
		fixture.write(g_cs5Base, 8, 0x8Eu);
		latch0Val = fixture.read(g_cs5Base, 8);
		checkEqual(latch0Val, uint32_t(0xBEu), "latch 0 read reflects slots B, C, D and var 2 with strap intact");

		// Clear all slot and variation LEDs: write 0x00
		fixture.write(g_cs5Base, 8, 0x00u);
		latch0Val = fixture.read(g_cs5Base, 8);
		checkEqual(latch0Val, uint32_t(0x30u), "clearing LEDs restores latch 0 to base strap 0x30");
	}

	// -----------------------------------------------------------------------
	// Scenario 4: Text write verification to the 5 LCD character buffers.
	//
	// The front panel hosts 5 alphanumeric LCD character buffers in CS4 display memory:
	// - Main Patch Display (2x16 = 32 bytes at offset 0x00..0x1F)
	// - 4 Parameter LCDs (each 2x16 = 32 bytes):
	//     Param LCD 1 (Encoders 0 & 1): offset 0x20..0x3F
	//     Param LCD 2 (Encoders 2 & 3): offset 0x40..0x5F
	//     Param LCD 3 (Encoders 4 & 5): offset 0x60..0x7F
	//     Param LCD 4 (Encoders 6 & 7): offset 0x80..0x9F
	{
		PanelFixture fixture;
		cf_bus_status status = CF_BUS_UNMAPPED;

		// 1. Verify display buffer reads zero prior to writes.
		for(uint32_t off = 0; off < 0xA0u; off += 16u)
		{
			const uint32_t val = fixture.read(g_cs4Base + off, 32, status);
			checkEqual(status, CF_BUS_OK, "display read completes");
			checkEqual(val, uint32_t(0u), "display buffer initializes quiescent zero");
		}

		// 2. Define text contents for all 5 displays (2 lines of 16 characters each).
		struct DisplayContent
		{
			uint32_t offset;
			const char* line1;
			const char* line2;
			const char* name;
		};

		const DisplayContent displays[5] = {
			{ 0x000u, "1:5 Nord Lead G2", "Poly Synth V1.6 ", "Main Patch 2x16 LCD" },
			{ 0x020u, "Freq    Res     ", "1.05kHz 1.15    ", "Param LCD 1 (Enc 0/1)" },
			{ 0x040u, "Rate    Wave    ", "4.20Hz  Tri     ", "Param LCD 2 (Enc 2/3)" },
			{ 0x060u, "Attack  Decay   ", "12ms    350ms   ", "Param LCD 3 (Enc 4/5)" },
			{ 0x080u, "Type    Drive   ", "LP24    15%     ", "Param LCD 4 (Enc 6/7)" }
		};

		// 3. Write and verify text across all 5 displays using 32-bit accesses.
		for(const auto& disp : displays)
		{
			fixture.writeText32(disp.offset, disp.line1);
			fixture.writeText32(disp.offset + 16u, disp.line2);

			const std::string readL1 = fixture.readText(disp.offset, 16);
			const std::string readL2 = fixture.readText(disp.offset + 16u, 16);

			checkEqual(readL1, std::string(disp.line1), std::string(disp.name) + " line 1 verified");
			checkEqual(readL2, std::string(disp.line2), std::string(disp.name) + " line 2 verified");
		}

		// 4. Mixed-width byte and word writes into display buffer:
		// Modify parameter on Param LCD 1 line 2: change "1.05kHz" to "2.40kHz"
		// Offset 0x30 is Line 2 of Param LCD 1.
		fixture.write(g_cs4Base + 0x30u, 8, static_cast<uint8_t>('2'));
		fixture.write(g_cs4Base + 0x31u, 16, (static_cast<uint16_t>('.') << 8) | static_cast<uint16_t>('4'));
		fixture.write(g_cs4Base + 0x33u, 8, static_cast<uint8_t>('0'));

		const std::string updatedParam1L2 = fixture.readText(0x30u, 16);
		checkEqual(updatedParam1L2, std::string("2.40kHz 1.15    "),
			"byte/word write successfully updates parameter value in Param LCD 1");

		// 5. Non-interference check: verify that all other 4 displays are intact.
		checkEqual(fixture.readText(0x000u, 16), std::string("1:5 Nord Lead G2"),
			"Main LCD line 1 unaffected by Param LCD 1 edit");
		checkEqual(fixture.readText(0x010u, 16), std::string("Poly Synth V1.6 "),
			"Main LCD line 2 unaffected by Param LCD 1 edit");
		checkEqual(fixture.readText(0x040u, 16), std::string("Rate    Wave    "),
			"Param LCD 2 line 1 unaffected");
		checkEqual(fixture.readText(0x060u, 16), std::string("Attack  Decay   "),
			"Param LCD 3 line 1 unaffected");
		checkEqual(fixture.readText(0x080u, 16), std::string("Type    Drive   "),
			"Param LCD 4 line 1 unaffected");

		// 6. Boundary check: access at end of configured display window (offset 0x3FF)
		fixture.write(g_cs4Base + 0x3FFu, 8, 0xA5u, status);
		checkEqual(status, CF_BUS_OK, "write to last byte of display window completes");
		const uint32_t lastByte = fixture.read(g_cs4Base + 0x3FFu, 8, status);
		checkEqual(status, CF_BUS_OK, "read from last byte of display window completes");
		checkEqual(lastByte, uint32_t(0xA5u), "last byte in display buffer returns stored value");

		// Access beyond display window is unmapped and returns zero
		const uint32_t beyondByte = fixture.read(g_cs4Base + 0x400u, 8, status);
		checkEqual(status, CF_BUS_UNMAPPED, "read beyond display window returns BUS_UNMAPPED");
		checkEqual(beyondByte, uint32_t(0u), "read beyond display window returns 0");
	}

	if(g_failures)
	{
		std::cout << "t0_front_panel_interaction: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_front_panel_interaction: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
