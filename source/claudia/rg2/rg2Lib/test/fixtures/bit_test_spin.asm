; bit_test_spin.asm -- a conditional bit-test spin.
;
; a conditional bit-test spin and the landing it falls through to. The first
; instruction branches to ITSELF while bit 0 of x:$20 is CLEAR, which is the
; shape a guest polling a flag has. The check drives it with that bit SET, so
; the branch is never taken and a correct machine falls through to the second
; instruction and stops there.
;
; Every branch target here is a PC-relative displacement of zero, so each
; instruction branches to itself and the program is POSITION-INDEPENDENT. That
; is not decoration: the check loads this same program at two different
; addresses -- one below the fast-interrupt boundary and one at it -- and a
; program carrying an absolute target could not serve both.
;
; The landing branches to itself rather than running on. The check asserts the
; program counter's POSITION after a fixed number of dispatches, so the
; position has to settle at an address named by this file rather than walk into
; whatever P memory holds next.
;
; It needs no firmware and no artifact. This is T0 input: a fork contributor
; with no G2 ROM can run the row that reads it.
;
; The format is one instruction for each line. A semicolon starts a comment and
; a blank line is ignored. The check assembles every remaining line with
; dsp56k::Assembler and writes the words into P memory in order, so the file
; carries no directive, no label and no origin: the load address belongs to the
; check.

; ---------------- the spin. Branch back to this instruction while bit 0 of
; x:$20 is clear.
	brclr #0,x:$20,$0

; ---------------- the landing. Branch back to this instruction, for ever.
	bra $0
