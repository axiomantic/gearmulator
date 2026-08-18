// Task BRD-24. The MAX1039 slave. See max1039.h for what this unit is.
//
// PROVENANCE. The register layout, the reference-selection table, the channel
// map, the scan modes and the two clock modes are read from Maxim document
// 19-2442 Rev 0. That is NOT the current revision, so no electrical limit from
// it is quoted here; the address, the register map and the cycle formats carry
// across revisions because a shipped part's two-wire address cannot move
// without breaking every existing design.
//
// WHAT IS DELIBERATELY NOT MODELLED, because nothing drives it and a model
// built for an unexercised path cannot be told from a wrong one: the
// pseudo-differential channel map, the scan mode that sweeps up from AIN6, the
// bipolar transfer function, the configuration-register reset the setup byte's
// RST bit performs, and the twelve-byte result RAM that internal clock mode
// reads back FIFO.

#include "max1039.h"

namespace g2
{
	namespace
	{
		// The setup byte's fields, Table 1.
		constexpr uint8_t g_registerSelect = 0x80u;   // 1 = setup, 0 = configuration
		constexpr int     g_selectShift    = 4;
		constexpr uint8_t g_selectMask     = 0x07u;
		constexpr uint8_t g_clockSelect    = 0x08u;   // 1 = external clock

		// The setup byte's SEL field, Table 6. SEL2 chooses the internal
		// reference and SEL1 turns AIN11/REF into a reference pin.
		constexpr uint8_t g_select1 = 0x02u;
		constexpr uint8_t g_select2 = 0x04u;

		// The configuration byte's fields, Table 2.
		constexpr int     g_scanShift       = 5;
		constexpr uint8_t g_scanMask        = 0x03u;
		constexpr int     g_channelShift    = 1;
		constexpr uint8_t g_channelMask     = 0x0fu;
		constexpr uint8_t g_singleEndedBit  = 0x01u;

		// Scan mode 00 sweeps up from AIN0 to the selected channel.
		constexpr uint8_t g_scanUpFromZero = 0u;

		// The result is eight bits, so one LSB is the reference over 256.
		constexpr float g_fullScaleCounts = 256.0f;
	}

	Max1039::Max1039(const Max1039Config& _config)
		: m_address(_config.address)
		, m_externalReferenceVolts(_config.externalReferenceVolts)
		, m_supplyVolts(_config.supplyVolts)
		, m_internalReferenceVolts(_config.internalReferenceVolts)
	{
	}

	void Max1039::setChannelVolts(const uint8_t _channel, const float _volts)
	{
		if(_channel >= g_settableChannels)
			return;

		m_channelVolts[_channel] = _volts;
	}

	float Max1039::channelVolts(const uint8_t _channel) const
	{
		/* PIN 13 CARRIES WHAT THE BOARD FEEDS IT whether the setup register is
		 * using it as a reference or reading it as a channel, so its potential
		 * is the reference argument and not a setter of its own. */
		if(_channel == g_referenceChannel)
			return m_externalReferenceVolts;

		if(_channel >= g_settableChannels)
			return 0.0f;

		return m_channelVolts[_channel];
	}

	Max1039::ReferenceSource Max1039::referenceSource() const
	{
		if((m_select & g_select2) != 0u)
			return ReferenceSource::Internal;

		if((m_select & g_select1) != 0u)
			return ReferenceSource::External;

		return ReferenceSource::Supply;
	}

	bool Max1039::referencePinIsReference() const
	{
		return (m_select & g_select1) != 0u;
	}

	float Max1039::referenceVolts() const
	{
		switch(referenceSource())
		{
		case ReferenceSource::External:
			return m_externalReferenceVolts;
		case ReferenceSource::Internal:
			return m_internalReferenceVolts;
		case ReferenceSource::Supply:
			break;
		}

		return m_supplyVolts;
	}

