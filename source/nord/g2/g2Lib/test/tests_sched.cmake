# Test registrations for the sched track. Owned by the sched track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The name is the exact string ctest -R must
# match. Edit no other CMake file in this tree.

# ---------------- t0_timebase_header
#
# t0_timebase_header.cmake compiles t0_timebase_header.c as C11 at test time and
# runs it. The compile is not left to the build on purpose: that separates "the
# build broke" from "the check reported", and keeps "g2/timebase.h is a C
# header" reportable through `ctest -R`.
#
# No library is linked and no C++ is involved, so this test needs no target.

add_test(NAME t0_timebase_header
	COMMAND ${CMAKE_COMMAND}
		-DG2_C_COMPILER=${CMAKE_C_COMPILER}
		-DG2_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}
		-DG2_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.c
		-DG2_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
		-DG2_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/t0_timebase_header
		-P ${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.cmake)

set_tests_properties(t0_timebase_header PROPERTIES LABELS "UnitTest")
