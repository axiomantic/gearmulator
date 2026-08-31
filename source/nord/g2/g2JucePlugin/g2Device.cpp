/* g2Device.cpp -- the synthLib::Device subclass body. Task PLG-1, which also
 * absorbed PLG-3 (plan section 17, steps 1 and 2; §24.6 row W3-390).
 *
 * Design sections 13.10 rule 3, 14.7 and 26.
 *
 * THE WIRING THIS FILE CARRIES FOR BRD-10. Plan section 17 step 1: "This task
 * also wires BRD-10's g2Lib/firmwareState.h into g2Device.cpp", because
 * section 7.4.2 gives g2JucePlugin/ to the plugin track and BRD-10 no longer
 * writes it. The constructor resolves the firmware state ONCE through
 * ArtifactResolver and never again: design section 7.7 requirement 4 says the
 * plugin does not look again on its own.
 *
 * THE STUB BODIES BELOW ARE NOT FINISHED BEHAVIOURS. Each carries the plan
 * block that owns its real body:
 *
 *   getChannelCountIn / getChannelCountOut -- PLG-2 has landed; the bodies
 *       below are final and t0_channel_counts pins them.
 *   processAudio -- PLG-4. The choreography (set m_inCallback, read
 *       isValid(), on false release-store false, zero the outputs and touch
 *       nothing) is STEP 2's and stays; the Scheduler half of the ready
 *       branch is PLG-4's.
 *   sendMidi -- PLG-4 step 2 (with PLG-6's offset conversion).
 *   readMidiOut -- PLG-7's body is in; see the function's own comment.
 *   getState / setState -- PLG-5 has landed. The bodies delegate to
 *       g2State.h's serializeState/deserializeState, which hold the
 *       seven-item format of design section 15.5 and the BRD-11 mismatch
 *       policy. The ONE property design section 17 row 7.29 declares holds
 *       through the delegate: every write into _state is an append, never
 *       an assign, so the version header the Plugin layer prepended
 *       survives. The hand-off pair runs first in both, as PLG-1 built it.
 *
 * The Scheduler, the Board and the boot thread are PLG-12's; no Scheduler
 * object exists yet, so every "touch the Scheduler" site below is the exact
 * line PLG-4 and PLG-12 replace and the flags around it are what this task
 * delivers.
 */

#include "g2Device.h"

#include "g2State.h"

#include "g2/timebase.h"

#include "memoryMap.h"
#include "sim.h"
#include "status.h"

#include "dsp56kEmu/audio.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <type_traits>

namespace
{
	/* ------------------------------------------------------------------
	 * PLG-12. THE BOOT COMPOSITION.
	 *
	 * EVERY VALUE BELOW IS COPIED FROM `g2Lib/test/t1_boot.cpp` AND
	 * `g2TestConsole/main.cpp`, WHICH ARE ITS TWO AUTHORITIES, and each
	 * one carries there the measurement that establishes it. This file is
	 * the THIRD site, which is a duplication and is recorded as one: the
	 * composition has no home in g2Lib -- board.cpp attaches the seven
	 * units plan §24.6 row W3-115 names and leaves Region::Sdram, MBAR and
	 * the chip-select windows to the composing party -- and PLG-12's
	 * `Files:` line names neither board.h nor a new g2Lib header, so this
	 * task may not create that home. The duplication is the plan's, not a
	 * choice made here.
	 *
	 * TWO OF THE VALUES ARE NOT MEASUREMENTS AND ARE LABELLED AS SUCH AT
	 * THEIR SITE, exactly as both authorities label them: CS0's and CS4's
	 * bases are recorded by no authority. */

	// MEASURED, plan section 6.6.3: the boot loader's `movel #0x10000001,%d0`
	// / `movec %d0,%mbar` at loader offset 0x1E. The OS image never writes
	// MBAR, so a direct boot of that image makes this the composer's job.
	constexpr uint32_t g_mbarBase = 0x10000000u;

	// MEASURED, plan section 6.6.9, from the loader's CSAR2 = $1200 and
	// CSMR2 = $007F0001 at loader offsets 0x70 and 0x7c.
	constexpr uint32_t g_cs2Base = 0x12000000u;
	constexpr uint32_t g_cs2Size = 0x00800000u;

	// INVENTED BY THE HARNESSES AND COPIED HERE UNCHANGED. No authority
	// records CS0's or CS4's base; plan section 4.2 register row 18 is still
	// open on both. Neither is a measurement.
	constexpr uint32_t g_cs0Base = 0x00000000u;
	constexpr uint32_t g_cs0Size = 0x00020000u;
	constexpr uint32_t g_cs4Base = 0x14000000u;
	constexpr uint32_t g_cs4Size = 0x00010000u;

	// MEASURED, workspace logbook section 3.8: CS3 is a 64 KiB window.
	constexpr uint32_t g_cs3Size   = 0x00010000u;
	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;
	constexpr uint32_t g_sdramSize = 0x00800000u;

	// The image loads where its name says it loads, and the initial stack
	// pointer is the one both authorities reset the core with.
	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr const char* g_codeImageName = "CODE_30000400.bin";

