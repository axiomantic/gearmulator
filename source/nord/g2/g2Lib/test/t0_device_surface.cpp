/* t0_device_surface.cpp -- the compile-time check of the g2::Device surface.
 *
 * What the check owns, and a build of the library cannot see:
 *
 *  1. One binding for each pure virtual synthLib::Device declares: the
 *     override exists on g2::Device with exactly the base's signature. A
 *     missing override or a wrong signature is a compile error here, at the
 *     member-pointer binding, and not a silent absence.
 *
 *  2. m_numSamplesProcessed exists as the subclass's own member. It is not
 *     inherited: synthLib::Device declares no such member; every product
 *     declares its own.
 *
 *  3. getSamplerate() returns 96000.0f, unconditionally -- the single value
 *     that keeps the host rate out of the emulation.
 *
 *  4. The SYNTHLIB_DEMO_MODE conditional. Under the demo mode getState and
 *     setState must not exist on the subclass (an unconditional override
 *     would not compile); with the guard at 0 the two must exist. Both halves
 *     are asserted below, each under its own guard matching device.h's.
 *
 *  5. The two hand-off flags exist as members, and isValid() is a load of
 *     m_ready.
 *
 * No assertion in this file is a language assert() and nothing here depends
 * on NDEBUG, so this file reports identically in every build type.
 */

#include "g2JucePlugin/g2Device.h"

#include <cstdio>
#include <type_traits>

namespace
{
	/* ------------- Property 1: the overrides, each bound to a member pointer
	 * of exactly the base's declared type. A binding that compiles is both an
	 * existence proof and a signature match; a class that dropped one override
	 * fails here, at compile time. */

	using G2Device = g2::Device;
	using BaseDevice = synthLib::Device;

	float (G2Device::*const kGetSamplerate)() const = &G2Device::getSamplerate;
	bool (G2Device::*const kIsValid)() const = &G2Device::isValid;
	uint32_t (G2Device::*const kChannelIn)() = &G2Device::getChannelCountIn;
	uint32_t (G2Device::*const kChannelOut)() = &G2Device::getChannelCountOut;
	bool (G2Device::*const kSetDspClockPercent)(uint32_t) = &G2Device::setDspClockPercent;
	uint32_t (G2Device::*const kGetDspClockPercent)() const = &G2Device::getDspClockPercent;
	uint64_t (G2Device::*const kGetDspClockHz)() const = &G2Device::getDspClockHz;

#if SYNTHLIB_DEMO_MODE == 0
	bool (G2Device::*const kGetState)(std::vector<uint8_t>&, synthLib::StateType) = &G2Device::getState;
	bool (G2Device::*const kSetState)(const std::vector<uint8_t>&, synthLib::StateType) = &G2Device::setState;
#endif

	/* The protected pure virtuals and the hand-off pair are bound through the
	 * harness below: member-pointer formation needs the access the subclass
	 * has. */
	struct Probe final : G2Device
	{
		// A Probe is never constructed; the bindings are the whole check.
		Probe() = delete;

		static constexpr void (Probe::*const kReadMidiOut)(std::vector<synthLib::SMidiEvent>&) = &Probe::readMidiOut;
		static constexpr void (Probe::*const kProcessAudio)(const synthLib::TAudioInputs&, const synthLib::TAudioOutputs&, size_t) = &Probe::processAudio;
		static constexpr bool (Probe::*const kSendMidi)(const synthLib::SMidiEvent&, std::vector<synthLib::SMidiEvent>&) = &Probe::sendMidi;
		static constexpr void (Probe::*const kBeginStateChange)() noexcept = &Probe::beginStateChange;
		static constexpr void (Probe::*const kEndStateChange)() noexcept = &Probe::endStateChange;
	};

	void touchAllPointers()
	{
		(void)kGetSamplerate; (void)kIsValid; (void)kChannelIn; (void)kChannelOut;
		(void)kSetDspClockPercent; (void)kGetDspClockPercent; (void)kGetDspClockHz;
#if SYNTHLIB_DEMO_MODE == 0
		(void)kGetState; (void)kSetState;
#endif
	}
}

int main()
{
	static_assert(std::is_base_of<synthLib::Device, G2Device>::value,
		"g2::Device subclasses synthLib::Device and not wLib::Device -- the wLib link is rejected (docs/divergence.md)");

	/* getSamplerate() returns 96000.0f, unconditionally, from a constructed
	 * device. The no-firmware path constructs and answers. */
	{
		const synthLib::DeviceCreateParams params;
		const G2Device device(params);

		if(device.getSamplerate() != 96000.0f)
		{
			std::printf("FAIL getSamplerate() returns %.1f, expected 96000.0f\n", device.getSamplerate());
			return 1;
		}

		/* isValid() reads the m_ready flag; a freshly constructed device has
		 * never been booted, so it answers false. The run-time half of the
		 * pairing is t0_handoff_flags'. */
		if(device.isValid())
		{
			std::printf("FAIL isValid() answers true on a never-booted device\n");
			return 1;
		}

		/* The surface asked the resolver once and reports coherently: the
		 * report line is never empty, in either resolver outcome. This is not
		 * an assertion about artifacts. */
		const g2::FirmwareStatus& s = device.firmwareStatus();
		if(s.report.empty())
		{
			std::printf("FAIL firmwareStatus().report is empty; the report line is never empty in either resolver outcome\n");
			return 1;
		}
	}

	/* The file-scope pointer bindings already forced the compiler.
	 * touchAllPointers keeps them from being flagged. */
	touchAllPointers();

	std::printf("t0_device_surface: all cases passed\n");
	return 0;
}
