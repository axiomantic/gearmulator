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
set(G2_NMG2_TOOLS_GIT_TAG "fbbff97deea9842ef4126dfe50082db2c21d85e7" CACHE STRING "The commit or tag of axiomantic/nmg2-tools to fetch")

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

# ----------------- the two-tier interrupt controller

add_executable(t0_interrupts t0_interrupts.cpp)
target_link_libraries(t0_interrupts PRIVATE g2Lib)
set_property(TARGET t0_interrupts PROPERTY FOLDER "G2/test")

add_test(NAME t0_interrupts COMMAND t0_interrupts)
set_tests_properties(t0_interrupts PROPERTIES LABELS "UnitTest")


# ----------------- the panel seam for criterion (h)

add_executable(t0_panel_seam t0_panel_seam.cpp)
target_link_libraries(t0_panel_seam PRIVATE g2Lib)
set_property(TARGET t0_panel_seam PROPERTY FOLDER "G2/test")

add_test(NAME t0_panel_seam COMMAND t0_panel_seam)
set_tests_properties(t0_panel_seam PROPERTIES LABELS "UnitTest")

# ----------------- the bootstrap ROM

add_executable(t0_bootstrap_rom t0_bootstrap_rom.cpp)
target_link_libraries(t0_bootstrap_rom PRIVATE g2Lib)
set_property(TARGET t0_bootstrap_rom PROPERTY FOLDER "G2/test")

add_test(NAME t0_bootstrap_rom COMMAND t0_bootstrap_rom)
set_tests_properties(t0_bootstrap_rom PROPERTIES LABELS "UnitTest")
# ----------------- the HDI08 host-port adapter

add_executable(t0_hdi08_adapter t0_hdi08_adapter.cpp)
target_link_libraries(t0_hdi08_adapter PRIVATE g2Lib)
set_property(TARGET t0_hdi08_adapter PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_adapter COMMAND t0_hdi08_adapter)
set_tests_properties(t0_hdi08_adapter PROPERTIES LABELS "UnitTest")

# ----------------- UART0

add_executable(t0_uart0 t0_uart0.cpp)
target_link_libraries(t0_uart0 PRIVATE g2Lib)
set_property(TARGET t0_uart0 PROPERTY FOLDER "G2/test")

add_test(NAME t0_uart0 COMMAND t0_uart0)
set_tests_properties(t0_uart0 PROPERTIES LABELS "UnitTest")

# ----------------- the Board class
#
# NMG2_ARTIFACTS is not read: the test is T0 and constructs a Board that drives
# no program, so no Clavia byte reaches it.

add_executable(t0_board_surface t0_board_surface.cpp)
target_link_libraries(t0_board_surface PRIVATE g2Lib)
set_property(TARGET t0_board_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_surface COMMAND t0_board_surface)
set_tests_properties(t0_board_surface PROPERTIES LABELS "UnitTest")

# ----------------- BRD-17, bounded non-blocking control on the HDI08 transfer path
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_nonblocking$
#
# TIMEOUT IS PART OF THE ASSERTION AND IS NOT HOUSEKEEPING. The defect this task
# prevents is a blocking wait on a full dsp56k receive ring under a
# single-threaded scheduler, which is a DEADLOCK and not a wrong answer. A build
# whose bound is removed therefore hangs rather than returning a wrong count, and
# a hung test reports neither pass nor fail. The timeout is what turns that
# deadlock into a red, and the mutation runs recorded in the task report depend
# on it.

add_executable(t0_hdi08_nonblocking t0_hdi08_nonblocking.cpp)
target_link_libraries(t0_hdi08_nonblocking PRIVATE g2Lib)
set_property(TARGET t0_hdi08_nonblocking PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_nonblocking COMMAND t0_hdi08_nonblocking)
set_tests_properties(t0_hdi08_nonblocking PROPERTIES LABELS "UnitTest" TIMEOUT 120)

# ----------------- BRD-22, the 1 kHz USB start-of-frame tick
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_sof_tick$
#
# THIS TARGET COMPILES board.cpp AND LINKS NO LIBRARY, and that is the
# observation mechanism rather than a shortcut. The behaviour under test is a
# call the Board makes OUT to isp1181_tick, and the shipped Board exposes no
# way to observe it. The test therefore supplies its own definitions of the
# mcf5307 entry points board.cpp uses, which requires that libmcf5307.a is
# absent from this link: defining isp1181_tick while that archive is on the
# link line is a duplicate-symbol error as soon as anything pulls the archive
# member that also defines it. Linking g2Lib would put that archive on the
# line through g2Lib's own PUBLIC link.
#
# The mcf5307 include directory is taken from the imported target's INTERFACE
# property rather than linking it, so the header arrives and the archive does
# not.
#
# THE EXECUTABLE IS DECLARED UNCONDITIONALLY AND ONLY THE INCLUDE DIRECTORY IS
# GUARDED, which is the same discipline the t0_mcf5307_link block above states.
# At G2_LINK_MCF5307=OFF the imported target does not exist, so naming it in a
# generator expression fails the GENERATE step of any configure that turns the
# option off -- and t0_clock_guard's control configure is exactly such a
# configure, so an unguarded reference here turns that test red. Guarding the
# target instead of the executable keeps BRD-23's negative case intact: at OFF
# this target still builds and still fails at the COMPILE step on the missing
# mcf5307.h, rather than passing by building nothing.

add_executable(t0_sof_tick t0_sof_tick.cpp ../board.cpp)
target_include_directories(t0_sof_tick PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(TARGET mcf5307::mcf5307)
	target_include_directories(t0_sof_tick PRIVATE
		$<TARGET_PROPERTY:mcf5307::mcf5307,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
set_property(TARGET t0_sof_tick PROPERTY FOLDER "G2/test")

add_test(NAME t0_sof_tick COMMAND t0_sof_tick)
set_tests_properties(t0_sof_tick PROPERTIES LABELS "UnitTest")

# ----------------- BRD-20, the P-memory write funnel
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_pmem_funnel$
#
# THE TEST LINKS g2Lib AND NAMES NO OTHER LIBRARY. The funnel is a g2Lib
# source, and the just-in-time compiler it must notify arrives through g2Lib's
# own PUBLIC link of dsp56kEmu. Nothing here references mcf5307::mcf5307, so
# no if(TARGET) guard is needed: this block is inert in the option-OFF
# configure that t0_clock_guard runs as its control.
#
# NMG2_ARTIFACTS is not read. Every word the test puts into P memory is
# assembled from text the test file authors, so no Clavia byte reaches it.

add_executable(t0_pmem_funnel t0_pmem_funnel.cpp)
target_link_libraries(t0_pmem_funnel PRIVATE g2Lib)
set_property(TARGET t0_pmem_funnel PROPERTY FOLDER "G2/test")

add_test(NAME t0_pmem_funnel COMMAND t0_pmem_funnel)
set_tests_properties(t0_pmem_funnel PROPERTIES LABELS "UnitTest")

