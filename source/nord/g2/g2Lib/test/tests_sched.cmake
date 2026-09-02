# Test registrations for the sched track. Owned by the sched track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The name is the exact string ctest -R must
# match. Edit no other CMake file in this tree.

# ---------------- t0_timebase_header
#
# The check compiles t0_timebase_header.c as C11 at test time and then runs the
# grep case. Both live in t0_timebase_header.cmake, which fails if either
# fails. The compile is not left to the build on purpose: that separates "the
# build broke" from "the check reported", and keeps "g2/timebase.h is a C
# header" reportable through `ctest -R`.
#
# No library is linked and no C++ is involved, so this test needs no target.

find_program(G2_GIT_EXECUTABLE NAMES git)

add_test(NAME t0_timebase_header
	COMMAND ${CMAKE_COMMAND}
		-DG2_C_COMPILER=${CMAKE_C_COMPILER}
		-DG2_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}
		-DG2_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.c
		-DG2_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
		-DG2_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/t0_timebase_header
		-DG2_REPO_ROOT=${CMAKE_SOURCE_DIR}
		-DG2_GIT_EXECUTABLE=${G2_GIT_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.cmake)

# ---------------- t0_frame_layout
#
# An ordinary executable, because the acceptance criterion is a build-time
# property: the fully qualified function-pointer types in t0_frame_layout.cpp
# must fail to compile when any parameter is removed, and a missing conversion
# overload must be a link error. Those failures belong to the compiler and the
# linker. What remains for ctest is the behaviour, and that is what the program
# checks when it runs.

add_executable(t0_frame_layout ${CMAKE_CURRENT_SOURCE_DIR}/t0_frame_layout.cpp)
target_link_libraries(t0_frame_layout PRIVATE g2Lib)
set_property(TARGET t0_frame_layout PROPERTY FOLDER "G2/test")

add_test(NAME t0_frame_layout COMMAND t0_frame_layout)

# ---------------- t0_alloc
#
# g2/timebase.h is a header with no compiled part, so the link against g2Lib is
# for the include directories the target carries and for nothing else.

add_executable(t0_alloc ${CMAKE_CURRENT_SOURCE_DIR}/t0_alloc.cpp)
target_link_libraries(t0_alloc PRIVATE g2Lib)
set_property(TARGET t0_alloc PROPERTY FOLDER "G2/test")

add_test(NAME t0_alloc COMMAND t0_alloc)

# ---------------- t0_frames_for_block

add_executable(t0_frames_for_block
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frames_for_block.cpp)
target_link_libraries(t0_frames_for_block PRIVATE g2Lib)
set_property(TARGET t0_frames_for_block PROPERTY FOLDER "G2/test")

add_test(NAME t0_frames_for_block COMMAND t0_frames_for_block)

# ---------------- t0_clock_guard
#
# Every path the test needs is passed on the command line. A path the test had
# to guess would be a path the test could get wrong in silence.
#
# The scratch header the negative case plants must live under source/nord/g2/,
# because that is the tree the guard scans. The test writes it, runs one
# configure and removes it again.
#
# The negative configure is not the whole project: the test writes a scratch
# CMake project whose only content is one add_subdirectory of g2Lib, so it
# configures the real g2Lib/CMakeLists.txt, where the guard lives, and nothing
# else.