	// TASK INT-8's vector table: 256 identical big-endian longwords at the
	// SDRAM base, with VBR pointed at it. Booting CODE directly skips the
	// code that would build it. Register index 18 is the mcf5307 C ABI's VBR.
	constexpr int      g_regVbr             = 18;
	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	/* THE STEP-4 CHUNK. runFrames takes a frame count and the boot's exit
	 * condition -- Scheduler::chainAttached() -- is a property with a
	 * moment, so the budget is spent in chunks and the condition is read
	 * between them. One frame per chunk is what both authorities drive; a
	 * larger chunk only coarsens where the boot is observed to have
	 * finished, and 64 keeps the read off the inner loop without moving the
	 * emulated result, because runFrames(64) runs the same 64 quanta in the
	 * same order as 64 calls of runFrames(1). */
	constexpr size_t g_bootChunkFrames = 64;

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);

		if(!in)
			return {};

		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
}

namespace g2
{
	/* THE SDRAM THE FIRMWARE EXECUTES FROM. board.cpp leaves Region::Sdram
	 * with no target on purpose (plan §24.6 row W3-115), so the store is the
	 * composing party's to supply. Big-endian, matching the part.
	 *
	 * IT IS A NESTED CLASS OF Device AND NOT A FILE-LOCAL ONE, because
	 * m_sdram is a member and a member's type must be nameable in the
	 * header. The header declares it and this is its definition. */
	class Device::Sdram final : public BusTarget
	{
	public:
		explicit Sdram(const size_t _size) : m_bytes(_size, 0u) {}

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(_offset > m_bytes.size() || _image.size() > m_bytes.size() - _offset)
				return false;

			std::copy(_image.begin(), _image.end(), m_bytes.begin() + _offset);
			return true;
		}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			/* A BusTarget's `_size` IS A WIDTH IN BITS AND NOT A COUNT OF
			 * BYTES. Board::onRead converts the core's byte count through
			 * busWidthBits() before the access reaches a target
			 * (board.cpp), so the three legal widths here are 8, 16 and 32 --
			 * the same three both harness stores accept. */
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			/* AN ACCESS PAST THE END READS ZERO AND REPORTS NO BUS ERROR, which
			 * is what all three harness stores do (t1_boot.cpp,
			 * t1_chain_order.cpp, g2TestConsole/main.cpp) and it is not a
			 * convenience. The window this store answers is the one the
			 * BoardConfig above declares, so an offset past its end has already
			 * been let through by the decode; reporting a fault here makes the
			 * core TRAP, which sets Board::faulted(), which the Scheduler
			 * reports and which stops the boot dead. MEASURED: with a bus error
			 * on this path the boot faulted inside its first 64 quanta and
			 * reached nothing. */
			const uint32_t bytes = uint32_t(_size) / 8u;

			uint32_t value = 0;

