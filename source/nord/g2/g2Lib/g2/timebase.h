/* g2/timebase.h -- the G2's timebase constants.
 *
 * This header is C11, and that choice decides two signatures. It uses only
 * #define, typedef struct and plain C functions. No declaration in it may use
 * a C++ reference parameter, a namespace, a template or an overload. Both
 * accumulator functions therefore take `uint32_t* acc` and never
 * `uint32_t& acc`, which does not compile in C at all. The C++ side and any
 * future C consumer include this file unchanged.
 */

#ifndef G2_TIMEBASE_H
#define G2_TIMEBASE_H

#include <stdint.h>

/* This header is included unchanged from C and from C++, so the assertion
 * keyword is selected rather than assumed: `_Static_assert` is the C11
 * spelling and `static_assert` is the C++ one. */
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
 * That is not an integer, so it cannot be one scalar constant. A rational is
 * named instead, and alloc() below turns it into the sequence 1562, 1563,
 * 1562, 1563 whose mean is exactly 1562.5.
 *
 * The numerator is provisional and unmeasured. The denominator is fixed at
 * the frame rate.
 *
 * The cadence guarantee does not depend on the numerator: the ESAI frame is
 * driven by the scheduler and not by a clock, so this rational decides only
 * how many cycles a context is given for each quantum.
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
 * used either one would compute with a zero, so g2Lib/CMakeLists.txt fails the
 * configure step if any file under source/nord/g2/ names either symbol. This
 * file is exempt from that guard, because it is their declaration site.
 */

/* The bus clock, BCLKO. Nothing in the scheduler may use it as a cycle
 * budget.                                                                   */
#define G2_MCU_BUS_CLOCK_HZ           0u         /* not derived              */

/* PSTCLK/BCLKO. The manual permits 2, 3 or 4, and nothing else.             */
#define G2_MCU_BUS_DIVIDER            0u         /* not derived              */

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

/* The MCU context's rational. The numerator is the core clock, not the bus
 * clock.                                                                    */
#define G2_MCU_CYCLES_PER_FRAME_NUM   G2_MCU_CORE_CLOCK_HZ
#define G2_MCU_CYCLES_PER_FRAME_DEN   G2_FRAME_RATE_HZ

/* ---------------- the chain hop
 *
 * H, the delay in 96 kHz frames that one inter-DSP hop costs. Each mailbox is
 * a ring of H + 1 frames, so a change of H deepens an array -- and it also
 * moves D_chain, and therefore the reported plugin latency and every rendered
 * sample. Provisional 1, unmeasured. Inside the bit-exactness boundary, so a
 * change invalidates the whole golden set.                                  */
#define G2_CHAIN_HOP_FRAMES           1u         /* provisional              */

/* ---------------- the second bus's frame divider
 *
 * ChainAdapter takes this as secondBusFrameDivider, and Peripherals56311
 * takes the second ESAI's frame rate as G2_FRAME_RATE_HZ divided by it. The
 * phase relation between the two buses depends on both being derived from one
 * symbol.
 *
 * Provisional 4, from the machine's 24 kHz control rate against the 96 kHz
 * frame rate. What is settled is that the machine has a 24 kHz control rate;
 * which bus carries it is not.                                              */
#define G2_SECOND_BUS_FRAME_DIVIDER   4u         /* provisional              */

/* ---------------- the host-block to frame mapping
 *
 * The numerator is the frame rate. The denominator is the host rate, so it is
 * a run-time value and not a macro: prepareToPlay fixes it and the framework's
 * own recreate() zeroes the accumulator.                                    */
#define G2_HOST_FRAMES_NUM            G2_FRAME_RATE_HZ

/* ---------------- the assertions this file owns
 *
 * Every Rational in this design is built from the macros above, so the
 * denominator is a compile-time constant and the invariant check is a
 * compile-time one. There is no construction-time assertion, because a release
 * build removes one.
 */
G2_STATIC_ASSERT(G2_DSP_CYCLES_PER_FRAME_DEN != 0u,
	"DSP rational denominator");
G2_STATIC_ASSERT(G2_MCU_CYCLES_PER_FRAME_DEN != 0u,
	"MCU rational denominator");

/* The unmeasured bus symbols. These assertions sit here rather than in a test
 * because this file is what the configure guard exempts. They also make the
 * eventual measurement a deliberate edit: giving either symbol a value fails
 * these lines. */
G2_STATIC_ASSERT(G2_MCU_BUS_CLOCK_HZ == 0u,
	"BCLKO is UNMEASURED.");
G2_STATIC_ASSERT(G2_MCU_BUS_DIVIDER == 0u,
	"PSTCLK/BCLKO is UNMEASURED.");

/* 1 is illegal for the bus divider whatever the measurement returns: the MCF5307
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
 * Invariant: den != 0, and num / den must not exceed UINT32_MAX, which every
 * value in this design satisfies by a wide margin. The check is not inside
 * alloc(), because alloc() runs once for each context for each quantum and
 * must carry no branch that cannot fire.
 *
 * A Rational built at run time is checked by Scheduler::create, which returns
 * Status::BadRational and no object.
 */
typedef struct {
	uint32_t num;      /* the clock rate in Hz    */
	uint32_t den;      /* G2_FRAME_RATE_HZ        */
} Rational;

/* alloc() is exact. It uses no floating point anywhere.
 *
 * Each context carries one unsigned integer accumulator and computes its
 * whole-cycle allocation for each quantum. For the DSP the sequence is 1562,
 * 1563, 1562, 1563 and the mean is exactly 1562.5. The accumulator is part of
 * the context state and part of the scheduler snapshot, and it never reads
 * wall-clock time, so the allocation is a pure function of the frame index and
 * the initial state.
 *
 * `acc` is a pointer and not a C++ reference, because this header is C.
 *
 * `static inline` rather than a bare definition: the body lives in the header,
 * and a header with an external definition included by two translation units
 * is a duplicate-symbol link error.
 */
static inline uint32_t alloc(Rational r, uint32_t* acc)
{
	uint32_t whole = r.num / r.den;                /* 1562 for the DSP      */
	*acc += r.num % r.den;                         /* 48000 for the DSP     */
	if(*acc >= r.den) { *acc -= r.den; ++whole; }  /* 1563 on alternate     */
	return whole;                                  /* frames                */
}

/* Whole 96 kHz frames for a block of n host samples. Exact. No floating point.
 * Identical in shape to alloc() above, widened to 64 bits because n * num
 * overflows 32 bits at a large block size.
 *
 * The range of m is stated exactly. Write r = num / den. For every block,
 *
 *     floor(n * r)  <=  m  <=  floor(n * r) + 1
 *
 * because acc is always below den at entry and at exit. m therefore takes one
 * of two adjacent integer values for a given n, and the long-run mean
 * is exactly n * r with no drift. At 44.1 kHz, r = 320/147 and n = 32, m is 69
 * or 70 and the pattern repeats every 147 blocks.
 *
 * `acc` is a pointer, not a C++ reference, for the reason alloc() gives.
 *
 * This accumulator sits outside the determinism boundary: it lives in the
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
