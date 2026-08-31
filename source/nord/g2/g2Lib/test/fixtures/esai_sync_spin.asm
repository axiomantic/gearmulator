; esai_sync_spin.asm -- a conditional spin on the ESAI sync flag.
;
; a conditional bit-test spin on the ESAI sync flag and the sentinel write
; it falls through to. The first instruction branches to ITSELF while the
; named sync bit of the ESAI status register is CLEAR, which is the shape a
; guest polling a frame-sync edge has. When the edge arrives the branch is
; not taken and a correct machine falls through to the sentinel write and
; the stop.
;
; Every branch target here is PC-relative with displacement zero, so each
; instruction branches to itself and the program is POSITION-INDEPENDENT.
; That is not decoration: the check loads this program at an address BELOW
; Vba_End ($100), and a program carrying an absolute target could not be
; loaded there.
;
; The sentinel write is the observable. X:$000100 receives $AAAAAA on
; the first instruction after the spin. The test asserts the pre-state is
; zero and the post-run value is $AAAAAA, which is evidence the guest
; executed an instruction it could not reach without seeing the edge.
;
; The stop is a self-branching bra, not a nop sled. A self-branching bra
; causes exec() to never return under the JIT with linkJitBlocks=false,
; which would hang runDspCycles. But below $100, dynamicFastInterrupts
; puts the JIT in FastInterruptMode::Dynamic and exec() returns after
; each instruction. The bra-to-self stops the guest from running past
; the sentinel write and into uncharted memory.

; ---------------- the transmit-side spin. Branch back to this instruction
; while bit 13 (M_TFS) of x:<<$FFFFB3 (M_SAISR) is clear.
	brclr #13,x:<<$FFFFB3,$0

; ---------------- the sentinel write. Load $AAAAAA into x0, then write
; x0 to x:>$000100.
	move #>$AAAAAA,x0
	move x0,x:>$000100

; ---------------- the stop. A bra-to-self so exec() returns each call
; under Dynamic mode and the cycle budget is consumed.
	bra $0
