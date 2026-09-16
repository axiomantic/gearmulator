# Test registrations for the dsp track. Owned by the dsp track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The name is the exact string passed to -R.
# Edit no other CMake file in this tree.

# ----------------- the CVR host-command interrupt path

add_executable(t0_hdi08_cvr_irq t0_hdi08_cvr_irq.cpp)
target_link_libraries(t0_hdi08_cvr_irq PRIVATE g2Lib)
set_property(TARGET t0_hdi08_cvr_irq PROPERTY FOLDER "G2/test")

add_test(NAME t0_hdi08_cvr_irq COMMAND t0_hdi08_cvr_irq)
set_tests_properties(t0_hdi08_cvr_irq PROPERTIES LABELS "UnitTest")

# ----------------- the ESAI receive frame is zeroed storage
#
# frame.h states the precondition -- "a frame handed to toEsaiFrame must
# already be zeroed elsewhere" -- and nothing in g2Lib can satisfy it, because
# the storage belongs to dsp56k::Audio::Frame in the vendored tree. This target
# checks it from the consumer that depends on it, over storage it poisons
# itself so the pre-fix failure is deterministic rather than a coin flip.
#
# Receiver 1 is enabled by the real firmware and fed by SDI1 on real hardware;
# this emulation models register 0 only. The zero asserted is the correct
# reading of an input nothing drives.

add_executable(t0_rx_frame_zeroed t0_rx_frame_zeroed.cpp)
target_link_libraries(t0_rx_frame_zeroed PRIVATE g2Lib)
set_property(TARGET t0_rx_frame_zeroed PROPERTY FOLDER "G2/test")

add_test(NAME t0_rx_frame_zeroed COMMAND t0_rx_frame_zeroed)
set_tests_properties(t0_rx_frame_zeroed PROPERTIES LABELS "UnitTest")
