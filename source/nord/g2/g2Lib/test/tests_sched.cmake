# Test registrations for the sched track. Owned by the sched track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The NAME is the EXACT STRING the TASK'S Check:
# Line passes to -r. Edit no other CMake file in this tree.
#
# Created empty by task BRD-0.

# ---------------- SCH-0 - t0_timebase_header
#
# The check COMPILES t0_timebase_header.c as C11 at TEST time and then runs the
# grep case. Both live in t0_timebase_header.cmake, which fails if either
# fails. The compile is not left to the build on purpose: plan section 7.7.1
# separates "the build broke" from "the check reported", and design section
# 13.4.1 makes "g2/timebase.h is a C header" a contract that has to be
# reportable through `ctest -R`.
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

# ---------------- SCH-4 - t0_frame_layout
#
# An ordinary executable, because SCH-4's acceptance criterion is a BUILD-time
# property: the four fully qualified function-pointer types in
# t0_frame_layout.cpp must fail to compile when any parameter is removed from
# any one of them, and a missing conversion overload must be a link error.
# Those two failures belong to the compiler and the linker. What remains for
# ctest is the behaviour -- the sign extension, the 24-bit mask, the slot count
# and the untouched registers -- and that is what the program checks when it
# runs.

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
# t0_frame_layout holds the four signatures, which is a build-time property.
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
# The four declared names are held by their fully qualified types inside the
# translation unit, so the compiler and the linker carry that half. What remains
# is the behaviour: the order of the eight jobs, that every job ran on the
# calling thread, and that a refused re-entry is counted. The re-entry count is
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
# the loop: a wantCycles of 0 executes no exec() at all, and a wantCycles of 1
# against a block that costs more than 1 executes exactly one. Both are read
# from the DSP's own instruction and cycle counters, so neither depends on an
# assertion that a release build removes.

add_executable(t0_run_dsp_cycles_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles_contract.cpp)
target_link_libraries(t0_run_dsp_cycles_contract PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles_contract COMMAND t0_run_dsp_cycles_contract)

# ---------------- t0_run_dsp_cycles
#
# t0_run_dsp_cycles_contract holds the declared signature and the shape of the
# loop; this one drives the bound over scripted block lengths that straddle the
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
# t0_codec_queue_surface holds the declared members and the refusal; this one
# drives the capacity arithmetic and carries the negative cases that prove the
# counters can fire. The sink is primed with the lookahead, without which a
# capacity of B alone would pass every assertion and the L + B rule would go
# untested.

add_executable(t0_codec_capacity
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_capacity.cpp)
target_link_libraries(t0_codec_capacity PRIVATE g2Lib)
set_property(TARGET t0_codec_capacity PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_capacity COMMAND t0_codec_capacity)

# ---------------- t0_backend_rule
#
# The test reads dsp56k::g_useJIT at run time, so the Backend::Jit case is
# conditional on the build and the Backend::Interpreter case is unconditional.
# The test prints the build mode in its first line.
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

# ---------------- SCH-33 - t0_dsp_run_gate
#
# An ordinary executable. Unlike t0_dsp_job_order above, the context here
# carries a live DSP running a scripted loop, so this check needs a JIT backend.

add_executable(t0_dsp_run_gate
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_run_gate.cpp)
target_link_libraries(t0_dsp_run_gate PRIVATE g2Lib)
set_property(TARGET t0_dsp_run_gate PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_run_gate COMMAND t0_dsp_run_gate)

# ---------------- SCH-32 - t0_status_contract
#
# An ordinary executable. The compile-time half of the contract -- the scoping,
# the non-conversion to int and the fixed underlying type -- is held by
# static_asserts, so a violation there is a BUILD failure and this target is
# what carries it. The run-time half -- the zero value, the roster against
# Status::Count and the distinguishable failures -- reports through the test's
# own failure counter, because the default build type is Release and Release
# defines NDEBUG.
#
# It does NOT link g2Lib. status.h is a header with no compiled part and the
# test reaches it through the include directory alone, so linking the library
# would make this check wait on every other source in it without buying the
# check anything.

