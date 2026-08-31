/* g2/timebase.h -- the only definition site for the G2's timebase constants.
 * C11. Task SCH-0. Design sections 13.4.1, 13.4.3 and 14.1.1.
 *
 * THIS HEADER IS C, AND THE DOCUMENT SAYS SO BECAUSE THE ANSWER CHANGES TWO
 * SIGNATURES. It uses only #define, typedef struct and plain C functions. No
 * declaration in it may use a C++ reference parameter, a namespace, a template
 * or an overload. Both accumulator functions therefore take `uint32_t* acc`
 * and never `uint32_t& acc`, which an earlier design draft carried and which
 * does not compile in C at all. The C++ side and any future C consumer include
 * this file unchanged.
 *
 * The Nim side includes NOTHING from this header. `mcf5307_exec` takes
 * `max_cycles` as a plain uint32_t and the MCU context's rational is computed
 * on the C++ side, inside the Scheduler, before the call.
 */

#ifndef G2_TIMEBASE_H
#define G2_TIMEBASE_H

#include <stdint.h>

/* This header is included unchanged from C and from C++, so the assertion
 * keyword is selected rather than assumed. `_Static_assert` is the C11
 * spelling and is what the C11 compile of t0_timebase_header.c exercises;
 * `static_assert` is the C++ one. */
#if defined(__cplusplus)
#	define G2_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#	define G2_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

/* ---------------- the frame rate */

#define G2_FRAME_RATE_HZ              96000u     /* the ESAI frame rate      */

/* ---------------- the DSP contexts
 *
 * At a 150 MHz DSP clock one frame is 150,000,000 / 96,000 = 1562.5 cycles.
 * THAT IS NOT AN INTEGER, so it cannot be one scalar constant. The design
 * names a rational instead, and alloc() below turns it into the sequence
 * 1562, 1563, 1562, 1563 whose mean is exactly 1562.5.
 *
 * The numerator is PROVISIONAL: measurement register row 8, owner SPK-8,
 * spike criterion (e). The denominator is fixed at the frame rate.
 *
 * The cadence guarantee does NOT depend on the numerator. Design section
 * 13.4.1 records why: the ESAI frame is driven by the scheduler and not by a
 * clock, so this rational decides only how many cycles a context is given for
 * each quantum.
 */
#define G2_DSP_CYCLES_PER_FRAME_NUM   150000000u /* DSP clock, Hz            */
#define G2_DSP_CYCLES_PER_FRAME_DEN   G2_FRAME_RATE_HZ

/* ---------------- the MCU clocks
 *
 * The MCU has two clock domains and they can never be equal, so two symbols and
 * the integer that relates them are named separately, and a bus-clock figure
 * can never be substituted into a core-cycle budget by accident.
 *
 * The part is an MCF5407CAI162, read off the schematic at U14. Oscillator Y1
 * is a 53.620 MHz can buffered by the 74AC14 at U26 into the net the drawing
 * labels `CPU Ck 54MHz` at CLKIN, and into `DSP Ck` at every DSP's EXTAL. The
 * internal PLL multiplies CLKIN by 3 for the core and the bus runs at CLKIN.
 *
 * The two bus symbols below are not derived here and read 0u. A source that
 * used either one would compute with a zero, so g2Lib/CMakeLists.txt FAILS THE
 * CONFIGURE STEP if any file under source/nord/g2/ names either symbol. This
 * file is the one exempt file, because it is their declaration site.
 *
 * That exemption is why their G2_STATIC_ASSERTs live here and not in
 * t0_timebase_header.c: the rule "one assertion for each declared macro" and
 * the configure guard would otherwise contradict each other, and the guard is
 * the one that must not yield.
 */

/* The bus clock, BCLKO. Criterion (j) methods 1 and 2 measure THIS one.
 * NOTHING IN THE SCHEDULER MAY USE IT AS A CYCLE BUDGET.                    */
#define G2_MCU_BUS_CLOCK_HZ           0u         /* NOT DERIVED              */

