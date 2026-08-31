/* t0_midi_offsets.cpp -- the check of PLG-4 step 2 (with PLG-6's offset
 * conversion; plan section 17, §24.6 row W3-390). Design sections 17 rows
 * 7.30, 7.31 and 7.34, and 14.7.
 *
 * WHAT THE CHECK OWNS: the offset conversion arithmetic and the delivery
 * contract, held against the REAL sendMidi.
 *
 * THE CASES:
 *
 *  1. THE CONVERSION ARITHMETIC. Block-relative offsets in (they arrive
 *      from juce::MidiMessageMetadata::samplePosition, an offset WITHIN the
 *      current host block); absolute offsets out:
 *
 *        absolute = m_numSamplesProcessed + getExtraLatencySamples() + offset
 *
 *      asserted per event through the staged events the harness reads back.
 *      The counter is advanced by driving the REAL processAudio, which is
 *      the only thing that moves it -- the plan's own wording: the
 *      conversion cannot be observed at all until the callback that moves
 *      the counter runs. The device is never valid in this fixture, so
 *      processAudio takes the not-ready path and STILL advances the counter,
 *      keeping this a T0 test with no machine.
 *
 *   2. THE REQUIRED-RED MUTATION. The conversion is "easy to get backwards"
 *      and the one line the previous revision gave would not have compiled
 *      (I18). Reversing it -- `offset -=` instead of `+=` -- turns the
 *      arithmetic case red. The reversed device is a subclass whose
 *      overridden sendMidi subtracts; the same assertions that pass the
 *      real device fail on it.
 *
 *   3. THE EXTRA-LATENCY TERM. getExtraLatencySamples() is set through the
 *      base's public setter, exactly as the framework's
 *      updateDeviceLatency does (plugin.cpp:232), and the staged offsets
 *      carry it: counter + latency + offset, not counter + offset.
 *
 *   4. THE NO-DOUBLE-DELIVERY CASE (design section 17 row 7.30). A reply
 *      the device originates for a host-sent message goes into _response,
 *      in the same call, and NEVER comes out through readMidiOut. The
 *      harness drives the UART0 parser the way t0_midi_out case group 2
 *      does, calls the real sendMidi, drains readMidiOut, and asserts the
 *      reply did not appear there.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() and nothing here depends
 * on NDEBUG.
 */

#include "g2JucePlugin/g2Device.h"

#include "uart0.h"

#include <array>
#include <cstdint>
#include <cstdio>
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
			std::printf("ok   %s\n", _what.c_str());
			return;
		}
		std::printf("FAIL %s\n", _what.c_str());
		++g_failures;
	}

	void checkEqual(const uint64_t _observed, const uint64_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_observed == _expected)
		{
			std::printf("ok   %s\n", _what.c_str());
			return;
		}
		std::printf("FAIL %s: expected <%llu>, got <%llu>\n", _what.c_str(),
			static_cast<unsigned long long>(_expected),
			static_cast<unsigned long long>(_observed));
		++g_failures;
	}

	/* The harness reaches the protected sendMidi and the staged events. The
	 * conversion arithmetic runs inside the real Device::sendMidi; the
	 * harness only reads what it stamped. */
	class OffsetsHarness final : public g2::Device
	{
	public:
		OffsetsHarness() : g2::Device(synthLib::DeviceCreateParams{}) {}

		using g2::Device::sendMidi;
		using g2::Device::processAudio;
		using g2::Device::readMidiOut;
		using g2::Device::uart0MidiOut;

		const std::vector<synthLib::SMidiEvent>& staged() const { return m_pendingMidi; }
		uint32_t samplesProcessed() const { return m_numSamplesProcessed; }
	};

	/* THE PLANTED MUTATION, case group 2. The subclass whose conversion runs
	 * the REVERSED arithmetic -- the mistake the plan's row 7.31 says is
	 * easy to make and review finding I18 proved would not even compile as
	 * given. Everything else is the real device; only the sign of the
	 * addition is flipped. */
	class ReversedDevice final : public g2::Device
	{
	public:
		ReversedDevice() : g2::Device(synthLib::DeviceCreateParams{}) {}

		using g2::Device::sendMidi;
		using g2::Device::processAudio;

		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override
		{
			auto e = _ev;
			// THE REVERSAL: relative-to-absolute backwards, -= instead of +=.
			e.offset -= m_numSamplesProcessed + getExtraLatencySamples();
			m_pendingMidi.push_back(std::move(e));
			return true;
		}

		const std::vector<synthLib::SMidiEvent>& staged() const { return m_pendingMidi; }
		uint32_t samplesProcessed() const { return m_numSamplesProcessed; }
	};

	synthLib::SMidiEvent hostEvent(const uint32_t _offset)
	{
		// A note-on on channel 1, source Host, at a block-relative offset --
		// the shape processor.cpp:809 produces.
		return synthLib::SMidiEvent(synthLib::MidiEventSource::Host, 0x90, 60, 100, _offset);
	}

	/* The not-ready callback driver. The device is never valid in this
	 * fixture; the buffers it zeroes are scratch. The template parameter is
	 * one of this file's harness subclasses, which publish processAudio. */
	template<typename TDevice>
	void runCallback(TDevice& _device, const uint32_t _samples)
	{
		std::vector<float> silence(_samples, 1.0f);	// pre-poisoned: the not-ready path must zero it
		synthLib::TAudioOutputs outs{};
		outs[0] = silence.data();
		outs[1] = silence.data();
		_device.processAudio(synthLib::TAudioInputs{}, outs, _samples);

		for(const float s : silence)
		{
			if(s != 0.0f)
			{
				check(false, "the not-ready path zeroed its output buffers");
				return;
			}
		}
	}
}

