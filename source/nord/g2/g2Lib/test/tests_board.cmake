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

# ----------------- the HDI08 host-to-DSP flag bridge
#
# T0 and ungated. The test constructs a single DSP behind one host port, bridges
# them, and asserts that an ICR write of HF0 (0x08) reaches the DSP's HSR, that
# an ICR write without HF0 does not, and that an unbridged port does not forward.
#
# It links g2Lib and nothing else: it constructs an Hdi08Adapter and an
# Hdi08Bridge with the real mc68k and dsp56kEmu behind them, and g2Lib carries
# that link itself.

add_executable(t0_hdi08_flag_bridge t0_hdi08_flag_bridge.cpp)
target_link_libraries(t0_hdi08_flag_bridge PRIVATE g2Lib)
set_property(TARGET t0_hdi08_flag_bridge PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_flag_bridge COMMAND t0_hdi08_flag_bridge)
set_tests_properties(t0_hdi08_flag_bridge PROPERTIES LABELS "UnitTest")

# ----------------- BRD-4, UART0
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_uart0$
#
# UART0 at MBAR+0x1C0, vector 0x42, divider 0x36, 8N1; UART1 unused reads reset
# values. The test drives the register file directly and wires UART0 to a
# BRD-3 controller to assert the vectored (vector 0x42, autovector 0) source,
# and exercises the one restricted width rule of MCF5307 UM section 14.3.7
# (all UART registers are bytes).

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

# ----------------- the 1 kHz USB start-of-frame tick
#
# This target compiles board.cpp and links no library, and that is the
# observation mechanism rather than a shortcut. The behaviour under test is a
# call the Board makes OUT to isp1181_tick, and the shipped Board exposes no way
# to observe it. The test therefore supplies its own definitions of the mcf5307
# entry points board.cpp uses, which requires that libmcf5307.a is absent from
# this link: defining isp1181_tick while that archive is on the link line is a
# duplicate-symbol error as soon as anything pulls the archive member that also
# defines it. Linking g2Lib would put that archive on the line through g2Lib's
# own PUBLIC link.
#
# The mcf5307 include directory is taken from the imported target's INTERFACE
# property rather than linking it, so the header arrives and the archive does
# not.
#
# The executable is declared unconditionally and only the include directory is
# guarded. At G2_LINK_MCF5307=OFF the imported target does not exist, so naming
# it in a generator expression fails the GENERATE step of any configure that
# turns the option off -- and t0_clock_guard's control configure is exactly such
# a configure. Guarding the target instead of the executable keeps the negative
# case intact: at OFF this target still builds and still fails at the COMPILE
# step on the missing mcf5307.h, rather than passing by building nothing.

