// The DSP56300 on-chip HDI08 bootstrap ROM. At the moment the OS starts, the
// DSPs are not running Clavia code: they sit in their factory mask-ROM HDI08
// bootstrap loader, released from reset at power-on. The OS then pushes a
// count word, an address word of 0x000000, and N data words through the HDI08,
// with no CVR host command. That is the standard DSP56300 protocol, not a
// Clavia invention.
//
// It is a model and not a pre-load of the kernel images at P:$0, because a
// pre-load hides a whole boot stage. This class is that boot stage: it turns a
// stream of received 24-bit words into the words that land in P memory.
//
// The state machine: three headers followed by a body, in the order the
// firmware pushes them:
//
//   WaitingForCount     -> the next word is the COUNT N
//   WaitingForAddress   -> the next word is the P-space START ADDRESS
//   Receiving           -> the next N words are DATA, written to
//                          P[address], P[address+1], ... in order
//   Complete            -> all N data words have been received and stored
//
// The protocol carries no end-of-transfer marker. The count is the only bound
// the bootstrap knows: it receives exactly N data words and then it is
// complete. So a stream that stops early -- N words promised, fewer than N
// delivered -- is not complete: isComplete() is false and nothing dispatches.
//
// The words are 24-bit. A received word fits in a uint32_t but only its low 24
// bits are meaningful.
//
// P memory is an argument, not a type: the bootstrap writes through a plain
// pointer to the P words and a capacity in words. A bounds check refuses a
// store that would leave the supplied buffer, and a refused store is tracked
// so the case is observable instead of silent.

#pragma once

#include <cstdint>

namespace g2
{
	class Hdi08Bootstrap
	{
	public:
		// The phase the state machine is in.
		enum class Status
		{
			WaitingForCount,    // the next word is the count
			WaitingForAddress,  // the next word is the P-space start address
			Receiving,          // the next words are data
			Complete            // all N data words have been stored
		};

		// _pMemory is the P memory the bootstrap writes into, and
		// _pMemoryWords is its capacity in 24-bit words. The model never
		// writes outside that capacity.
		Hdi08Bootstrap(uint32_t* _pMemory, uint32_t _pMemoryWords);

		// Feed ONE 24-bit word as received from the host through the HDI08,
		// with no CVR host command. Returns the status AFTER the word.
		Status feed(uint32_t _word) noexcept;

		Status status() const noexcept { return m_status; }

		// True only in the Complete state: every one of the N promised data
		// words has arrived and been stored. A stream that stopped short
		// reports false here, which is the model saying the load is
		// incomplete and must not dispatch.
		bool isComplete() const noexcept { return m_status == Status::Complete; }

		// The count word, the address word, and how many data words have been
		// stored so far.
		uint32_t count() const noexcept { return m_count; }
		uint32_t address() const noexcept { return m_address; }
		uint32_t receivedDataWords() const noexcept { return m_receivedDataWords; }

		// How many of the N promised words a bounds check refused because
		// P[address + i] would have left the supplied buffer. Above zero is a
		// defect report from the caller, not a tolerated loss.
		uint32_t refusedStores() const noexcept { return m_refusedStores; }

	private:
		uint32_t* m_pMemory;
		uint32_t  m_pMemoryWords;
		Status    m_status = Status::WaitingForCount;
		uint32_t  m_count = 0;
		uint32_t  m_address = 0;
		uint32_t  m_receivedDataWords = 0;
		uint32_t  m_refusedStores = 0;
	};
}
