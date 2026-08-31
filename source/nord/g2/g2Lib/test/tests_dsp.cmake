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

# ----------------- t1_kernel_load, the kernel download and the DMA constants
#
# Gated. The test boots the real firmware out of CODE_30000400.bin and reads the
# DMA registers the downloaded kernel programmed, so it needs the Clavia
# artifacts. A machine without them reaches ctest as SKIP_RETURN_CODE rather
# than as a silent pass.
#
# The gate variables are computed here under names of their own: a variable
# borrowed across blocks is how one edit silently changes another block's
# registration. The skip code is read out of gatedFixture.h by the same regex
# every other gated site uses, so the spellings cannot drift; NMG2_ARTIFACTS is
# a cache variable, so whichever include site sets it first wins and every later
# set is a no-op with the same value.

add_executable(t1_kernel_load t1_kernel_load.cpp)
target_link_libraries(t1_kernel_load PRIVATE g2Lib)
set_property(TARGET t1_kernel_load PROPERTY FOLDER "G2/test")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_kernelLoadSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_kernelLoadSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_kernelLoadSkipExitCode "${CMAKE_MATCH_1}")

if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_kernelLoadArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_kernelLoadArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_kernelLoadArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

# The timeout is part of the assertion and is not housekeeping. The drive leaves
# early on its convergence predicate; a machine that never converges walks the
# whole iteration bound, and a machine that hangs inside a peripheral callback
# returns neither pass nor fail. The timeout is what turns the second into a
# result.
add_test(NAME t1_kernel_load COMMAND t1_kernel_load)
set_tests_properties(t1_kernel_load PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_kernelLoadSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_kernel_load APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()