add_executable(t0_clock_guard ${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard.cpp)
set_property(TARGET t0_clock_guard PROPERTY FOLDER "G2/test")

add_test(NAME t0_clock_guard COMMAND t0_clock_guard
	${CMAKE_SOURCE_DIR}
	${G2_GIT_EXECUTABLE}
	${CMAKE_COMMAND}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard_scratch.h
	${CMAKE_CURRENT_BINARY_DIR}/t0_clock_guard_work)

# The work directory is not named after the target. CMAKE_CURRENT_BINARY_DIR is
# where the executable itself lands, so a work directory called t0_clock_guard
# would collide with the file t0_clock_guard: the test then cannot create it,
# every command it runs in it fails with no output, and the case reports
# failures that say nothing about the guard.

# ---------------- t0_frame_conversion
#
# t0_frame_layout holds the signatures, which is a build-time property.
# This one holds the behaviour, so it links the library the conversions live in.

add_executable(t0_frame_conversion
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frame_conversion.cpp)
target_link_libraries(t0_frame_conversion PRIVATE g2Lib)
set_property(TARGET t0_frame_conversion PROPERTY FOLDER "G2/test")

add_test(NAME t0_frame_conversion COMMAND t0_frame_conversion)

# ---------------- t0_dsp_context_layout
#
# The member list is a compile-time property, so most of this check is
# static_assert and a deleted member is a build failure. The registered program
# carries the runtime half: the executor's pointer recovery, and that the
# members are distinct objects.

add_executable(t0_dsp_context_layout
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_context_layout.cpp)
target_link_libraries(t0_dsp_context_layout PRIVATE g2Lib)
set_property(TARGET t0_dsp_context_layout PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_context_layout COMMAND t0_dsp_context_layout)

# ---------------- t0_esai_frame
#
# The fixture builds a real dsp56k::Memory, two PeripheralsNop and a real
# dsp56k::DSP, because both ESAI control-register writes reach through the
# peripheral set to the DSP.

add_executable(t0_esai_frame ${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_frame.cpp)
target_link_libraries(t0_esai_frame PRIVATE g2Lib)
set_property(TARGET t0_esai_frame PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_frame COMMAND t0_esai_frame)

# ---------------- t0_codec_queue_surface
#
# The address of every declared method is taken through its fully qualified
# type, so a renamed or re-signed method is a compile error and a
# declared-and-undefined one is a link error.

add_executable(t0_codec_queue_surface
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_queue_surface.cpp)
target_link_libraries(t0_codec_queue_surface PRIVATE g2Lib)
set_property(TARGET t0_codec_queue_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_queue_surface COMMAND t0_codec_queue_surface)

# ---------------- t0_executor
#
# The declared names are held by their fully qualified types inside the
# translation unit, so the compiler and the linker carry that half. What remains
# is the behaviour: the order of the jobs, that every job ran on the calling
# thread, and that a refused re-entry is counted. The re-entry count is
# an observable in every build type, because the default build is Release with
# NDEBUG and a check whose predicate is "the debug build asserted" cannot fail
# in it.

add_executable(t0_executor ${CMAKE_CURRENT_SOURCE_DIR}/t0_executor.cpp)
target_link_libraries(t0_executor PRIVATE g2Lib)
set_property(TARGET t0_executor PROPERTY FOLDER "G2/test")

add_test(NAME t0_executor COMMAND t0_executor)

# ---------------- t0_run_dsp_cycles_contract
#
# The declared signature is held by the whole function type inside the
# translation unit, so the compiler carries that half and a
# declared-and-undefined function is a link error. What remains is the shape of
# the loop, read from the DSP's own instruction and cycle counters, so it does
# not depend on an assertion that a release build removes.

add_executable(t0_run_dsp_cycles_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles_contract.cpp)
target_link_libraries(t0_run_dsp_cycles_contract PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles_contract COMMAND t0_run_dsp_cycles_contract)

# ---------------- t0_run_dsp_cycles
#
# This one drives the bound over scripted block lengths that straddle the
# budget. The upper half of the bound comes from the fixture's own largest
# measured dispatch unit, never from maxInstructionsPerBlock, which the shipped
# configuration leaves uncapped.

add_executable(t0_run_dsp_cycles
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles.cpp)
target_link_libraries(t0_run_dsp_cycles PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles COMMAND t0_run_dsp_cycles)

# ---------------- t0_block_table_harness
#
# The path of the committed synthetic program is passed on the command line. A
# path the check had to guess would be a path the check could get wrong in
# silence.
#
# This row establishes no maxDispatchCost. It verifies the instrument against a
# program whose longest block is known by construction.

add_executable(t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/t0_block_table_harness.cpp)
target_link_libraries(t0_block_table_harness PRIVATE g2Lib)
set_property(TARGET t0_block_table_harness PROPERTY FOLDER "G2/test")

add_test(NAME t0_block_table_harness COMMAND t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthetic_block_program.asm)

# ---------------- t0_codec_capacity
#
# The sink is primed with the lookahead, without which a capacity of B alone
# would satisfy the arithmetic and the L + B rule would go unexercised.

add_executable(t0_codec_capacity
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_capacity.cpp)
target_link_libraries(t0_codec_capacity PRIVATE g2Lib)
set_property(TARGET t0_codec_capacity PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_capacity COMMAND t0_codec_capacity)

# ---------------- t0_backend_rule
#
# The test reads dsp56k::g_useJIT at run time, so the Backend::Jit case is
# conditional on the build and the Backend::Interpreter case is unconditional.
#
# No library beyond g2Lib is linked: the g2Lib target carries the dsp56kEmu
# include directories transitively, so the test reaches dsp56k::g_useJIT
# through scheduler.h.

add_executable(t0_backend_rule ${CMAKE_CURRENT_SOURCE_DIR}/t0_backend_rule.cpp)
target_link_libraries(t0_backend_rule PRIVATE g2Lib)
set_property(TARGET t0_backend_rule PROPERTY FOLDER "G2/test")

add_test(NAME t0_backend_rule COMMAND t0_backend_rule)

# ---------------- t0_cycle_debt

add_executable(t0_cycle_debt ${CMAKE_CURRENT_SOURCE_DIR}/t0_cycle_debt.cpp)
target_link_libraries(t0_cycle_debt PRIVATE g2Lib)
set_property(TARGET t0_cycle_debt PROPERTY FOLDER "G2/test")

add_test(NAME t0_cycle_debt COMMAND t0_cycle_debt)

# ---------------- t0_long_dispatch

add_executable(t0_long_dispatch
	${CMAKE_CURRENT_SOURCE_DIR}/t0_long_dispatch.cpp)
target_link_libraries(t0_long_dispatch PRIVATE g2Lib)
set_property(TARGET t0_long_dispatch PROPERTY FOLDER "G2/test")

add_test(NAME t0_long_dispatch COMMAND t0_long_dispatch)

# ---------------- t0_dsp_job_order

add_executable(t0_dsp_job_order
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_job_order.cpp)
target_link_libraries(t0_dsp_job_order PRIVATE g2Lib)
set_property(TARGET t0_dsp_job_order PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_job_order COMMAND t0_dsp_job_order)

# ---------------- t0_dsp_run_gate
#
# The context here carries a live DSP running a scripted loop, so this check
# needs a JIT backend.

add_executable(t0_dsp_run_gate
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_run_gate.cpp)
target_link_libraries(t0_dsp_run_gate PRIVATE g2Lib)
set_property(TARGET t0_dsp_run_gate PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_run_gate COMMAND t0_dsp_run_gate)

# ---------------- t0_status_contract
#
# The compile-time half of the contract is held by static_asserts, so a
# violation there is a build failure and this target is what carries it.
#
# It does not link g2Lib. status.h is a header with no compiled part and the
# test reaches it through the include directory alone, so linking the library
# would make this check wait on every other source in it without buying the
# check anything.

add_executable(t0_status_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_status_contract.cpp)
target_include_directories(t0_status_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_property(TARGET t0_status_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_status_contract COMMAND t0_status_contract)

# ---------------- t0_construction_rejection
#
# Nothing in the check is an assert() and nothing in it catches an exception, so
# the status out-param is the whole observable in either build type. The
# generator here is single-config and the tree is configured Debug, so this
# add_test names one build type and can name no other; the release half needs a
# second tree, and this file configures none.

add_executable(t0_construction_rejection
	${CMAKE_CURRENT_SOURCE_DIR}/t0_construction_rejection.cpp)
target_link_libraries(t0_construction_rejection PRIVATE g2Lib)
set_property(TARGET t0_construction_rejection PROPERTY FOLDER "G2/test")

add_test(NAME t0_construction_rejection COMMAND t0_construction_rejection)

# ---------------- t0_order
#
# Nothing in the check is an assert() and nothing in it catches an exception, so
# the failure counter is the whole observable in either build type.

add_executable(t0_order ${CMAKE_CURRENT_SOURCE_DIR}/t0_order.cpp)
target_link_libraries(t0_order PRIVATE g2Lib)
set_property(TARGET t0_order PROPERTY FOLDER "G2/test")

add_test(NAME t0_order COMMAND t0_order)

# ---------------- t0_esai_slot_phase
#
# The path of the committed fixture is passed on the command line; the test
# reads it from argv[1] and assembles the file at run time.
#
# The test must run under the JIT: g_useJIT is read at run time and the test
# fails loudly rather than skip on a non-JIT build. The fixture spin sits below
# Vba_End ($100) so dynamicFastInterrupts puts the JIT in
# FastInterruptMode::Dynamic and exec() returns after each instruction. It uses
# DspSet rather than PeripheralsNop because dynamicFastInterrupts is set in
# DspSet::Slot::Slot.

add_executable(t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_slot_phase.cpp)
target_link_libraries(t0_esai_slot_phase PRIVATE g2Lib)
set_property(TARGET t0_esai_slot_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_slot_phase COMMAND t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/esai_sync_spin.asm)
set_tests_properties(t0_esai_slot_phase PROPERTIES LABELS "UnitTest")

# ---------------- t0_esai_idle_core
#
# No .asm is committed: the program is assembled in-test and sits below Vba_End
# for the same reason t0_esai_slot_phase's does.

add_executable(t0_esai_idle_core
	${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_idle_core.cpp)
target_link_libraries(t0_esai_idle_core PRIVATE g2Lib)
set_property(TARGET t0_esai_idle_core PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_idle_core COMMAND t0_esai_idle_core)
set_tests_properties(t0_esai_idle_core PROPERTIES LABELS "UnitTest")

# ---------------- t0_transport_hub
#
# The header path is passed on the command line. The declaration-order property
# -- that ProtocolFrame and StampedFrame are declared before TransportHub -- is
# a property of the header's source text that no C++ expression can read, so the
# check reads the file.

add_executable(t0_transport_hub
	${CMAKE_CURRENT_SOURCE_DIR}/t0_transport_hub.cpp)
target_link_libraries(t0_transport_hub PRIVATE g2Lib)
set_property(TARGET t0_transport_hub PROPERTY FOLDER "G2/test")

add_test(NAME t0_transport_hub COMMAND t0_transport_hub
	${CMAKE_CURRENT_SOURCE_DIR}/../transportHub.h)
set_tests_properties(t0_transport_hub PROPERTIES LABELS "UnitTest")

# ---------------- t0_begin_play_phase

add_executable(t0_begin_play_phase
	${CMAKE_CURRENT_SOURCE_DIR}/t0_begin_play_phase.cpp)
target_link_libraries(t0_begin_play_phase PRIVATE g2Lib)
set_property(TARGET t0_begin_play_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_begin_play_phase COMMAND t0_begin_play_phase)
set_tests_properties(t0_begin_play_phase PROPERTIES LABELS "UnitTest")

# ---------------- t0_codec_regimes

add_executable(t0_codec_regimes
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_regimes.cpp)
target_link_libraries(t0_codec_regimes PRIVATE g2Lib)
set_property(TARGET t0_codec_regimes PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_regimes COMMAND t0_codec_regimes)
set_tests_properties(t0_codec_regimes PROPERTIES LABELS "UnitTest")

# ---------------- t0_scheduler_faults

add_executable(t0_scheduler_faults
	${CMAKE_CURRENT_SOURCE_DIR}/t0_scheduler_faults.cpp)
target_link_libraries(t0_scheduler_faults PRIVATE g2Lib)
set_property(TARGET t0_scheduler_faults PROPERTY FOLDER "G2/test")

add_test(NAME t0_scheduler_faults COMMAND t0_scheduler_faults)
set_tests_properties(t0_scheduler_faults PROPERTIES LABELS "UnitTest")

# ---------------- t0_thread_map

add_executable(t0_thread_map
	${CMAKE_CURRENT_SOURCE_DIR}/t0_thread_map.cpp)
target_link_libraries(t0_thread_map PRIVATE g2Lib)
set_property(TARGET t0_thread_map PROPERTY FOLDER "G2/test")

add_test(NAME t0_thread_map COMMAND t0_thread_map)
set_tests_properties(t0_thread_map PROPERTIES LABELS "UnitTest")

# ---------------- t0_mcu_debt
#
# No cycle cost is compiled in. The cost and the byte length of one dispatch
# unit are measured at run time from the linked core, and both workload rates
# and every expected spend are derived from them.

add_executable(t0_mcu_debt
	${CMAKE_CURRENT_SOURCE_DIR}/t0_mcu_debt.cpp)
target_link_libraries(t0_mcu_debt PRIVATE g2Lib)
set_property(TARGET t0_mcu_debt PROPERTY FOLDER "G2/test")

add_test(NAME t0_mcu_debt COMMAND t0_mcu_debt)
set_tests_properties(t0_mcu_debt PROPERTIES LABELS "UnitTest")

# ---------------- t0_scheduler_state
#
# The fixture is a field of one repeated instruction: the MCU context is the one
# part of a T0 Scheduler whose emulated state moves, and a state round trip over
# a machine whose state never moves is satisfied by a snapshot of zero bytes.

add_executable(t0_scheduler_state
	${CMAKE_CURRENT_SOURCE_DIR}/t0_scheduler_state.cpp)
target_link_libraries(t0_scheduler_state PRIVATE g2Lib)
set_property(TARGET t0_scheduler_state PROPERTY FOLDER "G2/test")

add_test(NAME t0_scheduler_state COMMAND t0_scheduler_state)
set_tests_properties(t0_scheduler_state PROPERTIES LABELS "UnitTest")

# ---------------- t0_state_excludes_regime
#
# An ordinary executable. The codec regime does not travel with a saved state
# block: a play-regime snapshot loaded into a boot-regime machine leaves the
# boot regime standing, and every other state item survives the round trip by
# value.
#
# The regime is asserted through what a quantum does and never through a byte
# offset: a boot quantum and a play quantum emit different phase sequences. The
# source carries a known positive for that instrument, so a run in which every
# quantum looked like a boot quantum cannot pass by accident.
#
# Nothing in the source is an assert() and nothing in it catches an exception.
# Every verdict is the failure counter and the process exit status.

add_executable(t0_state_excludes_regime
	${CMAKE_CURRENT_SOURCE_DIR}/t0_state_excludes_regime.cpp)
target_link_libraries(t0_state_excludes_regime PRIVATE g2Lib)
set_property(TARGET t0_state_excludes_regime PROPERTY FOLDER "G2/test")

add_test(NAME t0_state_excludes_regime COMMAND t0_state_excludes_regime)
set_tests_properties(t0_state_excludes_regime PROPERTIES LABELS "UnitTest")
