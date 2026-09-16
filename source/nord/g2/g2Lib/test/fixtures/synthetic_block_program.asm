; The input of the block-table harness self-test.
;
; The length of each block is known by construction. A jmp ends a block, so each
; run of nop plus the jmp that closes it is exactly one block of the table the
; just-in-time compiler forms.
;
; It needs no firmware and no artifact, and it establishes no maxDispatchCost:
; it verifies the instrument.
;
; The format is one instruction for each line. A semicolon starts a comment and
; a blank line is ignored. The test assembles every remaining line with
; dsp56k::Assembler and writes the words into P memory in order, so the file
; carries no directive, no label and no origin: the load address belongs to the
; test.
;
; jmp $0 is the block terminator and its target is never taken. The program is
; walked, not run. A block ends at the branch whatever the branch's target is,
; so the blocks below sit one after another in P memory and the walk steps from
; each to the next.

; ---------------- block 1: three nop and the jmp that ends it
	nop
	nop
	nop
	jmp $0

; ---------------- block 2: seven nop and the jmp that ends it
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	jmp $0

; ---------------- block 3: twelve nop and the jmp that ends it.
; This is the longest block and the self-test names its figures exactly.
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	jmp $0
