/* t1_state_handoff_load.cpp -- the state hand-off under sustained load,
 * observed by a thread sanitizer.
 *
 * A dropped acknowledgement is caught here, measured and not assumed:
 * removing the message thread's wait lets the two threads reach the same plain
 * memory at once, and the sanitizer reports it within seconds. The negative
 * build below is that mutation.
 *
 * A weakened memory order is not caught. Measured on this host, macOS arm64,
 * over a 20-second window of this same workload: weakening the audio thread's
 * load of m_ready to acquire leaves this run green, and so does weakening the
 * message thread's store of m_ready to release. That is the expected behaviour
 * of a data-race detector and not a defect in the workload -- it reports
 * unsynchronized access to plain memory, and an atomic accessed with a weaker
 * order is still an atomic access. What pins those orders is the source and
 * its comments.
 *
 * One thread calls the real processAudio continuously. A second thread calls
 * the real getState and setState in a loop, and then makes one guarded edit to
 * a payload the audio callback also touches. Both halves run for the full
 * duration below.
 *
 * The payload is the instrument, and it is here because an unbooted device
 * shares almost nothing. The hand-off exists to keep the message thread off
 * the Scheduler while a callback is in flight. No Scheduler is installed on
 * this device, so the audio callback and the state calls touch no common
 * non-atomic object, and a sanitizer over that alone would report nothing
 * whether the pairing were correct or not -- a zero with no known positive
 * behind it. The payload stands in for the Scheduler: the audio half reaches
 * it through the Device's own ISchedulerDriver seam, from inside the real
 * ready branch, and the message half writes it inside a real
 * beginStateChange/endStateChange window. Both accesses are plain, unatomic
 * memory, which is what a data-race detector can speak about.
 *
 * The negative case: built from this same file with G2_HANDOFF_DROP_ACK
 * defined, the message half's payload edit drops the beginStateChange call --
 * the acknowledgement, and the store that withdraws readiness -- and keeps the
 * closing endStateChange. Nothing else changes. The audio half then stays on
 * the ready branch and touches the payload while the message half writes it,
 * and the sanitizer reports the race.
 *
 * A clean run is evidence over the accesses that occurred, not a proof over
 * all schedules.
 */

#include "g2JucePlugin/g2Device.h"

#include "gatedFixture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
	// The load window. The floor is 60 seconds of concurrent running.
	// The dropped-acknowledgement build needs only enough time to be caught in
	// the act, and a sanitizer halts on the first report, so a shorter window
	// there costs nothing and keeps a deliberately-racing binary from running
	// for a minute after it has already answered.
#ifdef G2_HANDOFF_DROP_ACK
	constexpr int g_loadSeconds = 15;
#else
	constexpr int g_loadSeconds = 60;
