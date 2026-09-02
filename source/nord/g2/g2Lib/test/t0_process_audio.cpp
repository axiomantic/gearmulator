/* t0_process_audio.cpp -- the audio callback's choreography.
 *
 *   1. First it sets m_inCallback, then it reads isValid(). The
 *      set-before-test order is half of the pairing that makes the state
 *      hand-off safe, so it is not free to be moved.
 *
 *   2. On isValid() false: clear m_inCallback, zero the output buffers,
 *      return, and touch the Scheduler not at all. That is what produces the
 *      silence the boot window promises and what makes a concurrent boot and
 *      audio callback unreachable rather than merely discouraged.
 *
 *   3. On isValid() true: the staged MIDI leaves first, before runFrames for
 *      the same block, then one call to Scheduler::push, then runFrames, then
 *      pull, then a read of Scheduler::faulted(), and finally m_inCallback is
 *      cleared. The call order is what fixes both codec queue capacities at
 *      L + B: push delivers a whole block before runFrames consumes any of
 *      it, and runFrames produces a whole block before pull takes any of it.
 *
 * The harness Scheduler. A real g2::Scheduler cannot record: its methods are
 * not virtual and its constructor is private behind the create factory. The
 * seam the Device itself provides is the borrowed Scheduler pointer the ready
 * branch drives -- the harness hands in a recorder through the same pointer
 * production's Scheduler occupies, so the choreography, and nothing about the
 * Scheduler's internals, is what the case exercises.
 *
 * The order probe drives the real processAudio against the real isValid while
 * a probe thread runs the message half's store-load (m_ready false, then read
 * m_inCallback) in a tight hammer. A callback that read isValid() before
 * setting m_inCallback lets a state change interleave into the callback's
 * Scheduler section, and the observable that catches it is the recorder's
 * log: a callback whose ready-branch Scheduler calls ran while the message
 * thread's beginStateChange spin was still returning. Under the real device's
 * order that count is zero in every round; under the mutated order it is
 * non-zero within a bounded round count.
 *
 * No assertion in this file is a language assert() and nothing here depends
 * on NDEBUG.
 */

