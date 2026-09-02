# Test registrations for the board track. Owned by the board track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The NAME is the exact string passed to -r. Edit no
# other CMake file in this tree.

# ----------------- the mcf5307::mcf5307 link
#
# The test links g2Lib and nothing else. It never names mcf5307::mcf5307 on its
# own link line, so the header and the symbol both have to arrive through
# g2Lib's own PUBLIC link. Naming the core here as well would let this test pass
# with that line deleted.
#
# The target is declared unconditionally and is not guarded by
# if(G2_LINK_MCF5307). The guard would make the option-OFF build succeed by
# building nothing; the negative case asserts that the option-OFF build fails at
# the compile step on the missing mcf5307.h.

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
set(G2_NMG2_TOOLS_GIT_TAG "oracle-wire-compose-2026-09-01" CACHE STRING "The commit or tag of axiomantic/nmg2-tools to fetch")

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

# ----------------- the HDI08 host-to-DSP flag bridge
#
# Tier T0 and ungated. The test constructs a single DSP behind one host port and
# bridges them; HF0 is 0x08 in the ICR.
#
# It links g2Lib and nothing else: it constructs an Hdi08Adapter and an
# Hdi08Bridge with the real mc68k and dsp56kEmu behind them, and g2Lib carries
# that link itself.

add_executable(t0_hdi08_flag_bridge t0_hdi08_flag_bridge.cpp)
target_link_libraries(t0_hdi08_flag_bridge PRIVATE g2Lib)
set_property(TARGET t0_hdi08_flag_bridge PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_flag_bridge COMMAND t0_hdi08_flag_bridge)
set_tests_properties(t0_hdi08_flag_bridge PROPERTIES LABELS "UnitTest")

# ----------------- UART0
#
# UART0 at MBAR+0x1C0, vector 0x42, divider 0x36, 8N1; UART1 unused reads reset
# values. The test drives the register file directly and wires UART0 to an
# interrupt controller to assert the vectored (vector 0x42, autovector 0)
# source, and exercises the one restricted width rule of MCF5307 UM section
# 14.3.7 (all UART registers are bytes).

add_executable(t0_uart0 t0_uart0.cpp)
target_link_libraries(t0_uart0 PRIVATE g2Lib)
set_property(TARGET t0_uart0 PROPERTY FOLDER "G2/test")

add_test(NAME t0_uart0 COMMAND t0_uart0)
set_tests_properties(t0_uart0 PROPERTIES LABELS "UnitTest")

# ----------------- bounded non-blocking control on the HDI08 transfer path
#
# The TIMEOUT is part of the assertion and is not housekeeping. The defect
# guarded against is a blocking wait on a full dsp56k receive ring under a
# single-threaded scheduler, which is a DEADLOCK and not a wrong answer. A build
# whose bound is removed hangs rather than returning a wrong count, and a hung
# test reports neither pass nor fail. The timeout is what turns that deadlock
# into a red.

add_executable(t0_hdi08_nonblocking t0_hdi08_nonblocking.cpp)
target_link_libraries(t0_hdi08_nonblocking PRIVATE g2Lib)
set_property(TARGET t0_hdi08_nonblocking PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_nonblocking COMMAND t0_hdi08_nonblocking)
set_tests_properties(t0_hdi08_nonblocking PROPERTIES LABELS "UnitTest" TIMEOUT 120)

# ----------------- the P-memory write funnel
#
# The test links g2Lib and names no other library. The funnel is a g2Lib source,
# and the just-in-time compiler it must notify arrives through g2Lib's own
# PUBLIC link of dsp56kEmu. Nothing here references mcf5307::mcf5307, so no
# if(TARGET) guard is needed: this block is inert in the option-OFF configure
# that t0_clock_guard runs as its control.
#
# NMG2_ARTIFACTS is not read. Every word the test puts into P memory is
# assembled from text the test file authors, so no Clavia byte reaches it.

add_executable(t0_pmem_funnel t0_pmem_funnel.cpp)
target_link_libraries(t0_pmem_funnel PRIVATE g2Lib)
set_property(TARGET t0_pmem_funnel PROPERTY FOLDER "G2/test")

add_test(NAME t0_pmem_funnel COMMAND t0_pmem_funnel)
set_tests_properties(t0_pmem_funnel PROPERTIES LABELS "UnitTest")

# ----------------- the isolated sprintf probe
#
# Tier T1 and gated: the test drives the firmware's own sprintf, and the only
# source of CODE_30000400.bin is the Clavia artifact directory. It resolves
# through ArtifactResolver, never through getenv, and skips with a reason when
# NMG2_ARTIFACTS is unset.
#
# The gate variables are computed here rather than borrowed from
# tests_int.cmake, because CMakeLists.txt includes this file first and the
# variables do not exist yet at this point. The skip code is read out of
# gatedFixture.h by the same regex tests_int.cmake uses, so the two spellings
# cannot drift; NMG2_ARTIFACTS is a cache variable, so whichever include site
# sets it first wins and the second set is a no-op with the same value.
#
# It links g2Lib and nothing else. Naming mcf5307::mcf5307 here would let the
# test pass with g2Lib's own link line deleted.

add_executable(t1_sprintf_isolated t1_sprintf_isolated.cpp)
target_link_libraries(t1_sprintf_isolated PRIVATE g2Lib)
set_property(TARGET t1_sprintf_isolated PROPERTY FOLDER "G2/test")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_sprintfSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_sprintfSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_sprintfSkipExitCode "${CMAKE_MATCH_1}")

if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_sprintfArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_sprintfArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_sprintfArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

add_test(NAME t1_sprintf_isolated COMMAND t1_sprintf_isolated)
set_tests_properties(t1_sprintf_isolated PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_sprintfSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_sprintf_isolated APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()

# ----------------- CallbackTimer
#
# Tier T0 and ungated: no firmware artifact, no booted machine, no scheduler.
# The test constructs the timer alone and drives its own begin/end pairs.
#
# reset() takes effect at the next end() and only then. The class declares no
# state surface, since it is not part of the Scheduler snapshot: the SFINAE
# probe turns any later stateSave/stateLoad into a compile failure here.
#
# The test reads no host clock. The duration magnitudes are whatever the host
# gives; the cases assert order-only properties any monotonic clock satisfies.
# It carries no std::chrono include, so the host-clock lint that sweeps the
# emulation sources stays green on this file; the one sanctioned reader of a
# host clock remains ../perf/CallbackTimer.cpp, the single named entry on that
# lint's exclusion list.
#
# It compiles ../perf/CallbackTimer.cpp directly and links g2Lib.
# CallbackTimer.cpp is not on sources_perf.cmake, so the direct compile is what
# puts the object into this test's link.

add_executable(t0_callback_timer
	t0_callback_timer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../perf/CallbackTimer.cpp)
target_link_libraries(t0_callback_timer PRIVATE g2Lib)
set_property(TARGET t0_callback_timer PROPERTY FOLDER "G2/test")

add_test(NAME t0_callback_timer COMMAND t0_callback_timer)
set_tests_properties(t0_callback_timer PROPERTIES LABELS "UnitTest")
