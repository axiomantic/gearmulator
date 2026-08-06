# Test registrations for the board track. Owned by the board track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.
#
# Created empty by task BRD-0.

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
