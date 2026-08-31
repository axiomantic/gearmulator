# Test registrations for the proto track. Owned by the proto track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- The CRC
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_crc16$
#
# The test compiles ../crc16.cpp directly and links no library: the checksum is
# free-standing arithmetic over a caller's bytes and needs nothing g2Lib links.

add_executable(t0_crc16 t0_crc16.cpp ../crc16.cpp)
set_property(TARGET t0_crc16 PROPERTY FOLDER "G2")

add_test(NAME t0_crc16 COMMAND t0_crc16)
set_tests_properties(t0_crc16 PROPERTIES LABELS "UnitTest")


# ----------------- the internal protocol client
#
# InternalClient and the hub both arrive through g2Lib.
#
# Every case reports through the test's own failure counter, so NDEBUG changes
# none of them -- nothing in the source is an assert() and nothing catches an
# exception. The compile-time half is the static_asserts, which the compiler
# carries in every build type.

add_executable(t0_internal_client
	${CMAKE_CURRENT_SOURCE_DIR}/t0_internal_client.cpp)
target_link_libraries(t0_internal_client PRIVATE g2Lib)
set_property(TARGET t0_internal_client PROPERTY FOLDER "G2/test")

add_test(NAME t0_internal_client COMMAND t0_internal_client)
set_tests_properties(t0_internal_client PROPERTIES LABELS "UnitTest")


# ----------------- the .pch2 load through the protocol, without firmware
#
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source, so this
# target compiles it directly. crc16 and InternalClient arrive through the
# library.
#
# The corpus comes from tests_board.cmake's G2_ORACLE_TOOLS_DIR, which is
# either a local axiomantic/nmg2-tools checkout or a FetchContent of a pinned
# commit. tests_board.cmake is included before this file, so the variable is in
# scope. No Clavia byte arrives through it; the corpus is synthesized.
#
# The test is registered whether or not the corpus was found, and it fails at
# run time when it is missing. Registering it conditionally would make
# `ctest --no-tests=error -R ^t0_pch2_load$` fail with "no tests found", which
# reads as a broken build rather than as an absent corpus; and skipping inside
# the test would return 0 and count as a pass.

add_executable(t0_pch2_load
	${CMAKE_CURRENT_SOURCE_DIR}/t0_pch2_load.cpp
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
# It constructs Boards, builds its own `.pch2` container in memory and reads no
# file at all, so it needs no artifact and boots no firmware.
#
# InternalClient and crc16 arrive through g2Lib. g2PatchLoad.cpp does not: it
# lives in g2JucePlugin and is not a g2Lib source, so this target compiles it.

add_executable(t0_board_transport
	${CMAKE_CURRENT_SOURCE_DIR}/t0_board_transport.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_board_transport PRIVATE g2Lib)
set_property(TARGET t0_board_transport PROPERTY FOLDER "G2/test")

add_test(NAME t0_board_transport COMMAND t0_board_transport)
set_tests_properties(t0_board_transport PROPERTIES LABELS "UnitTest")

# ----------------- The patch byte that reaches the device register file
#
# It builds its `.pch2` container in memory, boots no firmware and reads no
# file, and it compiles the same one extra source for the same reason:
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source.
#
# t0_board_transport stops at the hub. This target observes the device side of
# the Board's isp1181_rx call, at the CS3 data port, through the same bus
# callback the MCU core drives.

add_executable(t0_usb_ingress_byte
	${CMAKE_CURRENT_SOURCE_DIR}/t0_usb_ingress_byte.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t0_usb_ingress_byte PRIVATE g2Lib)
set_property(TARGET t0_usb_ingress_byte PROPERTY FOLDER "G2/test")

add_test(NAME t0_usb_ingress_byte COMMAND t0_usb_ingress_byte)
set_tests_properties(t0_usb_ingress_byte PROPERTIES LABELS "UnitTest")


# ----------------- a real `.pch2` into running firmware
#
# Gated: it boots the Clavia firmware and reads one file out of the artifact
# corpus, so it skips with a reason when NMG2_ARTIFACTS names no directory.
#
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source, so this
# target compiles it directly. crc16 and InternalClient arrive through the
# library.
#
# The patch is named relative to NMG2_ARTIFACTS and never copied into this
# tree: it is Clavia-derived, and a copy under source/ would put those bytes in
# the repository. The definition carries the relative name only, so a machine
# without the corpus reaches the gate rather than a missing file.

add_executable(t1_patch_running
	${CMAKE_CURRENT_SOURCE_DIR}/t1_patch_running.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2PatchLoad.cpp)
target_link_libraries(t1_patch_running PRIVATE g2Lib)
set_property(TARGET t1_patch_running PROPERTY FOLDER "G2/test")
target_compile_definitions(t1_patch_running PRIVATE
	G2_PATCH_RELATIVE_PATH="corpus/pch2/BackTo72 demo.pch2")

# The skip code is read out of gatedFixture.h here too, and not taken from
# tests_int.cmake: that file is included after this one, so its variable is not
# in scope. Both sites read the same header, so the two derivations cannot
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


# ----------------- packet -> IRQ3 -> ISR -> command stream
#
# Gated: it boots the Clavia firmware and reads one file out of the artifact
# corpus, so it skips with a reason when NMG2_ARTIFACTS names no directory.
#
# t1_patch_running observes the CS3 data port and the instruction-fetch stream.
# It observes neither the interrupt line nor the CS3 command port. This target
# records every command byte the firmware writes to the command port, samples
# the interrupt controller's presented level once per quantum, and counts
# instruction fetches at the address the CODE image installs as its level-3
# handler.
#
# g2PatchLoad.cpp lives in g2JucePlugin and is not a g2Lib source, so this
# target compiles it directly. crc16 and InternalClient arrive through the
# library.
#
# The patch is named relative to NMG2_ARTIFACTS and never copied into this
# tree: it is Clavia-derived and a copy under source/ would put those bytes in
# the repository.

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