#endif

	constexpr size_t g_blockSamples = 64;

	/* The shared object the pairing protects, in the role the Scheduler plays
	 * in production. Plain memory on purpose: an atomic here would be a
	 * payload the sanitizer has nothing to say about, and the property under
	 * test is that the hand-off -- not the payload's own type -- is what keeps
	 * the two threads apart. */
	struct Payload
	{
		uint32_t words[64] = {};
		uint32_t writes = 0;
	};

	Payload g_payload;

	// Written by the message thread inside the hand-off window.
	void writePayload(const uint32_t _value)
	{
		for(uint32_t& w : g_payload.words)
			w = _value;

		++g_payload.writes;
	}

	// Read by the audio thread from inside the real ready branch. The sum is
	// kept so the reads cannot be elided.
	uint64_t readPayload()
	{
		uint64_t sum = 0;

		for(const uint32_t w : g_payload.words)
			sum += w;

		return sum;
	}

	/* The audio half's reach into the payload. The Device's ready branch calls
	 * push, runFrames, pull and faulted in that order; this driver reads the
	 * payload on each of the first three, so the shared access sits where the
	 * production Scheduler's would. */
	class PayloadDriver final : public g2::Device::ISchedulerDriver
	{
	public:
		size_t push(const g2::Frame*, const size_t _frames) noexcept override
		{
			m_readSum += readPayload();
			return _frames;
		}

		void runFrames(const size_t) noexcept override
		{
			m_readSum += readPayload();
		}

		size_t pull(g2::Frame* const _out, const size_t _frames) noexcept override
		{
			m_readSum += readPayload();

			for(size_t i = 0; i < _frames; ++i)
				_out[i] = g2::Frame{};

			return _frames;
		}

		bool faulted() const noexcept override
		{
			// Never faulted: a fault would withdraw m_ready for the rest of
			// the run and the audio half would stop reaching the payload,
			// which would silently end the concurrency this test needs.
			return false;
		}

		uint64_t readSum() const noexcept { return m_readSum; }

	private:
		uint64_t m_readSum = 0;
	};

	class LoadHarness final : public g2::Device
	{
	public:
		LoadHarness() : g2::Device(synthLib::DeviceCreateParams{})
		{
			installDriver(&m_payloadDriver);
		}

		using g2::Device::beginStateChange;
		using g2::Device::endStateChange;
		using g2::Device::processAudio;

		uint32_t samplesProcessed() const { return m_numSamplesProcessed; }
		uint64_t driverReadSum() const { return m_payloadDriver.readSum(); }

	private:
		PayloadDriver m_payloadDriver;
	};

	struct Counts
	{
		uint64_t callbacks = 0;      // processAudio calls that completed
		uint64_t readyBranch = 0;    // of those, the ones that reached the driver
		uint64_t stateRounds = 0;    // getState+setState pairs that completed
		uint64_t waitsCompleted = 0; // beginStateChange returns on the payload edit
		uint64_t getStateFailures = 0;
		uint64_t setStateFailures = 0;
	};

	bool runLoad(std::ostream& _out)
	{
		LoadHarness device;

		// Publish readiness, as the boot thread's closing store would. Without
		// it every callback takes the not-ready branch and the audio half
		// never reaches the payload at all.
		device.endStateChange();

		std::atomic<bool> stop{false};
		Counts counts;

		std::atomic<uint64_t> stateRounds{0};
		std::atomic<uint64_t> waitsCompleted{0};
		std::atomic<uint64_t> getStateFailures{0};
		std::atomic<uint64_t> setStateFailures{0};

		/* The message thread. getState and setState take the real hand-off
		 * themselves; the payload edit takes it explicitly so the shared write
		 * has a window of its own. */
		std::thread messageThread([&]
		{
			uint64_t rounds = 0;
			uint64_t waits = 0;
			uint64_t getFailures = 0;
			uint64_t setFailures = 0;
			uint32_t value = 1;

			while(!stop.load(std::memory_order_relaxed))
			{
				std::vector<uint8_t> image;

#if SYNTHLIB_DEMO_MODE == 0
				if(!device.getState(image, synthLib::StateTypeGlobal))
					++getFailures;
				else if(!device.setState(image, synthLib::StateTypeGlobal))
					++setFailures;
#endif

#ifndef G2_HANDOFF_DROP_ACK
				/* The acknowledgement. Returning from here proves the message
				 * thread observed the callback flag clear; the count below is
				 * the "the wait completed on every iteration" observation. */
				device.beginStateChange();
#endif
				++waits;

				writePayload(value++);

				device.endStateChange();

				++rounds;

				// A brief pause so the audio half spends most of the run on
				// the ready branch rather than in the not-ready window the
				// state calls open. Without it the ready-branch count -- the
				// known positive for the audio half reaching the payload at
				// all -- collapses toward zero.
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}

			stateRounds.store(rounds, std::memory_order_relaxed);
			waitsCompleted.store(waits, std::memory_order_relaxed);
			getStateFailures.store(getFailures, std::memory_order_relaxed);
			setStateFailures.store(setFailures, std::memory_order_relaxed);
		});

		/* The audio thread is this one. The two frame buffers processAudio's
		 * ready branch places on the stack are sized for the framework's
		 * largest sub-block, and the main thread's stack holds them where a
		 * spawned thread's default would be tight under a sanitizer. */
		std::array<float, g_blockSamples> outL{};
		std::array<float, g_blockSamples> outR{};

		synthLib::TAudioOutputs outs{};
		outs[0] = outL.data();
		outs[1] = outR.data();

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(g_loadSeconds);

		uint64_t lastSamples = device.samplesProcessed();

		while(std::chrono::steady_clock::now() < deadline)
		{
			const uint64_t sumBefore = device.driverReadSum();

			device.processAudio(synthLib::TAudioInputs{}, outs, g_blockSamples);

			++counts.callbacks;

			if(device.driverReadSum() != sumBefore)
				++counts.readyBranch;
		}

		stop.store(true, std::memory_order_relaxed);
		messageThread.join();

		counts.stateRounds = stateRounds.load(std::memory_order_relaxed);
		counts.waitsCompleted = waitsCompleted.load(std::memory_order_relaxed);
		counts.getStateFailures = getStateFailures.load(std::memory_order_relaxed);
		counts.setStateFailures = setStateFailures.load(std::memory_order_relaxed);

		(void)lastSamples;

		_out << "load: seconds=" << g_loadSeconds
			<< " callbacks=" << counts.callbacks
			<< " readyBranch=" << counts.readyBranch
			<< " stateRounds=" << counts.stateRounds
			<< " waitsCompleted=" << counts.waitsCompleted
			<< " payloadWrites=" << g_payload.writes
			<< '\n';

		bool ok = true;

		/* The known positives, asserted rather than assumed. A run in which
		 * either half never ran is a run whose clean sanitizer report says
		 * nothing, so each half's evidence that it executed is a failure
		 * condition of its own. */
		if(counts.callbacks == 0)
		{
			_out << "FAIL the audio half ran no callback\n";
			ok = false;
		}

		if(counts.readyBranch == 0)
		{
			_out << "FAIL no callback reached the driver, so the audio half never touched the shared payload\n";
			ok = false;
		}

		if(counts.stateRounds == 0)
		{
			_out << "FAIL the message half completed no round\n";
			ok = false;
		}

		// The wait completed on every iteration: one per round, counted after
		// beginStateChange returned.
		if(counts.waitsCompleted != counts.stateRounds)
		{
			_out << "FAIL the wait completed " << counts.waitsCompleted
				<< " times over " << counts.stateRounds << " rounds\n";
			ok = false;
		}

		if(counts.getStateFailures != 0 || counts.setStateFailures != 0)
		{
			_out << "FAIL the state round trip failed: getState=" << counts.getStateFailures
				<< " setState=" << counts.setStateFailures << '\n';
			ok = false;
		}

		if(device.samplesProcessed() == 0)
		{
			_out << "FAIL the sample counter did not advance\n";
			ok = false;
		}

		return ok;
	}
}

int main()
{
	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, []
	{
		return runLoad(std::cout);
	});

	std::cout << g2::test::summaryLine(counters) << '\n';

	return g2::test::gatedExitCode(counters);
}
