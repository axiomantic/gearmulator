/* g2Device.h -- the synthLib::Device subclass.
 *
 * This subclass is the whole of this project's contact with the synthLib
 * framework: it presents the pure virtuals synthLib::Device declares, and it
 * holds the two hand-off flags.
 *
 * The two conditional virtuals. synthLib's device.h wraps getState, setState
 * and setStateFromUnknownCustomData in #if SYNTHLIB_DEMO_MODE == 0, of which
 * only the first two are pure virtuals. A subclass that marks those two
 * `override` unconditionally will not compile under SYNTHLIB_DEMO_MODE, so
 * the two overrides below carry the same guard.
 *
 * The hand-off pairing, stated here because the memory orders are the
 * deliverable:
 *
 *   audio thread, in processAudio:      m_inCallback.store(true, seq_cst);
 *                                       load m_ready (seq_cst); on false,
 *                                       m_inCallback.store(false, release),
 *                                       zero the output buffers and return;
 *                                       otherwise touch the Scheduler; then
 *                                       m_inCallback.store(false, release).
 *   message thread (getState/setState): m_ready.store(false, seq_cst);
 *                                       spin and yield while
 *                                       m_inCallback.load(seq_cst) is true;
 *                                       touch the Scheduler; then
 *                                       m_ready.store(true, release).
 *
 * The store-then-load operations must be memory_order_seq_cst, because each
 * thread stores one atomic and then loads the other: an acquire load does not
 * order a store-load pair, and without seq_cst both threads can observe the other's
 * pre-store value and both proceed -- exactly the interleaving the pairing
 * exists to exclude. The two closing false stores stay release. The wait is
 * unbounded by design; a debug build asserts a generous bound.
 *
 * wLib::Device was considered as a base class and rejected. It would supply
 * m_numSamplesProcessed for free, but wLib/wDevice.h declares a pure
 * virtual getDspEsxiClock() returning dsp56k::EsxiClock*, and the design
 * requires that this project constructs zero EsaiClock objects. See
 * docs/divergence.md. Do not re-open it.
 */

#pragma once

#include "synthLib/device.h"

#include "artifactResolver.h"
#include "board.h"
#include "executor.h"
#include "firmwareState.h"

#include "g2State.h"
#include "scheduler.h"

#include "synthLib/midiBufferParser.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace g2
{
	class Uart0;
}

namespace g2
{
	class Device : public synthLib::Device
	{
	public:
		explicit Device(const synthLib::DeviceCreateParams& _params);

		/* Declared here and defined in the .cpp because m_sdram holds an
		 * incomplete type: a destructor the compiler generated here could
		 * not see Sdram's definition. */
		~Device() override;

