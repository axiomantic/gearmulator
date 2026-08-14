/* t0_mailbox_count.cpp -- the check of task CHN-4. Design 12.3, 13.10.2.
 *
 * THE PROPERTY THIS ROW OWNS. `mailboxCount(ChainTopology, unsigned)` is a
 * CONSTANT EXPRESSION, and the mailbox arrays are sized from exactly such a
 * use. A function that is declared in the header but NOT defined inline
 * still compiles and links a target -- the failure appears only at a
 * constant-expression use -- so a static_assert is the only check that can
 * catch it. Every line below is a compile-time assertion; the source already
 * fails to build if the function is not usable in a constant expression.
 *
 * THE THREE-VALUE RULE OF SECTION 12.3, ASSERTED:
 *   Line -> N + 1, Ring -> N, Broadcast -> 1.
 * The audio bus is always Line, so it always has dspCount + 1 mailboxes
 * (section 2.4 proves the line from the DMA slot counts). The second bus
 * takes its topology as a parameter, provisionally Ring. The rule is
 * computed from the topology and never written down, and these asserts pin
 * the computation at every member of a fixed, small set of values plus the
 * exact triples the Check: line names.
 */

#include "chainAdapter.h"

#include <cstdio>

/* ---- THE CHECK: LINE's OWN THREE ASSERTIONS, AT N = 8. ------------------ */
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Line, 8u)
	== 9u,
	"a Line of 8 positions has 8 + 1 = 9 mailboxes (both ends open)");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Ring, 8u)
	== 8u,
	"a Ring of 8 positions has exactly 8 mailboxes (the two open ends "
	"become one)");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Broadcast, 8u)
	== 1u,
	"a Broadcast of 8 positions has exactly 1 shared mailbox");

/* ---- The same rule at N = 4 and N = 1, so the arithmetic generalises
 * beyond the single value the Check names. --------------------------------- */
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Line, 4u)
	== 5u,  "Line(4) = 4 + 1");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Ring, 4u)
	== 4u,  "Ring(4) = 4");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Broadcast, 4u)
	== 1u,  "Broadcast(4) = 1");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Line, 1u)
	== 2u,  "Line(1) = 1 + 1, the minimal line");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Ring, 1u)
	== 1u,  "Ring(1) = 1, the minimal ring");
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Broadcast, 1u)
	== 1u,  "Broadcast(1) = 1, and every broadcast is 1");

/* ---- A value the arrays are actually sized from in practice: the audio
 * chain is fixed to Line and the machine has eight DSPs (section 12.3), so
 * the audio bus carries 9 mailboxes. ---------------------------------------- */
static_assert(g2::ChainAdapter::mailboxCount(g2::ChainTopology::Line, 8u)
	== 9u,
	"the 8-DSP audio line carries 9 mailboxes");

/* ---- noexcept is part of the surface: a constant-expression sizing use
 * must not be able to throw. ------------------------------------------------ */
static_assert(noexcept(g2::ChainAdapter::mailboxCount(
	g2::ChainTopology::Line, 8u)),
	"mailboxCount is noexcept");

int main()
{
	/* The static_asserts above are the whole of the check: they will
	 * not run at runtime, they fail the BUILD. main exists only so the
	 * registered ctest target has something to execute and to print the
	 * result the way the other chain tests do. */
	std::printf("t0_mailbox_count: all constant-expression assertions passed\n");
	return 0;
}
