#pragma once

#include "mailbox.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp56k
{
	class Esai;
}

namespace rg2
{
	using EsaiReadRxCallback = dsp56k::Audio::ReadRxCallback;
	using EsaiWriteTxCallback = dsp56k::Audio::WriteTxCallback;

	enum class ChainTopology
	{
		Line, // Open both ends: N + 1 mailboxes
		Ring, // Tail feeds head: N mailboxes
		Broadcast // Shared 8-slot frame: 1 mailbox
	};

	// Coordinates inter-DSP ESAI mailboxes across audio and control chains.
	class ChainAdapter
	{
	public:
		ChainAdapter(unsigned dspCount, unsigned hopFrames, ChainTopology secondBusTopology,
					 unsigned secondBusFrameDivider);

		static constexpr unsigned mailboxCount(ChainTopology t, unsigned dspCount) noexcept
		{
			return t == ChainTopology::Line ? dspCount + 1u : t == ChainTopology::Ring ? dspCount : 1u;
		}

		unsigned dspCount() const noexcept;
		unsigned hopFrames() const noexcept;
		ChainTopology secondBusTopology() const noexcept;
		unsigned secondBusFrameDivider() const noexcept;
		unsigned audioMailboxCount() const noexcept;
		unsigned secondBusMailboxCount() const noexcept;

		// Execution phases (once per virtual frame)
		void advanceAll(uint64_t frameIndex) noexcept; // 1. Swap mailboxes and update underruns
		void injectCodecSource(const Frame&) noexcept; // 2. Audio ingress
		void extractCodecSink(Frame& out) noexcept; // 4. Audio egress (phase 3 runs via callbacks)

		// Callback factories for per-DSP ESAI wiring
		EsaiReadRxCallback audioRxCallback(unsigned position);
		EsaiWriteTxCallback audioTxCallback(unsigned position);
		EsaiReadRxCallback secondRxCallback(unsigned position);
		EsaiWriteTxCallback secondTxCallback(unsigned position);

		// Transmit tracking; checks M_TUE in Esai status register for delivery staleness
		void attachEsai(unsigned position, dsp56k::Esai& audio, dsp56k::Esai& second);
		bool audioWritten(unsigned position) const noexcept;
		bool secondWritten(unsigned position) const noexcept;

		uint64_t underrunFrames(unsigned position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned position) const noexcept;
		uint64_t phaseErrorFrames(unsigned position) const noexcept;

		size_t stateSize() const noexcept;
		void stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		void reset() noexcept;

	private:
		std::vector<Mailbox> m_audio;
		std::vector<Mailbox> m_second;

		unsigned m_dspCount = 0;
		unsigned m_hopFrames = 0;
		ChainTopology m_secondBusTopology = ChainTopology::Ring;
		unsigned m_secondBusFrameDivider = 1;

		std::vector<dsp56k::Esai*> m_audioEsai;
		std::vector<dsp56k::Esai*> m_secondEsai;
		std::vector<uint8_t> m_audioWritten;
		std::vector<uint8_t> m_secondWritten;

		std::vector<uint64_t> m_underrun;
		std::vector<uint64_t> m_secondUnderrun;
		std::vector<uint64_t> m_phaseError;
	};
} // namespace rg2
