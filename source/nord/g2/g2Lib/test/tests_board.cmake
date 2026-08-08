# Test registrations for the board track. Owned by the board track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The NAME is the EXACT STRING the TASK'S Check:
# Line passes to -r. Edit no other CMake file in this tree.
#
# Created empty by task BRD-0.

# ----------------- BRD-23, the mcf5307::mcf5307 link
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mcf5307_link$
#
# The test links g2Lib and NOTHING ELSE. It never names mcf5307::mcf5307 on
# its own link line, so the header and the symbol both have to arrive through
# g2Lib's own PUBLIC link -- the one line BRD-23 turns on. Naming the core
# here as well would let this test pass with that line deleted, which is the
# exact defect it exists to catch.
#
# The target is declared UNCONDITIONALLY and is not guarded by
# if(G2_LINK_MCF5307). The guard would make the option-OFF build succeed by
# building nothing, and BRD-23's negative case asserts that the option-OFF
# build FAILS at the COMPILE step on the missing mcf5307.h.

add_executable(t0_mcf5307_link t0_mcf5307_link.cpp)
target_link_libraries(t0_mcf5307_link PRIVATE g2Lib)
set_property(TARGET t0_mcf5307_link PROPERTY FOLDER "G2/test")

add_test(NAME t0_mcf5307_link COMMAND t0_mcf5307_link)
set_tests_properties(t0_mcf5307_link PROPERTIES LABELS "UnitTest")

# ----------------- the memory decode and the two bus callbacks

add_executable(t0_memory_map t0_memory_map.cpp)
target_link_libraries(t0_memory_map PRIVATE g2Lib)
set_property(TARGET t0_memory_map PROPERTY FOLDER "G2/test")

add_test(NAME t0_memory_map COMMAND t0_memory_map)
set_tests_properties(t0_memory_map PROPERTIES LABELS "UnitTest")

# ----------------- the SIM registers

add_executable(t0_sim t0_sim.cpp)
target_link_libraries(t0_sim PRIVATE g2Lib)
set_property(TARGET t0_sim PROPERTY FOLDER "G2/test")

add_test(NAME t0_sim COMMAND t0_sim)
set_tests_properties(t0_sim PROPERTIES LABELS "UnitTest")

# ----------------- the panel and the CS5 latches

add_executable(t0_panel t0_panel.cpp)
target_link_libraries(t0_panel PRIVATE g2Lib)
set_property(TARGET t0_panel PROPERTY FOLDER "G2/test")

add_test(NAME t0_panel COMMAND t0_panel)
set_tests_properties(t0_panel PROPERTIES LABELS "UnitTest")

# ----------------- the CS1 decode

add_executable(t0_cs1_decode t0_cs1_decode.cpp)
target_link_libraries(t0_cs1_decode PRIVATE g2Lib)
set_property(TARGET t0_cs1_decode PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs1_decode COMMAND t0_cs1_decode)
set_tests_properties(t0_cs1_decode PROPERTIES LABELS "UnitTest")

# ----------------- the anomaly log

add_executable(t0_anomaly_log t0_anomaly_log.cpp)
target_link_libraries(t0_anomaly_log PRIVATE g2Lib)
set_property(TARGET t0_anomaly_log PROPERTY FOLDER "G2/test")

add_test(NAME t0_anomaly_log COMMAND t0_anomaly_log)
set_tests_properties(t0_anomaly_log PROPERTIES LABELS "UnitTest")

# ----------------- the C++ firmware extractor
#
# The test compares the C++ extractor against the Python one in
# axiomantic/nmg2-tools, so the oracle has to be on disk when the test runs and
# this block is what puts it there. A cache variable names a sibling checkout
# when a local engineer has one, and FetchContent fetches a pinned commit when
# nobody has, mirroring the arrangement the root CMakeLists.txt uses for
# mcf5307.

