// Task BRD-12. The panel.
//
// Plan section 13.3, BRD-12. Design sections 8.1, 8.2, 8.3.
// Logbook: AGENTS.md sections 2.2, 2.3, 4.1.
//
// THE PANEL DISPLAY BUFFER SITS ON CS4 AND NO AUTHORITY RECORDS CS4'S BASE.
// AGENTS.md open question 21, widened on 2026-08-05, carries CS4 with CS0 and
// CS2, and SPK-13 reads the base from CSAR4. Plan section 1.3 rule 1 therefore
// applies: the base AND the size of the display window are configuration, and
// this file carries no number for either.
//
// THE PANEL IS A REAL LAYER, NOT DECORATION. A stub that returns zero gives
// panel identifier bits 0b00, which boots and presents a plain G2; a stub that
// returns any other value can stop the machine with no message. The identifier
// lives in latches.h, on CS5, which is the one address in this task with a
// recorded source.
//
// KEY, KNOB AND ENCODER INPUT IS LATER WORK and is not in this milestone,
// because the editor and the host provide the same control surface. This model
// reports a QUIESCENT panel: no key down, no encoder moving, no button
// pressed. It answers every poll at every legal width, so no boot loop spins
// for ever.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	class Panel final : public BusTarget
	{
	public:
		explicit Panel(uint32_t _displaySize);

		uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

		// -------------------------------------------------------------------
		// THE PANEL SEAM -- Task BRD-13, "The panel seam for criterion (h)".
		//
		// Plan section 13.4, BRD-13. Design sections 8.4, 13.5, 23.1.1.
		//
		// THE SEAM EXISTS SO THAT A SPIKE RESULT ARRIVES AS A VALUE AND NOT AS
		// A REWRITE. Design section 8.4 gates the depth of the panel model on
		// spike criterion (h): whether the note-on path from UART0 reaches
		// voice allocation without the keyboard scanner or a panel state
		// machine. If it does not, the panel stays a passive BusTarget and
		// nothing more is needed. If it does, the panel "becomes play-critical
		// and the model grows" -- a scanner state machine driven by the
		// emulated matrix. Section 13.5 and 23.1.1 both record that the seam
		// is the cheap way to make either outcome a change of body and not a
		// change of two interfaces at once.
		//
		// THE SEAM IS THE MEMBERS SECTION 23.1.1 NAMES AND NOTHING MORE, ALL
		// PRESENT NOW WITH AN EMPTY BODY AND A ZERO-BYTE STATE. Section 13.5's
		// order table lists the panel at position 0, before the MCU (the MCU
		// reads what the panel produced), and section 23.1.1's seam row
		// requires it to carry `tick(uint64_t frameIndex)` plus `stateSize`,
		// `stateSave` and `stateLoad` today. The MVP panel computes nothing, so
		// `tick` is an empty body and the state is zero bytes. A formally correct save or
		// load of a zero-byte block writes nothing and reads nothing, which is
		// what the bodies below do.
		//
		// NO EXCEPTION AND NO ASSERT() IN ANY OF THEM. The default build
		// is Release and it defines NDEBUG (see the correction log for the
		// 2026-08-06 measurement), and design section 13.10 rule 2 forbids
		// exceptions across the declared boundaries. Each body is `noexcept`
		// so that the scheduler's run phase can advance the panel without an
		// error channel that the next task over the boundary would have to
		// invent.
		//
		// THE PANEL IS A SCHEDULED BODY, NOT A CONTEXT -- section 13.5, and it
		// is restated here because it is the thing that keeps this seam small.
		// A context is a thing that carries a cycle budget, its own rational
		// accumulator and its own cycle debt. The MVP panel computes nothing
		// and consumes no emulated cycles, so it has none of them and it
		// has NO context index: `cycleDebt`, `longDispatchQuanta`,
		// `contextFaulted` and `contextFault` accept `0 .. dspCount` and
		// nothing else. No member here indexes those arrays. The `Executor`
		// job array does not move either: it holds the DSP contexts, and
		// the panel and the MCU both run serially in the `Scheduler`, outside
		// the `Executor`, so the count is unchanged before and after this seam.
		// THE BODIES ARE INLINE in this header on purpose, and the reason is
		// the plan's own File: accounting. BRD-12 owns panel.cpp and its
		// `Files:` line names it; BRD-13's `Files:` line names panel.h and
		// t0_panel_seam.cpp only. These bodies are short functions
		// that carry no data of their own, so defining them inline keeps them
		// in the file the task owns and touches the file the previous task
		// owns not at all.
		//
		// `tick` ADVANCES THE PANEL ONE QUANTUM. The MVP panel computes
		// nothing, so the body is empty. The parameter is named and stored in
		// no member because there is nothing to do with it; it exists so that
		// a later scanner state machine can read the frame index it is
		// advanced on without the call site changing. A deliberate
		// `(void)frameIndex` keeps the parameter "used" for -Wunused-parameter
		// cleanliness, but it is also the seam: deleting the cast and adding a
		// body is the whole of the change a "yes" from criterion (h) requires,
		// and section 23.1.1 records that this must stay one change.
		void tick(uint64_t frameIndex) noexcept
		{
			(void)frameIndex;
		}

		// ZERO BYTES. Section 23.1.1's snapshot row states the current state
		// as a zero-byte block that a later task will grow; this accessor is
		// the number that block must be today.
		size_t stateSize() const noexcept
		{
			return 0;
		}

		// `stateSave` WRITES THE STATE INTO `dst`. The current state is zero
		// bytes, so a correct save writes nothing. `dst` is deliberately
		// unused; the parameter exists so that the signature does not move
		// when the state becomes non-empty.
		void stateSave(void* dst) const noexcept
		{
			(void)dst;
		}

		// `stateLoad` RESTORES THE STATE FROM `src`. The current state is zero
		// bytes, so a correct load reads nothing and no member changes. The
		// return is deliberately `void`, because a zero-byte state cannot
		// fail to restore; `src` is unused for the same reason the save's
		// `dst` is.
		void stateLoad(const void* src) noexcept
		{
			(void)src;
		}

	private:
		// The display buffer. It starts at zero, which is the quiescent
		// report, and it keeps whatever is written into it. INT-1's t1_boot is
		// what reads this buffer for the OS banner; a T0 check asserts only
		// that the buffer returns what the test wrote.
		std::vector<uint8_t> m_display;
	};
}
