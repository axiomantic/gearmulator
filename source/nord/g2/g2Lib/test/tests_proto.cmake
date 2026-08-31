# Test registrations for the proto track. Owned by the proto track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- PROTO-1, the CRC
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_crc16$
#
# THE TEST COMPILES ../crc16.cpp DIRECTLY AND LINKS NO LIBRARY. PROTO-1's
# Files: line declares crc16.h, crc16.cpp, this test and this file -- it does
# NOT declare sources_proto.cmake, so the source is not in G2LIB_SOURCES and
# this target cannot reach it through g2Lib. The dependency is real either
# way: the checksum is free-standing arithmetic over a caller's bytes and
# needs nothing g2Lib links.

add_executable(t0_crc16 t0_crc16.cpp ../crc16.cpp)
set_property(TARGET t0_crc16 PROPERTY FOLDER "G2")

add_test(NAME t0_crc16 COMMAND t0_crc16)
set_tests_properties(t0_crc16 PROPERTIES LABELS "UnitTest")


# ----------------- PROTO-3, the internal protocol client
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_internal_client$
#
# THE TEST COMPILES ../internalClient.cpp DIRECTLY AND LINKS g2Lib FOR THE HUB.
# PROTO-3's Files: line declares internalClient.h, internalClient.cpp, this
# test and this file -- it does NOT declare sources_proto.cmake, so the client
# is not in G2LIB_SOURCES and this target cannot reach it through g2Lib. The
# hub is a different case: SCH-29 put transportHub.cpp in G2LIB_SOURCES, so
# TransportHub arrives through the library and is not compiled again here.
#
# A BUILD OF THIS TARGET SEES NONE OF THE PROPERTIES THE CHECK OWNS: that the
# client is a PEER of the usbip endpoint rather than a path through it, the
# inbox copy, the drop-and-count refusals, the receive borrow lifetime, the
# detach on destruction and the absence of allocation after construction. All
# of them report through the test's own failure counter, so NDEBUG changes no
# case in it -- nothing in the source is an assert() and nothing catches an
# exception. The compile-time half is the static_asserts, which the compiler
# carries in every build type.

add_executable(t0_internal_client
	${CMAKE_CURRENT_SOURCE_DIR}/t0_internal_client.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../internalClient.cpp)
target_link_libraries(t0_internal_client PRIVATE g2Lib)
set_property(TARGET t0_internal_client PROPERTY FOLDER "G2/test")

add_test(NAME t0_internal_client COMMAND t0_internal_client)
set_tests_properties(t0_internal_client PROPERTIES LABELS "UnitTest")


# ----------------- PROTO-11, the .pch2 load through the protocol (T0 half)
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_pch2_load$
#
# THE TEST COMPILES THREE SOURCES DIRECTLY AND LINKS g2Lib FOR THE HUB ALONE.
# ../../g2JucePlugin/g2PatchLoad.cpp, ../internalClient.cpp and ../crc16.cpp
# are all outside G2LIB_SOURCES, because sources_proto.cmake is EMPTY: PROTO-1
# declared crc16.cpp, PROTO-3 declared internalClient.cpp and PROTO-11 declares
# g2PatchLoad.cpp, and no block's Files: line declares sources_proto.cmake, so
# nothing ever appended any of the three. The hub is the one exception:
# SCH-29 put transportHub.cpp in G2LIB_SOURCES, so TransportHub arrives through
# the library and is not compiled again here.
#
# THAT WORKAROUND COVERS THIS CHECK AND DOES NOT COVER THE REAL CONSUMER.
# g2PatchLoad.cpp lives in g2JucePlugin, which is a real g2Lib consumer and
# links the library rather than compiling its sources again; the moment the
# plugin track adds it to that target it will fail to link InternalClient. That
# gap belongs to sources_proto.cmake, which is EMPTY and which no block's
# Files: line owns, and it is reported rather than widened here.
#
# THE CORPUS COMES FROM tests_board.cmake's G2_ORACLE_TOOLS_DIR, which is
# either a local axiomantic/nmg2-tools checkout or a FetchContent of a pinned
# commit. tests_board.cmake is included before this file, so the variable is in
# scope. NO CLAVIA BYTE ARRIVES THROUGH IT: the synthesized corpus is authored
# by task TOOL-12 and every byte of it is this project's own.
#
# THE TEST IS REGISTERED WHETHER OR NOT THE CORPUS WAS FOUND, and it FAILS at
# run time when it is missing. Registering it conditionally would make
# `ctest --no-tests=error -R ^t0_pch2_load$` fail with "no tests found", which
# reads as a broken build rather than as an absent corpus; and skipping inside
# the test would return 0 and count as a pass. The T0 half has no gate.

add_executable(t0_pch2_load
	${CMAKE_CURRENT_SOURCE_DIR}/t0_pch2_load.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../internalClient.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../crc16.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_pch2_load PRIVATE g2Lib)
