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
