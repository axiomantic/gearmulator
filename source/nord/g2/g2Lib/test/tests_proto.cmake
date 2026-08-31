# Test registrations for the proto track. Owned by the proto track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- The CRC
#
# The test compiles ../crc16.cpp directly and links no library: the source is
# not in G2LIB_SOURCES, so this target cannot reach it through g2Lib. The
# checksum is free-standing arithmetic over a caller's bytes and needs nothing
# g2Lib links.

add_executable(t0_crc16 t0_crc16.cpp ../crc16.cpp)
set_property(TARGET t0_crc16 PROPERTY FOLDER "G2")

add_test(NAME t0_crc16 COMMAND t0_crc16)
set_tests_properties(t0_crc16 PROPERTIES LABELS "UnitTest")