		float getSamplerate() const override;
		bool isValid() const override;
#if SYNTHLIB_DEMO_MODE == 0
		bool getState(std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		bool setState(const std::vector<uint8_t>& _state, synthLib::StateType _type) override;
#endif
		uint32_t getChannelCountIn() override;
		uint32_t getChannelCountOut() override;
		bool setDspClockPercent(uint32_t _percent) override;
		uint32_t getDspClockPercent() const override;
		uint64_t getDspClockHz() const override;

		/* The firmware state, resolved exactly once at construction: the plugin
		 * does not retry, so the constructor asks once and never again. */
		const g2::FirmwareStatus& firmwareStatus() const noexcept { return m_firmwareStatus; }

		/* The 16-bit firmware version word this machine is taken to run,
		 * resolved once beside the FirmwareStatus and saved as state item 5.
		 * Zero when no firmware is present, which decideFirmwareVersion never
		 * reads: the absent row answers NoFirmware before the words are
		 * compared.
		 *
		 * It is the expected word and not one read out of the binary, and the
		 * constructor is why: it resolves the firmware without booting, so
		 * there is no machine to ask. resolveFirmwareState decides presence
		 * from the artifact directory alone and never opens the image. A user
		 * who supplies a firmware of another version is therefore recorded
		 * here as g_expectedFirmwareVersion, and the mismatch that comparison
		 * exists to catch is a mismatch between two saved states rather than
		 * between the state and the bytes on disk. Reading the real word
		 * requires the boot this constructor deliberately does not perform. */
		uint16_t firmwareVersionWord() const noexcept { return m_firmwareVersionWord; }

		/* ---------------------------------------------------------------
		 * The boot-on-restore sequence. Every step below runs on the calling
		 * thread, which is the boot thread, and the last of them publishes
		 * the whole booted machine to the audio thread with one release
		 * store.
		 *
		 * The steps are an order, not a list, and BootStep is what makes the
		 * order observable from outside without a second implementation of
		 * it. Steps 4 and 5 must not merge: step 4 runs the boot codec
		 * regime -- a Scheduler is born in it (scheduler.h, CodecRegime) and
		 * beginPlayPhase is what leaves it -- so the boot touches neither
		 * codec queue and cannot stall on a full sink. */
		enum class BootStep
		{
			Create,          // 1. Scheduler::create
			Reset,           // 2. Scheduler::reset -- it does not prime
			StateLoad,       // 3. Scheduler::stateLoad, only when restoring
			RunFrames,       // 4. the boot quanta, in the boot codec regime
			BeginPlayPhase,  // 5. Scheduler::beginPlayPhase
			Publish          // 6. the release store of true into m_ready
		};

		/* The observer seam: the boot thread's twin of ISchedulerDriver.
		 * Each notification is delivered after its step has completed and
		 * carries the Scheduler the step acted on, so an observer reads the
		 * state the step left behind (a null Scheduler means step 1 produced
		 * none).
		 *
		 * Read-only observation. An observer that drove the Scheduler would
		 * be a second boot thread, which is forbidden. */
		class IBootObserver
		{
		public:
			virtual ~IBootObserver() = default;
			virtual void onBootStep(BootStep _step, Scheduler* _scheduler) noexcept = 0;
		};

		/* What the boot was asked for.
		 *
		 * `machineSnapshot` null or empty means a cold boot and step 3 does
		 * not run. Non-empty means restoring, and the bytes are the flat
		 * block Scheduler::stateSave wrote (scheduler.h's state trio). They
		 * are not the plugin state image: that format carries patch and
		 * performance data and no machine snapshot, so a snapshot travels
		 * only within one session, through machineSnapshot() below.
		 *
		 * `frameBudget` bounds step 4. The boot ends early on the machine's
		 * own signal -- Scheduler::chainAttached(), which turns true on the
		 * first quantum after the DSP programs land -- and the budget is
		 * the ceiling that keeps a firmware which never gets there from
		 * running forever. */
		struct BootRequest
		{
			Scheduler::Config           config{};
			const std::vector<uint8_t>* machineSnapshot = nullptr;
			uint64_t                    frameBudget     = 500000;
		};

		/* What the boot did. Every field is a measurement rather than a
		 * claim: a caller that wants to know whether the machine actually
		 * booted reads `chainAttached`, not `booted`. */
		struct BootResult
		{
			bool     booted        = false;  // steps 1..6 all ran
			bool     stateLoaded   = false;  // step 3 ran and was accepted
			bool     chainAttached = false;  // the DSP programs landed
			bool     faulted       = false;

			uint64_t framesRun     = 0;
			g2::Status status       = g2::Status::Unset;
			std::string why;                 // empty on success
		};

		/* The entry point. It constructs the Board, loads the firmware
		 * image, and runs the boot steps in order.
		 *
		 * Not called from the constructor, and that is deliberate: the boot
		 * belongs to setStateInformation and prepareToPlay, both
		 * message-thread moments, and a constructor that booted would make
		 * every Device construction run a firmware boot. */
		BootResult boot(const BootRequest& _request);

		/* The machine snapshot this Device holds. getState refreshes it
		 * from Scheduler::stateSave while the audio thread is held off, and
		 * boot() takes it back through BootRequest::machineSnapshot. */
		const std::vector<uint8_t>& machineSnapshot() const noexcept { return m_machineSnapshot; }

		void installBootObserver(IBootObserver* _observer) noexcept { m_bootObserver = _observer; }

		/* The scheduler driver, and the seam it provides. The audio thread's
		 * half of the Scheduler surface is push, runFrames, pull, and a read
		 * of faulted(), and this nested interface is that contract and
		 * nothing more. The production driver forwards each call to the real
		 * Scheduler; a harness driver records them. The call order is what
		 * fixes both codec queue capacities at L + B, so the contract is
		 * spelled at one place and every caller drives it through here.
		 * Public so a harness can implement it. */
		class ISchedulerDriver
		{
		public:
			virtual ~ISchedulerDriver() = default;
			virtual size_t push(const g2::Frame* _in, size_t _frames) noexcept = 0;
			virtual void runFrames(size_t _frames) noexcept = 0;
			virtual size_t pull(g2::Frame* _out, size_t _frames) noexcept = 0;
			virtual bool faulted() const noexcept = 0;
		};

		/* The production driver. Forwards to the Scheduler the boot thread
		 * installed; a null m_scheduler makes every call inert, which is the
		 * pre-boot state and unreachable on this path because the ready
		 * branch reads m_ready only after the boot thread has stored the
		 * pointer and released. */
		class SchedulerDriver final : public ISchedulerDriver
		{
		public:
			explicit SchedulerDriver(Scheduler* _scheduler) noexcept : m_scheduler(_scheduler) {}

			size_t push(const g2::Frame* _in, const size_t _frames) noexcept override
			{
				return m_scheduler ? m_scheduler->push(_in, _frames) : 0;
			}
			void runFrames(const size_t _frames) noexcept override
			{
				if(m_scheduler) m_scheduler->runFrames(_frames);
			}
			size_t pull(g2::Frame* _out, const size_t _frames) noexcept override
			{
				return m_scheduler ? m_scheduler->pull(_out, _frames) : 0;
			}
			bool faulted() const noexcept override
			{
				return m_scheduler && m_scheduler->faulted();
			}

		private:
			Scheduler* m_scheduler;
		};

		/* The owning driver. The Device owns exactly one; the constructor
		 * points m_driver at it, and a harness that replaces m_driver
		 * through installDriver() leaves this one alive underneath. */
		SchedulerDriver m_owningDriver{nullptr};

	protected:
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut) override;
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples) override;
		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override;

