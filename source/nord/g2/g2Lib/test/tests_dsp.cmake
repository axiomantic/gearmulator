# Test registrations for the dsp track. Owned by the dsp track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- eight DSPs attached face by face
#
# $FFFF88 is answered by the timers in the X space and by ESAI_1 in the Y space,
# and neither answers the other.

add_executable(t0_dsp_attach t0_dsp_attach.cpp)
target_link_libraries(t0_dsp_attach PRIVATE g2Lib)
set_property(TARGET t0_dsp_attach PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_attach COMMAND t0_dsp_attach)
set_tests_properties(t0_dsp_attach PROPERTIES LABELS "UnitTest")

# ----------------- the host-port callback bridge

add_executable(t0_hdi08_dsp_bridge t0_hdi08_dsp_bridge.cpp)
target_link_libraries(t0_hdi08_dsp_bridge PRIVATE g2Lib)
set_property(TARGET t0_hdi08_dsp_bridge PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_dsp_bridge COMMAND t0_hdi08_dsp_bridge)
set_tests_properties(t0_hdi08_dsp_bridge PROPERTIES LABELS "UnitTest")

# ----------------- the bootstrap consumer behind the host port

add_executable(t0_dsp_boot_consumer t0_dsp_boot_consumer.cpp)
target_link_libraries(t0_dsp_boot_consumer PRIVATE g2Lib)
set_property(TARGET t0_dsp_boot_consumer PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_boot_consumer COMMAND t0_dsp_boot_consumer)
set_tests_properties(t0_dsp_boot_consumer PROPERTIES LABELS "UnitTest")

# ----------------- the CVR host-command interrupt path

add_executable(t0_hdi08_cvr_irq t0_hdi08_cvr_irq.cpp)
target_link_libraries(t0_hdi08_cvr_irq PRIVATE g2Lib)
set_property(TARGET t0_hdi08_cvr_irq PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_cvr_irq COMMAND t0_hdi08_cvr_irq)
set_tests_properties(t0_hdi08_cvr_irq PROPERTIES LABELS "UnitTest")

# ----------------- the chain carries a frame
#
# TIMEOUT is part of the assertion and is not housekeeping. Peripherals56311
# reaches dsp56k::Audio's ring-buffer constructor, so an ESAI carries a default
# receive callback that pops an input ring on a real semaphore (audio.cpp,
# ringbuffer.h). A run that reaches that callback does not return, and a run
# that does not return reports neither pass nor fail. The timeout is what turns
# it into a result.

add_executable(t0_chain_data_flow t0_chain_data_flow.cpp)
target_link_libraries(t0_chain_data_flow PRIVATE g2Lib)
set_property(TARGET t0_chain_data_flow PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_data_flow COMMAND t0_chain_data_flow)
set_tests_properties(t0_chain_data_flow PROPERTIES LABELS "UnitTest" TIMEOUT 120)

# ----------------- dynamicFastInterrupts on every slot's JitConfig
#
# The observable is the program counter's position; an assertion on retired
# instructions is satisfied by the defect.
#
# The path of the committed program is passed on the command line: a path the
# check had to guess would be a path the check could get wrong in silence.
#
# The registration is unconditional while the check reads dsp56k::g_useJIT at
# run time. It has no opposite property to assert on an interpreter build, so it
# FAILS there rather than skipping, and it prints the build mode on its first
# line so that outcome is traceable to a configuration.

add_executable(t0_dynamic_fast_interrupts t0_dynamic_fast_interrupts.cpp)
target_link_libraries(t0_dynamic_fast_interrupts PRIVATE g2Lib)
set_property(TARGET t0_dynamic_fast_interrupts PROPERTY FOLDER "G2/test")

add_test(NAME t0_dynamic_fast_interrupts COMMAND t0_dynamic_fast_interrupts
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bit_test_spin.asm)
set_tests_properties(t0_dynamic_fast_interrupts PROPERTIES LABELS "UnitTest")
