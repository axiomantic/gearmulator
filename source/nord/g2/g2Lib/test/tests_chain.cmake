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


# ----------------- CHN-2, the mailbox index test
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_index$

add_executable(t0_mailbox_index t0_mailbox_index.cpp)
target_link_libraries(t0_mailbox_index PRIVATE g2Lib)
set_property(TARGET t0_mailbox_index PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_index COMMAND t0_mailbox_index)
set_tests_properties(t0_mailbox_index PROPERTIES LABELS "UnitTest")

# ----------------- CHN-4, ChainTopology and mailboxCount
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_count$
#
# The registered test asserts mailboxCount in a CONSTANT EXPRESSION: the
# mailbox arrays are sized from exactly such a use, so a declaration-only
# function compiles and links a target and fails only at the constant-
# expression use (plan section 7.7.1). A static_assert is the only check
# that can catch it.

add_executable(t0_mailbox_count t0_mailbox_count.cpp)
target_link_libraries(t0_mailbox_count PRIVATE g2Lib)
set_property(TARGET t0_mailbox_count PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_count COMMAND t0_mailbox_count)
set_tests_properties(t0_mailbox_count PROPERTIES LABELS "UnitTest")