int main()
{
	/* ------------- Case group 1. THE CONVERSION ARITHMETIC, latency zero. */
	{
		std::printf("case group 1: the conversion arithmetic (latency 0)\n");

		OffsetsHarness device;

		checkEqual(device.getExtraLatencySamples(), 0u, "a fresh device carries no extra latency");

		constexpr uint32_t kFirstBlock = 128;
		constexpr std::array<uint32_t, 3> kRelative{0u, 37u, 127u};

		for(const auto rel : kRelative)
		{
			std::vector<synthLib::SMidiEvent> response;
			check(device.sendMidi(hostEvent(rel), response), "the real sendMidi accepts a host event");
			check(response.empty(), "sendMidi's response is empty while no machine answers");
		}

		// The counter moves ONLY in processAudio. Drive one block; the
		// events were submitted BEFORE it, which is exactly the framework's
		// order (device.cpp:39-47 stamps and enqueues, :49 then runs audio).
		runCallback(device, kFirstBlock);
		checkEqual(device.samplesProcessed(), kFirstBlock, "the counter moved by the block the callback ran");

		const auto& staged = device.staged();
		checkEqual(staged.size(), kRelative.size(), "one staged event per delivered event");

		for(size_t i = 0; i < staged.size(); ++i)
		{
			checkEqual(staged[i].offset, kRelative[i],
				"staged offset " + std::to_string(i) + " is relative + counter + latency(0)");
		}
	}

	/* ------------- Case group 2. THE REQUIRED-RED MUTATION. The same
	 * delivery against the REVERSED device: every staged offset is wrong by
	 * 2 x (counter + latency), so the arithmetic case cannot pass both. */
	{
		std::printf("case group 2: the required-red reversal\n");

		ReversedDevice device;

		constexpr uint32_t kBlock = 96;
		constexpr uint32_t kRelative = 40u;

		std::vector<synthLib::SMidiEvent> response;
		check(device.sendMidi(hostEvent(kRelative), response), "the reversed device accepts the same event");

		runCallback(device, kBlock);

		checkEqual(device.samplesProcessed(), kBlock, "the reversed device's counter moved identically");

		const auto& staged = device.staged();
		checkEqual(staged.size(), 1u, "the reversed device staged one event");
		checkEqual(staged[0].offset, static_cast<uint64_t>(kRelative),
			"RED IF REACHED: the reversed arithmetic cannot produce counter + latency + offset");
	}

	/* ------------- Case group 3. THE EXTRA-LATENCY TERM. The framework's
	 * setter runs, and every staged offset carries it. */
	{
		std::printf("case group 3: the extra-latency term\n");

		OffsetsHarness device;

		constexpr uint32_t kLatency = 512;
		device.setExtraLatencySamples(kLatency);
		checkEqual(device.getExtraLatencySamples(), kLatency, "the base setter published the latency");

		constexpr uint32_t kBlock = 32;
		constexpr uint32_t kRelative = 7u;

		// One callback first, so the counter has moved and the conversion's
		// counter term is non-zero -- which is the case the framework runs
		// in from the second block onwards.
		runCallback(device, kBlock);
		checkEqual(device.samplesProcessed(), kBlock, "the counter advanced by the first callback");

		std::vector<synthLib::SMidiEvent> response;
		check(device.sendMidi(hostEvent(kRelative), response), "sendMidi accepted the event");

		// The next callback: the conversion already stamped counter + latency
		// + offset at submission time.
		runCallback(device, kBlock);

		const auto& staged = device.staged();
		checkEqual(staged.size(), 1u, "one staged event");
		checkEqual(staged[0].offset, static_cast<uint64_t>(kBlock) + kLatency + kRelative,
			"the staged offset is counter + latency + the block-relative offset");
	}

	/* ------------- Case group 4. THE NO-DOUBLE-DELIVERY. A reply the
	 * machine originated reaches the parser only through UART0's TX side;
	 * the events sendMidi staged never re-enter readMidiOut. */
	{
		std::printf("case group 4: no double delivery\n");

		OffsetsHarness device;

		// The machine's own output, delivered the way PLG-12 will deliver
		// it: Uart0's TX callback feeds the parser (t0_midi_out case group 2
		// drives the same path).
		device.uart0MidiOut(&device, 0xF0);
		device.uart0MidiOut(&device, 0x2D);
		device.uart0MidiOut(&device, 0xF7);

		// Host input in the same block.
		std::vector<synthLib::SMidiEvent> response;
		check(device.sendMidi(hostEvent(5), response), "sendMidi accepted the host event");

		runCallback(device, 16);

		std::vector<synthLib::SMidiEvent> midiOut;
		device.readMidiOut(midiOut);

		checkEqual(midiOut.size(), 1u,
			"readMidiOut carries exactly the machine-originated SysEx and nothing else");
	}

	std::printf("t0_midi_offsets: %d failure(s) in %d case(s)\n", g_failures, g_cases);
	return g_failures == 0 ? 0 : 1;
}
