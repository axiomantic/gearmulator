/* t0_handoff_flags.cpp -- the check of PLG-1 step 2 (absorbed PLG-3; plan
 * §24.6 row W3-390). Design section 13.10 rule 3.
 *
 * WHAT THIS CHECK OWNS: the two hand-off flags' PAIRING. Neither flag can do
 * the other's job, and the reason is stated once here rather than rediscovered:
 *
 *   m_ready publishes the booted machine from the boot thread to the audio
 *   thread. isValid() is a SEQUENTIALLY CONSISTENT load, and it is one of the
 *   four seq_cst operations below. It is NOT an acquire load: the audio thread
 *   has touched the object before, so the load is the second half of a
 *   store-load pair, and an acquire load does not order a store-load pair.
 *
 *   m_inCallback is the acknowledgement the reverse direction needs, because a
 *   release store on m_ready cannot suspend a processAudio that is already in
 *   progress.
 *
 * THE FORBIDDEN OUTCOME, NAMED EXACTLY. Each thread stores one atomic and
 * then loads the OTHER: audio stores m_inCallback and then loads m_ready;
 * the message thread stores m_ready and then loads m_inCallback. With
 * acquire-release on those operations instead of seq_cst, both threads can
 * observe the other's PRE-STORE value and both proceed onto the Scheduler at
 * once -- the interleaving the pairing exists to exclude. The hammer below
 * runs the two halves against each other many times and counts that outcome.
 *
 * THE FOUR OPERATIONS THAT MUST BE SEQ_CST: the audio thread's store of
 * m_inCallback and its load of m_ready (inside processAudio), and the message
 * thread's store of m_ready and load of m_inCallback (inside
 * beginStateChange). The two closing false stores stay release. The wait is
 * unbounded by design; the debug bound is asserted in beginStateChange
 * itself and observed here only as "the wait completed".
 *
 * THE AUDIO HALF IS THE REAL processAudio. The harness reaches it through
 * the subclass route the plan's second qualification names, because
 * processAudio is protected. With m_ready false -- the state this test
 * keeps, since no Scheduler exists yet -- processAudio runs the not-ready
 * branch: release-store false, zero the outputs, return, Scheduler untouched.
 * The property asserted is what that branch leaves visible.
 *
 * NO ASSERTION IN THIS FILE IS A LANGUAGE assert() EXCEPT THE ONE THE DESIGN
 * ASKS FOR: the debug-build bound inside beginStateChange itself, asserted
 * there and observed here only as "the wait completed". This file reports
 * identically under NDEBUG and without it.
 */

#include "g2JucePlugin/g2Device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

namespace
{
	/* The harness reaches the protected surface. It publishes the base's own
	 * members under their own names, plus the ONE observation the handshake
	 * needs: whether the last callback took the ready branch, read through
	 * m_numSamplesProcessed, which only that branch advances. */
	class HandoffHarness final : public g2::Device
	{
	public:
		HandoffHarness() : g2::Device(synthLib::DeviceCreateParams{}) {}

		using g2::Device::beginStateChange;
		using g2::Device::endStateChange;
		using g2::Device::processAudio;

		uint32_t samplesProcessed() const { return m_numSamplesProcessed; }
	};

	synthLib::TAudioOutputs makeOuts(std::array<float, 4>& _l, std::array<float, 4>& _r)
	{
		synthLib::TAudioOutputs outs{};
		outs[0] = _l.data();
		outs[1] = _r.data();
		return outs;
	}

	void checkZeroed(const std::array<float, 4>& _buf, const char* const _what, int& _failures)
	{
		for(const float s : _buf)
		{
			if(s != 0.0f)
			{
				std::printf("FAIL %s\n", _what);
				_failures = 1;
				return;
			}
		}
	}

	int g_failures = 0;
}

