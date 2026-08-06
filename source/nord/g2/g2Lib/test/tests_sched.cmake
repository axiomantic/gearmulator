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