add_executable(t0_sof_tick t0_sof_tick.cpp ../board.cpp)
target_include_directories(t0_sof_tick PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(TARGET mcf5307::mcf5307)
	target_include_directories(t0_sof_tick PRIVATE
		$<TARGET_PROPERTY:mcf5307::mcf5307,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
set_property(TARGET t0_sof_tick PROPERTY FOLDER "G2/test")

add_test(NAME t0_sof_tick COMMAND t0_sof_tick)
set_tests_properties(t0_sof_tick PROPERTIES LABELS "UnitTest")

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

# ----------------- the board composition
#
# The test links g2Lib and names no other library, which is t0_board_surface's
# form and not t0_sof_tick's. The distinction is deliberate. t0_sof_tick
# compiles ../board.cpp directly and DEFINES the mcf5307 entry points itself, so
# it must keep libmcf5307.a off the link line. This test needs the OPPOSITE: the
# real Flash, Panel, Latches, Hdi08Adapter, MemoryMap, Sim and Uart0, all of
# which are g2Lib sources, plus the real board.cpp that composes them. Linking
# g2Lib delivers every one of them with the real mcf5307 behind it.
#
# Nothing here references mcf5307::mcf5307, so no if(TARGET) guard is needed:
# this block is inert in the option-OFF configure that t0_clock_guard runs as
# its control.
#
# NMG2_ARTIFACTS is not read. Both flash images the test loads are byte patterns
# the test file authors, so no Clavia byte reaches it and the tier stays T0.

add_executable(t0_board_routing t0_board_routing.cpp)
target_link_libraries(t0_board_routing PRIVATE g2Lib)
set_property(TARGET t0_board_routing PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_routing COMMAND t0_board_routing)
set_tests_properties(t0_board_routing PROPERTIES LABELS "UnitTest")

# ----------------- t0_sof_tick's include path
#
# board.h holds the composed units BY VALUE, so it includes hdi08Adapter.h,
# which includes "mc68k/hdi08.h". Every consumer of board.h therefore needs the
# directory that resolves it. t0_board_routing and t0_board_surface get it for
# free because they LINK g2Lib, whose PUBLIC hardwareLib link exports it.
# t0_sof_tick does NOT link g2Lib: it compiles ../board.cpp directly and defines
# the mcf5307 entry points itself, to keep libmcf5307.a off its link line.
#
# The include directory is taken from hardwareLib's INTERFACE property and the
# target is NOT linked, so the header arrives and no archive joins the link. The
# guard is on the property reference and not on the executable: naming a target
# that does not exist fails the GENERATE step of a configure that turns the
# option off, and t0_clock_guard's control configure is exactly such a run.

if(TARGET hardwareLib)
	target_include_directories(t0_sof_tick PRIVATE
		$<TARGET_PROPERTY:hardwareLib,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# ----------------- t0_sof_tick's own sources
#
# board.cpp CONSTRUCTS the composed units and calls into them, so a target that
# compiles board.cpp on its own must supply their objects too.
#
# This does not weaken what t0_sof_tick's own block protects. That block keeps
# libmcf5307.a off the link line, because the test DEFINES the mcf5307 entry
# points itself and the archive would collide with them. None of the sources
# below is an mcf5307 source and 68kEmu is not that archive.
#
# 68kEmu IS LINKED because hdi08Adapter.cpp holds mc68k::Hdi08 instances by
# value and needs their definitions. It is guarded on the same principle as the
# include directory above.

target_sources(t0_sof_tick PRIVATE
	../flash.cpp
	../panel.cpp
	../latches.cpp
	../hdi08Decode.cpp
	../hdi08Adapter.cpp
	../memoryMap.cpp
	../sim.cpp
	../timer.cpp
	../uart0.cpp
	../interruptController.cpp)

# 68kEmu supplies mc68k::Hdi08, which hdi08Adapter.cpp holds by value. That
# class in turn calls dsp56k::HDI08 and baseLib's logging, so both follow it
# onto the link line. None of the three is the mcf5307 archive, so the property
# t0_sof_tick's own block protects is untouched.

foreach(lib 68kEmu dsp56kEmu baseLib)
	if(TARGET ${lib})
		target_link_libraries(t0_sof_tick PRIVATE ${lib})
	endif()
endforeach()


# ----------------- the CS2 CFI query-mode protocol
#
# Ungated and T0 on purpose: a gated test cannot report on a blocker that gates
# the gate.
#
# The test links g2Lib and nothing else, which is the arrangement
# t0_board_routing already uses for the same reason: it drives Board::onRead and
# Board::onWrite with the real mcf5307 behind them, and g2Lib carries that link
# itself. Nothing here references mcf5307::mcf5307, so no if(TARGET) guard is
# needed and none is written.

add_executable(t0_cs2_cfi t0_cs2_cfi.cpp)
target_link_libraries(t0_cs2_cfi PRIVATE g2Lib)
set_property(TARGET t0_cs2_cfi PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs2_cfi COMMAND t0_cs2_cfi)
set_tests_properties(t0_cs2_cfi PROPERTIES LABELS "UnitTest")


# ----------------- the unit of the core's `size` argument, proved end to end
#
# T0 and ungated. The test hand-encodes MOVE instructions and needs no firmware
# artifact of any kind: the defect it guards is what made the firmware execute
# zero instructions, and a gated test cannot report on a blocker that gates the
# gate.
#
# The test links g2Lib and nothing else, which is the arrangement
# t0_board_routing and t0_cs2_cfi already use: it drives Board::onRead and
# Board::onWrite with the real mcf5307 core behind them. Nothing here references
# mcf5307::mcf5307, so no if(TARGET) guard is needed and none is written.

add_executable(t0_bus_size_unit t0_bus_size_unit.cpp)
target_link_libraries(t0_bus_size_unit PRIVATE g2Lib)
set_property(TARGET t0_bus_size_unit PROPERTY FOLDER "G2/test")

add_test(NAME t0_bus_size_unit COMMAND t0_bus_size_unit)
set_tests_properties(t0_bus_size_unit PROPERTIES LABELS "UnitTest")

# ----------------- the M-Bus controller and the MAX1039 slave
#
# T0 and ungated. The test needs no firmware artifact of any kind: it drives the
# module the way the measured firmware drives it and asserts the interlock the
# firmware requires, which no static status byte can satisfy.
#
# The test links g2Lib and nothing else: some of its cases drive Board::onRead
# and Board::onWrite with the real mcf5307 core behind them. Nothing here
# references mcf5307::mcf5307, so no if(TARGET) guard is needed and none is
# written.

add_executable(t0_mbus t0_mbus.cpp)
target_link_libraries(t0_mbus PRIVATE g2Lib)
set_property(TARGET t0_mbus PROPERTY FOLDER "G2/test")

add_test(NAME t0_mbus COMMAND t0_mbus)
set_tests_properties(t0_mbus PROPERTIES LABELS "UnitTest")


# ----------------- t0_sof_tick's own sources, continued
#
# The composition gained the M-Bus and its slave, and t0_sof_tick compiles
# ../board.cpp on its own, so it must supply their objects too. Neither source
# is an mcf5307 source, so the property t0_sof_tick's own block protects -- no
# mcf5307 archive on its link line -- is untouched.

target_sources(t0_sof_tick PRIVATE
	../mbus.cpp
	../max1039.cpp)

# ----------------- The Board's own DspSet

add_executable(t0_board_dsp_set t0_board_dsp_set.cpp)
target_link_libraries(t0_board_dsp_set PRIVATE g2Lib)
set_property(TARGET t0_board_dsp_set PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_dsp_set COMMAND t0_board_dsp_set)
set_tests_properties(t0_board_dsp_set PROPERTIES LABELS "UnitTest")


# ----------------- t0_sof_tick's own sources
#
# The composition holds a DspSet by value and t0_sof_tick compiles ../board.cpp
# on its own, so it must supply that member's objects too. This block is
# appended rather than folded into t0_sof_tick's own block above: this file is
# written by more than one task and an edit inside another task's block is how
# two writers lose each other's work.

target_sources(t0_sof_tick PRIVATE
	../dspSet.cpp
	../hdi08Bridge.cpp)


# ----------------- t0_sof_tick's own sources
#
# ../dspSet.cpp calls into the chain adapter. Without the adapter's objects the
# link fails on g2::ChainAdapter::attachEsai and on every callback factory it
# hands out, and the directory's compile-failure fixture takes every test here
# with it. ../chainAdapter.cpp holds Mailbox objects by value and calls
# fromEsaiFrame and toEsaiFrame, so ../mailbox.cpp and ../frame.cpp come with it.

target_sources(t0_sof_tick PRIVATE
	../chainAdapter.cpp
	../mailbox.cpp
	../frame.cpp)


# ----------------- The Board's handle to its own MCU core

add_executable(t0_board_mcu_handle t0_board_mcu_handle.cpp)
target_link_libraries(t0_board_mcu_handle PRIVATE g2Lib)
set_property(TARGET t0_board_mcu_handle PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_mcu_handle COMMAND t0_board_mcu_handle)
set_tests_properties(t0_board_mcu_handle PROPERTIES LABELS "UnitTest")

# ----------------- CS3 wired to the ISP1181
#
# T0 and ungated. The test drives the Board's installed bus callbacks at the CS3
# window and asserts status only, because the unmapped read path zeroes its
# return exactly as a benign device answer does and a value assertion would pass
# without any wiring.
#
# It links g2Lib and nothing else: it constructs a Board over its own
# BoardConfig and drives Board::onRead / Board::onWrite, the exact pointers
# mcf5307_create receives.

add_executable(t0_cs3_wire t0_cs3_wire.cpp)
target_link_libraries(t0_cs3_wire PRIVATE g2Lib)
set_property(TARGET t0_cs3_wire PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs3_wire COMMAND t0_cs3_wire)
set_tests_properties(t0_cs3_wire PROPERTIES LABELS "UnitTest")

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

# ----------------- the MCF5307 general-purpose timers

add_executable(t0_timer t0_timer.cpp)
target_link_libraries(t0_timer PRIVATE g2Lib)
set_property(TARGET t0_timer PROPERTY FOLDER "G2/test")

add_test(NAME t0_timer COMMAND t0_timer)
set_tests_properties(t0_timer PROPERTIES LABELS "UnitTest")

# ----------------- the Board owns the interrupt controller
#
# The assembled Board is driven and the core-facing call is observed, so this
# check fails when the wire is absent rather than when the class is wrong.
# t0_interrupts already drives InterruptController directly and cannot see that
# defect at all.
#
# This target compiles board.cpp and links no mcf5307 archive. The behaviour
# under test is a call the Board makes out to mcf5307_set_irq, and mcf5307.h
# publishes no getter for the presented interrupt state, so the test supplies
# that entry point itself and records what arrives. The archive cannot be on the
# link line: `nm -g libmcf5307.a` puts _mcf5307_set_irq in the same member as
# _takeInterrupt and _pendingInterrupt, which the core needs, so the member is
# always pulled and the test's own definition would be a duplicate symbol.
# Linking g2Lib would put that archive on the line through g2Lib's own PUBLIC
# link, so this target names the g2Lib sources board.cpp needs instead.
#
# The mcf5307 include directory is taken from the imported target's INTERFACE
# property rather than linking it, so the header arrives and the archive does
# not. The executable is declared unconditionally and only the property
# references are guarded.

add_executable(t0_board_interrupts
	t0_board_interrupts.cpp
	../board.cpp
	../flash.cpp
	../panel.cpp
	../latches.cpp
	../hdi08Decode.cpp
	../hdi08Adapter.cpp
	../memoryMap.cpp
	../sim.cpp
	../timer.cpp
	../uart0.cpp
	../interruptController.cpp
	../mbus.cpp
	../max1039.cpp
	../dspSet.cpp
	../hdi08Bridge.cpp
	../chainAdapter.cpp
	../mailbox.cpp
	../frame.cpp)

target_include_directories(t0_board_interrupts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(TARGET mcf5307::mcf5307)
	target_include_directories(t0_board_interrupts PRIVATE
		$<TARGET_PROPERTY:mcf5307::mcf5307,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

if(TARGET hardwareLib)
	target_include_directories(t0_board_interrupts PRIVATE
		$<TARGET_PROPERTY:hardwareLib,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# 68kEmu supplies mc68k::Hdi08, which hdi08Adapter.cpp holds by value. That
# class in turn calls dsp56k::HDI08 and baseLib's logging, so both follow it
# onto the link line. None of the three is the mcf5307 archive, so the property
# this block protects is untouched.

foreach(lib 68kEmu dsp56kEmu baseLib)
	if(TARGET ${lib})
		target_link_libraries(t0_board_interrupts PRIVATE ${lib})
	endif()
endforeach()

set_property(TARGET t0_board_interrupts PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_interrupts COMMAND t0_board_interrupts)
set_tests_properties(t0_board_interrupts PROPERTIES LABELS "UnitTest")


# ----------------- the boot handshake driven and censused over all eight host
#                   ports
#
# Tier T1 and gated because the block declares the tier, which is not the usual
# reason. The test reads no firmware artifact: it composes a Board, drives host
# flags and reads host registers, and every input it has is compiled in. It
# resolves through ArtifactResolver, so a machine without artifacts skips with a
# reason rather than passing in silence.
#
# The gate variables are computed here under names of their own rather than
# borrowed from the t1_sprintf_isolated block above: a variable borrowed across
# blocks is how one edit silently changes another registration. The skip code is
# read out of gatedFixture.h by the same regex both other sites use, so the
# spellings cannot drift; NMG2_ARTIFACTS is a cache variable, so whichever
# include site sets it first wins and every later set is a no-op with the same
# value.
#
# It links g2Lib and nothing else: the real board.cpp with the real DspSet and
# Hdi08Bridge behind it are all g2Lib sources. Nothing here references
# mcf5307::mcf5307, so no if(TARGET) guard is needed.

add_executable(t1_dsp_handshake t1_dsp_handshake.cpp)
target_link_libraries(t1_dsp_handshake PRIVATE g2Lib)
set_property(TARGET t1_dsp_handshake PROPERTY FOLDER "G2/test")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_handshakeSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_handshakeSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_handshakeSkipExitCode "${CMAKE_MATCH_1}")

if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_handshakeArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_handshakeArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_handshakeArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

add_test(NAME t1_dsp_handshake COMMAND t1_dsp_handshake)
set_tests_properties(t1_dsp_handshake PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_handshakeSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_dsp_handshake APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()

# ----------------- the GDB remote stub
#
# Tier T0 and not gated: the test reads no firmware artifact. It composes a
# Board, pokes six hand-encoded instruction words into a RAM of its own, and
# drives the stub with a test client over a loopback socket -- no `gdb` binary
# is required, so the check runs anywhere the suite does.
#
# It links g2Lib and nothing else. gdbStub.cpp is inside g2Lib through
# sources_board.cmake, so the test drives the same translation unit
# `g2TestConsole --gdb` drives and not a second copy of it.

add_executable(t0_gdb_stub t0_gdb_stub.cpp)
target_link_libraries(t0_gdb_stub PRIVATE g2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_gdb_stub PRIVATE Threads::Threads)
set_property(TARGET t0_gdb_stub PROPERTY FOLDER "G2/test")

add_test(NAME t0_gdb_stub COMMAND t0_gdb_stub)
set_tests_properties(t0_gdb_stub PROPERTIES LABELS "UnitTest")

# ----------------- M4 clause 1, the DSP DMA check driven through the command
#                   that ships it
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_dump_dsp_dma$
#
# TIER T1 AND GATED because the body boots the firmware. The registers the check
# reads are the ones the emulated kernel programmed, so an unbooted machine
# gives the check nothing to be right or wrong about. A machine without the
# artifacts prints the section 18.5 skip line and reports NOT VERIFIED.
#
# IT RUNS THE BINARY AND NOT A LIBRARY CALL. The expectation table, the
# firmware's position-to-port mapping and the conjunction that turns the rows
# into one verdict all live in g2TestConsole/main.cpp. A test that re-stated any
# of them would assert its own copy, so the test spawns g2TestConsole and reads
# its stdout. G2_TEST_CONSOLE_BINARY is the generator expression for that target
# and never a composed path: a path spelled here would name today's layout.
#
# THE DEPENDENCY IS DECLARED, not assumed from build order. Without it the test
# links and runs against whatever g2TestConsole happens to be on disk, which is
# the stale-artifact shape.
#
# The gate variables carry names of their own, for the reason the blocks above
# state: a variable borrowed across blocks is how one task's edit silently
# changes another task's registration.

# THE GUARD IS A QUESTION ABOUT THE BUILD AND NOT ABOUT THE TARGET, and
# tests_plugin.cmake states the reason at length for the same condition.
# if(TARGET g2TestConsole) is false HERE in every configure, real or scratch:
# the parent adds g2Lib before g2TestConsole, so the target does not exist yet
# at this point and the guard would register nothing with configure exit 0 and
# no diagnostic. What discriminates the two configures is g2Lib's PARENT
# directory: source/nord/g2 in the real build, which adds g2TestConsole beside
# g2Lib, and t0_clock_guard's scratch directory otherwise. Without this,
# add_dependencies names a target the scratch project never creates and takes
# that unrelated test red.

get_directory_property(g2_dumpDspDmaParentOfG2Lib DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." PARENT_DIRECTORY)

if(EXISTS "${g2_dumpDspDmaParentOfG2Lib}/g2TestConsole/CMakeLists.txt")
	add_executable(t1_dump_dsp_dma t1_dump_dsp_dma.cpp)
	target_link_libraries(t1_dump_dsp_dma PRIVATE g2Lib)
	target_compile_definitions(t1_dump_dsp_dma PRIVATE
		G2_TEST_CONSOLE_BINARY="$<TARGET_FILE:g2TestConsole>")
	add_dependencies(t1_dump_dsp_dma g2TestConsole)
	set_property(TARGET t1_dump_dsp_dma PROPERTY FOLDER "G2/test")

	file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_dumpDspDmaSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

	if(NOT g2_dumpDspDmaSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
		message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
	endif()

	set(g2_dumpDspDmaSkipExitCode "${CMAKE_MATCH_1}")

	add_test(NAME t1_dump_dsp_dma COMMAND t1_dump_dsp_dma)
	set_tests_properties(t1_dump_dsp_dma PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_dumpDspDmaSkipExitCode})

	if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
		set_property(TEST t1_dump_dsp_dma APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	endif()
else()
	message(STATUS "g2TestConsole is not part of this configure; t1_dump_dsp_dma is not registered")
endif()


# ----------------- t1_gdb_dsp, the GDB stub advancing the whole machine
#
# Gated: the test reads CODE_30000400.bin. The defect it exists for only shows
# against the real firmware, because it is the real firmware's HDI08 handshake
# that a stub stepping Board::runMcu alone can never cross. A machine without
# artifacts skips rather than passing in silence.
#
# The gate variables are this block's own rather than borrowed from a
# neighbouring block: a variable borrowed across blocks is how one edit silently
# changes another block's registration. The skip code is read out of
# gatedFixture.h by the same regex the other sites use, so the spellings cannot
# drift.
#
# gdbStub.cpp and scheduler.cpp are both inside g2Lib, so the test drives the
# same two translation units `g2TestConsole --gdb` drives and not a copy of
# either. Threads is the test client, which lives on its own thread.
#
# The timeout is the suite's outer bound and not the test's. The test carries
# its own watchdog, which prints a named reason and exits 1; this property is
# the backstop for a process that could not even reach that thread.

add_executable(t1_gdb_dsp t1_gdb_dsp.cpp)
target_link_libraries(t1_gdb_dsp PRIVATE g2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t1_gdb_dsp PRIVATE Threads::Threads)
set_property(TARGET t1_gdb_dsp PROPERTY FOLDER "G2/test")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_gdbDspSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_gdbDspSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_gdbDspSkipExitCode "${CMAKE_MATCH_1}")

add_test(NAME t1_gdb_dsp COMMAND t1_gdb_dsp)
set_tests_properties(t1_gdb_dsp PROPERTIES LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_gdbDspSkipExitCode} TIMEOUT 1200)

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_gdb_dsp APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()


# ----------------- TOOL-18, the GDB-with-traffic harness's script client
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_gdb_script$
#
# TIER T0 AND NOT GATED: the test reads no firmware artifact. It places the
# same synthetic machine t0_gdb_stub places, serves the stub over a loopback
# socket, and drives the packet sequence gdbScript.py runs -- the RSP client
# promoted from the 2026-08-28 session's scratch client. The watchpoint case
# is the one the t0_gdb_stub tier cannot reach: the delivery driven through
# pch2Load -> InternalClient -> TransportHub crosses into the device on the
# SAME machine the stub serves, so the stop names an address only real
# traffic writes.
#
# IT LINKS g2Lib AND Threads AND COMPILES ONE SOURCE DIRECTLY. g2PatchLoad.cpp
# lives in g2JucePlugin and is not a g2Lib source, exactly as
# t0_board_transport records; Threads is the client thread, which owns the
# socket while the main thread pumps the stub.

add_executable(t0_gdb_script
	t0_gdb_script.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_gdb_script PRIVATE g2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_gdb_script PRIVATE Threads::Threads)
set_property(TARGET t0_gdb_script PROPERTY FOLDER "G2/test")

add_test(NAME t0_gdb_script COMMAND t0_gdb_script)
set_tests_properties(t0_gdb_script PROPERTIES LABELS "UnitTest")


# ----------------- Board-to-TransportHub consequence: the two targets that
#                   compile ../board.cpp on their own
#
# transportHub.cpp is not an mcf5307 source and pulls no library onto either
# link line -- it includes only <atomic>, <cstring> and its own header -- so the
# property both targets protect, that no mcf5307 archive reaches them, is
# untouched.

target_sources(t0_sof_tick PRIVATE ../transportHub.cpp)
target_sources(t0_board_interrupts PRIVATE ../transportHub.cpp)

# ----------------- PLG-13, CallbackTimer
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_callback_timer$
#
# TIER T0 AND UNGATED: no firmware artifact, no booted machine, no scheduler.
# The test constructs the timer alone and drives its own begin/end pairs.
#
# WHAT THE TEST HOLDS. Design section 18.10: the surface (every method and
# Report member pinned through fully qualified member-pointer types), the
# plain ring of the last N, the running count/mean/max since construction or
# reset, reset() taking effect AT THE NEXT END() and only then, and the
# seqlock under a concurrent end() and report() -- every returned Report
# internally consistent, a retry never surfacing. The class must declare NO
# state surface (it is not part of the Scheduler snapshot): the SFINAE probe
# turns any later stateSave/stateLoad into a compile failure here.
#
# THE TEST READS NO HOST CLOCK. The duration magnitudes are whatever the
# host gives; the cases assert exact counts and order-only properties any
# monotonic clock satisfies. It carries no std::chrono include, so the
# SCH-26 lint's search, which sweeps the emulation sources, stays green on
# this file; the one sanctioned reader of a host clock remains
# ../perf/CallbackTimer.cpp, the single named entry on the lint's exclusion
# list.
#
# IT COMPILES ../perf/CallbackTimer.cpp DIRECTLY and links g2Lib.
# CallbackTimer.cpp is not yet on sources_perf.cmake -- the source-list edit
# belongs to the perf track's own file -- so the direct compile is what puts
# the object into this test's link, the same arrangement the plugin-track
# registrations use for the g2JucePlugin sources.

add_executable(t0_callback_timer
	t0_callback_timer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../perf/CallbackTimer.cpp)
target_link_libraries(t0_callback_timer PRIVATE g2Lib)
set_property(TARGET t0_callback_timer PROPERTY FOLDER "G2/test")

add_test(NAME t0_callback_timer COMMAND t0_callback_timer)
set_tests_properties(t0_callback_timer PROPERTIES LABELS "UnitTest")