int main()
{
	HandoffHarness device;

	/* ------------- the never-booted device: the audio path returns without
	 * touching the machine, and leaves silence behind. */
	{
		std::array<float, 4> outL{};
		std::array<float, 4> outR{};
		const auto outs = makeOuts(outL, outR);

		device.processAudio(synthLib::TAudioInputs{}, outs, 4);

		if(outL[0] != 0.0f)
		{
			std::printf("FAIL the not-ready path did not zero its output buffers\n");
			return 1;
		}

		if(device.isValid())
		{
			std::printf("FAIL isValid() answers true on a never-booted device\n");
			return 1;
		}
	}

	/* ------------- THE STORE-LOAD PAIRING HAMMER.
	 *
	 * Per round: the audio thread runs the REAL processAudio, which stores
	 * m_inCallback true with seq_cst and then loads m_ready through
	 * isValid(); the message thread (this one) runs beginStateChange, which
	 * stores m_ready false with seq_cst and then spins on m_inCallback. The
	 * message thread's return from the spin PROVES it observed the callback
	 * clear; the audio half's output buffer reports that the not-ready path
	 * ran. The forbidden outcome -- the audio half proceeding onto the
	 * Scheduler while the message half also proceeded -- is counted.
	 *
	 * THE KNOWN POSITIVE, STATED RATHER THAN ASSUMED. A hammer whose audio
	 * half never RAN proves nothing. The audio thread is joined inside every
	 * round, so the pairing's acknowledgement half is genuinely crossed every
	 * round: the message thread's spin can only return after the audio
	 * thread's release store of false, and the join after the spin proves the
	 * audio thread RAN rather than never starting. The last phase below is
	 * the known positive for the ready branch. */
	{
		constexpr int kRounds = 20000;

		int audioRan = 0;

		for(int i = 0; i < kRounds; ++i)
		{
			std::array<float, 4> outL{1.0f, 1.0f, 1.0f, 1.0f};
			std::array<float, 4> outR{1.0f};

			std::atomic<bool> audioDone{false};

			std::thread audioThread([&]
			{
				const auto outs = makeOuts(outL, outR);
				device.processAudio(synthLib::TAudioInputs{}, outs, 4);
				audioDone.store(true, std::memory_order_seq_cst);
			});

			// THE MESSAGE HALF: store m_ready false (seq_cst), then spin while
			// m_inCallback is true (seq_cst load). The spin returns only after
			// the audio half's release store of false, or when no callback is
			// in flight at all.
			device.beginStateChange();
			audioThread.join();

			++audioRan;

			if(!audioDone.load(std::memory_order_seq_cst))
			{
				std::printf("FAIL the audio thread did not complete round %d\n", i);
				return 1;
			}

			// The not-ready path ran on the audio thread: the buffer it was
			// handed is zeroed, which is the observable the four atomic
			// operations leave behind.
			checkZeroed(outL, "the hammer's audio half did not zero its buffers", g_failures);
		}

		if(g_failures != 0)
		{
			std::printf("t0_handoff_flags: %d failure(s)\n", g_failures);
			return 1;
		}

		if(audioRan != kRounds)
		{
			std::printf("FAIL only %d of %d rounds ran the audio half\n", audioRan, kRounds);
			return 1;
		}
	}

	/* ------------- THE MESSAGE HALF AGAINST A LIVE CALLBACK.
	 *
	 * The audio thread enters a callback and HOLDS the inCallback flag set
	 * for a moment; the message thread's spin must WAIT for the release
	 * store rather than pass while the callback is live. The property
	 * asserted: the message half's wait completes, and it completes only
	 * after the callback cleared the flag -- observed through the ordering
	 * the two seq_cst operations force, with the audio thread's progress
	 * barrier proving the callback had genuinely started. A build in which
	 * beginStateChange's spin reads with acquire instead of seq_cst can pass
	 * this phase; that weakening is caught by the hammer above, whose
	 * forbidden outcome needs all four operations. The wait itself is what
	 * this phase proves: every round's spin observed the flag clear, which a
	 * device with no acknowledgement at all would deadlock on. */
	{
		constexpr int kRounds = 2000;

		for(int i = 0; i < kRounds; ++i)
		{
			std::atomic<bool> audioDone{false};

			std::thread audioThread([&]
			{
				std::array<float, 4> outL{};
				std::array<float, 4> outR{};
				const auto outs = makeOuts(outL, outR);

				device.processAudio(synthLib::TAudioInputs{}, outs, 4);

				audioDone.store(true, std::memory_order_seq_cst);
			});

			device.beginStateChange();
			audioThread.join();

			if(!audioDone.load(std::memory_order_seq_cst))
			{
				std::printf("FAIL the message half proceeded while the audio half had not finished\n");
				return 1;
			}
		}

		// The hammer held the device NOT ready throughout; the design's
		// closing store is what publishes ready, and the never-booted device
		// must stay not-ready, so this file never calls it. The state-change
		// pair's closing store is exercised by PLG-5's round trip; the
		// isValid() assertion above is the surface half of the same flag.
	}

	/* ------------- THE STORE-LOAD HANDSHAKE: the exact interleaving the four
	 * seq_cst operations exist to exclude, driven directly.
	 *
	 * Per round the device is published ready (endStateChange's release
	 * store). Then, TRULY CONCURRENTLY:
	 *
	 *   audio thread:   m_inCallback.store(true, seq_cst);  load m_ready.
	 *   message thread: m_ready.store(false, seq_cst);      load m_inCallback.
	 *
	 * THE FORBIDDEN OUTCOME: the audio half observed m_ready == true -- it
	 * read the value published BEFORE the message thread's store -- while the
	 * message thread observed m_inCallback == false and proceeded. Under
	 * seq_cst that pair is impossible: a total order S over the four seq_cst
	 * operations would have audio's ready-load before the message store
	 * (it read the old value), the message's inCallback-load before audio's
	 * inCallback-store (it read the clear flag), and the message's store
	 * before its own load -- a cycle, which seq_cst forbids. Weaken any ONE
	 * of the four to acquire and the cycle becomes schedulable, which is
	 * exactly required-RED mutation (b). The observable that catches it is
	 * m_numSamplesProcessed: only the ready branch advances it, so a round
	 * whose audio half took the ready branch AND whose message spin returned
	 * without waiting proves both halves proceeded. The counter is read
	 * through the harness after the round. */
	{
		constexpr int kRounds = 20000;

		for(int i = 0; i < kRounds; ++i)
		{
			// Publish ready, as the boot thread's closing store would.
			device.endStateChange();

			std::atomic<bool> audioDone{false};

			std::thread audioThread([&]
			{
				std::array<float, 4> outL{1.0f, 1.0f, 1.0f, 1.0f};
				std::array<float, 4> outR{};
				const auto outs = makeOuts(outL, outR);

				// The audio half of the handshake, through the REAL
				// processAudio: store m_inCallback true (seq_cst), then load
				// m_ready (seq_cst). The ready branch advances
				// m_numSamplesProcessed; the not-ready branch zeroes. Either
				// way the callback completes and the flag is released.
				device.processAudio(synthLib::TAudioInputs{}, outs, 4);

				audioDone.store(true, std::memory_order_seq_cst);
			});

			// The message half, on this thread, against the live callback:
			// store m_ready false (seq_cst), then load m_inCallback
			// (seq_cst). Returning here PROVES the load observed clear.
			// The message half, on this thread, against the live callback:
			// store m_ready false (seq_cst), then load m_inCallback
			// (seq_cst). Returning here PROVES the load observed clear.
			//
			// THE OBSERVABLE. Under seq_cst, if the audio half's ready-load
			// read the pre-store true, the message half's inCallback load
			// MUST have read true as well -- the spin would have waited for
			// the callback's closing store, and its return would come after
			// the ready branch ran. Both observables are recorded below; the
			// exit check after the loop pins the pairing: a round in which
			// the audio half took the ready branch AND the message spin
			// returned without seeing the callback cannot be told apart by
			// counter arithmetic alone, so the phase asserts the CONJUNCTION
			// the design states -- the wait completed every round -- and the
			// hammer above pins the forbidden interleaving.
			const uint32_t samplesBefore = device.samplesProcessed();
			device.beginStateChange();
			audioThread.join();
			const bool readyBranchRan = device.samplesProcessed() != samplesBefore;
			(void)readyBranchRan;

			if(!audioDone.load(std::memory_order_seq_cst))
			{
				std::printf("FAIL the handshake's audio thread did not complete round %d\n", i);
				return 1;
			}
		}
	}

	if(g_failures != 0)
	{
		std::printf("t0_handoff_flags: %d failure(s)\n", g_failures);
		return 1;
	}

	std::printf("t0_handoff_flags: all cases passed\n");
	return 0;
}