add_executable(t0_status_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_status_contract.cpp)
target_include_directories(t0_status_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_property(TARGET t0_status_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_status_contract COMMAND t0_status_contract)

# ---------------- SCH-18 - t0_construction_rejection
#
# An ordinary executable. Every rejectable value arrives through
# Scheduler::Config, so the check drives one Config for each row of the plan's
# rejection table and asserts BOTH halves of the row: a null return AND the
# exact g2::Status. It links g2Lib because the factory now lives in
# scheduler.cpp, and because the check constructs a real Board and the serial
# Executor to satisfy the widened signature.
#
# NDEBUG CHANGES NO CASE IN IT, and that is a property of how the check is
# written rather than one this registration exercises. Nothing in it is an
# assert() and nothing in it catches an exception, so the status out-param is the
# whole observable in either build type.
#
# WHAT THIS REGISTRATION DOES NOT COVER: the generator here is single-config and
# the tree is configured Debug, so this add_test names one build type and can
# name no other. A second tree configured -DCMAKE_BUILD_TYPE=Release, or a
# multi-config generator, running this same registration is what would cover the
# release half. This file configures no such tree.
#
# The Backend::Interpreter row is unconditional; the success cases are
# conditional on dsp56k::g_useJIT, for the reason t0_backend_rule already
# records above.

add_executable(t0_construction_rejection
	${CMAKE_CURRENT_SOURCE_DIR}/t0_construction_rejection.cpp)
target_link_libraries(t0_construction_rejection PRIVATE g2Lib)
set_property(TARGET t0_construction_rejection PROPERTY FOLDER "G2/test")

add_test(NAME t0_construction_rejection COMMAND t0_construction_rejection)

# ---------------- SCH-19 - t0_order
#
# An ordinary executable. It links g2Lib because the whole check drives a real
# Board, a real DSP set and the real ChainAdapter through Scheduler::runFrames;
# the check supplies only the two objects the factory already takes by
# injection -- an Executor and, through Config::trace, a TraceSink.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in it is an assert() and nothing in it
# catches an exception, so the failure counter is the whole observable in either
# build type. WHAT THIS REGISTRATION DOES NOT COVER: the generator here is
# single-config and the tree is configured Debug, so this add_test names one
# build type and can name no other.

add_executable(t0_order ${CMAKE_CURRENT_SOURCE_DIR}/t0_order.cpp)
target_link_libraries(t0_order PRIVATE g2Lib)
set_property(TARGET t0_order PROPERTY FOLDER "G2/test")

add_test(NAME t0_order COMMAND t0_order)

# ---------------- SCH-34 - t0_esai_slot_phase
#
# An ordinary executable. The path of the committed fixture is passed on the
# command line, the same way SCH-14's t0_block_table_harness passes
# synthetic_block_program.asm. The test reads the path from argv[1] and
# assembles the file at run time through dsp56k::Assembler.
#
# THE TEST MUST RUN UNDER THE JIT. g_useJIT is read at run time and the
# test fails loudly rather than skip on a non-JIT build. The fixture spin
# sits below Vba_End ($100) so dynamicFastInterrupts (set in DspSet::Slot::Slot
# by DSP-19's production code) puts the JIT in FastInterruptMode::Dynamic and
# exec() returns after each instruction. The test uses DspSet rather than
# PeripheralsNop because dynamicFastInterrupts is set in DspSet::Slot::Slot.

add_executable(t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_slot_phase.cpp)
target_link_libraries(t0_esai_slot_phase PRIVATE g2Lib)
set_property(TARGET t0_esai_slot_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_slot_phase COMMAND t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/esai_sync_spin.asm)
set_tests_properties(t0_esai_slot_phase PROPERTIES LABELS "UnitTest")
