/* t0_chain_adapter_surface.cpp -- the check of task CHN-5. Design 12.3,
 * 13.10.2.
 *
 * The properties this row owns, and that a target build cannot see:
 *
 *  1. The constructor arguments are FORWARDED and READABLE. The test
 *     constructs one adapter with each argument set to a distinct value and
 *     reads each back through its accessor.
 *
 *  2. The audio chain reports EXACTLY dspCount + 1 mailboxes at every
 *     second-bus topology. The audio bus is fixed to Line (section 2.4);
 *     this asserts over a range of topology arguments that the second-bus
 *     topology parameter cannot move it. It is what makes "the audio chain
 *     is fixed to Line" testable rather than merely stated.
 *
 *  3. Every method on the declared public surface has a definition. The test
 *     takes the address of each one; a method declared in chainAdapter.h but
 *     never defined in chainAdapter.cpp fails as a link error here.
 *
 * A build of g2Lib tests none of the three: a class with a declared surface
 * and no definition still compiles and links the library; the checks only
 * drive when a TU constructs, reads, and takes addresses. Section 7.7.1
 * gives the class.
 *
 * The callback factories are exercised for their signature only (taking the
 * address of a member does not call it), because the per-position wiring is
 * CHN-12's and the written-flag rule is CHN-6's.
 */

#include "chainAdapter.h"

#include <cstdint>
#include <cstdio>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}
}

/* ------------- Property 3: the declared public surface, all addressable.
 *
 * Each of these binds the address of one member to a variable of exactly the
 * member's type. A member that exists only as a declaration forces the linker
 * to look for its definition here; a missing definition is a link error.
 * Taking the address does not CALL any of them, so the bodies are not
 * exercised here -- that is the owning tasks' job. */

using ChainAdapter = g2::ChainAdapter;

/* the phase methods */
void (ChainAdapter::*const kAdvanceAll)(uint64_t) noexcept = &ChainAdapter::advanceAll;
void (ChainAdapter::*const kInjectCodecSource)(const g2::Frame&) noexcept = &ChainAdapter::injectCodecSource;
void (ChainAdapter::*const kExtractCodecSink)(g2::Frame&) noexcept = &ChainAdapter::extractCodecSink;

/* the callback factories */
g2::EsaiReadRxCallback (ChainAdapter::*const kAudioRx)(unsigned) = &ChainAdapter::audioRxCallback;
g2::EsaiWriteTxCallback (ChainAdapter::*const kAudioTx)(unsigned) = &ChainAdapter::audioTxCallback;
g2::EsaiReadRxCallback (ChainAdapter::*const kSecondRx)(unsigned) = &ChainAdapter::secondRxCallback;
g2::EsaiWriteTxCallback (ChainAdapter::*const kSecondTx)(unsigned) = &ChainAdapter::secondTxCallback;

/* the counters */
uint64_t (ChainAdapter::*const kUnderrun)(unsigned) const noexcept = &ChainAdapter::underrunFrames;
uint64_t (ChainAdapter::*const kSecondUnderrun)(unsigned) const noexcept = &ChainAdapter::secondBusUnderrunFrames;
uint64_t (ChainAdapter::*const kPhaseError)(unsigned) const noexcept = &ChainAdapter::phaseErrorFrames;

/* the accessors */
unsigned (ChainAdapter::*const kDspCount)() const noexcept = &ChainAdapter::dspCount;
unsigned (ChainAdapter::*const kHopFrames)() const noexcept = &ChainAdapter::hopFrames;
g2::ChainTopology (ChainAdapter::*const kSecondTopology)() const noexcept = &ChainAdapter::secondBusTopology;
unsigned (ChainAdapter::*const kSecondDivider)() const noexcept = &ChainAdapter::secondBusFrameDivider;
unsigned (ChainAdapter::*const kAudioMailboxCount)() const noexcept = &ChainAdapter::audioMailboxCount;
unsigned (ChainAdapter::*const kSecondMailboxCount)() const noexcept = &ChainAdapter::secondBusMailboxCount;

/* the state trio */
size_t (ChainAdapter::*const kStateSize)() const noexcept = &ChainAdapter::stateSize;
void (ChainAdapter::*const kStateSave)(void*) const noexcept = &ChainAdapter::stateSave;
void (ChainAdapter::*const kStateLoad)(const void*) noexcept = &ChainAdapter::stateLoad;