/* PSTCLK/BCLKO. The manual permits 2, 3 or 4, and nothing else.             */
#define G2_MCU_BUS_DIVIDER            0u         /* NOT DERIVED              */

/* The core clock. This is the only one that may reach mcf5307_exec's
 * max_cycles.
 *
 * Three times the 54 MHz the schematic itself labels at CLKIN. The oscillator
 * can is rated 53.620 MHz, so a strict reading of the part value gives
 * 160,860,000 instead. The label is carried because it agrees with the
 * MCF5407CAI162 speed grade exactly and the can's rating does not; the two are
 * 0.7 per cent apart, which is close enough that only a scope on CLKIN
 * separates them.                                                           */
#define G2_MCU_CORE_CLOCK_HZ          162000000u

/* The MCU context's rational. THE NUMERATOR IS THE CORE CLOCK, NOT THE BUS
 * CLOCK.                                                                    */
#define G2_MCU_CYCLES_PER_FRAME_NUM   G2_MCU_CORE_CLOCK_HZ
#define G2_MCU_CYCLES_PER_FRAME_DEN   G2_FRAME_RATE_HZ

/* ---------------- the chain hop
 *
 * H, the delay in 96 kHz frames that one inter-DSP hop costs. Each mailbox is
 * a ring of H + 1 frames, so a change of H deepens an array -- and it also
 * moves D_chain, and therefore the reported plugin latency and every rendered
 * sample. Provisional 1: measurement register row 9, owner SPK-3, criterion
 * (d). Inside the bit-exactness boundary, so a change invalidates the whole
 * golden set.                                                               */
#define G2_CHAIN_HOP_FRAMES           1u         /* PROVISIONAL              */

/* ---------------- the second bus's frame divider
 *
 * THE ONLY DEFINITION SITE. Two consumers derive from it and neither carries
 * its own copy: ChainAdapter takes it as secondBusFrameDivider, and
 * Peripherals56311 takes the second ESAI's frame rate as G2_FRAME_RATE_HZ
 * divided by this value. The phase relation of design section 12.3 depends on
 * the two being derived from one symbol.
 *
 * Provisional 4, from the 24 kHz control rate that AGENTS.md section 2.2
 * records against the 96 kHz frame rate. What is settled is that the machine
 * HAS a 24 kHz control rate; what is NOT settled is which bus carries it, and
 * criterion (i) is the measurement that answers that.                       */
#define G2_SECOND_BUS_FRAME_DIVIDER   4u         /* PROVISIONAL              */

/* ---------------- the host-block to frame mapping
 *
 * The numerator is the frame rate. THE DENOMINATOR IS THE HOST RATE, so it is
 * a run-time value and not a macro: prepareToPlay fixes it and the framework's
 * own recreate() zeroes the accumulator. Design section 14.1.1.             */
#define G2_HOST_FRAMES_NUM            G2_FRAME_RATE_HZ

/* ---------------- the assertions this file owns
 *
 * The two denominators, per design section 13.4.1. Every Rational in this
 * design is built from the macros above, so the denominator is a compile-time
 * constant and the invariant check is a compile-time one. There is no
 * construction-time assertion, because a release build removes one.
 */
G2_STATIC_ASSERT(G2_DSP_CYCLES_PER_FRAME_DEN != 0u,
	"DSP rational denominator");
G2_STATIC_ASSERT(G2_MCU_CYCLES_PER_FRAME_DEN != 0u,
	"MCU rational denominator");

/* The two unmeasured bus symbols. These assertions are HERE rather than in the
 * test because this file is the one the configure guard exempts. They also
 * make the eventual measurement a deliberate edit: when criterion (j) reports,
 * these two lines fail and force a reader to the register rows. */
G2_STATIC_ASSERT(G2_MCU_BUS_CLOCK_HZ == 0u,
	"BCLKO is UNMEASURED. Register row 5, owner SPK-9 criterion (j).");
G2_STATIC_ASSERT(G2_MCU_BUS_DIVIDER == 0u,
	"PSTCLK/BCLKO is UNMEASURED. Register row 6, owner SPK-9 criterion (j).");

