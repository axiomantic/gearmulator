#pragma once

// quantum.h — SCH-12
//
// Declares runQuantum, the top-level entry point for one audio quantum.

#include <cstdint>

namespace g2
{
	class Board;

	/* runQuantum — SCH-12
	 *
	 * Runs one audio quantum on _board for _audioFrames audio samples.
	 *
	 * Returns:
	 *   0          on success
	 *   non-zero   on overrun (the quantum did not finish in time)
	 */
	int runQuantum(Board& _board, uint32_t _audioFrames);
} // namespace g2