			for(uint32_t i = 0; i < bytes; ++i)
			{
				const size_t index = size_t(_offset) + i;
				value = (value << 8) | (index < m_bytes.size() ? m_bytes[index] : 0u);
			}

			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status) override
		{
			_status = MCF5307_BUS_OK;

			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			// A write past the end is DISCARDED and reports no bus error, for the
			// reason the read path states.
			const uint32_t bytes = uint32_t(_size) / 8u;

			for(uint32_t i = 0; i < bytes; ++i)
			{
				const size_t index = size_t(_offset) + i;

				if(index >= m_bytes.size())
					continue;

				m_bytes[index] = uint8_t((_value >> ((bytes - 1u - i) * 8u)) & 0xffu);
			}
		}

	private:
		std::vector<uint8_t> m_bytes;
	};

	Device::Device(const synthLib::DeviceCreateParams& _params)
		: synthLib::Device(_params)
		, m_driver(&m_owningDriver)
	{
		// BRD-10 wiring: ask the resolver ONCE, at construction, and never
		// again. The no-exceptions rule of design sections 5.3 and 13.10 holds
		// inside resolveFirmwareState itself.
		EnvArtifactResolver resolver;
		m_firmwareStatus = resolveFirmwareState(resolver);
		m_firmwareVersionWord = m_firmwareStatus.state == FirmwareState::Present
			? g_expectedFirmwareVersion
			: 0;

		// The four slots exist before the machine does: design section 15.5
		// item 1 saves all four slots, so the state this device saves carries
		// four of them from the first revision, empty and zero until PLG-12's
		// boot fills them.
		m_stateData.slotPatches.resize(g_stateSlotCount);
		m_stateData.slotPatchIds.resize(g_stateSlotCount);
	}

	/* PLG-12 OWNS THE TEARDOWN, as the header's installScheduler comment
	 * states. The unique_ptrs do it in the one order that is safe, which is
	 * why they are declared in the order they are; this destructor exists
	 * because Sdram is incomplete in the header and for no other reason. */
	Device::~Device() = default;

	/* ------------------------------------------------------------------
	 * PLG-12. THE BOOT-ON-RESTORE SEQUENCE. Design section 15.6; plan block
	 * PLG-12's ordered table.
	 *
	 * THE SIX STEPS BELOW ARE NUMBERED IN THE CODE and each notifies the
	 * observer AFTER it has completed. The order is the deliverable: it is
	 * what section 15.6's five safety statements are written about, and
	 * t1_boot_on_restore holds it.
	 *
	 * STEPS 4 AND 5 ARE THE TWO THAT MUST NOT MERGE. Step 4 runs in the
	 * BOOT codec regime, which is the regime a Scheduler is BORN in
	 * (scheduler.h's CodecRegime) and which beginPlayPhase is the only exit
	 * from -- so this function selects no regime and must not: it merely
	 * refrains from calling beginPlayPhase until step 5. Merge the two and
	 * step 4 fills the sink, push refuses, and the boot stalls with no
	 * callback that could ever drain it.
	 *
	 * WHAT STEP 3 CAN AND CANNOT DO TODAY, STATED RATHER THAN IMPLIED.
	 * Scheduler::stateLoad takes the FLAT MACHINE SNAPSHOT the Scheduler's
	 * own stateSave writes. Design section 15.5's seven-item image, which
	 * PLG-5 owns, carries patch and performance data and NO machine
	 * snapshot, so a snapshot survives a getState/boot pair within one
	 * session (machineSnapshot(), refreshed in getState) and does NOT
	 * survive a save to a host project file. Making it survive means adding
	 * an item to the section 15.5 format, which is g2State.{h,cpp} and
	 * PLG-5's file, not this task's. THAT WORK IS UNDONE AND THIS IS ITS
	 * ONLY RECORD.
	 *
	 * THE FRAMEWORK CALL SITE IS ALSO NOT HERE. Design section 15.6 boots
	 * during setStateInformation and prepareToPlay; prepareToPlay lives in
	 * g2Plugin.cpp, which is PLG-9's file and not on this task's Files:
	 * line. This function is the boot; wiring the host's two moments to it
	 * is OWED by the block that owns that file. */
	Device::BootResult Device::boot(const BootRequest& _request)
	{
		BootResult result;

		// The firmware image. The resolver is asked for the artifact
		// directory that actually holds the image, exactly as every gated
		// consumer asks, and a machine with no artifacts is a boot that
		// says why and changes nothing.
		EnvArtifactResolver resolver;
		std::string         why;

		const std::string directory = resolver.resolve(why, g_codeImageName);

		if(directory.empty())
		{
			result.why = why;
			return result;
		}

		const std::vector<uint8_t> code = readFile(directory + "/" + g_codeImageName);

		if(code.empty())
		{
			result.why = std::string(g_codeImageName) + " is empty or unreadable under " + directory;
			return result;
		}

		/* THE BOARD AND ITS STORE, BUILT BEFORE STEP 1, because
		 * Scheduler::create takes a Board reference and a Board with no
		 * firmware in it is a Scheduler that would boot nothing. Nothing
		 * here is one of the six steps and nothing here is notified as one.
		 *
		 * A SECOND boot() on the same Device REPLACES the machine, which is
		 * what a restore is: the Scheduler goes first, then the Board, then
		 * the store, and the release order is the destruction order the
		 * members already declare. m_scheduler is cleared through the same
		 * seam that installs it, so the audio thread's driver never holds a
		 * pointer to a destroyed object. */
		installScheduler(nullptr);
		m_ownedScheduler.reset();
		m_executor.reset();
		m_board.reset();
		m_sdram.reset();

		m_sdram = std::make_unique<Sdram>(g_sdramSize);

		if(!m_sdram->place(g_entryPc - g_sdramBase, code))
		{
			result.why = "the firmware image does not fit the configured SDRAM window";
			return result;
		}

		// TASK INT-8's vector table, big-endian, beside the image. It is a
		// FLOOR and not a fix: t1_boot measured that the firmware fills the
		// table itself before the first exception is taken, so what is
		// load-bearing here is VBR.
		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);

			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
			{
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] = uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			}

			if(!m_sdram->place(g_vectorTableBase - g_sdramBase, table))
			{
				result.why = "the vector table does not fit the configured SDRAM window";
				return result;
			}
		}

		BoardConfig boardConfig;

		boardConfig.memory.cs0   = {g_cs0Base,   g_cs0Size};
		boardConfig.memory.cs1   = {g_cs1Base,   g_cs1Size};
		boardConfig.memory.cs2   = {g_cs2Base,   g_cs2Size};
		boardConfig.memory.cs3   = {g_cs3Base,   g_cs3Size};
		boardConfig.memory.cs4   = {g_cs4Base,   g_cs4Size};
		boardConfig.memory.cs5   = {g_cs5Base,   g_cs5Size};
		boardConfig.memory.mbar  = {g_mbarBase,  g_simSpaceSize};
		boardConfig.memory.sdram = {g_sdramBase, g_sdramSize};

		m_board = std::make_unique<Board>(boardConfig);
		m_board->memory().attach(Region::Sdram, m_sdram.get());

		/* PLG-7's ONE INSTALLATION, PERFORMED HERE BECAUSE THIS IS WHERE
		 * THE BOARD EXISTS. readMidiOut's comment names this exact call: the
		 * Uart0 the Board owns delivers every byte the firmware writes to
		 * UTB into this class's static sink, which feeds m_midiOutParser.
		 * Until this line ran the parser received nothing, which is why an
		 * unbooted Device truthfully reports no MIDI out. */
		m_board->uart0().setMidiOut(&Device::uart0MidiOut, this);

		m_executor = std::make_unique<SerialExecutor>();

		// ---- STEP 1. Scheduler::create -- the object, or nullptr and a
		// g2::Status. There is no exception and no assertion: design section
		// 13.10 rule 2 forbids the first and a release build removes the
		// second, so the rejection is observable through the pair and
		// through nothing else.
		m_ownedScheduler = Scheduler::create(_request.config, *m_executor, *m_board, result.status);

		notifyBootStep(BootStep::Create, m_ownedScheduler.get());

		if(!m_ownedScheduler)
		{
			result.why = "Scheduler::create returned no object";
			return result;
		}

		Scheduler& scheduler = *m_ownedScheduler;

		// ---- STEP 2. Scheduler::reset -- every emulated memory zeroed,
		// every accumulator, debt and counter zeroed, the virtual clock at
		// frame 0, both codec queues EMPTY. IT DOES NOT PRIME: priming the
		// sink is step 5's job and doing it here would put L frames into a
		// queue the boot regime must leave untouched.
		scheduler.reset();

		/* THE POWER-ON ENTRY STATE, ESTABLISHED AFTER STEP 2 AND NOT BEFORE
		 * IT, and the order here is a MEASUREMENT and not a preference.
		 * Scheduler::reset returns the machine to the state a freshly created
		 * Scheduler is in, and that includes the Board's core: an entry point
		 * written before step 2 is erased by it and the core is left HALTED.
		 * MEASURED -- with resetMcu called before Scheduler::create, the boot
		 * ran 64 quanta and stopped with context 0 (the MCU) reporting
		 * JobFault::CoreHalted, and nothing else in the run said so.
		 *
		 * IT IS NOT ONE OF THE SIX STEPS and is not notified as one: design
		 * section 15.6's table names six calls, and inventing a seventh
		 * notification would make the order this task delivers unreadable.
		 *
		 * VBR IS WRITTEN AFTER THE CORE RESET, because the reset defines the
		 * starting state and a value written ahead of it would depend on what
		 * the reset does NOT clear. The return is checked: setMcuReg answers
		 * false for an index the core refuses, and a core that refused this
		 * one would leave the table based at zero with nothing said about it.
		 *
		 * THE SDRAM IS NOT RE-PLACED HERE. It is this object's own store and
		 * not one the Board owns, so Scheduler::reset does not reach it and
		 * the image placed above is still there. */
		m_board->resetMcu(g_entrySp, g_entryPc);

		if(!m_board->setMcuReg(g_regVbr, g_vectorTableBase))
		{
			result.why = "the core refused VBR";
			return result;
		}

		notifyBootStep(BootStep::Reset, &scheduler);

		// ---- STEP 3. Scheduler::stateLoad, ONLY IF RESTORING. A cold boot
		// does not run it at all, and the notification is not emitted for a
		// step that did not run: an observer must be able to tell a restore
		// from a cold boot, and a step notified either way could not.
		if(_request.machineSnapshot && !_request.machineSnapshot->empty())
		{
			if(_request.machineSnapshot->size() != scheduler.stateSize())
			{
				result.status = Status::BadStateImage;
				result.why    = "the machine snapshot is not the size this build's Scheduler writes";
				return result;
			}

			const Status loadStatus = scheduler.stateLoad(_request.machineSnapshot->data());

			if(loadStatus != Status::Ok)
			{
				result.status = loadStatus;
				result.why    = "Scheduler::stateLoad refused the machine snapshot";
				return result;
			}

			result.stateLoaded = true;

			notifyBootStep(BootStep::StateLoad, &scheduler);
		}

		/* ---- STEP 4. MANY runFrames CALLS -- the booted machine. These run
		 * the BOOT codec regime, so neither codec queue is touched and the
		 * boot cannot stall on a full sink.
		 *
		 * THE EXIT CONDITION IS THE MACHINE'S OWN AND NOT A FRAME COUNT.
		 * Scheduler::chainAttached() turns true on the first quantum after
		 * the firmware's port table lands, which is the moment the DSP
		 * programs are down and the chain is wired -- exactly the state
		 * design section 15.6 step 4 describes. The budget is the ceiling
		 * that keeps a firmware which never gets there from running for
		 * ever, and a boot that spends the whole budget reports
		 * chainAttached false rather than claiming success.
		 *
		 * A FAULT ENDS IT TOO. A faulted context is never dispatched again,
		 * so running the remaining budget would burn wall-clock time to
		 * reach the same answer. */
		while(result.framesRun < _request.frameBudget)
		{
			const uint64_t remaining = _request.frameBudget - result.framesRun;
			const size_t   chunk     = size_t(remaining < g_bootChunkFrames ? remaining : g_bootChunkFrames);

			scheduler.runFrames(chunk);
			result.framesRun += chunk;

			if(scheduler.faulted() || scheduler.chainAttached())
				break;
		}

		result.chainAttached = scheduler.chainAttached();
		result.faulted       = scheduler.faulted();

		notifyBootStep(BootStep::RunFrames, &scheduler);

		/* THE ONE PLACE THIS SEQUENCE CANNOT BE WRITTEN AS DESIGN SECTION
		 * 15.6 STATES IT, NAMED RATHER THAN PAPERED OVER.
		 *
		 * MEASURED HERE. Scheduler::stateSave writes the CODEC REGIME into
		 * its own limb of the state block (scheduler.cpp, the state block
		 * layout: "the version word first, then the regime"), so
		 * Scheduler::stateLoad RESTORES it. A snapshot taken through
		 * getState is necessarily a PLAY-regime snapshot -- getState runs
		 * after the boot published the machine -- so step 3 leaves this
		 * object in the PLAY regime and step 4's quanta then run the play
		 * regime during what is supposed to be the boot. That is EXACTLY the
		 * merge design section 15.6 forbids: the sink fills, push refuses,
		 * and the run stops part-way.
		 *
		 * PLG-12 CANNOT FIX IT. The regime is private, reset() is the only
		 * thing that selects the boot regime, and re-running reset() after
		 * step 3 would throw away the state step 3 just loaded. The fix is on
		 * the SCHEDULER side -- stateLoad leaving the regime alone, or a
		 * declared way to re-enter the boot regime without a reset -- and
		 * scheduler.{h,cpp} is the SCH track's file and not on PLG-12's
		 * `Files:` line.
		 *
		 * WHAT THIS DOES INSTEAD IS THE SMALLEST HONEST THING. It reports the
		 * condition through the result, and it empties the sink so that step
		 * 5's own premise holds. Emptying costs nothing that beginPlayPhase
		 * does not already do -- step 1 of that call RECONSTRUCTS both queues
		 * -- so this changes no emulated state and no observable outcome; it
		 * only stops a debug build from asserting on a premise the restore
		 * path violated upstream. THE COUNTERS ARE NOT CLEARED, so the
		 * condition stays visible after the fact. */
		if(result.stateLoaded && (scheduler.droppedFrames() > 0 || scheduler.starvedFrames() > 0))
		{
			result.regimeRestoredFromSnapshot = true;
			result.why = "the restored snapshot carried the PLAY codec regime, so the boot quanta "
			             "did not run the boot regime; Scheduler::stateLoad restores the regime and "
			             "PLG-12 has no declared way to re-enter the boot regime without a reset";

			Frame  drained[64];
			size_t taken = 0;

			do
			{
				taken = scheduler.pull(drained, sizeof(drained) / sizeof(drained[0]));
			}
			while(taken > 0);
		}

		// ---- STEP 5. beginPlayPhase -- the CodecSource empty, the CodecSink
		// holding exactly L frames, all seven chain-health counters zero and
		// the recorded owning-thread identity cleared, so the first runFrames
		// on the audio thread re-establishes the owner instead of tripping a
		// debug assertion.
		scheduler.beginPlayPhase();

		notifyBootStep(BootStep::BeginPlayPhase, &scheduler);

		// ---- STEP 6. THE PUBLICATION. The pointer is installed while
		// m_ready is still false and no callback can be in flight, and the
		// release store is what publishes the whole booted machine to the
		// audio thread. isValid()'s seq_cst load is the other half.
		installScheduler(&scheduler);
		m_ready.store(true, std::memory_order_release);

		notifyBootStep(BootStep::Publish, &scheduler);

		result.booted = true;
		result.status = Status::Ok;

		return result;
	}

	void Device::notifyBootStep(const BootStep _step, Scheduler* const _scheduler) noexcept
	{
		if(m_bootObserver)
			m_bootObserver->onBootStep(_step, _scheduler);
	}

	float Device::getSamplerate() const
	{
		// 96000.0f, unconditionally. This single value keeps the host rate out
		// of the emulation: the framework computes every conversion ratio from
		// it (design section 14.7, third qualification).
		return 96000.0f;
	}

	bool Device::isValid() const
	{
		// SEQUENTIALLY CONSISTENT, and it is one of the four seq_cst operations
		// of the hand-off pairing. It is NOT an acquire load: the audio thread
		// has touched the object before, so the load is the second half of a
		// store-load pair and only seq_cst orders that pair. Plan section 17,
		// step 2; design section 13.10 rule 3.
		return m_ready.load(std::memory_order_seq_cst);
	}

