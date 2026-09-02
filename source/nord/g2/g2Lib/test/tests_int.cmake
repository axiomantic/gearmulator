# Test registrations for the int track. Owned by the int track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The name is the exact string ctest -R must
# match. Edit no other CMake file in this tree.

# ----------------- boot the firmware with a stubbed CS3
#
# Tier T1. The test needs the Clavia artifacts and resolves them through
# ArtifactResolver, never through getenv. With NMG2_ARTIFACTS unset it prints
# the skip line, reports NOT VERIFIED and exits 0; it does not pass silently and
# it does not fail an artifact-less machine.
#
# It links g2Lib and nothing else, so the Board, the MemoryMap and the core all
# have to arrive through g2Lib's own PUBLIC link. Naming mcf5307::mcf5307 here
# would let the test pass with g2Lib's own link line deleted.

add_executable(t1_boot t1_boot.cpp)
target_link_libraries(t1_boot PRIVATE g2Lib)
set_property(TARGET t1_boot PROPERTY FOLDER "G2/test")

# ----------------- t1_egress, audio through the chain: the egress arrival
#
# Gated exactly as t1_boot is.

add_executable(t1_egress t1_egress.cpp)
target_link_libraries(t1_egress PRIVATE g2Lib)
set_property(TARGET t1_egress PROPERTY FOLDER "G2/test")

# ----------------- t1_chain_health, the chain-health counters
#
# Gated exactly as t1_boot is.

add_executable(t1_chain_health t1_chain_health.cpp)
target_link_libraries(t1_chain_health PRIVATE g2Lib)
set_property(TARGET t1_chain_health PROPERTY FOLDER "G2/test")

# ----------------- t1_chain_order: which hardware port carries which position
#
# Gated exactly as t1_boot is. The firmware's own port ordering is derived
# twice by paths that share no arithmetic: the base table at 0x30116970, and
# the kernel's own DMA slot counts.

add_executable(t1_chain_order t1_chain_order.cpp)
target_link_libraries(t1_chain_order PRIVATE g2Lib)
set_property(TARGET t1_chain_order PROPERTY FOLDER "G2/test")

# ----------------- the gate, made visible to ctest and satisfiable by build
#
# ctest reads an exit status and never a summary line. gatedFixture.h owns the
# skip code; it is read out of the header here rather than restated, so the two
# cannot drift, and a header that stops defining it fails the configure instead
# of quietly restoring the exit-0 skip that ctest scores Passed.

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_gatedSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_gatedSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_gatedSkipExitCode "${CMAKE_MATCH_1}")

# The artifacts the gate asks for. A checkout beside the artifacts directory
# needs no argument; anything else passes -DNMG2_ARTIFACTS=<path>. An absolute
# path to one developer's home would not survive a second checkout, and an
# unset variable is what made the gate skip in silence.
#
# An ambient NMG2_ARTIFACTS wins the default, so a shell that already exports it
# is not overridden by a guess.
if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_artifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_artifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_artifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

add_test(NAME t1_boot COMMAND t1_boot)
set_tests_properties(t1_boot PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_gatedSkipExitCode})

add_test(NAME t1_egress COMMAND t1_egress)
set_tests_properties(t1_egress PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_gatedSkipExitCode})

add_test(NAME t1_chain_health COMMAND t1_chain_health)
set_tests_properties(t1_chain_health PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_gatedSkipExitCode})

add_test(NAME t1_chain_order COMMAND t1_chain_order)
set_tests_properties(t1_chain_order PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_gatedSkipExitCode})

# Only when the directory is really there. Handing the gate a path that does not
# exist would trade the resolver's "unset" message for its "names no directory"
# message and report a machine without artifacts as a machine misconfigured.
if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_boot APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	set_property(TEST t1_egress APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	set_property(TEST t1_chain_health APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	set_property(TEST t1_chain_order APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	message(STATUS "g2 gated tests: NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
else()
	message(STATUS "g2 gated tests: no artifacts at '${NMG2_ARTIFACTS}', gated tests will report Skipped")
endif()

# t0_skip_discipline runs this binary to prove a skip reaches ctest as an exit
# code. The wiring lives here rather than in tests_repo.cmake because both
# targets exist at this point: tests_repo.cmake is included before this file,
# and t1_boot is not a target yet when it is read.
target_compile_definitions(t0_skip_discipline PRIVATE G2_GATED_EXECUTABLE="$<TARGET_FILE:t1_boot>")
add_dependencies(t0_skip_discipline t1_boot)
