// The bootstrap ROM. It only moves words the host supplies; it is the OS that
// decides what the words are.
//
// A received word is one of three headers or one of the data words of the
// body, decided by where the state machine is. The count is read first, then
// the address, then the body. Each body word is stored at P[address + i] for
// i = 0, 1, ..., and a body word whose target lies at or past the supplied
// capacity is refused: it is not stored, and refusedStores() counts it,
// because silently writing past the end of a caller's buffer is the one
// failure a model like this must never be able to hide.

#include "hdi08Bootstrap.h"

namespace g2
{
	// A DSP56300 word is 24 bits. The host stores a 24-bit word in the high
	// three bytes of the TX registers, but a value that arrives in a uint32_t
	// is only meaningful in its low 24 bits. Masking here keeps a stray high
	// byte from polluting the count, the address, or the stored data.
	namespace
	{
		constexpr uint32_t g_wordMask = 0x00ffffffu;
	}

	Hdi08Bootstrap::Hdi08Bootstrap(uint32_t* _pMemory, const uint32_t _pMemoryWords)
		: m_pMemory(_pMemory)
		, m_pMemoryWords(_pMemoryWords)
	{
	}

	Hdi08Bootstrap::Status Hdi08Bootstrap::feed(const uint32_t _word) noexcept
	{
		const uint32_t word = _word & g_wordMask;

		switch(m_status)
		{
		case Status::WaitingForCount:
			// The first word is the number of data words to follow. It tells
			// the machine how long the body is, which is the only bound the
			// protocol carries (there is no end-of-transfer marker).
			m_count = word;
			m_receivedDataWords = 0;
			m_refusedStores = 0;
			m_status = Status::WaitingForAddress;
			break;

		case Status::WaitingForAddress:
			// The second word is the P-space start address. The firmware
			// pushes 0x000000 here, but the model does not special-case it:
			// whatever address the host chooses is where the body lands.
			//
			// A count of zero promises an empty body, so the load is complete
			// the moment the address is known: there are no data words to wait
			// for.
			m_address = word;
			m_status = Status::Receiving;
			if(m_receivedDataWords == m_count && m_refusedStores == 0)
				m_status = Status::Complete;
			break;

		case Status::Receiving:
			// The body. Store at P[address + i]. A store that would leave the
			// supplied buffer is refused and counted, and the machine STAYS in
			// Receiving: the promised count has not all landed, so the load is
			// not complete.
			if(m_receivedDataWords < m_count)
			{
				const uint32_t target = m_address + m_receivedDataWords;
				if(target < m_pMemoryWords)
				{
					m_pMemory[target] = word;
				}
				else
				{
					++m_refusedStores;
				}
				++m_receivedDataWords;

				// Completion requires that every promised word landed. A store
				// the bounds check refused means part of the image did not
				// reach P memory, so a corrupt image must not dispatch: the
				// machine stays Receiving and reports incomplete through
				// isComplete() being false.
				if(m_receivedDataWords == m_count && m_refusedStores == 0)
					m_status = Status::Complete;
			}
			// A word past the count is beyond the promised body. It is
			// discarded: the bootstrap knows exactly N words and reads none
			// after them.
			break;

		case Status::Complete:
			// A word after a completed load has no legal destination in the
			// protocol. It is discarded.
			break;
		}

		return m_status;
	}
}
