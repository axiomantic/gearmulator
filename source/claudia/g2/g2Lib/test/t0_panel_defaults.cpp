// Tier T0: this test needs no firmware artifact of any kind.
//
// What it asserts: that a Board given the panel's description presents the
// panel's own potentials to the firmware's scan, rather than ground. It drives the
// MAX1039 with the setup and configuration bytes the firmware is measured to
// send, reads the seven-byte sweep back through the M-Bus, and holds each
// result against the code the firmware's own consumers require.
//
// Why the codes are the assertion and not the volts. Nothing downstream of the
// converter sees a potential: the master-volume getter takes result >> 1 and
// indexes a 128-entry table with it, and the pitch-stick boot calibration
// compares a modal bin against a fixed constant. A test on the volts alone
// would pass with a reference that puts every control outside the range its
// consumer accepts.

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
		std::cout << "FAIL " << _what << ": expected " << _expected
		          << ", got " << _actual << std::endl;
		++g_failures;
	}

	// The two bytes the firmware is measured to send, in the order it sends
	// them: SEL=010 selects the external reference on pin 13 and CLK=1 the
	// external clock; SCAN=00 with CS=6 sweeps AIN0 upward to AIN6.
	constexpr uint8_t g_addressWrite = 0xCAu;
	constexpr uint8_t g_addressRead  = 0xCBu;
	constexpr uint8_t g_setupByte    = 0xAAu;
	constexpr uint8_t g_configByte   = 0x0Du;

	// The code the master-volume getter must see for the firmware's volume
	// table to be indexed at its top entry. The getter is result >> 1, so the
	// top of a 128-entry table wants a result of 254 or 255.
	constexpr uint8_t g_volumeIndexAtFullScale = 127u;

	// The boot calibration accepts a modal pitch-stick bin in [0x1000, 0x1020)
	// of its 1/32-LSB units, which is code 128 up to but not including 129.
	constexpr uint8_t g_pitchStickRestCode = 128u;

	std::vector<uint8_t> sweep(g2::Max1039& _adc)
	{
		_adc.start(g2::g_max1039Address, false);
		_adc.write(g_setupByte);
		_adc.stop();

		_adc.start(g2::g_max1039Address, false);
		_adc.write(g_configByte);
		_adc.stop();

		const std::vector<uint8_t> channels = _adc.scanChannels();

		std::vector<uint8_t> results;
		_adc.start(g2::g_max1039Address, true);
		for(size_t i = 0; i < channels.size(); ++i)
			results.push_back(_adc.read());
		_adc.stop();

		return results;
	}
}

int main()
{
	/* The panel description assigned the way a caller modelling this machine
	 * assigns it, and through a Board rather than a bare converter, so that a
	 * description the Board failed to carry through would fail here. */
	g2::BoardConfig config;
	config.adc = g2::panelAdcConfig();

	g2::Board board(config);

	const std::vector<uint8_t> results = sweep(board.adc());

	checkEqual(uint32_t(results.size()), 7u,
	           "the firmware's configuration byte sweeps seven channels");

	if(results.size() != 7u)
	{
		std::cout << (g_failures ? "FAILED " : "passed ") << g_cases << " case(s)" << std::endl;
		return g_failures ? 1 : 0;
	}

	checkEqual(results[size_t(g2::PanelControl::MasterVolume)] >> 1, g_volumeIndexAtFullScale,
	           "the master-volume wiper rests at the top of the firmware's volume table");

	checkEqual(results[size_t(g2::PanelControl::ControlPedal)], 0u,
	           "the control pedal rests at ground");

	checkEqual(results[size_t(g2::PanelControl::Aftertouch)], 0u,
	           "aftertouch rests at ground");

	checkEqual(results[size_t(g2::PanelControl::PitchStick)], g_pitchStickRestCode,
	           "the pitch stick rests inside the boot calibration's acceptance window");

	checkEqual(results[size_t(g2::PanelControl::ModWheel)], 0u,
	           "the mod wheel rests at ground");

	// The two channels past the controls are tied to ground on the panel PCB,
	// and the firmware scans them anyway.
	checkEqual(results[5], 0u, "the first unwired scanned channel reads ground");
	checkEqual(results[6], 0u, "the second unwired scanned channel reads ground");

	// The reference is what makes every code above mean anything, so it is
	// asserted rather than inferred from the codes it produced.
	check(board.adc().referenceSource() == g2::Max1039::ReferenceSource::External,
	      "the firmware's setup byte selects the reference the panel feeds pin 13");
	check(board.adc().referenceVolts() > 3.0f && board.adc().referenceVolts() < 3.1f,
	      "the panel's reference divider is configured, not left at zero");

	std::cout << (g_failures ? "FAILED " : "passed ") << g_cases << " case(s)" << std::endl;
	return g_failures ? 1 : 0;
}
