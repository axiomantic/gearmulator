#include "hdi08Bridge.h"

#include "hdi08Adapter.h"

#include "dsp56kEmu/hdi08.h"

namespace g2
{
	namespace
	{
		// HF2 and HF3 sit at bits 3 and 4 of the DSP's HCR and of the host ISR
		// alike, so the mirror is a masked copy and not a translation.
		constexpr uint8_t g_hostFlagMask = mc68k::Hdi08::Hf2 | mc68k::Hdi08::Hf3;
	}

	Hdi08Bridge::Hdi08Bridge(mc68k::Hdi08& _host, dsp56k::DSP& _core, dsp56k::HDI08& _dsp)
		: m_host(_host)
		, m_dsp(_dsp)
		, m_boot(_core)
	{
		/* THE BOOT CONSUMER SITS IN FRONT OF THE RUNTIME PATH AND IS REPLACED BY
		 * IT. The firmware pushes a program into every port before it speaks to
		 * the DSP at all, so a bridge that forwarded the first words to the
		 * receive path would leave program memory zero-filled -- and 0x000000 is
		 * a no-operation, so the core would walk it and fault nowhere. */
		m_host.setWriteTxCallback([this](const uint32_t _word)
		{
			onBootWord(_word);
		});

		/* The host asks for data both when it finds the receive register empty
		 * and when it has just emptied it. Both are the moment a word held on
		 * the DSP side can move, so neither is filtered on the flag. */
		m_host.setRxEmptyCallback([this](bool)
		{
			drainDspToHost();
		});

		/* THIS CALLBACK DOES NOT DRAIN, unlike the n2x precedent's. Draining
		 * calls `mc68k::Hdi08::writeRx`, which reads the ISR, which re-enters
		 * this callback. */
		m_host.setReadIsrCallback([this](const uint8_t _isr)
		{
			return mirrorDspHostFlags(_isr);
		});

		m_dsp.setWriteTxCallback([this]
		{
			drainDspToHost();
		});
	}

	void Hdi08Bridge::onBootWord(const uint32_t _word)
	{
		if(!m_boot.hdiWriteTX(dsp56k::TWord(_word)))
			return;

		m_programLanded = true;

		/* THE ASSIGNMENT DESTROYS THE CLOSURE THIS CALL IS RUNNING INSIDE.
		 * `mc68k::Hdi08` holds ONE write-transmit callback and assigns rather
		 * than chains, so nothing that closure captured may be read afterwards
		 * and this line is last for that reason. */
		m_host.setWriteTxCallback([this](const uint32_t _runtimeWord)
		{
			onHostWord(_runtimeWord);
		});
	}

	void Hdi08Bridge::onHostWord(const uint32_t _word)
	{
		m_pending.push_back(dsp56k::TWord(_word));
		offerPendingToDsp();
	}

	void Hdi08Bridge::offerPendingToDsp()
	{
		if(m_pending.empty())
			return;

		const uint32_t moved = hdi08MoveWordsForQuantum(m_dsp, m_pending.data(),
			static_cast<uint32_t>(m_pending.size()));

		m_pending.erase(m_pending.begin(), m_pending.begin() + moved);
	}

	void Hdi08Bridge::drainDspToHost()
	{
		while(m_host.canReceiveData() && m_dsp.hasTX())
			m_host.writeRx(m_dsp.readTX());
	}

	uint8_t Hdi08Bridge::mirrorDspHostFlags(const uint8_t _isr) const
	{
		const auto flags = uint8_t(m_dsp.readControlRegister()) & g_hostFlagMask;

		return uint8_t((_isr & uint8_t(~g_hostFlagMask)) | flags);
	}
}