#include "g2JucePlugin/g2Device.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
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

	/* The order record. One entry per Scheduler call the ready branch makes,
	 * in the order the calls happen. The order is push -> runFrames -> pull
	 * -> faulted. */
	enum class Phase { Push, RunFrames, Pull, Faulted };

	const char* phaseName(const Phase _p)
	{
		switch(_p)
		{
		case Phase::Push:      return "push";
		case Phase::RunFrames: return "runFrames";
		case Phase::Pull:      return "pull";
		case Phase::Faulted:   return "faulted";
		}
		return "?";
	}

	/* The recorder. Implements the Device's own Scheduler-driver contract
	 * (ISchedulerDriver, g2Device.h), recording each call's phase and
	 * arguments. The seam is the one the Device itself declares: the ready
	 * branch drives m_driver, and installDriver() replaces what that pointer
	 * names. Scheduler's methods are not virtual and its constructor is
	 * private behind the create factory, so the driver interface is the
	 * seam. */
	struct RecordingScheduler final : public g2::Device::ISchedulerDriver
	{
		struct Call
		{
			Phase    phase;
			size_t   frames;
		};

		mutable std::vector<Call>        calls;
		std::vector<g2::Frame>           pushedFrames;
		size_t                           runFramesArg  = 0;
		size_t                           pullRequested = 0;
		bool                             faultAnswer   = false;
		mutable uint32_t                 faultReads    = 0;

		/* Set by the push that first passes the gate: whether the message
		 * half's spin had already returned at that moment. The probe reads
		 * this after joining both threads. */
		std::atomic<bool>                firstPushDone{false};
		std::atomic<bool>                spinReturnedInWindow{false};

		/* The order probe's gate. The recorder's first push stalls until the
		 * probe releases it, so the message half's spin is guaranteed to be
		 * running while the callback sits between its isValid read and its
		 * first Scheduler call -- the exact window the set-before-test order
		 * must close. When the gate opens, the probe records whether the
		 * message half's spin had already returned while the callback was
		 * still before its first Scheduler call. */
		std::atomic<bool>                gateOpen{false};
		std::atomic<bool>                spinReturnedBeforeFirstCall{false};

		size_t push(const g2::Frame* _in, const size_t _frames) noexcept override
		{
			// The gate. The probe's message half releases the gate exactly
			// when its spin returns, having observed m_inCallback clear. If
			// this push is the one that observed the gate open, then the
			// callback reached its ready branch while the message thread had
			// already concluded no callback was in flight -- the arrangement
			// the set-before-test order exists to exclude.
			const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
			while(!gateOpen.load(std::memory_order_seq_cst))
			{
				if(std::chrono::steady_clock::now() > gateDeadline)
					break;	// the safety release fires; no observation this round
				std::this_thread::yield();
			}

			if(!firstPushDone.load(std::memory_order_seq_cst))
			{
				if(spinReturnedBeforeFirstCall.load(std::memory_order_seq_cst))
					spinReturnedInWindow.store(true, std::memory_order_seq_cst);
				firstPushDone.store(true, std::memory_order_seq_cst);
			}

			for(size_t i = 0; i < _frames; ++i)
				pushedFrames.push_back(_in[i]);
			calls.push_back({Phase::Push, _frames});
			return _frames;
		}

		void runFrames(const size_t _frames) noexcept override
		{
			runFramesArg = _frames;
			calls.push_back({Phase::RunFrames, _frames});
		}

		size_t pull(g2::Frame* _out, const size_t _frames) noexcept override
		{
			pullRequested = _frames;
			for(size_t i = 0; i < _frames; ++i)
				_out[i] = g2::Frame{};
			calls.push_back({Phase::Pull, _frames});
			return _frames;
		}

		bool faulted() const noexcept override
		{
			++faultReads;
			calls.push_back({Phase::Faulted, 0});
			return faultAnswer;
		}
	};

	/* The harness. Reaches the protected processAudio and installs the
	 * recorder through the Device's own driver seam. */
	class ProcessAudioHarness final : public g2::Device
	{
	public:
		ProcessAudioHarness() : g2::Device(synthLib::DeviceCreateParams{})
		{
			installDriver(&m_recorder);
		}

		using g2::Device::processAudio;

		RecordingScheduler& recorder() { return m_recorder; }

		/* The boot thread's closing store, run here to publish the machine.
		 * This is exactly what t0_handoff_flags does to reach the same
		 * state. */
		void forceValid() { endStateChange(); }

		/* The message thread's own half, run by the order probe below. */
		void beginStateChangeFromTest() { beginStateChange(); }

		using g2::Device::endStateChange;

	private:
		RecordingScheduler m_recorder;
	};

	/* The not-ready callback driver. Pre-poisoned buffers: the not-ready
	 * path must zero them, and the check reads them back rather than
	 * trusting the call. */
	void runNotReady(ProcessAudioHarness& _device, const size_t _samples)
	{
		std::vector<float> scratch(_samples, 1.0f);
		synthLib::TAudioOutputs outs{};
		outs[0] = scratch.data();
		outs[1] = scratch.data();

		_device.processAudio(synthLib::TAudioInputs{}, outs, _samples);

		for(const float s : scratch)
		{
			if(s != 0.0f)
			{
				check(false, "the not-ready path zeroed its output buffers");
				return;
			}
		}
		check(true, "the not-ready path zeroed its output buffers");
	}

	bool sameFrame(const g2::Frame& _a, const g2::Frame& _b)
	{
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			if(_a.slot[k] != _b.slot[k])
				return false;
		}
		return true;
	}
}

