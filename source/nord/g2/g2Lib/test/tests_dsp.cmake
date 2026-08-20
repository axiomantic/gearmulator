# Test registrations for the dsp track. Owned by the dsp track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- DSP-7, eight DSPs attached face by face
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_dsp_attach$
#
# Asserts the properties this row owns: the X-space face in _pX and the
# Y-space face in _pY on each of eight slots; one armed DMA channel per slot,
# checked through the dispatch branch's own effect rather than through an
# assert() that NDEBUG removes; the two slot-mask registers round-tripped
# through the attached Y pointer and read back through both faces; $FFFF88
# answered by the timers in the X space and by ESAI_1 in the Y space and
# neither answering the other; and the state trio, with a save-and-load round
# trip over the register block and the three memory areas of every slot.

add_executable(t0_dsp_attach t0_dsp_attach.cpp)
target_link_libraries(t0_dsp_attach PRIVATE g2Lib)
set_property(TARGET t0_dsp_attach PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_attach COMMAND t0_dsp_attach)
set_tests_properties(t0_dsp_attach PROPERTIES LABELS "UnitTest")

# ----------------- DSP-16, the host-port callback bridge
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_dsp_bridge$
#
# Constructs a real DSP behind ONE host port and asserts a word crosses in each
# direction, that an unbridged pair carries nothing either way, that a word
# driven at an unbridged port reaches no bridged DSP, and that the DSP's HF2 and
# HF3 reach the host ISR.

add_executable(t0_hdi08_dsp_bridge t0_hdi08_dsp_bridge.cpp)
target_link_libraries(t0_hdi08_dsp_bridge PRIVATE g2Lib)
set_property(TARGET t0_hdi08_dsp_bridge PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_dsp_bridge COMMAND t0_hdi08_dsp_bridge)
set_tests_properties(t0_hdi08_dsp_bridge PROPERTIES LABELS "UnitTest")

# ----------------- DSP-17, the bootstrap consumer behind the host port
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_dsp_boot_consumer$

add_executable(t0_dsp_boot_consumer t0_dsp_boot_consumer.cpp)
target_link_libraries(t0_dsp_boot_consumer PRIVATE g2Lib)
set_property(TARGET t0_dsp_boot_consumer PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_boot_consumer COMMAND t0_dsp_boot_consumer)
set_tests_properties(t0_dsp_boot_consumer PROPERTIES LABELS "UnitTest")

# ----------------- DSP-18, the CVR host-command interrupt path
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_cvr_irq$

add_executable(t0_hdi08_cvr_irq t0_hdi08_cvr_irq.cpp)
target_link_libraries(t0_hdi08_cvr_irq PRIVATE g2Lib)
set_property(TARGET t0_hdi08_cvr_irq PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_cvr_irq COMMAND t0_hdi08_cvr_irq)
set_tests_properties(t0_hdi08_cvr_irq PROPERTIES LABELS "UnitTest")

# ----------------- DSP-8, the chain carries a frame
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_chain_data_flow$
#
# Drives an eight-slot DSP set through a chain adapter, installs the Rx/Tx pair
# on every ESAI of every slot, and asserts that a frame injected at slot i's
# audio ESAI arrives at slot i + 1's audio ESAI while slot i + 1's second-bus
# ESAI receives nothing from that injection.
#
# TIMEOUT IS PART OF THE ASSERTION AND IS NOT HOUSEKEEPING, for the reason
# BRD-17's t0_hdi08_nonblocking states above. Peripherals56311 reaches
# dsp56k::Audio's ring-buffer constructor, so an ESAI carries a default receive
# callback that pops an input ring on a real semaphore (audio.cpp,
# ringbuffer.h). A run that reaches that callback does not return, and a run
# that does not return reports neither pass nor fail. The timeout is what turns
# it into a result.

add_executable(t0_chain_data_flow t0_chain_data_flow.cpp)
target_link_libraries(t0_chain_data_flow PRIVATE g2Lib)
set_property(TARGET t0_chain_data_flow PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_data_flow COMMAND t0_chain_data_flow)
set_tests_properties(t0_chain_data_flow PROPERTIES LABELS "UnitTest" TIMEOUT 120)

# ----------------- DSP-19, dynamicFastInterrupts on every slot's JitConfig
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_dynamic_fast_interrupts$
#
# Drives a bit-test spin, loaded from a committed program, through the DSPs a
# g2::DspSet built. Three arms: the spin BELOW the fast-interrupt boundary
# through DSP::exec(), the same spin AT the boundary, and the first arm again
# through DSP::execInterpreter(). The observable is the PROGRAM COUNTER'S
# POSITION; an assertion on retired instructions is satisfied by the defect.
#
# THE PATH OF THE COMMITTED PROGRAM IS PASSED ON THE COMMAND LINE, on SCH-14's
# precedent above: a path the check had to guess would be a path the check could
# get wrong in silence.
#
# THE REGISTRATION IS UNCONDITIONAL WHILE THE ROW READS dsp56k::g_useJIT AT RUN
# TIME, which is the shape t0_backend_rule, t0_order and t0_construction_rejection
# already carry. Where this row differs: those three have an opposite property to
# assert on an interpreter build and this one has none, so it FAILS there rather
# than skipping, and it prints the build mode on its first line so that outcome
# is traceable to a configuration without a second device.

add_executable(t0_dynamic_fast_interrupts t0_dynamic_fast_interrupts.cpp)
target_link_libraries(t0_dynamic_fast_interrupts PRIVATE g2Lib)
set_property(TARGET t0_dynamic_fast_interrupts PROPERTY FOLDER "G2/test")

add_test(NAME t0_dynamic_fast_interrupts COMMAND t0_dynamic_fast_interrupts
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bit_test_spin.asm)
set_tests_properties(t0_dynamic_fast_interrupts PROPERTIES LABELS "UnitTest")
