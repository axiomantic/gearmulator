/* transmitDspFrame and receiveDspFrame.
 *
 * One execTX() is one ESAI slot, not one frame, and the whole shape of these
 * two calls follows from that. Esai::execTX latches the active slot into the
 * transmit frame, advances the slot counter, and only when that counter passes
 * getTxWordCount() does it resize the frame and fire the transmit callback. So
 * one transmit callback costs getTxWordCount() + 1 slots. GetTxWordCount() is
 * the TDC field of the emulated TCCR, so the slot count is derived from guest
 * register state and is not a constant this project may name.
 *
 * The scheduler drives the ESAI frame and no clock does. An EsaiClock cannot
 * follow a rational cycles-for-each-frame rate, because its consumer is a
 * uint32_t phase accumulator and the DSP rational is not a whole number. These
 * two functions are what replaces it.
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
	 * Returns 0 when no transmitter is enabled. That guard is what makes the
	 * loop terminate: Esai::execTX returns without advancing the slot counter
	 * when no transmitter is enabled, so the loop could not end. That state is
	 * the whole of the boot phase before the kernel programs TCR.
	 *
	 * The return is an observable. A transmit frame that costs a number of
	 * slots other than getTxWordCount() + 1 is a phase perturbation, which is
	 * the only way a transmitter-enable event can show itself.
	 */
	uint32_t transmitDspFrame(dsp56k::Esai& esai) noexcept;

	/* Advances one whole receive FRAME and returns the slot count, which is
	 * exactly getRxWordCount() + 1.
	 *
	 * Returns 0 when no receiver is enabled. The receive guard is a different
	 * thing from the transmit one: this loop is a fixed count and terminates
	 * either way, and the guard is there so that a disabled receiver reports 0
	 * rather than a count of calls that did nothing.
	 *
	 * The fixed count is exact because the scheduler is the only execRX caller.
	 * The library's mirror call inside the receive-control-register write is
	 * commented out upstream, receive slot phase is reset to 0 at an enable and
	 * at an RDC change, and no second caller exists. The transmit side has one,
	 * which is why the two are not symmetric.
	 */
	uint32_t receiveDspFrame(dsp56k::Esai& esai) noexcept;
}
