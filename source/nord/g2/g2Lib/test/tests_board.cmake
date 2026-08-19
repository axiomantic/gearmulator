# Test registrations for the board track. Owned by the board track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- BRD-23, the mcf5307::mcf5307 link
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mcf5307_link$
#
# THE TEST LINKS g2Lib AND NOTHING ELSE. It never names mcf5307::mcf5307 on
# its own link line, so the header and the symbol both have to arrive through
# g2Lib's own PUBLIC link -- the one line BRD-23 turns on. Naming the core
# here as well would let this test pass with that line deleted, which is the
# exact defect it exists to catch.
#
# The target is declared UNCONDITIONALLY and is NOT guarded by
# if(G2_LINK_MCF5307). The guard would make the option-OFF build succeed by
# building nothing, and BRD-23's negative case asserts that the option-OFF
# build FAILS at the COMPILE step on the missing mcf5307.h.

add_executable(t0_mcf5307_link t0_mcf5307_link.cpp)
target_link_libraries(t0_mcf5307_link PRIVATE g2Lib)
set_property(TARGET t0_mcf5307_link PROPERTY FOLDER "G2/test")

add_test(NAME t0_mcf5307_link COMMAND t0_mcf5307_link)
set_tests_properties(t0_mcf5307_link PROPERTIES LABELS "UnitTest")

# ----------------- BRD-1, the memory decode and the two bus callbacks
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_memory_map$

add_executable(t0_memory_map t0_memory_map.cpp)
target_link_libraries(t0_memory_map PRIVATE g2Lib)
set_property(TARGET t0_memory_map PROPERTY FOLDER "G2/test")

add_test(NAME t0_memory_map COMMAND t0_memory_map)
set_tests_properties(t0_memory_map PROPERTIES LABELS "UnitTest")

# ----------------- BRD-2, the SIM registers
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_sim$

add_executable(t0_sim t0_sim.cpp)
target_link_libraries(t0_sim PRIVATE g2Lib)
set_property(TARGET t0_sim PROPERTY FOLDER "G2/test")

add_test(NAME t0_sim COMMAND t0_sim)
set_tests_properties(t0_sim PROPERTIES LABELS "UnitTest")

# ----------------- BRD-12, the panel and the CS5 latches
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_panel$

add_executable(t0_panel t0_panel.cpp)
target_link_libraries(t0_panel PRIVATE g2Lib)
set_property(TARGET t0_panel PROPERTY FOLDER "G2/test")

add_test(NAME t0_panel COMMAND t0_panel)
set_tests_properties(t0_panel PROPERTIES LABELS "UnitTest")

# ----------------- BRD-15, the CS1 decode
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_cs1_decode$

add_executable(t0_cs1_decode t0_cs1_decode.cpp)
target_link_libraries(t0_cs1_decode PRIVATE g2Lib)
set_property(TARGET t0_cs1_decode PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs1_decode COMMAND t0_cs1_decode)
set_tests_properties(t0_cs1_decode PROPERTIES LABELS "UnitTest")

# ----------------- BRD-5, the anomaly log
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_anomaly_log$

add_executable(t0_anomaly_log t0_anomaly_log.cpp)
target_link_libraries(t0_anomaly_log PRIVATE g2Lib)
set_property(TARGET t0_anomaly_log PROPERTY FOLDER "G2/test")

add_test(NAME t0_anomaly_log COMMAND t0_anomaly_log)
set_tests_properties(t0_anomaly_log PROPERTIES LABELS "UnitTest")

# ----------------- BRD-6, the C++ firmware extractor
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_extract_matches_python$
#
# THIS TEST NEEDS THE PYTHON ORACLE, because design section 20.2 makes the
# Python extractor in axiomantic/nmg2-tools the oracle for the C++ one and
# BRD-6 asserts the two produce byte-identical output. The oracle therefore has
# to be on disk when the test runs, and this block is what puts it there.
#
# THE ARRANGEMENT MIRRORS THE ONE THE ROOT CMakeLists.txt USES FOR mcf5307, and
# it is the same choice for the same reason: a cache variable names a sibling
# checkout when a local engineer has one, and FetchContent fetches a PINNED
# commit when nobody has. Two mechanisms with one spelling.
#
# NO CLAVIA BYTE ARRIVES THROUGH EITHER ROUTE. axiomantic/nmg2-tools is PUBLIC
# and MIT, it holds no firmware, and this test authors every container it reads.

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

