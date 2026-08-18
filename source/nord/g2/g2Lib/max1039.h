// Task BRD-24. The MAX1039 slave, U12 on the panel board.
//
// Plan sections 13.5, 13.5.1, 13.5.3. The device is covered by no design
// section.
//
// WHAT THIS FILE IS. A twelve-channel eight-bit two-wire analogue-to-digital
// converter, modelled as far as this machine's own evidence reaches: the
// schematic that wires it, the datasheet that describes it and the firmware
// that drives it.
//
// THE ADDRESS IS A PARAMETER WITH THE MEASURED VALUE AS ITS SHIPPED DEFAULT.
// It is documentary rather than empirical -- read from one datasheet at a
// superseded revision, with no bus scan behind it -- so the day a hardware read
// disagrees, one default moves and no test is rewritten. One parameterised
// model also serves the whole family, whose members differ at the bus in
// nothing but this value.
//
// PIN 13 IS AIN11/REF AND NOT AN UNWIRED TWELFTH CHANNEL. The board feeds the
// reference into it, so the datasheet's own rule applies: while SEL1 is set the
// pin is excluded from a multichannel scan and a direct single-ended read of it
// returns ground. That is why the reference pin is not a settable channel and
// takes its potential from the reference argument.
//
// NO VOLTAGE IS SHIPPED HERE. Every potential is a configuration argument,
// because the only figure anyone has for this board comes off a schematic
// annotation and a header carrying it would make it look measured.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mbus.h"

namespace g2
{
	/* The factory-programmed seven-bit address of the MAX1038 and the MAX1039.
	 * The MAX1036 and the MAX1037 answer at 0x64 instead, which is what
	 * separates them at the bus. */
	constexpr uint8_t g_max1039Address = 0x65u;

	/* Everything a caller must supply. The potentials have no default that
	 * means anything: a board nobody configured converts to zero, which is the
	 * honest answer and not a claim about any machine. */
	struct Max1039Config
	{
		float externalReferenceVolts = 0.0f;
		float supplyVolts            = 0.0f;
		float internalReferenceVolts = 0.0f;
		uint8_t address              = g_max1039Address;
	};

	class Max1039 final : public BusSlave
	{
	public:
		// AIN0 to AIN10. AIN11 is the reference pin and takes its potential
		// from the reference argument rather than from a setter.
		static constexpr uint8_t g_settableChannels = 11u;
		static constexpr uint8_t g_referenceChannel = 11u;

		enum class ReferenceSource { Supply, External, Internal };

		explicit Max1039(const Max1039Config& _config = {});

		void setChannelVolts(uint8_t _channel, float _volts);
		float channelVolts(uint8_t _channel) const;

		uint8_t address() const { return m_address; }

		ReferenceSource referenceSource() const;

		// TRUE when SEL1 makes AIN11/REF a reference pin rather than an
		// ordinary analogue input.
		bool referencePinIsReference() const;

		float referenceVolts() const;

		bool externalClock() const { return m_externalClock; }

		// The device holds SCL low while it tracks and converts in internal
		// clock mode, and does not in external clock mode.
		bool stretchesClock() const { return !m_externalClock; }

		uint8_t scanMode() const { return m_scan; }
		uint8_t channelSelect() const { return m_channelSelect; }
		bool singleEnded() const { return m_singleEnded; }

		// The channels a read transaction would deliver, in order, before the
		// sequence repeats.
		std::vector<uint8_t> scanChannels() const;

		uint8_t convert(uint8_t _channel) const;

		bool start(uint8_t _address7, bool _read) override;
		bool write(uint8_t _byte) override;
		uint8_t read() override;
		void stop() override;

	private:
		void applySetup(uint8_t _byte);
		void applyConfiguration(uint8_t _byte);

		uint8_t m_address;

		float m_externalReferenceVolts;
		float m_supplyVolts;
		float m_internalReferenceVolts;

		std::array<float, g_settableChannels> m_channelVolts{};

		// The setup register's fields this model decodes. The power-on state is
		// SEL 000 and internal clock. BIP/UNI is NOT decoded: nothing drives
		// bipolar mode and a field stored with no reader is a claim the code
		// does not keep.
		uint8_t m_select        = 0u;
		bool    m_externalClock = false;

		// The configuration register's fields. The power-on state is scan-up,
		// channel 0, single-ended.
		uint8_t m_scan          = 0u;
		uint8_t m_channelSelect = 0u;
		bool    m_singleEnded   = true;

		bool m_selected = false;
		bool m_reading  = false;

		std::vector<uint8_t> m_sequence;
		size_t m_position = 0u;
	};
}