#if SYNTHLIB_DEMO_MODE == 0
	bool Device::getState(std::vector<uint8_t>& _state, const synthLib::StateType _type)
	{
		// PLG-5. The hand-off pair runs first (design section 13.10 rule 3);
		// the seven-item format of design section 15.5 is g2State's, and the
		// one property row 7.29 declares -- insert, never assign -- holds
		// through the delegate: serializeState's first write is push_back
		// and every later one is an append, so the header the Plugin layer
		// already put in _state survives.
		beginStateChange();

		/* PLG-12. THE MACHINE SNAPSHOT IS TAKEN HERE, INSIDE THE HAND-OFF
		 * WINDOW, and that is the only place it can be taken: stateSave
		 * reads the emulated machine, and reading it while a callback runs
		 * would race the audio thread. It is held on this object rather
		 * than appended to _state because design section 15.5's format is
		 * PLG-5's and has no item for it -- see boot()'s comment for what
		 * that leaves undone. */
		if(m_ownedScheduler)
		{
			m_machineSnapshot.resize(m_ownedScheduler->stateSize());
			m_ownedScheduler->stateSave(m_machineSnapshot.data());
		}

		const g2::StateData& data = m_stateData;
		const bool ok = g2::serializeState(_state,
			data.performance, data.slotPatches, data.slotPatchIds,
			data.parameterBindings, firmwareVersionWord(), data.parameterOverflowCount);

		endStateChange();
		return ok;
	}

	bool Device::setState(const std::vector<uint8_t>& _state, const synthLib::StateType _type)
	{
		// PLG-5. Same hand-off, then the parse. A refused image (wrong
		// magic, format version, or geometry) changes nothing and reports
		// failure; a firmware-version mismatch loads the machine and
		// withholds the patch data per BRD-11, with the decision's message
		// carried in the result the delegate returns.
		beginStateChange();

		g2::StateData loaded;
		const g2::StateLoadResult result = g2::deserializeState(_state,
			loaded.performance, loaded.slotPatches, loaded.slotPatchIds,
			loaded.parameterBindings, loaded.parameterOverflowCount,
			m_firmwareStatus.state == g2::FirmwareState::Present,
			m_firmwareVersionWord);

		if(!result.machineLoaded)
		{
			endStateChange();
			return false;
		}

		// The machine side of a restore re-boots through design section
		// 15.6's sequence; until PLG-12's Scheduler exists there is no
		// machine to load into, so the payload lands in the held data and
		// the boot-on-restore window's absence is the honest state.
		if(result.patchLoaded)
			m_stateData = std::move(loaded);

		endStateChange();
		return true;
	}
