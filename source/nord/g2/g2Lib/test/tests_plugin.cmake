# Test registrations for the plugin track. Owned by the plugin track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- g2TestConsole's subcommand surface
#
# Tier T0. Every child it spawns runs with NMG2_ARTIFACTS unset, so it boots no
# firmware and needs no artifact.
#
# It reads two things and holds no roster: the subcommand block `--help` prints,
# and the `command == "--name"` comparisons in main.cpp. The rule is that the
# two sets are equal, so the source path is a compile definition rather than a
# copied list.
#
# g2TestConsole lives in a sibling directory that is added after this one, so
# the path arrives as a generator expression and the build order as an explicit
# dependency. Without the dependency the fixture that builds this directory's
# executables would leave the console binary from the previous generation in
# place, and the test would read a stale surface.

# Registered only when g2TestConsole is part of this configure, and the
# condition is a question about the build and not about the source tree.
# t0_clock_guard configures a scratch project whose whole content is one
# add_subdirectory of g2Lib, so this file is read there too -- and there
# g2TestConsole is never added and $<TARGET_FILE:g2TestConsole> fails the
# generate step, taking that unrelated test red. `EXISTS` on the sibling path
# cannot discriminate the two: it is the same source tree in both. What
# discriminates them is g2Lib's parent directory: source/nord/g2 in the real
# build, which adds g2TestConsole beside g2Lib, and t0_clock_guard's scratch
# directory otherwise.
get_directory_property(g2_parentOfG2Lib DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." PARENT_DIRECTORY)

if(EXISTS "${g2_parentOfG2Lib}/g2TestConsole/CMakeLists.txt")
	add_executable(t0_console_subcommands t0_console_subcommands.cpp)
	set_property(TARGET t0_console_subcommands PROPERTY FOLDER "G2/test")
	target_compile_definitions(t0_console_subcommands PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>"
		G2_TEST_CONSOLE_SOURCE="${g2_parentOfG2Lib}/g2TestConsole/main.cpp")
	add_dependencies(t0_console_subcommands g2TestConsole)

	add_test(NAME t0_console_subcommands COMMAND t0_console_subcommands)
	set_tests_properties(t0_console_subcommands PROPERTIES LABELS "UnitTest")
else()
	message(STATUS "g2TestConsole is not part of this configure; t0_console_subcommands is not registered")
endif()