		/* The boot thread creates the Scheduler and installs it here --
		 * construct, reset, boot runFrames, beginPlayPhase -- and its final
		 * act is the release store of true into m_ready. Until that
		 * installation the pointer is null, and processAudio's ready branch
		 * cannot run, because m_ready is false until the same thread stores
		 * it after the installation.
		 *
		 * Borrowed, not owned: the Scheduler borrows its Board and the
		 * Board outlives it (scheduler.h), and the Device owns the pair. */
		void installScheduler(Scheduler* _scheduler) noexcept
		{
			m_scheduler = _scheduler;
		}

		/* The message thread's half of the hand-off, shared by getState and
		 * setState. beginStateChange() stores m_ready false with seq_cst and
		 * spins until the audio callback is observed clear; the caller then
		 * touches the Scheduler and finishes with endStateChange().
		 *
		 * The wait is unbounded by design: a callback that never returns is a
		 * host already broken in a way a timeout would convert from a hang
		 * into silent state corruption. A debug build asserts a generous
		 * bound so the condition is loud in development. */
		void beginStateChange() noexcept;
		void endStateChange() noexcept;

		/* The test seam. A harness subclass replaces the driver to observe
		 * the ready branch's call order without a real booted machine. The
		 * hook is protected, so only this class and its subclasses reach it,
		 * and the production value is the owning SchedulerDriver over
		 * m_scheduler. */
		void installDriver(ISchedulerDriver* _driver) noexcept { m_driver = _driver; }
		ISchedulerDriver* driver() const noexcept { return m_driver; }