#endif

	uint32_t Device::getChannelCountIn()
	{
		// PLG-2, design sections 14.6, 14.7 and 17 rows 7.32/7.33. These are
		// FINAL VALUES, not stubs: the Plugin queries them exactly once, in
		// its constructor's member-initializer list (plugin.cpp:15, the only
		// call site anywhere in synthLib), and ResamplerInOut stores both as
		// const members that nothing re-reads. They must therefore be final
		// the instant this constructor returns -- before the firmware is
		// loaded, before the boot, and whether or not any artifact was found;
		// the no-firmware path of design section 7.7 presents 2 and 2 as well.
		// The framework ceiling is 4 in / 12 out (TAudioInputs/TAudioOutputs);
		// the presented machine is 2 and 2.
		return 2;
	}

	uint32_t Device::getChannelCountOut()
	{
		// See getChannelCountIn: same task, same const-stored pair, same
		// construction-time finality.
		return 2;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		// The DSP clock is a gated constant of design section 13.4.1 and this
		// design does not offer to vary it: accept 100 and refuse anything
		// else. canModifyDspClock() answers false by the base class default.
		return _percent == 100;
	}

	uint32_t Device::getDspClockPercent() const
	{
		return 100;
	}

	uint64_t Device::getDspClockHz() const
	{
		// The DSP clock is the numerator of the gated constant design section
		// 13.4.1 fixes; timebase.h is its declaration site.
		return G2_DSP_CYCLES_PER_FRAME_NUM;
	}

	/* PLG-7. readMidiOut carries only what the machine originated (design
	 * section 14.5, section 17 row 7.30). The source is the emulated UART0
	 * transmit register: Uart0 delivers each byte the firmware writes to UTB
	 * to the MidiOutFn callback this class installs on it, and the callback
	 * feeds the byte into m_midiOutParser, whose completed events readMidiOut
	 * drains here.
	 *
	 * THE PLUMBING QUESTION, ANSWERED FROM THE PLAN. Neither PLG-7's block,
	 * design section 14.5 nor PLG-12's block names a readMidiOut-side
	 * accessor: the design row says only "the source is the emulated UART0
	 * transmit register". What the plan does fix is the Board's ownership --
	 * the Board owns its Uart0 and hands out references (g2Lib/board.h) --
	 * and PLG-12's ownership of the boot. The Device therefore holds its own
	 * byte sink (m_midiOutBuffer + m_midiOutParser) and PLG-12, which
	 * constructs the Board, installs this class's uart0MidiOut on
	 * Board::uart0() via Uart0::setMidiOut. This file constructs no Board:
	 * doing so is PLG-12's, and constructing one here would boot the
	 * emulation before the Scheduler exists.
	 *
	 * Until that installation happens the parser receives nothing, so this
	 * function appends nothing -- which is the correct answer for a machine
	 * that has not run: no unsolicited SysEx, nothing periodic, no keepalive.
	 * The parser, not a buffer swap, owns the shape, because Uart0 delivers
	 * raw bytes and the framework's readMidiOut contract is completed
	 * SMidiEvents -- every sibling device (mqLib, n2x, jeLib) parses the same
	 * way, and MidiBufferParser is synthLib's own tool for it.
	 */
	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		m_midiOutParser.getEvents(_midiOut);
		m_midiOutBuffer.clear();
	}

	void Device::uart0MidiOut(void* _user, uint8_t _byte)
	{
		auto* self = static_cast<Device*>(_user);
		self->m_midiOutParser.write(_byte);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		// THE AUDIO THREAD'S SEQUENCE, and the set-before-test order is half of
		// the pairing that makes the state hand-off safe, so it is not free to
		// be moved (plan section 17, PLG-4 step 1). First it sets m_inCallback,
		// then it reads isValid().
		m_inCallback.store(true, std::memory_order_seq_cst);

		// THE SAMPLE COUNTER ADVANCES ON EVERY CALLBACK, ready or not: it is
		// emulated time, which passes whether or not the machine is running,
		// and the block-relative to absolute offset conversion of PLG-6
		// reads it. The sibling that shares the base class advances it the
		// same way (n2xdevice.cpp:83).
		m_numSamplesProcessed += static_cast<uint32_t>(_samples);

		if(!isValid())
		{
			m_inCallback.store(false, std::memory_order_release);

			// The silence the boot window promises: zero the output buffers and
			// touch the Scheduler not at all. PLG-4 fills the Scheduler branch.
			for(auto& out : _outputs)
			{
				if(out)
					std::fill(out, out + _samples, 0.0f);
			}
			return;
		}

		// PLG-4. The ready branch, and the call order is the deliverable: the
	// stamped MIDI staged by sendMidi goes out first (design row 7.31's
	// "before runFrames for the same block"), then ONE call to
	// Scheduler::push, then runFrames, then pull, then a read of
	// Scheduler::faulted(). THE CALL ORDER IS WHAT FIXES BOTH CODEC QUEUE
	// CAPACITIES AT L + B (design section 13.6): push delivers a whole block
	// before runFrames consumes any of it, and runFrames produces a whole
	// block before pull takes any of it, so each queue must hold the
	// lookahead plus the largest host block. Reorder the calls and the
	// capacity argument collapses.
	//
	// THE FRAME CONVERSION IS THIS FILE'S, because the queues carry g2::Frame
	// values (eight int32_t slots of Q23, design section 13.10.4) and the
	// callback receives the framework's float buffers. The scale is
	// dsp56k::g_float2dspScale and the clamp bounds are dsp56k's own, so the
	// two directions are exact inverses of dsp56k::sample2dsp/dsp2sample and
	// the determinism boundary of design section 14.3.1 stays integer up to
	// the frames themselves.

	// THE SUBMISSION-TO-APPLICATION JUNCTION (design row 7.31a). sendMidi
	// only stamped and enqueued; THIS is where the emulated machine drains
	// the queue, against the running sample counter, which is where sample
	// accuracy actually happens. The events carry absolute frame indices
	// already -- sendMidi added m_numSamplesProcessed + extraLatency -- and
	// the delivery target is Scheduler::queueMidi(uint64_t frameIndex, ...),
	// part of the Scheduler's finished surface that does not exist in g2Lib
	// yet (W3-415 measured it so); PLG-12's boot wiring owns every other
	// Scheduler-touching line and calls queueMidi through these stamped
	// events when it lands. Until then the drain is the observable: the
	// events leave the pending vector in stamp order, at the top of the
	// callback, before any audio moves.
	/* PLG-12 REPLACES PLG-4's NO-OP DRAIN WITH A REAL DELIVERY, AND THE
	 * DELIVERY IS NOT THE ONE DESIGN ROW 7.31 NAMES. That row's target is
	 * Scheduler::queueMidi(uint64_t frameIndex, ...). THAT FUNCTION IS
	 * DECLARED NOWHERE IN g2Lib -- W3-415 measured it so and a grep over
	 * source/nord/g2 confirms it still -- and scheduler.h is an SCH-track
	 * file, not this task's, so PLG-12 cannot add it.
	 *
	 * WHAT IS AVAILABLE IS THE MACHINE'S OWN RECEIVE PATH: Uart0::receive
	 * lands a byte in the emulated receiver FIFO, and uart0.h names the
	 * Device as its feeder. So the events reach the machine, in stamp
	 * order, at the top of the callback and before runFrames -- every
	 * property design row 7.31 asks for EXCEPT the sample-accurate frame
	 * index, which only queueMidi can carry. THE ACCURACY GAP IS REAL: a
	 * block's events all arrive at the block's start rather than at their
	 * own frames. It closes when queueMidi lands and this loop calls it
	 * with e.offset, which sendMidi already made absolute.
	 *
	 * NO BOARD MEANS NO DELIVERY, which is the unbooted Device's honest
	 * state and is what keeps the ungated T0 tests observing the drain
	 * alone. */
	if(m_board)
	{
		Uart0& uart = m_board->uart0();

		for(const auto& e : m_pendingMidi)
		{
			if(!e.sysex.empty())
			{
				for(const uint8_t byte : e.sysex)
					uart.receive(byte);

				continue;
			}

			/* THE BYTE COUNT COMES FROM THE STATUS BYTE and never from
			 * the struct's three fields: a program change carries one
			 * data byte and a note-on carries two, so a fixed three-byte
			 * write would feed the emulated receiver a byte the host
			 * never sent. synthLib's own parser owns that mapping and
			 * this is its declared entry point. */
			const uint32_t length = synthLib::MidiBufferParser::lengthFromStatusByte(e.a);

			uart.receive(e.a);

			if(length > 1)
				uart.receive(e.b);
			if(length > 2)
				uart.receive(e.c);
		}
	}

	m_pendingMidi.clear();

	// THE INGRESS CONVERSION. Host floats to Q23 frames, one per sample.
	// The stack buffer is sized to the framework's largest sub-block the
	// callback can be asked for; ResamplerInOut never exceeds it at 96 kHz,
	// and the debug assert below keeps an unexpected block size loud.
	std::array<g2::Frame, 2048> inFrames;
	assert(_samples <= inFrames.size() && "host block exceeded the per-callback frame buffer");

	for(uint32_t s = 0; s < _samples && s < inFrames.size(); ++s)
	{
		inFrames[s].slot[0] = _inputs[0] ? static_cast<int32_t>(dsp56k::sample2dsp(_inputs[0][s])) : 0;
		inFrames[s].slot[1] = _inputs[1] ? static_cast<int32_t>(dsp56k::sample2dsp(_inputs[1][s])) : 0;
	}

	// ONE call to Scheduler::push for the whole block -- the call order's
	// first act, and the shape that makes the L + B capacity argument hold:
	// the whole block is accepted BEFORE runFrames consumes any of it.
	m_driver->push(inFrames.data(), _samples);

	// ONE QUANTUM ENTRY POINT for the whole block. runFrames takes a frame
	// count, and at the device rate one sample IS one frame (design section
	// 13.4: "one ESAI TDM frame, which is one 96 kHz sample period"), so the
	// callback's _samples IS the m the framework requested.
	m_driver->runFrames(_samples);

	// THE EGRESS. ONE call to Scheduler::pull for the whole block, into the
	// same stack buffer -- the audio thread allocates nothing. The part
	// pull could not supply reads as silence (CodecSink::pull's contract),
	// and the tail loop below writes that silence explicitly so the host
	// buffers are always fully written, never preserved.
	std::array<g2::Frame, 2048> outFrames;
	const size_t taken = m_driver->pull(outFrames.data(), _samples);

	for(uint32_t s = 0; s < taken; ++s)
	{
		if(_outputs[0])
			_outputs[0][s] = dsp56k::dsp2sample<float>(static_cast<dsp56k::TWord>(outFrames[s].slot[0]));
		if(_outputs[1])
			_outputs[1][s] = dsp56k::dsp2sample<float>(static_cast<dsp56k::TWord>(outFrames[s].slot[1]));
	}
	for(uint32_t s = static_cast<uint32_t>(taken); s < _samples; ++s)
	{
		if(_outputs[0])
			_outputs[0][s] = 0.0f;
		if(_outputs[1])
			_outputs[1][s] = 0.0f;
	}

	// THE FAULT CHANNEL, and the response design section 13.10.3 step 4
	// states: the Device learns of a fault after runFrames returns, and its
	// one response is to withdraw -- a release store of false into m_ready,
	// so isValid() answers false and every later callback takes the
	// silence-and-zero path until the host re-boots the instance. The fault
	// is sticky; this device throws nothing and aborts nothing.
	if(m_driver->faulted())
		m_ready.store(false, std::memory_order_release);

	m_inCallback.store(false, std::memory_order_release);
}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		// PLG-4 step 2 (with PLG-6's offset conversion). SUBMISSION, NOT
		// APPLICATION (design section 17 row 7.31a): the framework calls
		// sendMidi for every event of the block BEFORE processAudio, and this
		// body only stamps and enqueues -- the emulated machine drains the
		// queue inside processAudio, against the running sample counter,
		// which is where sample accuracy actually happens.
		//
		// THE OFFSET IS BLOCK-RELATIVE, NOT ABSOLUTE. SMidiEvent::offset
		// arrives from juce::MidiMessageMetadata::samplePosition, an offset
		// WITHIN the current host block, and synthLib sorts on it, scales it
		// by the sample-rate ratio and clamps it into the sub-block. The
		// conversion this device owns is therefore block-relative to
		// ABSOLUTE, not the reverse:
		//
		//   e.offset += m_numSamplesProcessed + getExtraLatencySamples();
		//
		// and the result goes to Scheduler::queueMidi(uint64_t frameIndex,
		// ...), which takes the absolute index. Adding
		// getExtraLatencySamples() pushes MIDI the same distance into the
		// future as the buffered audio. The required-red mutation reverses
		// the arithmetic (offset -=, or absolute-to-relative) and
		// t0_midi_offsets goes red.
		//
		// ONE ACCEPTED FRAMEWORK LIMIT, RECORDED AND NOT FIXED HERE (design
		// section 17 row 7.34): at a host rate other than 96 kHz, all MIDI of
		// a host block lands in the FIRST device sub-block, with offsets
		// saturated to that sub-block's length. The timing error is bounded
		// by one device sub-block, not by one 96 kHz frame. At 96 kHz the
		// resampler is bypassed entirely and offsets pass through untouched,
		// so the path the bit-exactness claim is measured on is unaffected.
		// Contribution 9 of design section 21.2 is the upstream fix.
		auto e = _ev;
		e.offset += m_numSamplesProcessed + getExtraLatencySamples();

		// SYNCHRONOUS ANSWER, per design section 17 row 7.30: a reply to a
		// message the host sent comes back in _response, in the same call,
		// and NOT through readMidiOut -- a device that wants to batch
		// incoming events batches them itself. The machine that would answer
		// arrives with PLG-12's boot; until then the truthful answer is an
		// empty response vector and success.
		m_pendingMidi.push_back(std::move(e));
		return true;
	}

	void Device::beginStateChange() noexcept
	{
		// THE MESSAGE THREAD'S SEQUENCE, design section 13.10 rule 3. The
		// store must be seq_cst: this thread stores m_ready and then loads
		// m_inCallback, the OTHER flag.
		m_ready.store(false, std::memory_order_seq_cst);

#ifndef NDEBUG
		// The wait is unbounded in the shipping behaviour; this bound exists
		// only so that a callback that never returns is loud in development.
		// It is generous: thousands of host callback periods.
		constexpr uint64_t kDebugWaitBound = 1u << 31;
		uint64_t spins = 0;
#endif

		while(m_inCallback.load(std::memory_order_seq_cst))
		{
			std::this_thread::yield();

#ifndef NDEBUG
			++spins;
			assert(spins < kDebugWaitBound);
#endif
		}
	}

	void Device::endStateChange() noexcept
	{
		// The closing store stays RELEASE: the four seq_cst operations are the
		// two audio-thread operations and the two message-thread operations
		// above; a false store here closes a callback and orders nothing that
		// needs seq_cst.
		m_ready.store(true, std::memory_order_release);
	}

	/* THE MEMBER PIN, CHECKED INSIDE THE CLASS'S OWN ACCESS. The plan
	 * verifies that synthLib::Device declares no m_numSamplesProcessed and
	 * that every product declares its own; these static_asserts are what
	 * keep the members' types from drifting once they exist. A
	 * t0_device_surface that bound them from outside the class could not:
	 * the members are protected and private by design, so the pin lives in a
	 * NESTED class of Device, where the access exists. The pairing of design
	 * section 13.10 rule 3 is written against both flags being
	 * std::atomic<bool>. */
	struct Device::MemberPin
	{
		static_assert(std::is_same_v<decltype(m_numSamplesProcessed), uint32_t>,
			"m_numSamplesProcessed is the subclass's OWN uint32_t member, not an inherited one (PLG-6 reads it)");
		static_assert(std::is_same_v<decltype(m_ready), std::atomic<bool>>,
			"m_ready is an std::atomic<bool>: the hand-off pairing's first flag (design 13.10 rule 3)");
		static_assert(std::is_same_v<decltype(m_inCallback), std::atomic<bool>>,
			"m_inCallback is an std::atomic<bool>: the acknowledgement the reverse direction needs (design 13.10 rule 3)");
	};
}
