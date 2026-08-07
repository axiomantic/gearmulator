# Test registrations for the sched track. Owned by the sched track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.
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

# ---------------- SCH-1 - t0_alloc
#
# An ordinary executable. g2/timebase.h is a header with no compiled part, so
# the link against g2Lib is for the include directories the target carries and
# for nothing else.

add_executable(t0_alloc ${CMAKE_CURRENT_SOURCE_DIR}/t0_alloc.cpp)
target_link_libraries(t0_alloc PRIVATE g2Lib)
set_property(TARGET t0_alloc PROPERTY FOLDER "G2/test")

add_test(NAME t0_alloc COMMAND t0_alloc)

# ---------------- SCH-2 - t0_frames_for_block
#
# An ordinary executable, for the reason t0_alloc gives: g2/timebase.h has no
# compiled part and the link supplies the include directories.

add_executable(t0_frames_for_block
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frames_for_block.cpp)
target_link_libraries(t0_frames_for_block PRIVATE g2Lib)
set_property(TARGET t0_frames_for_block PROPERTY FOLDER "G2/test")

add_test(NAME t0_frames_for_block COMMAND t0_frames_for_block)

# ---------------- SCH-3 - t0_clock_guard
#
# An ordinary executable, and every path it needs is passed on the command
# line. A path the test had to guess would be a path the test could get wrong
# in silence.
#
# The scratch header the negative case plants must live under source/nord/g2/,
# because that is the tree BRD-0's guard scans. The test writes it, runs one
# configure and removes it again.
#
# THE NEGATIVE CONFIGURE IS NOT THE WHOLE PROJECT. The test writes a scratch
# CMake project whose only content is one add_subdirectory of g2Lib, so it
# configures the real g2Lib/CMakeLists.txt -- where the guard lives -- and
# nothing else. That costs about a second.