	std::vector<uint8_t> Max1039::scanChannels() const
	{
		std::vector<uint8_t> channels;

		/* SCAN 01 AND SCAN 11 BOTH CONVERT THE SELECTED CHANNEL ALONE, and in
		 * external clock mode the datasheet says there is no difference between
		 * them at all. Scan 10's own sweep start is not modelled and this
		 * branch answers for it. */
		if(m_scan != g_scanUpFromZero)
		{
			channels.push_back(m_channelSelect);
			return channels;
		}

		for(uint8_t channel = 0; channel <= m_channelSelect; ++channel)
		{
			// AIN11/REF is excluded from a multichannel scan while SEL1 makes
			// it a reference pin.
			if(channel == g_referenceChannel && referencePinIsReference())
				continue;

			channels.push_back(channel);
		}

		return channels;
	}

	uint8_t Max1039::convert(const uint8_t _channel) const
	{
		// Table 3 note 2: a single-ended read of AIN11/REF returns ground while
		// SEL1 makes the pin a reference.
		if(_channel == g_referenceChannel && referencePinIsReference())
			return 0u;

		const float reference = referenceVolts();

		// A board nobody configured has no reference and converts to zero,
		// which is the honest answer and not a division.
		if(!(reference > 0.0f))
			return 0u;

		const float counts = channelVolts(_channel) * g_fullScaleCounts / reference;

		// The transfer function saturates. A cast of an out-of-range float to
		// an integer type is undefined, so each bound is tested first.
		if(counts <= 0.0f)
			return 0u;
		if(counts >= g_fullScaleCounts - 1.0f)
			return 255u;

		return uint8_t(counts);
	}

	bool Max1039::start(const uint8_t _address7, const bool _read)
	{
		/* THE ADDRESS DISCRIMINATES, AND THIS IS THE ONE SITE THAT DECIDES IT.
		 * The HS-mode master code 0000 1xxx falls out of the same rule: its
		 * address bits match nothing, so it is not acknowledged, which is the
		 * response the datasheet calls expected rather than an error. */
		if(_address7 != m_address)
		{
			m_selected = false;
			return false;
		}

		m_selected = true;
		m_reading  = _read;

		if(_read)
		{
			// The sequence is built at the address phase, so a configuration
			// byte written between two read transactions takes effect at the
			// next one rather than mid-scan.
			m_sequence = scanChannels();
			m_position = 0u;
		}

		return true;
	}

	bool Max1039::write(const uint8_t _byte)
	{
		if(!m_selected)
			return false;

		/* THE TWO REGISTERS ARE WRITE-ONLY AND THERE IS NO REGISTER-ADDRESS
		 * BYTE. Bit 7 of each written byte routes it, so a master may send one
		 * byte or two, in either order, and this model decodes each byte on its
		 * own rather than by its position in the transaction. */
		if((_byte & g_registerSelect) != 0u)
			applySetup(_byte);
		else
			applyConfiguration(_byte);

		return true;
	}

	uint8_t Max1039::read()
	{
		if(!m_selected || !m_reading || m_sequence.empty())
			return 0u;

		const uint8_t channel = m_sequence[m_position];

		/* THE SCAN REPEATS AND THE READ NEVER ENDS ITSELF. In external clock
		 * mode the part converts until it receives a not-acknowledge, and this
		 * firmware never sends one: it clears TXAK on every byte with no index
		 * test and issues no STOP for the read. The driver carries no timeout,
		 * no retry and no error path, so a read that ended itself would stop the
		 * machine rather than mis-report it. */
		m_position = (m_position + 1u) % m_sequence.size();

		return convert(channel);
	}

	void Max1039::stop()
	{
		m_selected = false;
	}

	void Max1039::applySetup(const uint8_t _byte)
	{
		m_select        = uint8_t((_byte >> g_selectShift) & g_selectMask);
		m_externalClock = (_byte & g_clockSelect) != 0u;
	}

	void Max1039::applyConfiguration(const uint8_t _byte)
	{
		m_scan          = uint8_t((_byte >> g_scanShift) & g_scanMask);
		m_channelSelect = uint8_t((_byte >> g_channelShift) & g_channelMask);
		m_singleEnded   = (_byte & g_singleEndedBit) != 0u;
	}
}
