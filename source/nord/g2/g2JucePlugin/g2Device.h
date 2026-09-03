/* g2Device.h -- the synthLib::Device subclass. Task PLG-1, which also
 * absorbed PLG-3 (plan section 17, step 2; §24.6 row W3-390).
 *
 * Design sections 13.10 rule 3, 14.7 and 26.
 *
 * WHAT THIS CLASS IS. The whole of the design's contact with the synthLib
 * framework is this subclass. It presents the twelve pure virtuals
 * synthLib::Device declares, at device.h lines 54, 70, 73, 74, 78, 79, 81,
 * 82, 83, 91, 92 and 93, and it holds the two hand-off flags of design
 * section 13.10 rule 3.
 *
 * THE TWO CONDITIONAL VIRTUALS. device.h:72 opens #if SYNTHLIB_DEMO_MODE == 0
 * and the guard wraps three declarations -- getState, setState and
 * setStateFromUnknownCustomData -- of which only the first two are pure
 * virtuals. Under SYNTHLIB_DEMO_MODE the count is ten, and a subclass that
 * marks those two `override` unconditionally will not compile. The two
 * overrides below carry the same guard.
 *
 * THREE OF THE TWELVE ARE PROTECTED: readMidiOut, processAudio and sendMidi.
 * Nothing outside the class hierarchy calls them; a test harness that wants
 * to call them goes through a subclass of this one, which is what
 * t0_handoff_flags does.
 *
 * THE OWNERS OF WHAT IS NOT HERE YET, so that nobody reads a stub body as a
 * finished behaviour. PLG-2 has landed: the channel counts are the final 2
 * and 2, final the instant the constructor returns, pinned by
 * t0_channel_counts. PLG-4 HAS LANDED (merged with PLG-6, §24.6 row W3-390):
 * processAudio's full choreography and sendMidi's offset conversion are in,
 * pinned by t0_process_audio and t0_midi_offsets. PLG-5 HAS LANDED:
 * the seven-item state format of design section 15.5 lives in g2State.{h,cpp}
 * and the two overrides delegate to it (see that header for the format and
 * the BRD-11 mismatch policy); PLG-12 owns the boot thread, the Scheduler
 * construction, and every OTHER Scheduler-touching line -- this class borrows
 * the Scheduler through m_scheduler and drives only push, runFrames, pull,
 * queueMidi and faulted, the audio thread's rows of docs/threading.md's map.
 * PLG-1's own
 * surface is the flags, isValid(), getSamplerate(), m_numSamplesProcessed,
 * the firmware wiring, and the two hand-off sequences, which the later bodies
 * are required to reuse rather than restate. PLG-7 has since landed
 * readMidiOut (see the m_midiOutParser block below); its Board half waits on
 * PLG-12.
 *
 * THE HAND-OFF PAIRING, STATED HERE BECAUSE THE MEMORY ORDERS ARE THE
 * DELIVERABLE AND NOT A COMMENT (plan section 17, step 2):
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
 * FOUR of those operations must be memory_order_seq_cst, because each thread
 * stores one atomic and then loads the OTHER: an acquire load does not order
 * a store-load pair, and without seq_cst both threads can observe the other's
 * pre-store value and both proceed -- exactly the interleaving the pairing
 * exists to exclude. The two closing false stores stay release. The wait is
 * unbounded by design; a debug build asserts a generous bound.
 *
 * wLib::Device was considered as a base class and rejected. It would supply
 * m_numSamplesProcessed for free, but wLib/wDevice.h:22 declares a pure
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

		/* DECLARED HERE AND DEFINED IN THE .cpp because m_sdram holds an
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

		/* The firmware state BRD-10's surface reports, resolved EXACTLY ONCE at
		 * construction -- requirement 4 of design section 7.7 is that the plugin
		 * does not retry, so the constructor asks once and never again. */
		const g2::FirmwareStatus& firmwareStatus() const noexcept { return m_firmwareStatus; }

		/* The 16-bit firmware version word this machine is taken to run,
		 * resolved once beside the FirmwareStatus and saved as state item 5.
		 * Zero when no firmware is present, which decideFirmwareVersion never
		 * reads: the absent row answers NoFirmware before the words are
		 * compared.
		 *
		 * IT IS THE EXPECTED WORD AND NOT ONE READ OUT OF THE BINARY, and the
		 * constructor is why: it resolves the firmware without booting, so
		 * there is no machine to ask. resolveFirmwareState decides presence
		 * from the artifact directory alone and never opens the image. A user
		 * who supplies a firmware of another version is therefore recorded
		 * here as g_expectedFirmwareVersion, and the mismatch that comparison
		 * exists to catch is a mismatch between two SAVED states rather than
		 * between the state and the bytes on disk. Reading the real word
		 * requires the boot this constructor deliberately does not perform. */
		uint16_t firmwareVersionWord() const noexcept { return m_firmwareVersionWord; }

		/* ---------------------------------------------------------------
		 * PLG-12. THE BOOT-ON-RESTORE SEQUENCE. Design section 15.6, plan
		 * block PLG-12. Every step below runs on the CALLING thread, which
		 * is the boot thread, and the last of them publishes the whole
		 * booted machine to the audio thread with one release store.
		 *
		 * THE SIX STEPS ARE AN ORDER AND NOT A LIST, and BootStep is what
		 * makes the order observable from outside without a second
		 * implementation of it. Steps 4 and 5 are the two that must not
		 * merge: step 4 runs the BOOT codec regime -- a Scheduler is born
		 * in it (scheduler.h, CodecRegime) and beginPlayPhase is the only
		 * thing that leaves it -- so the boot touches neither codec queue
		 * and cannot stall on a full sink. */
		enum class BootStep
		{
			Create,          // 1. Scheduler::create
			Reset,           // 2. Scheduler::reset -- it does NOT prime
			StateLoad,       // 3. Scheduler::stateLoad, only when restoring
			RunFrames,       // 4. the boot quanta, in the boot codec regime
			BeginPlayPhase,  // 5. Scheduler::beginPlayPhase
			Publish          // 6. the release store of true into m_ready
		};

		/* THE OBSERVER SEAM, and it is the boot-thread twin of
		 * ISchedulerDriver: that interface makes the AUDIO thread's four
		 * calls observable, this one makes the BOOT thread's six steps
		 * observable. Each notification is delivered AFTER its step has
		 * completed and carries the Scheduler the step acted on, so an
		 * observer reads the state the step left behind (a null Scheduler
		 * means step 1 produced none).
		 *
		 * Read-only observation. An observer that drove the Scheduler would
		 * be a second boot thread, which design section 15.6's first safety
		 * statement forbids. */
		class IBootObserver
		{
		public:
			virtual ~IBootObserver() = default;
			virtual void onBootStep(BootStep _step, Scheduler* _scheduler) noexcept = 0;
		};

		/* WHAT THE BOOT WAS ASKED FOR.
		 *
		 * `machineSnapshot` NULL OR EMPTY MEANS A COLD BOOT and step 3 does
		 * not run. Non-empty means RESTORING, and the bytes are the flat
		 * block Scheduler::stateSave wrote (scheduler.h's state trio). They
		 * are NOT design section 15.5's seven-item image: that format
		 * carries patch and performance data and no machine snapshot, so a
		 * snapshot travels only within one session, through
		 * machineSnapshot() below. See the boot() comment in the .cpp for
		 * what that leaves undone and who owns it.
		 *
		 * `frameBudget` bounds step 4. The boot ends EARLY on the machine's
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

		/* WHAT THE BOOT DID, and every field is a measurement rather than a
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

		/* THE ENTRY POINT. It constructs the Board, loads the firmware
		 * image, and runs design section 15.6's six steps in order.
		 *
		 * NOT CALLED FROM THE CONSTRUCTOR, and that is deliberate: design
		 * section 15.6 boots during setStateInformation and prepareToPlay,
		 * both message-thread moments, and a constructor that booted would
		 * make every Device construction -- including the ungated T0 tests
		 * that construct one to read its surface -- run a firmware boot. */
		BootResult boot(const BootRequest& _request);

		/* THE MACHINE SNAPSHOT THIS DEVICE HOLDS. getState refreshes it
		 * from Scheduler::stateSave while the audio thread is held off, and
		 * boot() takes it back through BootRequest::machineSnapshot. */
		const std::vector<uint8_t>& machineSnapshot() const noexcept { return m_machineSnapshot; }

		void installBootObserver(IBootObserver* _observer) noexcept { m_bootObserver = _observer; }

		/* THE SCHEDULER DRIVER, AND THE SEAM IT PROVIDES. The audio thread's
		 * half of the Scheduler surface is exactly four calls -- push,
		 * runFrames, pull, and a read of faulted() -- and this nested
		 * interface is that contract, and nothing more. The production
		 * driver forwards each call to the real Scheduler PLG-12 installs;
		 * the harness driver records them. THE CALL ORDER IS THE
		 * DELIVERABLE of PLG-4 step 1 (it is what fixes both codec queue
		 * capacities at L + B, design section 13.6), so the contract is
		 * spelled at one place and every caller drives it through here.
		 * Public: a harness implements it, and the test's access to it is
		 * read-only observation of the order. */
		class ISchedulerDriver
		{
		public:
			virtual ~ISchedulerDriver() = default;
			virtual size_t push(const g2::Frame* _in, size_t _frames) noexcept = 0;
			virtual void runFrames(size_t _frames) noexcept = 0;
			virtual size_t pull(g2::Frame* _out, size_t _frames) noexcept = 0;
			virtual bool faulted() const noexcept = 0;
		};

		/* THE PRODUCTION DRIVER. Forwards to the Scheduler the boot thread
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

		/* THE OWNING DRIVER. The Device owns exactly one; the constructor
		 * points m_driver at it, and a harness that replaces m_driver
		 * through installDriver() leaves this one alive underneath. */
		SchedulerDriver m_owningDriver{nullptr};

	protected:
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut) override;
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples) override;
		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override;

		/* PLG-12's boot thread creates the Scheduler and installs it here --
		 * construct, reset, boot runFrames, beginPlayPhase -- and its final
		 * act is the release store of true into m_ready (design section
		 * 15.6's six steps). Until that installation the pointer is null,
		 * and processAudio's ready branch cannot run, because m_ready is
		 * false until the same thread stores it AFTER the installation.
		 *
		 * Borrowed, not owned: the Scheduler borrows its Board and the
		 * Board outlives it (scheduler.h), and the Device owns the pair.
		 * PLG-12 owns the construction and the teardown. */
		void installScheduler(Scheduler* _scheduler) noexcept
		{
			m_scheduler = _scheduler;
		}

		/* THE MESSAGE THREAD'S HALF OF THE HAND-OFF, shared by getState and
		 * setState (PLG-5 builds on it). beginStateChange() stores m_ready
		 * false with seq_cst and spins until the audio callback is observed
		 * clear; the caller then touches the Scheduler and finishes with
		 * endStateChange().
		 *
		 * THE WAIT IS UNBOUNDED BY DESIGN: a callback that never returns is a
		 * host already broken in a way a timeout would convert from a hang
		 * into silent state corruption. A debug build asserts a generous
		 * bound so the condition is loud in development. */
		void beginStateChange() noexcept;
		void endStateChange() noexcept;

		/* THE TEST SEAM. PLG-4's t0_process_audio must observe the ready
		 * branch's call ORDER without a real booted machine, so the harness
		 * subclass replaces the driver. The hook is protected, only this
		 * class and its subclasses reach it, and the production value is
		 * always the owning SchedulerDriver over m_scheduler. */
		void installDriver(ISchedulerDriver* _driver) noexcept { m_driver = _driver; }
		ISchedulerDriver* driver() const noexcept { return m_driver; }

		/* PLG-12. THE BOOTED MACHINE, FOR EVIDENCE AND FOR NOTHING ELSE.
		 * t1_boot_on_restore reads the display cells through Board::onRead
		 * to show that the machine this Device booted composed a banner --
		 * a claim that cannot be made from the Scheduler's counters alone.
		 * PROTECTED, so the evidence path is a subclass and not the whole
		 * plugin: nothing outside this hierarchy may reach the Board.
		 * NULL until boot() has built one. */
		Board* board() const noexcept { return m_board.get(); }

		uint32_t m_numSamplesProcessed = 0;

		/* PLG-7. The byte stream the emulated UART0 transmitter originates.
		 * Uart0 delivers each byte the firmware writes to UTB through the
		 * MidiOutFn callback installed on it (g2Lib/uart0.h), and the static
		 * sink below feeds that stream into this parser; readMidiOut drains
		 * the parser's completed events into _midiOut. The Board that owns
		 * the Uart0 arrives with PLG-12's boot; until then the parser never
		 * receives a byte and readMidiOut appends nothing. */
		std::vector<uint8_t> m_midiOutBuffer;
		synthLib::MidiBufferParser m_midiOutParser{synthLib::MidiEventSource::Device};

		/* PLG-4 step 2 (with PLG-6's conversion). THE STAGED QUEUE.
		 * sendMidi only stamps and enqueues (design section 17 row 7.31a);
		 * the converted events wait here and processAudio delivers them, at
		 * the top of the callback and before runFrames, which is what makes
		 * the offset sample-accurate against the machine's own frame index.
		 * Single-producer single-consumer on the audio thread itself -- the
		 * framework calls sendMidi on the same thread that will run
		 * processAudio (synthLib/device.cpp:35-50), so no lock is involved.
		 *
		 * THE DELIVERABLE TARGET: design row 7.31 says the result lands in
		 * Scheduler::queueMidi(uint64_t frameIndex, ...). queueMidi is part
		 * of the Scheduler's FINISHED surface (design section 13.10.5;
		 * W3-415 records it as declared nowhere in g2Lib yet), so this file
		 * stages into m_pendingMidi and the delivery site -- PLG-12's boot
		 * wiring, which owns every other Scheduler-touching line -- calls
		 * queueMidi through it when it lands. The staging is the device's
		 * half of the contract: the conversion arithmetic, the ordering, and
		 * the no-double-delivery property are all observable here. */
		std::vector<synthLib::SMidiEvent> m_pendingMidi;

		/* The Uart0 MidiOutFn sink. Static because MidiOutFn is a plain
		 * function pointer; _user carries the Device. */
		static void uart0MidiOut(void* _user, uint8_t _byte);

	private:
		/* PLG-12. THE MACHINE THIS DEVICE OWNS, AND THE DECLARATION ORDER IS
		 * THE DESTRUCTION ORDER REVERSED, which is the whole reason these
		 * four are here in this sequence. The Scheduler borrows the Board's
		 * DSP set and installs chain callbacks into ESAIs the Board owns,
		 * and the Board reads its firmware out of the SDRAM store, so the
		 * Scheduler must die first and the store last. Members destruct in
		 * reverse declaration order, so the order below is: Scheduler,
		 * Executor, Board, SDRAM.
		 *
		 * Sdram is defined in the .cpp: the Board leaves Region::Sdram with
		 * no target on purpose (board.cpp, plan §24.6 row W3-115) and the
		 * store is the composing party's to supply. */
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

		/* PLG-12 installs it, the audio thread drives it, and the hand-off
		 * pairing (below) is what makes the transfer total: the pointer is
		 * written only while m_ready is false and no callback is in flight. */
		Scheduler* m_scheduler = nullptr;

		/* The active driver. It is the SchedulerDriver over m_scheduler in
		 * production; a harness subclass may replace it through
		 * installDriver(). */
		ISchedulerDriver* m_driver = nullptr;

		/* The two hand-off flags. NEITHER CAN DO THE OTHER'S JOB.
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

		/* PLG-5. The seven-item state of design section 15.5, held between
		 * serializeState and deserializeState. The emulated machine arrives
		 * with PLG-12's boot; until then these are the empty and zero values
		 * the format round-trips exactly. */
		g2::StateData m_stateData;

		/* The type pin of the three members the plan requires, instantiated
		 * once from g2Device.cpp. The members are protected and private by
		 * design, so the pin lives where the access exists. */
		struct MemberPin;
	};
}
