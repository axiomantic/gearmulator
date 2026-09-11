/* Which hardware port carries which chain position, read out of the booted
 * machine.
 *
 * The two orders are not the same order, and nothing in the emulator may assume
 * they are. A chain position is a place in the audio chain: position 0 receives
 * the codec's stereo pair and position N - 1 transmits it back. A hardware port
 * is a DSP's HDI08 host port and a slot of the DSP set. The firmware chooses the
 * assignment between them, and it is not the identity.
 *
 * The order is derived and never written down. The firmware builds a nine-entry
 * base table at 0x30116970 at boot (`set_hdi08_bases`, 0x300391E8); the table is
 * zero-filled in the firmware image, so nothing static can carry it. Entry i
 * holds the CS1 address of the port at chain position i, and hdi08Decode.h --
 * the single definition site of the rule that A3 to A10 are eight active-low
 * one-cold selects -- turns such an address into the ports it selects. An entry
 * that selects exactly one port names that port. The ninth entry drives every
 * line low, which is the broadcast address, and it belongs to no position.
 *
 * So this cannot be answered before the firmware has run. readChainOrder answers
 * 0 for a machine whose table is still zero, and the caller's own contract has
 * to say what it does until the answer arrives.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace g2
{
	class Board;

	/* Fills _portOfPosition[position] with the hardware port the firmware placed
	 * at that chain position, and returns how many positions were named.
	 *
	 * A position the table does not name is left at _count, which is out of
	 * range: a caller that ignored the return would index a port that does not
	 * exist rather than silently reading position zero's.
	 *
	 * The return is the only reading of "is the table there yet". A machine that
	 * has not built the table answers 0, and one part way through answers fewer
	 * than _count. Only `named == _count` is an order. */
	unsigned readChainOrder(Board& _board, unsigned _count, std::vector<unsigned>& _portOfPosition);
}
