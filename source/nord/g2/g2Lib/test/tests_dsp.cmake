# Test registrations for the dsp track. Owned by the dsp track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

add_executable(t0_dspset t0_dspset.cpp)
target_link_libraries(t0_dspset PRIVATE g2Lib)
set_property(TARGET t0_dspset PROPERTY FOLDER "G2")
add_test(NAME t0_dspset COMMAND t0_dspset)
set_tests_properties(t0_dspset PROPERTIES LABELS "UnitTest")
