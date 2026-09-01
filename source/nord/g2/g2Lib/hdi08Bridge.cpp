#include "hdi08Bridge.h"

#include "hdi08Adapter.h"

#include "dsp56kEmu/dsp.h"
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
		, m_core(_core)
		, m_boot(_core)
	{
		/* The boot consumer sits in front of the runtime path and is replaced by
		 * it. The firmware pushes a program into every port before it speaks to
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

		/* This callback does not drain, unlike the n2x precedent's. Draining
		 * calls `mc68k::Hdi08::writeRx`, which reads the ISR, which re-enters
		 * this callback. */
		m_host.setReadIsrCallback([this](const uint8_t _isr)
		{
			return mirrorDspHostFlags(_isr);
		});

		/* The byte is passed through and not re-derived. The host port has
		 * already computed `(_val & Hv) << 1` and the written byte is not in
		 * scope here, so any arithmetic on this side would be a second
		 * derivation of a value that is already the vector.
		 *
		 * `injectInterruptImmediate` IS REFUSED. It runs the vector
		 * synchronously inside the MCU bus cycle that stored the CVR, which
		 * re-enters the deterministic scheduler's own quantum accounting, and
		 * it drops the vector in silence when the mask is up. */
		m_host.setWriteIrqCallback([this](const uint8_t _vector)
		{
			m_core.injectInterrupt(_vector);
		});

		m_dsp.setWriteTxCallback([this]
		{
			drainDspToHost();
		});

		/* HF0 and HF1 sit at bits 3 and 4 of the host ICR and of the DSP HSR
		 * alike, so the forwarding is a masked copy and not a translation. */
		m_host.setWriteIcrCallback([this](const uint8_t _icr)
		{
			const uint8_t flags = _icr & 0x18;
			m_dsp.setPendingHostFlags01(static_cast<uint32_t>(flags));
		});
	}

	/* NULL is the uninstall and not a hole. Every `mc68k::Hdi08` setter puts the
	 * port's own default back when it is handed an empty function, so the port
	 * returns to what it did before the bridge existed rather than to a callback
	 * that cannot be called; `dsp56k::HDI08` leaves its transmit callback empty
	 * instead and tests it before the one call it makes.
	 *
	 * The init slot is left alone here for the reason it is left alone above.
	 * `Hdi08Adapter` owns that one and the bridge never wrote it. */
	Hdi08Bridge::~Hdi08Bridge()
	{
		m_host.setWriteTxCallback(nullptr);
		m_host.setRxEmptyCallback(nullptr);
		m_host.setReadIsrCallback(nullptr);
		m_host.setWriteIrqCallback(nullptr);
		m_host.setWriteIcrCallback(nullptr);

		m_dsp.setWriteTxCallback(nullptr);
	}

	void Hdi08Bridge::onBootWord(const uint32_t _word)
	{
		if(!m_boot.hdiWriteTX(dsp56k::TWord(_word)))
			return;

		m_programLanded = true;

		/* The assignment destroys the closure this call is running inside.
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
