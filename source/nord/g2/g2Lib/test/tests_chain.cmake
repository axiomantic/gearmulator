# Test registrations for the chain track. Owned by the chain track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.
#
# Created empty by task BRD-0.

# ----------------- CHN-3, SlotWriteView
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_slot_write_view$

add_executable(t0_slot_write_view t0_slot_write_view.cpp)
target_link_libraries(t0_slot_write_view PRIVATE g2Lib)
set_property(TARGET t0_slot_write_view PROPERTY FOLDER "G2/test")

add_test(NAME t0_slot_write_view COMMAND t0_slot_write_view)
set_tests_properties(t0_slot_write_view PROPERTIES LABELS "UnitTest")


# ----------------- CHN-1, the mailbox surface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_surface$

add_executable(t0_mailbox_surface t0_mailbox_surface.cpp)
target_link_libraries(t0_mailbox_surface PRIVATE g2Lib)
set_property(TARGET t0_mailbox_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_surface COMMAND t0_mailbox_surface)
set_tests_properties(t0_mailbox_surface PROPERTIES LABELS "UnitTest")

