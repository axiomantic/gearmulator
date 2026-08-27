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
