; synthetic_block_program.asm -- the input of task SCH-14's self-test.
;
; THREE BLOCKS, AND THE LENGTH OF EACH IS KNOWN BY CONSTRUCTION. A jmp ends a
; block, so each run of nop plus the jmp that closes it is exactly one block of
; the table the just-in-time compiler forms. The self-test asserts the block
; count and the longest block's encoded figures against this file and against
; nothing else.
;
; IT NEEDS NO FIRMWARE AND NO ARTIFACT. This is T0 input: a fork contributor
; with no G2 ROM can run the row that reads it.
;
; IT ESTABLISHES NO maxDispatchCost AND MUST NOT BE READ AS DOING SO. It
; verifies the instrument. SPK-5 produces the number this project actually
; uses, measured against the real compiled kernel through SCH-31.
;
; THE FORMAT IS ONE INSTRUCTION FOR EACH LINE. A semicolon starts a comment and
; a blank line is ignored. The test assembles every remaining line with
; dsp56k::Assembler and writes the words into P memory in order, so the file
; carries no directive, no label and no origin: the load address belongs to the
; test.
;
; jmp $0 IS THE BLOCK TERMINATOR AND ITS TARGET IS NEVER TAKEN. The program is
; walked, not run. A block ends at the branch whatever the branch's target is,
; so the three blocks below sit one after another in P memory and the walk
; steps from each to the next.

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
; THIS IS THE LONGEST BLOCK and the self-test names its figures exactly.
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