/* the static constexpr mailboxCount */
unsigned (*const kMailboxCount)(g2::ChainTopology, unsigned) noexcept = &ChainAdapter::mailboxCount;

/* Suppress "set but not used" for the pointer variables: binding the address
 * is the whole of the check, and nothing here calls through them. */
void touchAllPointers()
{
	(void)kAdvanceAll; (void)kInjectCodecSource; (void)kExtractCodecSink;
	(void)kAudioRx; (void)kAudioTx; (void)kSecondRx; (void)kSecondTx;
	(void)kUnderrun; (void)kSecondUnderrun; (void)kPhaseError;
	(void)kDspCount; (void)kHopFrames; (void)kSecondTopology; (void)kSecondDivider;
	(void)kAudioMailboxCount; (void)kSecondMailboxCount;
	(void)kStateSize; (void)kStateSave; (void)kStateLoad;
	(void)kMailboxCount;
}

int main()
{
	/* ------------- Property 1: each of the four arguments forwarded.
	 *
	 * dspCount 8, hopFrames 3, a Line second bus and divider 5 are all
	 * distinct values, so each accessor can only read back its own argument
	 * if the constructor really stored it. */
	const unsigned       kDspCountArg  = 8u;
	const unsigned       kHopFramesArg = 3u;
	const g2::ChainTopology kTopologyArg = g2::ChainTopology::Line;
	const unsigned       kDividerArg   = 5u;

	{
		g2::ChainAdapter adapter(kDspCountArg, kHopFramesArg, kTopologyArg,
		                         kDividerArg);
		check(adapter.dspCount() == kDspCountArg,
			"dspCount() forwards the constructor's dspCount argument");
		check(adapter.hopFrames() == kHopFramesArg,
			"hopFrames() forwards the constructor's hopFrames argument");
		check(adapter.secondBusTopology() == kTopologyArg,
			"secondBusTopology() forwards the constructor's topology argument");
		check(adapter.secondBusFrameDivider() == kDividerArg,
			"secondBusFrameDivider() forwards the constructor's divider "
			"argument");
	}

	/* ------------- Property 2: the audio chain reports dspCount + 1
	 * mailboxes at every second-bus topology.
	 *
	 * The audio bus is fixed to Line; the second-bus topology is a
	 * parameter. Each adapter below changes only that parameter, and the
	 * audio chain must still report exactly dspCount + 1. The second-bus
	 * count, by contrast, follows section 12.3's rule through the same
	 * constructor argument. */
	{
		static const unsigned kN = 8u;

		const g2::ChainAdapter line(kN, 1u, g2::ChainTopology::Line, 1u);
		check(line.audioMailboxCount() == kN + 1u,
			"an 8-position adapter with a Line second bus reports 9 audio "
			"mailboxes");
		check(line.secondBusMailboxCount() == kN + 1u,
			"a Line second bus reports N + 1 = 9 mailboxes");

		const g2::ChainAdapter ring(kN, 1u, g2::ChainTopology::Ring, 1u);
		check(ring.audioMailboxCount() == kN + 1u,
			"an 8-position adapter with a Ring second bus STILL reports 9 "
			"audio mailboxes - the audio chain is fixed to Line");
		check(ring.secondBusMailboxCount() == kN,
			"a Ring second bus reports N = 8 mailboxes");

		const g2::ChainAdapter broadcast(kN, 1u, g2::ChainTopology::Broadcast, 1u);
		check(broadcast.audioMailboxCount() == kN + 1u,
			"an 8-position adapter with a Broadcast second bus STILL reports "
			"9 audio mailboxes - the audio chain is fixed to Line");
		check(broadcast.secondBusMailboxCount() == 1u,
			"a Broadcast second bus reports exactly 1 mailbox");
	}

	/* Property 3's pointer bindings are file-scope and already forced the
	 * linker. touchAllPointers just keeps them from being flagged. */
	touchAllPointers();

	if(failures != 0)
	{
		printf("t0_chain_adapter_surface: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_chain_adapter_surface: all cases passed\n");
	return 0;
}