add_executable(t0_clock_guard ${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard.cpp)
set_property(TARGET t0_clock_guard PROPERTY FOLDER "G2/test")

add_test(NAME t0_clock_guard COMMAND t0_clock_guard
	${CMAKE_SOURCE_DIR}
	${G2_GIT_EXECUTABLE}
	${CMAKE_COMMAND}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard_scratch.h
	${CMAKE_CURRENT_BINARY_DIR}/t0_clock_guard_work)

# THE WORK DIRECTORY IS NOT NAMED AFTER THE TARGET. CMAKE_CURRENT_BINARY_DIR is
# where the executable itself lands, so a work directory called t0_clock_guard
# would collide with the file t0_clock_guard. The test then cannot create it,
# every command it runs in it fails with no output, and the case reports
# failures that say nothing about the guard.

# ---------------- SCH-5 - t0_frame_conversion
#
# SCH-4's t0_frame_layout holds the four SIGNATURES, which is a build-time
# property. This one holds the BEHAVIOUR, so it is an ordinary executable that
# links the library the conversions live in.

add_executable(t0_frame_conversion
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frame_conversion.cpp)
target_link_libraries(t0_frame_conversion PRIVATE g2Lib)
set_property(TARGET t0_frame_conversion PROPERTY FOLDER "G2/test")

add_test(NAME t0_frame_conversion COMMAND t0_frame_conversion)

# ---------------- SCH-6 - t0_dsp_context_layout
#
# The member list is a compile-time property, so most of this check is
# static_assert and a deleted member is a build failure. Plan section 7.7.1
# separates that from a check report, so the registered program also carries a
# runtime half: the executor's pointer recovery, and that the ten members are
# ten distinct objects.

add_executable(t0_dsp_context_layout
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_context_layout.cpp)
target_link_libraries(t0_dsp_context_layout PRIVATE g2Lib)
set_property(TARGET t0_dsp_context_layout PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_context_layout COMMAND t0_dsp_context_layout)

# ---------------- SCH-10 - t0_esai_frame
#
# An ordinary executable. The fixture builds a real dsp56k::Memory, two
# PeripheralsNop and a real dsp56k::DSP, because both ESAI control-register
# writes reach through the peripheral set to the DSP.

add_executable(t0_esai_frame ${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_frame.cpp)
target_link_libraries(t0_esai_frame PRIVATE g2Lib)
set_property(TARGET t0_esai_frame PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_frame COMMAND t0_esai_frame)

# ---------------- SCH-15 - t0_codec_queue_surface
#
# An ordinary executable. A BUILD of the target tests neither the surface nor
# the refusal, so both belong to a registered program that runs. The address of
# every declared method is taken through its fully qualified type, so a renamed
# or re-signed method is a compile error and a declared-and-undefined one is a
# link error.

add_executable(t0_codec_queue_surface
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_queue_surface.cpp)
target_link_libraries(t0_codec_queue_surface PRIVATE g2Lib)
set_property(TARGET t0_codec_queue_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_queue_surface COMMAND t0_codec_queue_surface)

# ---------------- SCH-7 - t0_executor
#
# An ordinary executable. The four declared names are held by their fully
# qualified types inside the translation unit, so the compiler and the linker
# carry that half. What remains for ctest is the behaviour: the order of the
# eight jobs, that every job ran on the CALLING thread, and that a refused
# re-entry is counted. The re-entry count is an observable in every build type,
# because the default build here is Release with NDEBUG and a check whose
# predicate is "the debug build asserted" cannot fail in it.

add_executable(t0_executor ${CMAKE_CURRENT_SOURCE_DIR}/t0_executor.cpp)
target_link_libraries(t0_executor PRIVATE g2Lib)
set_property(TARGET t0_executor PROPERTY FOLDER "G2/test")

add_test(NAME t0_executor COMMAND t0_executor)

# ---------------- SCH-8 - t0_run_dsp_cycles_contract
#
# An ordinary executable. The declared signature is held by the whole function
# type inside the translation unit, so the compiler carries that half and a
# declared-and-undefined function is a link error. What remains for ctest is
# the SHAPE OF THE LOOP: a wantCycles of 0 executes no exec() at all, and a
# wantCycles of 1 against a block that costs more than 1 executes exactly one.
# Both are read from the DSP's own instruction and cycle counters, so neither
# depends on an assertion that a release build removes.

add_executable(t0_run_dsp_cycles_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles_contract.cpp)
target_link_libraries(t0_run_dsp_cycles_contract PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles_contract COMMAND t0_run_dsp_cycles_contract)

# ---------------- SCH-9 - t0_run_dsp_cycles
#
# An ordinary executable. SCH-8's t0_run_dsp_cycles_contract holds the declared
# signature and the shape of the loop; this one drives the BOUND over 1,000
# quanta for each of four scripted block lengths that straddle the budget. The
# upper half of the bound comes from the fixture's own largest measured
# dispatch unit, never from maxInstructionsPerBlock, which the shipped
# configuration leaves uncapped.

add_executable(t0_run_dsp_cycles
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles.cpp)
target_link_libraries(t0_run_dsp_cycles PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles COMMAND t0_run_dsp_cycles)

# ---------------- SCH-14 - t0_block_table_harness
#
# An ordinary executable, and the path of the committed synthetic program is
# passed on the command line. A path the check had to guess would be a path the
# check could get wrong in silence.
#
# THIS ROW ESTABLISHES NO maxDispatchCost. It verifies the instrument against a
# program whose longest block is known by construction. SCH-31 is the
# measurement that reads the real compiled kernel, and it is T1.

add_executable(t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/t0_block_table_harness.cpp)
target_link_libraries(t0_block_table_harness PRIVATE g2Lib)
set_property(TARGET t0_block_table_harness PROPERTY FOLDER "G2/test")

add_test(NAME t0_block_table_harness COMMAND t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthetic_block_program.asm)

# ---------------- SCH-16 - t0_codec_capacity
#
# An ordinary executable. SCH-15's t0_codec_queue_surface holds the declared
# members and the refusal; this one drives the CAPACITY ARITHMETIC over 1,000
# blocks and carries the negative cases that prove the three counters can fire.
# The sink is primed with the lookahead, without which a capacity of B alone
# would pass every assertion and the L + B rule would go untested.

add_executable(t0_codec_capacity
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_capacity.cpp)
target_link_libraries(t0_codec_capacity PRIVATE g2Lib)
set_property(TARGET t0_codec_capacity PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_capacity COMMAND t0_codec_capacity)

# ---------------- SCH-17 - t0_backend_rule
#
# An ordinary executable, because SCH-17's acceptance criterion is the
# NULL-vs-non-null distinction of Scheduler::create. The test reads
# dsp56k::g_useJIT at run time, so the Backend::Jit case is conditional on
# the build and the Backend::Interpreter case is unconditional. The test
# prints the build mode in its first line so the outcome is traceable
# without a separate device.
#
# No library beyond g2Lib is linked, and the g2Lib target carries the
# dsp56kEmu include directories transitively, so the test reaches
# dsp56k::g_useJIT through scheduler.h.

add_executable(t0_backend_rule ${CMAKE_CURRENT_SOURCE_DIR}/t0_backend_rule.cpp)
target_link_libraries(t0_backend_rule PRIVATE g2Lib)
set_property(TARGET t0_backend_rule PROPERTY FOLDER "G2/test")

add_test(NAME t0_backend_rule COMMAND t0_backend_rule)
