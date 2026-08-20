; bit_test_spin.asm -- the input of task DSP-19's check.
;
; A CONDITIONAL BIT-TEST SPIN AND THE LANDING IT FALLS THROUGH TO. The first
; instruction branches to ITSELF while bit 0 of x:$20 is CLEAR, which is the
; shape a guest polling a flag has. The check drives it with that bit SET, so
; the branch is never taken and a correct machine falls through to the second
; instruction and stops there.
;
; EVERY BRANCH TARGET HERE IS A PC-RELATIVE DISPLACEMENT OF ZERO, so each
; instruction branches to itself and the program is POSITION-INDEPENDENT. That
; is not decoration: the check loads this same program at two different
; addresses -- one below the fast-interrupt boundary and one at it -- and a
; program carrying an absolute target could not serve both.
;
; THE LANDING BRANCHES TO ITSELF RATHER THAN RUNNING ON. The check asserts the
; program counter's POSITION after a fixed number of dispatches, so the
; position has to settle at an address named by this file rather than walk into
; whatever P memory holds next.
;
; IT NEEDS NO FIRMWARE AND NO ARTIFACT. This is T0 input: a fork contributor
; with no G2 ROM can run the row that reads it.
;
; THE FORMAT IS ONE INSTRUCTION FOR EACH LINE. A semicolon starts a comment and
; a blank line is ignored. The check assembles every remaining line with
; dsp56k::Assembler and writes the words into P memory in order, so the file
; carries no directive, no label and no origin: the load address belongs to the
; check.
;
; THE LIMIT: this file is a spin and nothing else. It exercises no peripheral,
; no interrupt and no audio path, and a run of it says nothing about either.

; ---------------- the spin. Branch back to this instruction while bit 0 of
; x:$20 is clear.
	brclr #0,x:$20,$0

; ---------------- the landing. Branch back to this instruction, for ever.
	bra $0