		/* The booted machine, for evidence and for nothing else. Protected,
		 * so the evidence path is a subclass and not the whole plugin:
		 * nothing outside this hierarchy may reach the Board. Null until
		 * boot() has built one. */
		Board* board() const noexcept { return m_board.get(); }

		uint32_t m_numSamplesProcessed = 0;

		/* The byte stream the emulated UART0 transmitter originates. Uart0
		 * delivers each byte the firmware writes to UTB through the
		 * MidiOutFn callback installed on it (g2Lib/uart0.h), and the static
		 * sink below feeds that stream into this parser; readMidiOut drains
		 * the parser's completed events into _midiOut. */
		std::vector<uint8_t> m_midiOutBuffer;
		synthLib::MidiBufferParser m_midiOutParser{synthLib::MidiEventSource::Device};

		/* The staged queue. sendMidi only stamps and enqueues; the converted
		 * events wait here and processAudio delivers them, at the top of the
		 * callback and before runFrames, which is what makes the offset
		 * sample-accurate against the machine's own frame index.
		 * Single-producer single-consumer on the audio thread itself -- the
		 * framework calls sendMidi on the same thread that will run
		 * processAudio (synthLib/device.cpp), so no lock is involved. */
		std::vector<synthLib::SMidiEvent> m_pendingMidi;

		/* The Uart0 MidiOutFn sink. Static because MidiOutFn is a plain
		 * function pointer; _user carries the Device. */
		static void uart0MidiOut(void* _user, uint8_t _byte);

	private:
		/* The machine this Device owns. The declaration order is the
		 * destruction order reversed, which is the whole reason for this
		 * sequence: the Scheduler borrows the Board's DSP set and installs
		 * chain callbacks into ESAIs the Board owns, and the Board reads its
		 * firmware out of the SDRAM store, so the Scheduler must die first
		 * and the store last. Members destruct in reverse declaration order,
		 * so the order below is: Scheduler, Executor, Board, SDRAM.
		 *
		 * Sdram is defined in the .cpp: the Board leaves Region::Sdram with
		 * no target on purpose (board.cpp) and the store is the composing
		 * party's to supply. */
		class Sdram;

		std::unique_ptr<Sdram>          m_sdram;
		std::unique_ptr<Board>          m_board;
		std::unique_ptr<SerialExecutor> m_executor;
		std::unique_ptr<Scheduler>      m_ownedScheduler;

		/* The flat block Scheduler::stateSave writes. Refreshed by getState,
		 * consumed by boot()'s step 3. */
		std::vector<uint8_t> m_machineSnapshot;

		IBootObserver* m_bootObserver = nullptr;

		void notifyBootStep(BootStep _step, Scheduler* _scheduler) noexcept;

		/* The boot thread installs it, the audio thread drives it, and the
		 * hand-off pairing (below) is what makes the transfer total: the
		 * pointer is written only while m_ready is false and no callback is
		 * in flight. */
		Scheduler* m_scheduler = nullptr;

		/* The active driver. It is the SchedulerDriver over m_scheduler in
		 * production; a harness subclass may replace it through
		 * installDriver(). */
		ISchedulerDriver* m_driver = nullptr;

		/* The two hand-off flags. Neither can do the other's job.
		 * m_ready publishes the booted machine from the boot thread to the
		 * audio thread; m_inCallback is the acknowledgement the reverse
		 * direction needs, because a release store on m_ready cannot suspend a
		 * processAudio that is already in progress. */
		std::atomic<bool> m_ready{false};
		std::atomic<bool> m_inCallback{false};

		FirmwareStatus m_firmwareStatus;

		/* State item 5. The expected firmware version word, resolved once at
		 * construction; zero when the firmware is absent. firmwareVersionWord()
		 * above says why it is the expected word and not a read one. */
		uint16_t m_firmwareVersionWord = 0;

		/* The plugin state items, held between serializeState and
		 * deserializeState. */
		g2::StateData m_stateData;

		/* The type pin, instantiated once from g2Device.cpp. The pinned
		 * members are protected and private by design, so the pin lives
		 * where the access exists. */
		struct MemberPin;
	};
}
