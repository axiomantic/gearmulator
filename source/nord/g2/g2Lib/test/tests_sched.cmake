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