/* 1 is illegal for the bus divider whatever criterion (j) returns: the MCF5307
 * manual permits only 2, 3 or 4, and the MCF5407 the schematic reads at U14
 * multiplies CLKIN by 3. */
G2_STATIC_ASSERT(G2_MCU_BUS_DIVIDER != 1u,
	"Neither reading of the part has a divide-by-one option.");

/* ---------------- Rational
 *
 * A cycles-per-frame ratio, held exactly. Trivially copyable, no invariant
 * beyond den != 0, passed by value because it is two words.
 *
 * Ownership   Each context holds one by value, fixed at construction from the
 *             macros above. Nothing shares one.
 * Lifetime    The life of the context.
 * Threading   Read-only after construction, so it crosses no thread rule.
 *
 * INVARIANT: den != 0, and num / den must not exceed UINT32_MAX, which every
 * value in this design satisfies by a wide margin. THE CHECK IS NOT INSIDE
 * alloc(), because alloc() runs once for each context for each quantum -- nine
 * times at 96 kHz -- and must carry no branch that cannot fire.
 *
 * NO CODE OUTSIDE Scheduler::create BUILDS A RATIONAL THAT REACHES A SHIPPING
 * CONTEXT. A test may build one directly to drive alloc() and the cycle-debt
 * loop. A Rational built at run time is checked by Scheduler::create, which
 * returns Status::BadRational and no object.
 */
typedef struct {
	uint32_t num;      /* the clock rate in Hz    */
	uint32_t den;      /* G2_FRAME_RATE_HZ        */
} Rational;

/* alloc() is EXACT. It uses no floating point anywhere.
 *
 * Each context carries one unsigned integer accumulator and computes its
 * whole-cycle allocation for each quantum. For the DSP the sequence is 1562,
 * 1563, 1562, 1563 and the mean is exactly 1562.5. The accumulator is part of
 * the context state and part of the scheduler snapshot, and it never reads
 * wall-clock time, so the allocation is a pure function of the frame index and
 * the initial state.
 *
 * `acc` is a POINTER and not a C++ reference, because this header is C.
 *
 * `static inline` rather than a bare definition: design section 13.4.1 shows
 * the body in the header, and a header with an external definition included by
 * two translation units is a duplicate-symbol link error. Internal linkage
 * keeps the declared signature exactly as the design states it.
 */
static inline uint32_t alloc(Rational r, uint32_t* acc)
{
	uint32_t whole = r.num / r.den;                /* 1562 for the DSP      */
	*acc += r.num % r.den;                         /* 48000 for the DSP     */
	if(*acc >= r.den) { *acc -= r.den; ++whole; }  /* 1563 on alternate     */
	return whole;                                  /* frames                */
}

/* Whole 96 kHz frames for a block of n host samples. Exact. No floating point.
 * Identical in shape to alloc() above, WIDENED TO 64 BITS because n * num
 * overflows 32 bits at a large block size.
 *
 * The range of m is stated exactly. Write r = num / den. For every block,
 *
 *     floor(n * r)  <=  m  <=  floor(n * r) + 1
 *
 * because acc is always below den at entry and at exit. m therefore takes one
 * of exactly TWO adjacent integer values for a given n, and the long-run mean
 * is exactly n * r with no drift. At 44.1 kHz, r = 320/147 and n = 32, m is 69
 * or 70 and the pattern repeats every 147 blocks.
 *
 * `acc` is a POINTER, not a C++ reference, for the reason alloc() gives.
 *
 * This accumulator sits OUTSIDE the determinism boundary: it lives in the
 * framework's ResamplerInOut, one layer above the Device. It decides how many
 * frames a host block asks for and never touches the value of any frame.
 */
static inline uint32_t framesForBlock(uint32_t n, uint32_t num, uint32_t den,
	uint32_t* acc)
{
	uint64_t t = (uint64_t) n * num + *acc;
	*acc = (uint32_t) (t % den);      /* acc stays below den, always        */
	return (uint32_t) (t / den);
}

#endif /* G2_TIMEBASE_H */
