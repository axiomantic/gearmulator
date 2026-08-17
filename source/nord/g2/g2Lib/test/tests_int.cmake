# Test registrations for the int track. Owned by the int track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- INT-1, boot the firmware with a stubbed CS3
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_boot$
#
# TIER T1. The test needs the Clavia artifacts and resolves them through
# ArtifactResolver, never through getenv. With NMG2_ARTIFACTS unset it prints
# the section 18.5 skip line, reports NOT VERIFIED and exits 0; it does not
# pass silently and it does not fail an artifact-less machine.
#
# IT LINKS g2Lib AND NOTHING ELSE, so the Board, the MemoryMap and the core all
# have to arrive through g2Lib's own PUBLIC link. Naming mcf5307::mcf5307 here
# would let the test pass with BRD-23's link line deleted.

add_executable(t1_boot t1_boot.cpp)
target_link_libraries(t1_boot PRIVATE g2Lib)
set_property(TARGET t1_boot PROPERTY FOLDER "G2/test")

add_test(NAME t1_boot COMMAND t1_boot)
set_tests_properties(t1_boot PROPERTIES LABELS "IntegrationTest")