set_property(TARGET t0_pch2_load PROPERTY FOLDER "G2/test")
target_compile_definitions(t0_pch2_load PRIVATE
	G2_PCH2_SYNTH_CORPUS_DIR="${G2_ORACLE_TOOLS_DIR}/nmg2_tools/testdata/pch2_synth"
	G2_PCH2_EXPECTED_SEQUENCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/protocol/synth_editor_sequence.txt")

add_test(NAME t0_pch2_load COMMAND t0_pch2_load)
set_tests_properties(t0_pch2_load PROPERTIES LABELS "UnitTest")


# ----------------- The Board-to-TransportHub wiring
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_board_transport$
#
# TIER T0. It constructs Boards, builds its own `.pch2` container in memory and
# reads no file at all, so it needs no artifact and boots no firmware.
#
# IT LINKS g2Lib AND COMPILES ONE SOURCE DIRECTLY. InternalClient and crc16
# now arrive THROUGH the library -- sources_proto.cmake above appends both, for
# the reason stated there. g2PatchLoad.cpp does not: it lives in g2JucePlugin
# and is not a g2Lib source, so this target compiles it, exactly as
# t0_pch2_load does.
#
# NO PLAN BLOCK OWNS THIS REGISTRATION YET. The behaviour it holds -- that a
# frame the plugin originates crosses the quantum boundary into the Board's USB
# device and that a frame the device emits crosses back to the attachments --
# had no mechanism at all before it. The owning block is OWED.

add_executable(t0_board_transport
	${CMAKE_CURRENT_SOURCE_DIR}/t0_board_transport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_board_transport PRIVATE g2Lib)
set_property(TARGET t0_board_transport PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_transport COMMAND t0_board_transport)
set_tests_properties(t0_board_transport PROPERTIES LABELS "UnitTest")

# ----------------- The patch byte that reaches the device register file
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_usb_ingress_byte$
#
# TIER T0. It builds its `.pch2` container in memory, boots no firmware and
# reads no file, exactly as t0_board_transport does, and it compiles the same
# one extra source for the same reason: g2PatchLoad.cpp lives in g2JucePlugin
# and is not a g2Lib source.
#
# WHAT IT HOLDS THAT t0_board_transport DOES NOT. t0_board_transport stops at
# the hub: its case 3 records that the Board's isp1181_rx call had an effect no
# check could observe, because isp1181_create answers a Stub-backed handle.
# This target observes the DEVICE SIDE of that call, at the CS3 data port,
# through the same bus callback the MCU core drives.
#
# NO PLAN BLOCK OWNS THIS REGISTRATION YET, for the same reason
# t0_board_transport's does not. The owning block is OWED.

add_executable(t0_usb_ingress_byte
	${CMAKE_CURRENT_SOURCE_DIR}/t0_usb_ingress_byte.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_usb_ingress_byte PRIVATE g2Lib)
set_property(TARGET t0_usb_ingress_byte PROPERTY FOLDER "G2/test")

add_test(NAME t0_usb_ingress_byte COMMAND t0_usb_ingress_byte)
set_tests_properties(t0_usb_ingress_byte PROPERTIES LABELS "UnitTest")


# ----------------- The M6 remainder: a real `.pch2` into RUNNING firmware
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_patch_running$
#
# TIER T1, gated exactly as t1_egress is: it boots the Clavia firmware and it
# reads one file out of the artifact corpus, so it SKIPS with a reason when
# NMG2_ARTIFACTS names no directory.
#
# WHAT IT HOLDS THAT NOTHING ELSE DOES. t0_usb_ingress_byte proves a patch byte
# reaches the device register file the firmware reads, on a board with NO
# FIRMWARE IN IT; t1_boot and t1_egress boot the firmware and load NO PATCH.
# The two sets of files were DISJOINT. This target is the only one in the tree
# in which a real `.pch2` is handed to a machine that has really booted.
#
# IT COMPILES ONE SOURCE DIRECTLY, for the reason stated above:
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source. crc16 and
# InternalClient arrive THROUGH the library, because sources_proto.cmake
# appends both.
#
# THE PATCH IS NAMED RELATIVE TO NMG2_ARTIFACTS AND NEVER COPIED INTO THIS
# TREE. It is Clavia-derived, and the artifact directory is where this project
# keeps Clavia-derived bytes; a copy under source/ would put them in the
# repository. The definition carries the relative name only, so a machine
# without the corpus reaches the gate rather than a missing file.

add_executable(t1_patch_running
	${CMAKE_CURRENT_SOURCE_DIR}/t1_patch_running.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t1_patch_running PRIVATE g2Lib)
set_property(TARGET t1_patch_running PROPERTY FOLDER "G2/test")
target_compile_definitions(t1_patch_running PRIVATE
	G2_PATCH_RELATIVE_PATH="corpus/pch2/BackTo72 demo.pch2")

# THE SKIP CODE IS READ OUT OF gatedFixture.h HERE TOO, and not taken from
# tests_int.cmake: that file is included AFTER this one, so its variable is not
# in scope. Both sites read the SAME header, so the two derivations cannot
# drift, and a header that stops defining it fails the configure instead of
# quietly restoring the exit-0 skip that ctest scores Passed.

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_protoGatedSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_protoGatedSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_protoGatedSkipExitCode "${CMAKE_MATCH_1}")

