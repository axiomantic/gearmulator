# Test registrations for the dsp track. Owned by the dsp track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- DSP-7, eight DSPs attached face by face
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_dsp_attach$
#
# Asserts the properties this row owns: the X-space face in _pX and the
# Y-space face in _pY on each of eight slots; one armed DMA channel per slot,
# checked through the dispatch branch's own effect rather than through an
# assert() that NDEBUG removes; the two slot-mask registers round-tripped
# through the attached Y pointer and read back through both faces; $FFFF88
# answered by the timers in the X space and by ESAI_1 in the Y space and
# neither answering the other; and the state trio, with a save-and-load round
# trip over the register block and the three memory areas of every slot.

add_executable(t0_dsp_attach t0_dsp_attach.cpp)
target_link_libraries(t0_dsp_attach PRIVATE g2Lib)
set_property(TARGET t0_dsp_attach PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_attach COMMAND t0_dsp_attach)
set_tests_properties(t0_dsp_attach PROPERTIES LABELS "UnitTest")

# ----------------- DSP-16, the host-port callback bridge
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_hdi08_dsp_bridge$
#
# Constructs a real DSP behind ONE host port and asserts a word crosses in each
# direction, that an unbridged pair carries nothing either way, that a word
# driven at an unbridged port reaches no bridged DSP, and that the DSP's HF2 and
# HF3 reach the host ISR.

add_executable(t0_hdi08_dsp_bridge t0_hdi08_dsp_bridge.cpp)
target_link_libraries(t0_hdi08_dsp_bridge PRIVATE g2Lib)
set_property(TARGET t0_hdi08_dsp_bridge PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_dsp_bridge COMMAND t0_hdi08_dsp_bridge)
set_tests_properties(t0_hdi08_dsp_bridge PROPERTIES LABELS "UnitTest")
