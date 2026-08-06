/* esaiFrame.h -- transmitDspFrame and receiveDspFrame. Task SCH-10.
 * Design section 13.10.3.
 *
 * ONE execTX() IS ONE ESAI SLOT, NOT ONE FRAME, and the whole shape of these
 * two calls follows from that. Esai::execTX latches the active slot into the
 * transmit frame, advances the slot counter, and only when that counter passes
 * getTxWordCount() does it resize the frame and fire the transmit callback --
 * which is the ChainAdapter's wrapper. So ONE TRANSMIT CALLBACK COSTS
 * getTxWordCount() + 1 SLOTS. getTxWordCount() is the TDC field of the
 * emulated TCCR, so the slot count is DERIVED FROM GUEST REGISTER STATE and is
 * not a constant this project may name.
 *
 * NOTHING HERE NEEDS AN UPSTREAM CHANGE. Every member used is public today.
 *
 * THE SCHEDULER DRIVES THE ESAI FRAME AND NO CLOCK DOES. An EsaiClock cannot
 * follow a rational cycles-for-each-frame rate, because its consumer is a
 * uint32_t phase accumulator and the DSP rational is not a whole number. These
 * two functions are what replaces it, and NO EsaiClock IS CONSTRUCTED OR NAMED
 * ANYWHERE IN g2Lib.
 */

#pragma once

#include <cstdint>

namespace dsp56k
{
	class Esai;
}

namespace g2
{
	/* Advances one whole transmit FRAME and returns the slot count it cost.
	 *
	 * Returns 0 when no transmitter is enabled. THAT GUARD IS WHAT MAKES THE
	 * LOOP TERMINATE and it is not a convenience: Esai::execTX returns without
	 * advancing the slot counter when no transmitter is enabled, so the loop
	 * could not end. That state is the whole of the boot phase before the
	 * kernel programs TCR, and returning 0 there is correct -- no callback
	 * fires and no written flag is set.
	 *
	 * THE RETURN IS AN OBSERVABLE. A transmit frame that costs a number of
	 * slots other than getTxWordCount() + 1 is a phase perturbation, which is
	 * the only way a transmitter-enable event can show itself.
	 */
	uint32_t transmitDspFrame(dsp56k::Esai& esai) noexcept;

	/* Advances one whole receive FRAME and returns the slot count, which is
	 * exactly getRxWordCount() + 1.
	 *
	 * Returns 0 when no receiver is enabled. THE RECEIVE GUARD IS A DIFFERENT
	 * THING from the transmit one: this loop is a fixed count and terminates
	 * either way, and the guard is there so that a disabled receiver reports 0
	 * rather than a count of calls that did nothing.
	 *
	 * THE FIXED COUNT IS EXACT BECAUSE THE SCHEDULER IS THE ONLY execRX
	 * CALLER. The library's mirror call inside the receive-control-register
	 * write is commented out upstream, receive slot phase is reset to 0 at an
	 * enable and at an RDC change, and no second caller exists. The transmit
	 * side has one, which is why the two are not symmetric.
	 */
	uint32_t receiveDspFrame(dsp56k::Esai& esai) noexcept;
}