add_test(NAME t1_patch_running COMMAND t1_patch_running)
set_tests_properties(t1_patch_running PROPERTIES
	LABELS "IntegrationTest" TIMEOUT 900 SKIP_RETURN_CODE ${g2_protoGatedSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_patch_running APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()


# ----------------- The end-to-end check: packet -> IRQ3 -> ISR -> command stream
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_usb_isr$
#
# TIER T1, gated exactly as t1_patch_running is: it boots the Clavia firmware
# and reads one file out of the artifact corpus, so it SKIPS with a reason when
# NMG2_ARTIFACTS names no directory.
#
# WHAT IT HOLDS THAT t1_patch_running DOES NOT. t1_patch_running observes the
# CS3 DATA port and the instruction-fetch stream, and reports that the firmware
# never took the packet out. It observes neither the interrupt line nor the CS3
# COMMAND port, so it cannot say WHY. This target records every command byte the
# firmware writes to the command port, samples the interrupt controller's
# presented level once per quantum, and counts instruction fetches at the
# address the CODE image installs as its level-3 handler.
#
# IT COMPILES ONE SOURCE DIRECTLY, for the reason stated above:
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source. crc16 and
# InternalClient arrive THROUGH the library, because sources_proto.cmake
# appends both.
#
# THE PATCH IS NAMED RELATIVE TO NMG2_ARTIFACTS AND NEVER COPIED INTO THIS
# TREE, exactly as t1_patch_running names it and for the same reason: it is
# Clavia-derived and a copy under source/ would put those bytes in the
# repository.

add_executable(t1_usb_isr
	${CMAKE_CURRENT_SOURCE_DIR}/t1_usb_isr.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t1_usb_isr PRIVATE g2Lib)
set_property(TARGET t1_usb_isr PROPERTY FOLDER "G2/test")
target_compile_definitions(t1_usb_isr PRIVATE
	G2_PATCH_RELATIVE_PATH="corpus/pch2/BackTo72 demo.pch2")

# The skip code is the one g2_protoGatedSkipExitCode above already read out of
# gatedFixture.h. It is not read a second time: two derivations of one number
# can drift, and the variable is in scope here.

add_test(NAME t1_usb_isr COMMAND t1_usb_isr)
set_tests_properties(t1_usb_isr PROPERTIES
	LABELS "IntegrationTest" TIMEOUT 900 SKIP_RETURN_CODE ${g2_protoGatedSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_usb_isr APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()


# ----------------- The 0x01/0x37 patch-load framing repair
#
# TIER T1, gated exactly as t1_patch_running is: it reads one file out of the
# artifact corpus, so it SKIPS with a reason when NMG2_ARTIFACTS names no
# directory. It boots no firmware, which is why it carries no TIMEOUT of the
# size the booting targets above need.
#
# IT COMPILES ONE SOURCE DIRECTLY, for the reason stated above:
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source. crc16 and
# InternalClient arrive THROUGH the library, because sources_proto.cmake
# appends both.
#
# THE PATCH IS NAMED RELATIVE TO NMG2_ARTIFACTS AND NEVER COPIED INTO THIS
# TREE, exactly as t1_patch_running names it and for the same reason: it is
# Clavia-derived and a copy under source/ would put those bytes in the
# repository. THE ENTRY NAME IS DERIVED FROM THIS PATH BY THE TEST and is not
# defined a second time here.
#
# IT NEEDS THE PYTHON ORACLE, because the composed stream is compared against
# nmg2-tools' own composer byte for byte. The three definitions below are the
# same ones tests_board.cmake hands t0_extract_matches_python, and they are in
# scope because that file is included before this one. AN ORACLE THAT IS NOT
# THERE IS A FAILURE OF THE CHECK AND NEVER A SKIP: only a missing artifact
# directory skips, and an oracle-less machine that scored Passed would report a
# framing this run never compared.

add_executable(t1_patch_load_accepted
	${CMAKE_CURRENT_SOURCE_DIR}/t1_patch_load_accepted.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t1_patch_load_accepted PRIVATE g2Lib)
set_property(TARGET t1_patch_load_accepted PROPERTY FOLDER "G2/test")
target_compile_definitions(t1_patch_load_accepted PRIVATE
	G2_PATCH_RELATIVE_PATH="corpus/pch2/BackTo72 demo.pch2"
	G2_ORACLE_PYTHON="${Python3_EXECUTABLE}"
	G2_ORACLE_TOOLS_DIR="${G2_ORACLE_TOOLS_DIR}"
	G2_ORACLE_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# The skip code is the one g2_protoGatedSkipExitCode above already read out of
# gatedFixture.h. It is not read a second time: two derivations of one number
# can drift, and the variable is in scope here.

add_test(NAME t1_patch_load_accepted COMMAND t1_patch_load_accepted)
set_tests_properties(t1_patch_load_accepted PROPERTIES
	LABELS "IntegrationTest" SKIP_RETURN_CODE ${g2_protoGatedSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_patch_load_accepted APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()