set(G2_NMG2_TOOLS_SOURCE_DIR "" CACHE PATH "A checkout of axiomantic/nmg2-tools to use instead of fetching one")
set(G2_NMG2_TOOLS_GIT_TAG "968090b52b4d9198027dc71587bfc97b33bc2283" CACHE STRING "The commit or tag of axiomantic/nmg2-tools to fetch")

if(G2_NMG2_TOOLS_SOURCE_DIR)
	set(G2_ORACLE_TOOLS_DIR "${G2_NMG2_TOOLS_SOURCE_DIR}")
else()
	include(FetchContent)
	FetchContent_Declare(nmg2tools
		GIT_REPOSITORY https://github.com/axiomantic/nmg2-tools.git
		GIT_TAG ${G2_NMG2_TOOLS_GIT_TAG})
	# The repository holds no CMakeLists.txt, so this populates it and adds no
	# subdirectory. The Python package is then at ${nmg2tools_SOURCE_DIR}.
	FetchContent_MakeAvailable(nmg2tools)
	set(G2_ORACLE_TOOLS_DIR "${nmg2tools_SOURCE_DIR}")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

# The test is registered whether or not either path was found, and it fails at
# run time when one is missing. Registering it conditionally would make
# `ctest --no-tests=error` report "no tests found", which reads as a broken
# build rather than as an absent oracle; and skipping inside the test would
# return 0 and count as a pass.

add_executable(t0_extract_matches_python t0_extract_matches_python.cpp)
target_link_libraries(t0_extract_matches_python PRIVATE g2Lib)
set_property(TARGET t0_extract_matches_python PROPERTY FOLDER "G2/test")
target_compile_definitions(t0_extract_matches_python PRIVATE
	G2_ORACLE_PYTHON="${Python3_EXECUTABLE}"
	G2_ORACLE_TOOLS_DIR="${G2_ORACLE_TOOLS_DIR}"
	G2_ORACLE_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_test(NAME t0_extract_matches_python COMMAND t0_extract_matches_python)
set_tests_properties(t0_extract_matches_python PROPERTIES LABELS "UnitTest")

# ----------------- the firmware version and the mismatch policy

add_executable(t0_version_mismatch t0_version_mismatch.cpp)
target_link_libraries(t0_version_mismatch PRIVATE g2Lib)
set_property(TARGET t0_version_mismatch PROPERTY FOLDER "G2/test")

add_test(NAME t0_version_mismatch COMMAND t0_version_mismatch)
set_tests_properties(t0_version_mismatch PROPERTIES LABELS "UnitTest")

# ----------------- the no-firmware path
#
# The test is not gated and must not be. It drives the case where the firmware
# is absent, so a gate on NMG2_ARTIFACTS would skip the one state it exists to
# answer. The test sets and clears the variable itself.

add_executable(t0_no_firmware t0_no_firmware.cpp)
target_link_libraries(t0_no_firmware PRIVATE g2Lib)
set_property(TARGET t0_no_firmware PROPERTY FOLDER "G2/test")

add_test(NAME t0_no_firmware COMMAND t0_no_firmware)
set_tests_properties(t0_no_firmware PROPERTIES LABELS "UnitTest")

# ----------------- the flash model
#
# The test authors both chip-select images and both reset-vector longwords.
# NMG2_ARTIFACTS is unset: every byte the test loads is synthetic and no
# Clavia byte is read.

add_executable(t0_flash t0_flash.cpp)
target_link_libraries(t0_flash PRIVATE g2Lib)
set_property(TARGET t0_flash PROPERTY FOLDER "G2/test")

add_test(NAME t0_flash COMMAND t0_flash)
set_tests_properties(t0_flash PROPERTIES LABELS "UnitTest")

# ----------------- BRD-3, the two-tier interrupt controller
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_interrupts$

add_executable(t0_interrupts t0_interrupts.cpp)
target_link_libraries(t0_interrupts PRIVATE g2Lib)
set_property(TARGET t0_interrupts PROPERTY FOLDER "G2/test")

add_test(NAME t0_interrupts COMMAND t0_interrupts)
set_tests_properties(t0_interrupts PROPERTIES LABELS "UnitTest")