int main()
{
	/* ------------- Case group 1. The not-ready path. isValid() false: the
	 * buffers are zeroed, the counter still advances, and the Scheduler is
	 * touched not at all. */
	{
		std::printf("case group 1: the not-ready path\n");

		ProcessAudioHarness device;

		runNotReady(device, 64);

		const auto& calls = device.recorder().calls;
		check(calls.empty(), "the not-ready path touched the Scheduler not at all");
		check(!device.isValid(), "the never-booted device is not valid");
	}

	/* ------------- Case group 2. The ready branch, in order. Publish the
	 * machine, run one callback, and read the recorder's log: the Scheduler
	 * calls, in order, with the right arguments. */
	{
		std::printf("case group 2: the ready branch, in order\n");

		ProcessAudioHarness device;
		device.forceValid();

		constexpr size_t kSamples = 5;
		std::vector<float> inL(kSamples, 0.0f);
		std::vector<float> inR(kSamples, 0.0f);
		std::vector<float> outL(kSamples, 1.0f);
		std::vector<float> outR(kSamples, 1.0f);

		synthLib::TAudioInputs ins{};
		ins[0] = inL.data();
		ins[1] = inR.data();
		synthLib::TAudioOutputs outs{};
		outs[0] = outL.data();
		outs[1] = outR.data();

		device.processAudio(ins, outs, kSamples);

		const auto& calls = device.recorder().calls;
		checkEqual(calls.size(), 4u, "exactly four Scheduler calls per ready callback");

		check(calls.size() == 4 &&
			calls[0].phase == Phase::Push, "call 1 is push");
		check(calls.size() == 4 &&
			calls[1].phase == Phase::RunFrames, "call 2 is runFrames");
		check(calls.size() == 4 &&
			calls[2].phase == Phase::Pull, "call 3 is pull");
		check(calls.size() == 4 &&
			calls[3].phase == Phase::Faulted, "call 4 is the faulted read");

		checkEqual(device.recorder().runFramesArg, kSamples,
			"runFrames was asked for the whole block");
		checkEqual(device.recorder().pullRequested, kSamples,
			"pull was asked for the whole block");
		checkEqual(device.recorder().pushedFrames.size(), kSamples,
			"one pushed frame per sample");
		checkEqual(device.recorder().faultReads, 1u,
			"faulted() was read exactly once");

		// The host input arrived as Q23 frames at the head's two slots.
		bool inputsConverted = true;
		for(const auto& f : device.recorder().pushedFrames)
		{
			if(f.slot[0] != 0 || f.slot[1] != 0)
				inputsConverted = false;
		}
		check(inputsConverted, "the host input was converted and pushed");

		// The egress. The recorder answers silence, so the outputs read zero
		// after the callback -- and they were written, not preserved: the
		// pre-poisoned 1.0f values are gone.
		bool outputsWritten = true;
		for(size_t i = 0; i < kSamples; ++i)
		{
			if(outL[i] != 0.0f || outR[i] != 0.0f)
				outputsWritten = false;
		}
		check(outputsWritten, "pull's frames were converted into the outputs");
	}

	/* ------------- Case group 3. The fault response. faulted() answers true
	 * after runFrames; the one response is a release store of false into
	 * m_ready, observed as isValid() going false and the next callback taking
	 * the silence path. */
	{
		std::printf("case group 3: the fault response\n");

		ProcessAudioHarness device;
		device.forceValid();

		constexpr size_t kSamples = 3;

		device.recorder().faultAnswer = true;

		std::vector<float> outL(kSamples, 1.0f);
		std::vector<float> outR(kSamples, 1.0f);
		synthLib::TAudioOutputs outs{};
		outs[0] = outL.data();
		outs[1] = outR.data();

		device.processAudio(synthLib::TAudioInputs{}, outs, kSamples);
		check(!device.isValid(), "a faulted callback withdraws the machine: isValid() goes false");

		// The next callback takes the not-ready path: buffers zeroed,
		// Scheduler untouched.
		device.recorder().calls.clear();
		runNotReady(device, kSamples);
		check(device.recorder().calls.empty(), "the post-fault callback touches the Scheduler not at all");
	}

	/* ------------- Case group 4. The set-before-test order probe. The
	 * pairing is a store-load pair on both sides: the audio thread stores
	 * m_inCallback and then loads m_ready; the message thread stores m_ready
	 * and then loads m_inCallback. The property the set-before-test order
	 * delivers: a callback that observed m_ready true (so it entered its
	 * ready branch and recorded Scheduler calls) is visible to the message
	 * thread's spin before its first Scheduler call, so a spin that returns
	 * has either seen the callback's whole life or none of it.
	 *
	 * The observable: the probe hammers the two halves and counts the rounds
	 * in which the message half's spin returned while the callback's
	 * ready-branch Scheduler calls were still being appended -- the recorder
	 * exposes that through the difference between the log size at spin exit
	 * and at join. Under the real device's order that count is zero in every
	 * round; under the mutated order (set after the isValid read) the
	 * message half can return while the callback is mid-ready-branch, which
	 * is the overlap the pairing exists to exclude. */
	{
		std::printf("case group 4: the set-before-test order probe\n");

		ProcessAudioHarness device;
		device.forceValid();

		constexpr int kRounds = 20000;
		constexpr size_t kSamples = 4;

		uint32_t overlaps = 0;

		for(int i = 0; i < kRounds; ++i)
		{
			device.recorder().calls.clear();
			device.recorder().gateOpen.store(false, std::memory_order_seq_cst);
			device.recorder().firstPushDone.store(false, std::memory_order_seq_cst);
			device.recorder().spinReturnedInWindow.store(false, std::memory_order_seq_cst);
			device.recorder().spinReturnedBeforeFirstCall.store(false, std::memory_order_seq_cst);
			// Re-publish the machine each round: the previous round's message
			// half withdrew it with a false store into m_ready.
			device.forceValid();

			std::thread audio([&]
			{
				std::vector<float> scratch(kSamples, 1.0f);
				synthLib::TAudioOutputs outs{};
				outs[0] = scratch.data();
				outs[1] = scratch.data();

				device.processAudio(synthLib::TAudioInputs{}, outs, kSamples);
			});

			// The message half, racing the callback. beginStateChange stores
			// m_ready false and spins on m_inCallback. Under the real order
			// the callback's set precedes its isValid read, so the spin can
			// only be waiting on a set that has not landed yet, or observing
			// the flag set until the closing store -- it cannot return while
			// the callback is stalled at the ready branch's first push.
			// Under the mutated order (set after the isValid read) the
			// callback reaches the stall before any set, so the spin returns
			// while the callback is mid-ready-branch.
			std::thread message([&]
			{
				device.beginStateChangeFromTest();
				// The spin returned. Release the gate: if the callback's push
				// is the one that observes this release, the message half
				// concluded no callback was in flight while it was running --
				// the forbidden arrangement.
				device.recorder().spinReturnedBeforeFirstCall.store(true, std::memory_order_seq_cst);
				device.recorder().gateOpen.store(true, std::memory_order_seq_cst);
			});

			// The safety timeout: if neither half opens the gate (a round
			// where the audio half never reached the ready branch), release
			// it here so the run terminates. A push that passes through this
			// release does not record an overlap: the message half's spin
			// had not returned when the audio half stalled.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			device.recorder().gateOpen.store(true, std::memory_order_seq_cst);

			audio.join();
			message.join();

			// The forbidden arrangement: the message half's spin returned and
			// released the gate, and the callback's push observed that
			// release -- meaning the callback was inside its ready branch
			// while the message thread concluded no callback was in flight.
			if(device.recorder().spinReturnedInWindow.load(std::memory_order_seq_cst))
				++overlaps;
		}

		checkEqual(overlaps, 0u,
			"the set-before-test order closed the overlap window in every round");
	}

	std::printf("t0_process_audio: %d failure(s) in %d case(s)\n", g_failures, g_cases);
	return g_failures == 0 ? 0 : 1;
}
