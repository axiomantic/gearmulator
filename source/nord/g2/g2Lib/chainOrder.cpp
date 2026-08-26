#include "chainOrder.h"

#include "board.h"
#include "hdi08Decode.h"

namespace g2
{
	namespace
	{
		/* THE ADDRESS IS FIRMWARE GEOMETRY AND IT IS THE ONLY LITERAL HERE. The
		 * ORDER is read out of the table this names; nothing in this file says
		 * which port is which. */
		constexpr uint32_t g_portTableBase = 0x30116970u;

		/* The table holds 32-bit addresses, and Board::onRead takes a width in
		 * BYTES -- board.h states that the two callbacks are below the
		 * MemoryMap's bit unit and take the core's byte unit instead. */
		constexpr uint32_t g_entryBytes = 4u;
	}

	unsigned readChainOrder(Board& _board, const unsigned _count, std::vector<unsigned>& _portOfPosition)
	{
		/* OUT OF RANGE AND NOT ZERO. A caller that ignored the return would then
		 * index a port that does not exist rather than reading position zero's
		 * port for every position the table did not name. */
		_portOfPosition.assign(_count, _count);

		/* THE EXPANDED SET, because this asks which port an address NAMES and
		 * not which ports a machine carries. Narrowing to the base machine would
		 * make an expansion port's entry decode to no port at all and report the
		 * table as incomplete on a machine whose table is whole. */
		const Hdi08Decode decode(g_hdi08ExpandedPorts);

		unsigned named = 0;

		for(unsigned position = 0; position < _count; ++position)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;

			const uint32_t entry =
				Board::onRead(&_board, g_portTableBase + position * g_entryBytes,
					int(g_entryBytes), &status);

			if(status != MCF5307_BUS_OK)
				continue;

			/* THE EMULATOR'S OWN DECODE AND NOT A SECOND SPELLING OF THE RULE.
			 * A shift-and-invert written here would be a copy of hdi08Decode's
			 * one line that a change to the select field could not move. */
			const uint8_t ports = decode.decode(entry).ports;

			/* EXACTLY ONE LINE LOW IS A PORT. Zero lines low is 0x7F8, which
			 * selects nothing; more than one is the broadcast address, which
			 * belongs to no position. Both are skipped rather than guessed at,
			 * and an unbuilt table -- every entry zero -- selects every port and
			 * therefore names nothing, which is what makes the return value the
			 * reading of "not built yet". */
			if(ports == 0u || (ports & uint8_t(ports - 1u)) != 0u)
				continue;

			unsigned port = 0;
			for(uint8_t bit = ports; bit > 1u; bit >>= 1)
				++port;

			if(port >= _count)
				continue;

			_portOfPosition[position] = port;
			++named;
		}

		return named;
	}
}
