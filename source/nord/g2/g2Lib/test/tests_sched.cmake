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
