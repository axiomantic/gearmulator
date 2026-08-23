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

# ----------------- INT-6, the HDI08 host-to-DSP flag bridge
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_flag_bridge$
#
# T0 AND UNGATED. The test constructs a single DSP behind one host port, bridges
# them, and asserts that an ICR write of HF0 (0x08) reaches the DSP's HSR, that
# an ICR write without HF0 does not, and that an unbridged port does not forward.
# No firmware artifact of any kind reaches it.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement t0_board_routing,
# t0_cs2_cfi and t0_bus_size_unit already use: it constructs an Hdi08Adapter and
# an Hdi08Bridge with the real mc68k and dsp56kEmu behind them, and g2Lib carries
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
	../uart0.cpp
	../interruptController.cpp)

# 68kEmu supplies mc68k::Hdi08, which hdi08Adapter.cpp holds by value. That
# class in turn calls dsp56k::HDI08 and baseLib's logging, so both follow it
# onto the link line. NONE of the three is the mcf5307 archive, so the property
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

# ----------------- BRD-29, CS3 wired to the ISP1181 stub
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_cs3_wire$
#
# T0 AND UNGATED. The test needs no firmware artifact of any kind: it drives
# the Board's installed bus callbacks at the CS3 window and asserts STATUS
# only, because the unmapped READ path zeroes its return exactly as the benign
# stub answer does and a value assertion would pass without any wiring.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement t0_board_routing,
# t0_board_mcu_handle, t0_cs2_cfi and t0_bus_size_unit already use: it constructs
# a Board over its own BoardConfig and drives Board::onRead / Board::onWrite,
# the exact pointers mcf5307_create receives.

add_executable(t0_cs3_wire t0_cs3_wire.cpp)
target_link_libraries(t0_cs3_wire PRIVATE g2Lib)
set_property(TARGET t0_cs3_wire PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs3_wire COMMAND t0_cs3_wire)
set_tests_properties(t0_cs3_wire PROPERTIES LABELS "UnitTest")

# ----------------- BRD-31, the CS2 flash status-responder
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_cs2_status$
#
# T0 AND UNGATED. The test needs no firmware artifact of any kind: it creates
# a Flash instance directly over synthetic CS2 image bytes, reads the status
# address the firmware probes, and asserts the intercept returns the AMD
# status value 3. NMG2_ARTIFACTS is unset.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement t0_flash
# already uses: it constructs a Flash over its own bases and sizes, drives
# read8 directly, and needs no Board, no mcf5307 core and no firmware image.

add_executable(t0_cs2_status t0_cs2_status.cpp)
target_link_libraries(t0_cs2_status PRIVATE g2Lib)
set_property(TARGET t0_cs2_status PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs2_status COMMAND t0_cs2_status)
set_tests_properties(t0_cs2_status PROPERTIES LABELS "UnitTest")