# THE TEST IS REGISTERED WHETHER OR NOT EITHER PATH WAS FOUND, and it FAILS at
# run time when one is missing. Registering it conditionally would make
# `ctest --no-tests=error -R ^t0_extract_matches_python$` fail with "no tests
# found", which reads as a broken build rather than as an absent oracle; and
# skipping inside the test would return 0 and count as a pass. The check has no
# gate, so an oracle that is not there is a failure of the check.

add_executable(t0_extract_matches_python t0_extract_matches_python.cpp)
target_link_libraries(t0_extract_matches_python PRIVATE g2Lib)
set_property(TARGET t0_extract_matches_python PROPERTY FOLDER "G2/test")
target_compile_definitions(t0_extract_matches_python PRIVATE
	G2_ORACLE_PYTHON="${Python3_EXECUTABLE}"
	G2_ORACLE_TOOLS_DIR="${G2_ORACLE_TOOLS_DIR}"
	G2_ORACLE_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_test(NAME t0_extract_matches_python COMMAND t0_extract_matches_python)
set_tests_properties(t0_extract_matches_python PROPERTIES LABELS "UnitTest")

# ----------------- BRD-11, the firmware version and the mismatch policy
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_version_mismatch$

add_executable(t0_version_mismatch t0_version_mismatch.cpp)
target_link_libraries(t0_version_mismatch PRIVATE g2Lib)
set_property(TARGET t0_version_mismatch PROPERTY FOLDER "G2/test")

add_test(NAME t0_version_mismatch COMMAND t0_version_mismatch)
set_tests_properties(t0_version_mismatch PROPERTIES LABELS "UnitTest")

# ----------------- BRD-10, the no-firmware path
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_no_firmware$
#
# THE TEST IS NOT GATED AND IT MUST NOT BE. It drives the case where the
# firmware is ABSENT, so a gate on NMG2_ARTIFACTS would skip the one state the
# task exists to answer. The test sets and clears the variable itself.

add_executable(t0_no_firmware t0_no_firmware.cpp)
target_link_libraries(t0_no_firmware PRIVATE g2Lib)
set_property(TARGET t0_no_firmware PROPERTY FOLDER "G2/test")

add_test(NAME t0_no_firmware COMMAND t0_no_firmware)
set_tests_properties(t0_no_firmware PROPERTIES LABELS "UnitTest")

# ----------------- BRD-7, the flash model
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_flash$
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


# ----------------- BRD-13, the panel seam for criterion (h)
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_panel_seam$
#
# The panel carries `tick(uint64_t frameIndex)`, `stateSize`, `stateSave` and
# `stateLoad` now, with an empty body and a zero-byte state. The scheduler's
# order table (design section 13.5, owned by the SCH track) lists the panel at
# position 0, before the MCU. This test asserts the seam surface only; the
# scheduler and executor halves of section 13.5 are asserted by their own
# tracks' tests.

add_executable(t0_panel_seam t0_panel_seam.cpp)
target_link_libraries(t0_panel_seam PRIVATE g2Lib)
set_property(TARGET t0_panel_seam PROPERTY FOLDER "G2/test")

add_test(NAME t0_panel_seam COMMAND t0_panel_seam)
set_tests_properties(t0_panel_seam PROPERTIES LABELS "UnitTest")

# ----------------- BRD-19, the bootstrap ROM
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_bootstrap_rom$
#
# The test drives the bootstrap protocol against a synthetic image: a count
# word, an address word of 0x000000, and N data words, with no CVR host
# command. It asserts all N words land at P:$0 onward in order, asserts that
# P:$0 held none of them before the push (which is what separates a modelled
# bootstrap from a pre-load), and a negative case pushes a count of N with N-1
# data words and asserts the model reports an incomplete load rather than
# dispatching.

add_executable(t0_bootstrap_rom t0_bootstrap_rom.cpp)
target_link_libraries(t0_bootstrap_rom PRIVATE g2Lib)
set_property(TARGET t0_bootstrap_rom PROPERTY FOLDER "G2/test")

add_test(NAME t0_bootstrap_rom COMMAND t0_bootstrap_rom)
set_tests_properties(t0_bootstrap_rom PROPERTIES LABELS "UnitTest")
# ----------------- BRD-16, the HDI08 host-port adapter
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_adapter$

add_executable(t0_hdi08_adapter t0_hdi08_adapter.cpp)
target_link_libraries(t0_hdi08_adapter PRIVATE g2Lib)
set_property(TARGET t0_hdi08_adapter PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_adapter COMMAND t0_hdi08_adapter)
set_tests_properties(t0_hdi08_adapter PROPERTIES LABELS "UnitTest")

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

# ----------------- BRD-21, the Board class
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_board_surface$
#
# The surface test asserts concreteness two ways (the five static_asserts in
# board.h at compile time, the same properties through <type_traits> at run
# time), the six methods the Scheduler uses, and the single construction log
# line that names G2_MCU_CORE_CLOCK_HZ, the derived value and criterion (j).
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

# ----------------- INT-1, the board composition (plan section 24.6 row W3-115)
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_board_routing$
#
# THE TEST LINKS g2Lib AND NAMES NO OTHER LIBRARY, which is t0_board_surface's
# form and not t0_sof_tick's. The distinction is deliberate. t0_sof_tick
# compiles ../board.cpp directly and DEFINES the mcf5307 entry points itself,
# so it must keep libmcf5307.a off the link line. This test needs the OPPOSITE:
# the real Flash, Panel, Latches, Hdi08Adapter, MemoryMap, Sim and Uart0, all of
# which are g2Lib sources, plus the real board.cpp that composes them -- and
# board.cpp is itself a g2Lib source. Linking g2Lib delivers every one of them
# with the real mcf5307 behind it, which is the composition W3-115 asks for.
#
# NOTHING HERE REFERENCES mcf5307::mcf5307, so no if(TARGET) guard is needed and
# adding one would be noise: this block is inert in the option-OFF configure
# that t0_clock_guard runs as its control, exactly as t0_pmem_funnel's is.
#
# NMG2_ARTIFACTS is not read. Both flash images the test loads are byte patterns
# the test file authors, so no Clavia byte reaches it and the tier stays T0.

add_executable(t0_board_routing t0_board_routing.cpp)
target_link_libraries(t0_board_routing PRIVATE g2Lib)
set_property(TARGET t0_board_routing PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_routing COMMAND t0_board_routing)
set_tests_properties(t0_board_routing PROPERTIES LABELS "UnitTest")

# ----------------- INT-1 consequence: t0_sof_tick's include path
#
# THIS BLOCK IS APPENDED RATHER THAN FOLDED INTO t0_sof_tick's OWN BLOCK ABOVE,
# because this file is written by more than one task and an edit inside another
# task's block is how two writers lose each other's work. It adds one include
# directory to an existing target and changes nothing else about it.
#
# WHY IT IS NEEDED. INT-1's composition makes board.h hold the seven units BY
# VALUE, so board.h now includes hdi08Adapter.h, which includes "mc68k/hdi08.h".
# Every consumer of board.h therefore needs the directory that resolves it.
# t0_board_routing and t0_board_surface get it for free because they LINK g2Lib,
# whose PUBLIC hardwareLib link exports it. t0_sof_tick does NOT link g2Lib: it
# compiles ../board.cpp directly and defines the mcf5307 entry points itself, to
# keep libmcf5307.a off its link line (its own block states why). That is still
# correct and is not changed here -- it just leaves the target without the
# include directory that board.h now requires.
#
# THE INCLUDE DIRECTORY IS TAKEN FROM hardwareLib's INTERFACE PROPERTY and the
# target is NOT linked, so the header arrives and no archive joins the link. The
# GUARD IS ON THE PROPERTY REFERENCE AND NOT ON THE EXECUTABLE, which is the
# discipline the t0_mcf5307_link and t0_sof_tick blocks above both state: naming
# a target that does not exist fails the GENERATE step of a configure that turns
# the option off, and t0_clock_guard's control configure is exactly such a run.

if(TARGET hardwareLib)
	target_include_directories(t0_sof_tick PRIVATE
		$<TARGET_PROPERTY:hardwareLib,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# ----------------- INT-1 consequence: t0_sof_tick's own sources
#
# THE COMPOSITION CHANGED WHAT ../board.cpp DEPENDS ON, and this is the second
# half of that consequence. board.cpp now CONSTRUCTS the seven units and calls
# into them, so a target that compiles board.cpp on its own must supply their
# objects too. Before INT-1, board.cpp referenced no g2Lib symbol at all and
# t0_sof_tick's two-source executable was complete.
#
# THIS DOES NOT WEAKEN WHAT t0_sof_tick's OWN BLOCK PROTECTS. That block keeps
# libmcf5307.a off the link line, because the test DEFINES the mcf5307 entry
# points itself and the archive would collide with them. None of the sources
# below is an mcf5307 source and 68kEmu is not that archive, so the property is
# untouched: the test still defines its own mcf5307 stubs and still links no
# mcf5307 archive.
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


# ----------------- BRD-8, the CS2 CFI query-mode protocol
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_cs2_cfi$
#
# UNGATED AND T0 ON PURPOSE. This is the check for the M3 CFI blocker that plan
# section 6.6.9 specifies, so it belongs in the default suite: a gated test
# cannot report on a blocker that gates the gate. BRD-8's other deliverable, the
# gated T1 CS2 layout test, stays gated and is untouched by this block.
#
# THE T1 IDENTIFIER IS SPELLED OUT IN WORDS RATHER THAN WRITTEN AS A TOKEN ON
# PURPOSE. The plan measures "is any T1 test registered here?" with a grep for
# the bare prefix over this file and records the answer as zero. A mention of
# the literal inside a COMMENT would turn that measurement into a false
# positive, which is the failure shape where an absence and an unsearched file
# produce the same output.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement t0_board_routing
# already uses for the same reason: it drives Board::onRead and Board::onWrite
# with the real mcf5307 behind them, and g2Lib carries that link itself. NOTHING
# HERE REFERENCES mcf5307::mcf5307, so no if(TARGET) guard is needed and none is
# written -- an unguarded reference is what the t0_mcf5307_link block warns about.

add_executable(t0_cs2_cfi t0_cs2_cfi.cpp)
target_link_libraries(t0_cs2_cfi PRIVATE g2Lib)
set_property(TARGET t0_cs2_cfi PROPERTY FOLDER "G2/test")

add_test(NAME t0_cs2_cfi COMMAND t0_cs2_cfi)
set_tests_properties(t0_cs2_cfi PROPERTIES LABELS "UnitTest")


# ----------------- the unit of the core's `size` argument, proved end to end
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_bus_size_unit$
#
# T0 AND UNGATED. The test hand-encodes six MOVE instructions and needs no
# firmware artifact of any kind, so it belongs in the default suite: the defect
# it guards is what made the firmware execute zero instructions, and a gated
# test cannot report on a blocker that gates the gate.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement t0_board_routing
# and t0_cs2_cfi already use: it drives Board::onRead and Board::onWrite with the
# real mcf5307 core behind them, and g2Lib carries that link itself. NOTHING HERE
# REFERENCES mcf5307::mcf5307, so no if(TARGET) guard is needed and none is
# written -- an unguarded reference is what the t0_mcf5307_link block warns about.

add_executable(t0_bus_size_unit t0_bus_size_unit.cpp)
target_link_libraries(t0_bus_size_unit PRIVATE g2Lib)
set_property(TARGET t0_bus_size_unit PROPERTY FOLDER "G2/test")

add_test(NAME t0_bus_size_unit COMMAND t0_bus_size_unit)
set_tests_properties(t0_bus_size_unit PROPERTIES LABELS "UnitTest")

# ----------------- BRD-24, the M-Bus controller and the MAX1039 slave
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mbus$
#
# T0 AND UNGATED. The test needs no firmware artifact of any kind: it drives
# the module the way the measured firmware drives it and asserts the interlock
# the firmware requires, which is the M3 blocker that no static status byte can
# satisfy.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement
# t0_board_routing, t0_cs2_cfi and t0_bus_size_unit already use: its clause-3
# cases drive Board::onRead and Board::onWrite with the real mcf5307 core behind
# them, and g2Lib carries that link itself. NOTHING HERE REFERENCES
# mcf5307::mcf5307, so no if(TARGET) guard is needed and none is written.

add_executable(t0_mbus t0_mbus.cpp)
target_link_libraries(t0_mbus PRIVATE g2Lib)
set_property(TARGET t0_mbus PROPERTY FOLDER "G2/test")

add_test(NAME t0_mbus COMMAND t0_mbus)
set_tests_properties(t0_mbus PROPERTIES LABELS "UnitTest")


# ----------------- BRD-24 consequence: t0_sof_tick's own sources
#
# THE COMPOSITION GAINED TWO UNITS AND t0_sof_tick COMPILES ../board.cpp ON ITS
# OWN, so it must supply their objects too. This block is appended rather than
# folded into the INT-1 block above, for the reason that block already states:
# this file is written by more than one task and an edit inside another task's
# block is how two writers lose each other's work.
#
# NEITHER SOURCE IS AN mcf5307 SOURCE, so the property t0_sof_tick's own block
# protects -- no mcf5307 archive on its link line -- is untouched.

target_sources(t0_sof_tick PRIVATE
	../mbus.cpp
	../max1039.cpp)

# ----------------- BRD-26, the Board's own DspSet
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_board_dsp_set$
#
# T0 AND UNGATED. The test authors every word it drives, so no firmware artifact
# reaches it.
#
# THE TEST LINKS g2Lib AND NOTHING ELSE, which is the arrangement
# t0_board_routing, t0_cs2_cfi, t0_bus_size_unit and t0_mbus already use: it
# needs the real board.cpp composition with the real DspSet and Hdi08Bridge
# behind it, and every one of those is a g2Lib source. NOTHING HERE REFERENCES
# mcf5307::mcf5307, so no if(TARGET) guard is needed and none is written.

add_executable(t0_board_dsp_set t0_board_dsp_set.cpp)
target_link_libraries(t0_board_dsp_set PRIVATE g2Lib)
set_property(TARGET t0_board_dsp_set PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_dsp_set COMMAND t0_board_dsp_set)
set_tests_properties(t0_board_dsp_set PROPERTIES LABELS "UnitTest")


# ----------------- BRD-26 consequence: t0_sof_tick's own sources
#
# THE COMPOSITION GAINED A DspSet BY VALUE AND t0_sof_tick COMPILES ../board.cpp
# ON ITS OWN, so it must supply that member's objects too. This block is
# appended rather than folded into t0_sof_tick's own block above, for the reason
# the INT-1 block already states: this file is written by more than one task and
# an edit inside another task's block is how two writers lose each other's work.
#
# NEITHER SOURCE IS AN mcf5307 SOURCE, so the property t0_sof_tick's own block
# protects -- no mcf5307 archive on its link line -- is untouched. No library
# joins the link line either: dsp56kEmu is already on it through the INT-1
# consequence block above.

target_sources(t0_sof_tick PRIVATE
	../dspSet.cpp
	../hdi08Bridge.cpp)


# ----------------- DSP-8 consequence: t0_sof_tick's own sources
#
# ../dspSet.cpp GAINED A CALL INTO THE CHAIN ADAPTER AND t0_sof_tick COMPILES
# THAT SOURCE ON ITS OWN, so it must supply the adapter's objects too. Without
# them the link fails on g2::ChainAdapter::attachEsai and on every callback
# factory it hands out, and the directory's compile-failure fixture takes every
# test here with it. This block is appended rather than folded into
# t0_sof_tick's own block above, for the reason the INT-1 block already
# states: this file is
# written by more than one task and an edit inside another task's block is how
# two writers lose each other's work.
#
# THE REMAINING SOURCES FOLLOW THE FIRST. ../chainAdapter.cpp holds Mailbox
# objects by value and calls fromEsaiFrame and toEsaiFrame, so ../mailbox.cpp
# and ../frame.cpp come with it.
#
# NONE IS AN mcf5307 SOURCE, so the property t0_sof_tick's own block protects
# -- no mcf5307 archive on its link line -- is untouched. No library joins the
# link line either: dsp56kEmu is already on it through the INT-1 consequence
# block above.

target_sources(t0_sof_tick PRIVATE
	../chainAdapter.cpp
	../mailbox.cpp
	../frame.cpp)
